/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include "core/ai-json-util.h"
#include "core/ai-http-redirect-private.h"
#include "call/ai-call-config.h"
#include "call/ai-call-speech-cache.h"
#include "call/ai-call-transcript.h"

#define MEMBER "org.matrix.msc3401.call.member"
#define RING "org.matrix.msc4075.rtc.notification"
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AiVoiceAgent/1.0"

typedef struct _App App;
typedef struct _Call Call;
typedef struct _LiveReply LiveReply;
typedef void (*Reply)(App *, JsonNode *, const GError *, gpointer);
struct _App {
	GMainLoop *loop;
	SoupSession *http;
	AiConfig *config;
	AiCallConfig *call_config;
	AiCallSpeechCache *speech_cache;
	GHashTable *calls;
	GCancellable *sync_cancel;
	gchar *homeserver, *mxid, *access, *device, *jwt_url, *foci;
	gchar *stt_url, *tts_url, *livekit_url, *identity, *model;
	gchar *since, *drop_path, *greeting, *outbound_greeting;
	/* NULL directory: no transcripts. Resolved once from the call config. */
	gchar *transcript_dir, *transcript_hook;
	/* live-text: 0 off, 1 the replies, 2 the replies and the caller's words. */
	gint live_text;
	gboolean speak_code, voice_commands, room_mode;
	guint chime_threshold, chime_cooldown_ms;
	gchar *relevance_provider, *relevance_model;
	gchar *speaker_url, *voiceprint_dir, *unknown_speaker_name;
	guint speaker_threshold, guest_threshold, naming_timeout_ms;
	gboolean track_guests;
	/* --local: one session on this machine's audio devices, no Matrix. */
	gboolean local, local_echo_cancel;
	/* NULL: no state file. What a display beside the speaker reads. */
	gchar *state_file;
	AiProviderType provider;
	guint pending, startup;
	gint exit_status;
	gboolean stopping, primed, drop_busy;
	guint stop_signals;
};
struct _Call {
	grefcount refs;
	App *app;
	gchar *room, *key, *tx_token, *rx_token, *opening, *target;
	GString *context;
	AiAudioTransport *transport;
	AiVoiceSession *voice;
	AiSpeechSynthesizer *synthesizer;
	gboolean outbound, closing, history_ready, greeted, clearing, left;
	gboolean transcript_stopped; /* a write failed: no more lines, still closed */
	guint clear_attempts;
	guint retry_source, answer_source;
	GSource *goodbye_source, *goodbye_retry;
	gchar *goodbye_text;
	GCancellable *goodbye_cancel;
	guint goodbye_writes, transcripts, errors;
	gboolean goodbye_pending, goodbye_done, media_joined;
	gint64 started_at;
	gint64 answer_deadline, answered_at, transcript_at;
	GDateTime *began;
	AiCallTranscript *transcript;
	LiveReply *live; /* this turn's reply in the room, when live-text is on */
	const gchar *state_name;
	gchar *heard, *said; /* the last final utterance, the last line spoken */
};
static void
live_reply_unref(gpointer data);
typedef struct {
	App *app;
	SoupMessage *message;
	Reply callback;
	gpointer data;
	GDestroyNotify destroy;
} Request;
static void
sync_next(App *app);
static void
close_call(Call *call);
static void
finish_transcript(Call *call);
static void
shutdown_call(Call *call);
static void
write_state(Call *call);
static void
maybe_connect(Call *call);
static void
maybe_exit(App *app)
{
	if (app->stopping && app->pending == 0 && g_hash_table_size(app->calls) == 0)
		g_main_loop_quit(app->loop);
}
static void
fallback_ready(AiCallSpeechCache *cache, AiVoiceSession *session)
{
	GBytes *pcm = ai_call_speech_cache_get_pcm(cache);
	if (pcm != NULL)
		g_object_set(session, "fallback-pcm", pcm, "fallback-sample-rate",
					 ai_call_speech_cache_get_sample_rate(cache), NULL);
}
static void
fallback_completed(AiCallSpeechCache *cache, gboolean available, gpointer data)
{
	App *app = data;
	app->pending--;
	maybe_exit(app);
}
static Call *
call_ref(Call *call)
{
	g_ref_count_inc(&call->refs);
	return call;
}
static void
call_unref(gpointer data)
{
	Call *call = data;
	if (!g_ref_count_dec(&call->refs))
		return;
	if (call->retry_source != 0)
		g_source_remove(call->retry_source);
	if (call->answer_source != 0)
		g_source_remove(call->answer_source);
	if (call->voice != NULL)
		ai_voice_session_stop(call->voice);
	g_clear_object(&call->goodbye_cancel);
	g_clear_object(&call->voice);
	g_clear_object(&call->transport);
	g_clear_object(&call->synthesizer);
	g_clear_object(&call->transcript);
	g_clear_pointer(&call->live, live_reply_unref);
	g_free(call->heard);
	g_free(call->said);
	g_clear_pointer(&call->began, g_date_time_unref);
	g_free(call->room);
	g_free(call->key);
	g_free(call->tx_token);
	g_free(call->rx_token);
	g_free(call->opening);
	g_free(call->goodbye_text);
	g_free(call->target);
	g_string_free(call->context, TRUE);
	g_free(call);
}
/* A Matrix answer that was not 2xx; the code is the HTTP status. */
#define HTTP_STATUS_ERROR (g_quark_from_static_string("ai-call-http-status"))
static void
request_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Request *r = data;
	App *app = r->app;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes =
		soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root = NULL;
	if (bytes != NULL) {
		gsize size;
		const gchar *raw = g_bytes_get_data(bytes, &size);
		if (ai_http_redirect_refused(r->message, &error)) {
			/* error now says where the redirect pointed and why */
		} else if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(r->message)))
			g_set_error(&error, HTTP_STATUS_ERROR, soup_message_get_status(r->message),
						"HTTP %u", soup_message_get_status(r->message));
		else if (size > 8 * 1024 * 1024)
			g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
								"Matrix response too large");
		else if (json_parser_load_from_data(parser, raw, size, &error))
			root = json_parser_get_root(parser);
	}
	app->pending--;
	r->callback(app, root, error, r->data);
	if (r->destroy != NULL)
		r->destroy(r->data);
	g_object_unref(r->message);
	g_free(r);
	maybe_exit(app);
}
static void
request(App *app, const gchar *method, const gchar *url, gboolean matrix, JsonNode *body,
		GCancellable *cancel, Reply callback, gpointer data, GDestroyNotify destroy)
{
	Request *r = g_new0(Request, 1);
	g_autofree gchar *authorization = NULL;
	r->app = app;
	r->callback = callback;
	r->data = data;
	r->destroy = destroy;
	/* Matrix requests carry the access token and the JWT service's carry
	 * an OpenID token in the body: none of them follows a redirect. */
	r->message = ai_http_message_new(method, url, AI_HTTP_CARRIES_CREDENTIALS);
	if (r->message == NULL) {
		g_autoptr(GError) error = g_error_new_literal(
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid HTTP URL");
		callback(app, NULL, error, data);
		if (destroy != NULL)
			destroy(data);
		g_free(r);
		return;
	}
	soup_message_headers_replace(soup_message_get_request_headers(r->message),
								 "User-Agent", USER_AGENT);
	if (matrix) {
		authorization = g_strconcat("Bearer ", app->access, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(r->message),
									 "Authorization", authorization);
	}
	if (body != NULL) {
		g_autofree gchar *json = json_to_string(body, FALSE);
		g_autoptr(GBytes) bytes = g_bytes_new(json, strlen(json));
		soup_message_set_request_body_from_bytes(r->message, "application/json", bytes);
	}
	app->pending++;
	soup_session_send_and_read_async(app->http, r->message, G_PRIORITY_DEFAULT, cancel,
									 request_done, r);
}
static JsonObject *
object(JsonNode *root)
{
	return root != NULL && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root)
														: NULL;
}
static JsonNode *
empty_object(void)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, json_object_new());
	return node;
}
static gchar *
room_url(Call *call, const gchar *suffix)
{
	g_autofree gchar *room = g_uri_escape_string(call->room, NULL, FALSE);
	return g_strdup_printf("%s/_matrix/client/v3/rooms/%s/%s", call->app->homeserver,
						   room, suffix);
}
static void
put_member(Call *call, JsonNode *body, Reply callback)
{
	g_autofree gchar *key = g_uri_escape_string(call->key, NULL, FALSE);
	g_autofree gchar *suffix = g_strdup_printf("state/" MEMBER "/%s", key);
	g_autofree gchar *url = room_url(call, suffix);
	request(call->app, "PUT", url, TRUE, body, NULL, callback, call_ref(call),
			call_unref);
}
static void
removed_if_done(Call *call)
{
	App *app = call->app;
	/* A call that ended can finish clearing after the caller rang back and a
	 * new call took the room: remove only ourselves. */
	if (call->left && !call->clearing && g_hash_table_lookup(app->calls, call->room) == call)
		g_hash_table_remove(app->calls, call->room);
	maybe_exit(app);
}
static gboolean
retry_clear(gpointer data);
static void
cleared(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	if (error != NULL) {
		gboolean permanent = error->domain == HTTP_STATUS_ERROR && error->code >= 400 &&
							 error->code < 500 && error->code != 429;
		/* 401 or 403 will not change by asking again, and a shutdown must end:
		 * leave the membership for the next start's stale sweep. */
		if (permanent || (call->app->stopping && ++call->clear_attempts >= 3)) {
			g_printerr("Membership clear failed; giving up: %s\n", error->message);
			call->clearing = FALSE;
			removed_if_done(call);
			return;
		}
		g_printerr("Membership clear failed; retrying: %s\n", error->message);
		call->retry_source = g_timeout_add_seconds_full(
			G_PRIORITY_DEFAULT, 2, retry_clear, call_ref(call), call_unref);
		return;
	}
	call->clearing = FALSE;
	removed_if_done(call);
}
static gboolean
retry_clear(gpointer data)
{
	Call *call = data;
	g_autoptr(JsonNode) body = empty_object();
	call->retry_source = 0;
	put_member(call, body, cleared);
	return G_SOURCE_REMOVE;
}
static void
left_room(GObject *source, GAsyncResult *result, gpointer data)
{
	Call *call = data;
	g_autoptr(GError) error = NULL;
	ai_audio_transport_leave_finish(AI_AUDIO_TRANSPORT(source), result, &error);
	if (error != NULL)
		g_printerr("Media leave: %s\n", error->message);
	call->left = TRUE;
	removed_if_done(call);
	call_unref(call);
}
static guint64
transport_counter(Call *call, const gchar *name)
{
	guint64 value = 0;
	if (call->transport != NULL &&
		g_object_class_find_property(G_OBJECT_GET_CLASS(call->transport), name) != NULL)
		g_object_get(call->transport, name, &value, NULL);
	return value;
}
static void
close_call(Call *call)
{
	g_autoptr(JsonNode) body = empty_object();
	if (call->closing)
		return;
	call->closing = TRUE;
	if (call->goodbye_source != NULL) {
		g_source_destroy(call->goodbye_source);
		g_clear_pointer(&call->goodbye_source, g_source_unref);
	}
	if (call->goodbye_retry != NULL) {
		g_source_destroy(call->goodbye_retry);
		g_clear_pointer(&call->goodbye_retry, g_source_unref);
	}
	if (call->goodbye_cancel != NULL)
		g_cancellable_cancel(call->goodbye_cancel);
	g_log("ai-call", G_LOG_LEVEL_INFO,
		  "Call summary: room=%s duration-ms=%" G_GINT64_FORMAT
		  " transcripts=%u errors=%u reconnects=%" G_GUINT64_FORMAT
		  " dropped-buffers=%" G_GUINT64_FORMAT " late-buffers=%" G_GUINT64_FORMAT,
		  call->room, (g_get_monotonic_time() - call->started_at) / 1000,
		  call->transcripts, call->errors, transport_counter(call, "reconnect-count"),
		  transport_counter(call, "dropped-buffers"),
		  transport_counter(call, "late-buffers"));
	finish_transcript(call);
	write_state(call);
	call->clearing = TRUE;
	if (call->answer_source != 0) {
		g_source_remove(call->answer_source);
		call->answer_source = 0;
	}
	if (call->voice != NULL) {
		g_signal_handlers_disconnect_by_data(call->voice, call);
		ai_voice_session_stop(call->voice);
	}
	if (call->synthesizer != NULL)
		g_signal_handlers_disconnect_by_data(call->synthesizer, call);
	if (call->transport != NULL)
		g_signal_handlers_disconnect_by_data(call->transport, call);
	/* Membership removal runs independently of media teardown and uses a fresh
	 * cancellable: SIGTERM must not cancel its own cleanup request. */
	if (call->app->local) {
		/* No membership to clear, and the program is its one session. */
		call->clearing = FALSE;
		call->app->stopping = TRUE;
	} else
		put_member(call, body, cleared);
	if (call->transport != NULL)
		ai_audio_transport_leave_async(call->transport, NULL, left_room, call_ref(call));
	else
		call->left = TRUE;
}
static void
failed(Call *call, const GError *error)
{
	g_printerr("Call setup failed: %s\n",
			   error != NULL ? error->message : "malformed server response");
	close_call(call);
}
static void
voice_error(AiVoiceSession *voice, GError *error, gpointer data)
{
	Call *call = data;
	call->errors++;
	g_log("ai-call", G_LOG_LEVEL_INFO, "Voice error: room=%s error=%s", call->room,
		  error->message);
}
static void
terminal_media_error(AiAudioTransport *transport, GError *error, gpointer data)
{
	Call *call = data;
	if (call->closing)
		return;
	/* Transient recovery uses reconnecting/reconnected. The error signal
	 * means this transport cannot recover; release the room membership. */
	g_log("ai-call", G_LOG_LEVEL_INFO,
		  "Call media unavailable; ending call: room=%s error=%s", call->room,
		  error != NULL ? error->message : "unknown media error");
	close_call(call);
}
/* state-file: rewritten whole, atomically, on every change, so a reader never
 * sees half a document. A failure costs the file, never the call. */
static void
write_state(Call *call)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	gboolean muted = FALSE;
	if (call->app->state_file == NULL)
		return;
	/* One file for every call: "ended" only when the last one is. */
	if (call->closing) {
		GHashTableIter iter;
		gpointer other;
		g_hash_table_iter_init(&iter, call->app->calls);
		while (g_hash_table_iter_next(&iter, NULL, &other))
			if (other != call && !((Call *)other)->closing)
				return;
	}
	if (call->voice != NULL && !call->closing)
		g_object_get(call->voice, "muted", &muted, NULL);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder, call->closing	  ? "ended"
										   : call->state_name ? call->state_name
															  : "starting");
	json_builder_set_member_name(builder, "muted");
	json_builder_add_boolean_value(builder, muted);
	json_builder_set_member_name(builder, "heard");
	json_builder_add_string_value(builder, call->heard != NULL ? call->heard : "");
	json_builder_set_member_name(builder, "said");
	json_builder_add_string_value(builder, call->said != NULL ? call->said : "");
	json_builder_set_member_name(builder, "updated_us");
	json_builder_add_int_value(builder, g_get_real_time());
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json = json_to_string(root, FALSE);
	if (!g_file_set_contents_full(call->app->state_file, json, -1,
								  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
		g_debug("state file not written: %s", error->message);
}
static void
voice_state(AiVoiceSession *voice, AiVoiceState state, gpointer data)
{
	Call *call = data;
	GEnumClass *states = g_type_class_ref(AI_TYPE_VOICE_STATE);
	GEnumValue *value = g_enum_get_value(states, state);
	g_log("ai-call", G_LOG_LEVEL_INFO, "Voice state: room=%s state=%s", call->room,
		  value != NULL ? value->value_nick : "unknown");
	/* Nicks are static strings owned by the enum class, which outlives this. */
	call->state_name = value != NULL ? value->value_nick : NULL;
	g_type_class_unref(states);
	write_state(call);
}
static void
voice_muted(GObject *voice, GParamSpec *pspec, gpointer data)
{
	write_state(data);
}
/* SIGUSR1, or a button: flip whether every live session hears its caller. */
static gboolean
toggle_mute(gpointer data)
{
	App *app = data;
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, app->calls);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Call *call = value;
		gboolean muted;
		if (call->voice == NULL || call->closing)
			continue;
		g_object_get(call->voice, "muted", &muted, NULL);
		g_log("ai-call", G_LOG_LEVEL_INFO, "Mute toggled: room=%s muted=%d", call->room,
			  !muted);
		g_object_set(call->voice, "muted", !muted, NULL);
	}
	return G_SOURCE_CONTINUE;
}
static void
info_log(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer data)
{
	/* INFO lifecycle messages are visible without G_MESSAGES_DEBUG. */
	g_printerr("%s INFO: %s\n", domain, message);
}
/* live-text: what is said appears in the room as it is said. A reply is one
 * notice, edited as each sentence plays, rather than a message per sentence.
 * Sends for one reply are serialised so edits cannot land out of order, and a
 * reply owns its own state: an answer arriving after the next turn began
 * updates its own message, never the new one. */
struct _LiveReply {
	grefcount refs;
	App *app;
	gchar *room, *event_id;
	GString *text;
	gboolean sending, dirty;
};
static LiveReply *
live_reply_ref(LiveReply *live)
{
	g_ref_count_inc(&live->refs);
	return live;
}
static void
live_reply_unref(gpointer data)
{
	LiveReply *live = data;
	if (!g_ref_count_dec(&live->refs))
		return;
	g_free(live->room);
	g_free(live->event_id);
	g_string_free(live->text, TRUE);
	g_free(live);
}
static gchar *
live_url(App *app, const gchar *room)
{
	g_autofree gchar *escaped = g_uri_escape_string(room, NULL, FALSE);
	g_autofree gchar *txn = g_uuid_string_random();
	return g_strdup_printf("%s/_matrix/client/v3/rooms/%s/send/m.room.message/%s",
						   app->homeserver, escaped, txn);
}
static void
live_reply_send(LiveReply *live);
static void
live_reply_sent(App *app, JsonNode *root, const GError *error, gpointer data)
{
	LiveReply *live = data;
	live->sending = FALSE;
	if (error != NULL)
		/* Not the call's problem; the next sentence sends the whole reply again. */
		g_log("ai-call", G_LOG_LEVEL_INFO, "Live text not sent: %s", error->message);
	else if (live->event_id == NULL)
		live->event_id = g_strdup(ai_json_get_string(object(root), "event_id", NULL));
	if (live->dirty && !app->stopping)
		live_reply_send(live);
}
static void
live_reply_send(LiveReply *live)
{
	g_autoptr(JsonNode) body = empty_object();
	JsonObject *o = object(body);
	g_autofree gchar *url = live_url(live->app, live->room);
	json_object_set_string_member(o, "msgtype", "m.notice");
	if (live->event_id == NULL)
		json_object_set_string_member(o, "body", live->text->str);
	else {
		JsonObject *content = json_object_new(), *relation = json_object_new();
		g_autofree gchar *fallback = g_strconcat("* ", live->text->str, NULL);
		json_object_set_string_member(o, "body", fallback);
		json_object_set_string_member(content, "msgtype", "m.notice");
		json_object_set_string_member(content, "body", live->text->str);
		json_object_set_object_member(o, "m.new_content", content);
		json_object_set_string_member(relation, "rel_type", "m.replace");
		json_object_set_string_member(relation, "event_id", live->event_id);
		json_object_set_object_member(o, "m.relates_to", relation);
	}
	live->sending = TRUE;
	live->dirty = FALSE;
	request(live->app, "PUT", url, TRUE, body, NULL, live_reply_sent,
			live_reply_ref(live), live_reply_unref);
}
static void
live_notice_sent(App *app, JsonNode *root, const GError *error, gpointer data)
{
	if (error != NULL)
		g_log("ai-call", G_LOG_LEVEL_INFO, "Live text not sent: %s", error->message);
}
/* "@user:server:DEVICE" is a participant identity; the room knows who "user" is. */
static gchar *
speaker_label(const gchar *speaker)
{
	const gchar *colon = speaker[0] == '@' ? strchr(speaker, ':') : NULL;
	return colon != NULL ? g_strndup(speaker + 1, colon - speaker - 1) : g_strdup(speaker);
}
static void
live_caller(Call *call, const gchar *speaker, const gchar *text)
{
	g_autoptr(JsonNode) body = NULL;
	g_autofree gchar *label = NULL, *line = NULL, *url = NULL;
	/* Whatever the assistant says next is a new reply. */
	g_clear_pointer(&call->live, live_reply_unref);
	if (call->app->live_text < 2)
		return;
	body = empty_object();
	label = speaker_label(speaker);
	line = g_strdup_printf("%s: %s", label, text);
	url = live_url(call->app, call->room);
	json_object_set_string_member(object(body), "msgtype", "m.notice");
	json_object_set_string_member(object(body), "body", line);
	request(call->app, "PUT", url, TRUE, body, NULL, live_notice_sent, NULL, NULL);
}
static void
live_spoken(Call *call, const gchar *text)
{
	LiveReply *live = call->live;
	if (live == NULL) {
		live = call->live = g_new0(LiveReply, 1);
		g_ref_count_init(&live->refs);
		live->app = call->app;
		live->room = g_strdup(call->room);
		live->text = g_string_new(NULL);
	}
	if (live->text->len > 0)
		g_string_append_c(live->text, ' ');
	g_string_append(live->text, text);
	if (live->sending)
		live->dirty = TRUE;
	else
		live_reply_send(live);
}
static void
voice_transcript(AiVoiceSession *voice, const gchar *speaker, const gchar *text,
				 gboolean final, gpointer data)
{
	Call *call = data;
	if (final && call->app->live_text > 0)
		live_caller(call, speaker, text);
	if (final) {
		g_free(call->heard);
		call->heard = g_strdup(text);
		write_state(call);
		call->transcripts++;
		call->transcript_at = g_get_monotonic_time();
		g_print("Transcript [%s]: %s\n", speaker, text);
		if (call->transcript != NULL && !call->transcript_stopped) {
			g_autoptr(GDateTime) now = g_date_time_new_now_utc();
			g_autoptr(GError) error = NULL;
			if (!ai_call_transcript_append(call->transcript, now, speaker, text, &error)) {
				/* Report once and stop: a full disk would otherwise log per line.
				 * The file is still closed and handed to the hook at hangup. */
				g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript stopped: %s",
					  error->message);
				call->transcript_stopped = TRUE;
			}
		}
	}
}
static void
voice_spoken(AiVoiceSession *voice, const gchar *text, gboolean complete, gpointer data)
{
	Call *call = data;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	if (call->app->live_text > 0 && *text != '\0')
		live_spoken(call, text);
	g_free(call->said);
	call->said = g_strdup(text);
	write_state(call);
	if (call->transcript == NULL || call->transcript_stopped)
		return;
	now = g_date_time_new_now_utc();
	if (!ai_call_transcript_append_spoken(call->transcript, now, call->app->mxid, text,
										  complete, &error)) {
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript stopped: %s", error->message);
		call->transcript_stopped = TRUE;
	}
}
static void
voice_tool(AiVoiceSession *voice, const gchar *name, const gchar *arguments,
		   const gchar *result, gboolean is_error, gpointer data)
{
	Call *call = data;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	if (call->transcript == NULL || call->transcript_stopped)
		return;
	now = g_date_time_new_now_utc();
	if (!ai_call_transcript_append_tool(call->transcript, now, name, arguments, result,
										is_error, &error)) {
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript stopped: %s", error->message);
		call->transcript_stopped = TRUE;
	}
}
static void
voice_relevance(AiVoiceSession *voice, guint score, const gchar *reason, gboolean chimed,
				gpointer data)
{
	Call *call = data;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	if (call->transcript == NULL || call->transcript_stopped)
		return;
	now = g_date_time_new_now_utc();
	if (!ai_call_transcript_append_relevance(call->transcript, now, score, reason, chimed,
											 &error)) {
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript stopped: %s", error->message);
		call->transcript_stopped = TRUE;
	}
}
/* Room mode, and a relevance judge of its own when chiming in is on. The judge
 * is a separate client even for the same provider: the conversation's runs on
 * the session's worker thread. A judge that cannot be built costs chiming in,
 * never the call. */
static void
room_settings(Call *call)
{
	App *app = call->app;
	g_autoptr(GObject) judge = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(call->voice, "room-mode", app->room_mode, "chime-in-threshold",
				 app->chime_threshold, "chime-in-cooldown-ms", app->chime_cooldown_ms, NULL);
	if (app->chime_threshold == 0)
		return;
	judge = app->relevance_provider != NULL
				? ai_provider_factory_new_from_string(app->relevance_provider, app->config,
													  &error)
				: ai_provider_factory_new(app->provider, app->config, &error);
	if (judge == NULL) {
		g_log("ai-call", G_LOG_LEVEL_INFO, "Chiming in is off: %s", error->message);
		return;
	}
	if (app->relevance_model != NULL)
		g_object_set(judge, "model", app->relevance_model, NULL);
	else if (app->model != NULL && app->relevance_provider == NULL)
		g_object_set(judge, "model", app->model, NULL);
	g_object_set(call->voice, "relevance-provider", judge, NULL);
}
/* Speakers named by voice, when a voiceprint service is configured. */
static AiVoiceprintIdentifier *
voiceprints(App *app)
{
	AiVoiceprintIdentifier *identifier =
		ai_voiceprint_identifier_new(app->speaker_url, app->voiceprint_dir);
	g_object_set(identifier, "threshold", app->speaker_threshold / 100.0, "track-guests",
				 app->track_guests, "guest-threshold", app->guest_threshold / 100.0, NULL);
	return identifier;
}
static void
naming_settings(Call *call)
{
	g_autoptr(AiVoiceprintIdentifier) identifier = NULL;
	if (call->app->speaker_url == NULL)
		return;
	identifier = voiceprints(call->app);
	g_log("ai-call", G_LOG_LEVEL_INFO, "Naming speakers by voice: %u enrolled",
		  ai_voiceprint_identifier_get_n_voices(identifier));
	g_object_set(call->voice, "speaker-identifier", identifier, "unknown-speaker-name",
				 call->app->unknown_speaker_name, "naming-timeout-ms",
				 call->app->naming_timeout_ms, NULL);
}

/* ---- enrolling a voice ---- */

/* Little-endian fields read through memcpy: a chunk can start on any byte,
 * and a cast to a wider pointer there is undefined behaviour. */
static guint16
le16_at(const gchar *at)
{
	guint16 v;
	memcpy(&v, at, sizeof(v));
	return GUINT16_FROM_LE(v);
}
static guint32
le32_at(const gchar *at)
{
	guint32 v;
	memcpy(&v, at, sizeof(v));
	return GUINT32_FROM_LE(v);
}
/* The most audio an enrolment file may hold: ten minutes of 16 kHz 16-bit. */
#define RECORDING_MAX_BYTES ((gsize)600 * 16000 * 2 + 4096)
/* Raw 16 kHz mono S16LE, or a WAV file holding exactly that. */
static GBytes *
read_recording(const gchar *path, GError **error)
{
	g_autofree gchar *data = NULL;
	gsize size = 0, at = 12;
	gboolean format_ok = FALSE;
	GStatBuf st;
	if (g_stat(path, &st) == 0 && (gsize)st.st_size > RECORDING_MAX_BYTES) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"%s is longer than ten minutes of audio", path);
		return NULL;
	}
	if (!g_file_get_contents(path, &data, &size, error))
		return NULL;
	if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
		if (size % 2 != 0) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
						"%s is not 16-bit PCM", path);
			return NULL;
		}
		return g_bytes_new_take(g_steal_pointer(&data), size);
	}
	while (at + 8 <= size) {
		guint32 length = le32_at(data + at + 4);
		const gchar *chunk = data + at + 8;
		if (length > size - at - 8)
			break;
		if (memcmp(data + at, "fmt ", 4) == 0 && length >= 16)
			format_ok = le16_at(chunk) == 1 && le16_at(chunk + 2) == 1 &&
						le32_at(chunk + 4) == 16000 && le16_at(chunk + 14) == 16;
		else if (memcmp(data + at, "data", 4) == 0) {
			if (!format_ok)
				break;
			return g_bytes_new(chunk, length - length % 2);
		}
		at += 8 + length + (length & 1);
	}
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				"%s is not a 16 kHz mono 16-bit WAV file", path);
	return NULL;
}
typedef struct {
	GByteArray *audio;
	gboolean done;
	GError *error;
	guint timer; /* 0 once it has fired */
} Recording;
static void
recorded_audio(AiAudioTransport *transport, const gchar *speaker, GBytes *pcm, gpointer data)
{
	Recording *r = data;
	gsize size;
	const guint8 *raw = g_bytes_get_data(pcm, &size);
	g_byte_array_append(r->audio, raw, size);
}
static void
recording_step(GObject *source, GAsyncResult *result, gpointer data)
{
	Recording *r = data;
	if (!ai_audio_transport_join_finish(AI_AUDIO_TRANSPORT(source), result, &r->error))
		r->done = TRUE;
}
static gboolean
recording_over(gpointer data)
{
	((Recording *)data)->done = TRUE;
	((Recording *)data)->timer = 0;
	return G_SOURCE_REMOVE;
}
static void
recording_left(GObject *source, GAsyncResult *result, gpointer data)
{
	ai_audio_transport_leave_finish(AI_AUDIO_TRANSPORT(source), result, NULL);
	((Recording *)data)->done = TRUE;
}
/* SECONDS of this machine's microphone, through local-input. */
static GBytes *
record_microphone(App *app, guint seconds, GError **error)
{
	g_autofree gchar *input = NULL;
	g_autoptr(AiLocalAudioTransport) transport = NULL;
	Recording r = {0};
	GMainContext *context = g_main_context_get_thread_default();
	g_object_get(app->call_config, "local-input", &input, NULL);
	transport = g_object_new(AI_TYPE_LOCAL_AUDIO_TRANSPORT, "input", input, "output",
							 "fakesink", NULL);
	r.audio = g_byte_array_new();
	g_signal_connect(transport, "audio", G_CALLBACK(recorded_audio), &r);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(transport), "enroll", NULL, NULL,
								  recording_step, &r);
	/* The timer points at R on this stack: it must not outlive the call,
	 * whichever way the recording ends. */
	r.timer = g_timeout_add_seconds(seconds, recording_over, &r);
	while (!r.done)
		g_main_context_iteration(context, TRUE);
	g_clear_handle_id(&r.timer, g_source_remove);
	if (r.error == NULL) {
		r.done = FALSE;
		ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(transport), NULL, recording_left, &r);
		while (!r.done)
			g_main_context_iteration(context, TRUE);
	}
	g_signal_handlers_disconnect_by_data(transport, &r);
	if (r.error != NULL) {
		g_propagate_error(error, r.error);
		g_byte_array_unref(r.audio);
		return NULL;
	}
	return g_byte_array_free_to_bytes(r.audio);
}
typedef struct {
	gboolean done, ok;
	GError *error;
} Enrolling;
static void
enrolled_voice(GObject *source, GAsyncResult *result, gpointer data)
{
	Enrolling *e = data;
	e->ok = ai_voiceprint_identifier_enroll_finish(AI_VOICEPRINT_IDENTIFIER(source), result,
												   &e->error);
	e->done = TRUE;
}
/* NAME's voiceprint from the recording at PATH, or from SECONDS of the
 * microphone when PATH is NULL. */
static gboolean
enroll_voice(App *app, const gchar *name, const gchar *path, guint seconds, GError **error)
{
	g_autoptr(AiVoiceprintIdentifier) identifier = NULL;
	g_autoptr(GBytes) pcm = NULL;
	Enrolling e = {0};
	if (app->speaker_url == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"Configure ai_call.speaker-url to enroll voices");
		return FALSE;
	}
	if (path != NULL)
		pcm = read_recording(path, error);
	else {
		g_print("Recording %u seconds. Talk normally until it stops...\n", seconds);
		pcm = record_microphone(app, seconds, error);
	}
	if (pcm == NULL)
		return FALSE;
	identifier = voiceprints(app);
	ai_voiceprint_identifier_enroll_async(identifier, name, pcm, NULL, enrolled_voice, &e);
	while (!e.done)
		g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
	if (!e.ok) {
		g_propagate_error(error, e.error);
		return FALSE;
	}
	return TRUE;
}
static gboolean
hangup_idle(gpointer data)
{
	shutdown_call(data);
	return G_SOURCE_REMOVE;
}
/* The session has acted on stop, mute and unmute already; hang-up is ours. */
static void
voice_command(AiVoiceSession *voice, const gchar *name, gpointer data)
{
	Call *call = data;
	const gchar *note = g_str_equal(name, "mute")	  ? "(muted)"
						: g_str_equal(name, "unmute") ? "(listening again)"
						: g_str_equal(name, "stop")   ? "(stopped)"
						: g_str_equal(name, "hangup") ? "(hung up)"
													  : NULL;
	g_log("ai-call", G_LOG_LEVEL_INFO, "Voice command: room=%s command=%s", call->room, name);
	if (call->transcript != NULL && !call->transcript_stopped) {
		g_autoptr(GDateTime) now = g_date_time_new_now_utc();
		g_autoptr(GError) error = NULL;
		if (!ai_call_transcript_append_command(call->transcript, now, name, &error)) {
			g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript stopped: %s", error->message);
			call->transcript_stopped = TRUE;
		}
	}
	if (call->app->live_text > 0 && note != NULL) {
		g_autoptr(JsonNode) body = empty_object();
		g_autofree gchar *url = live_url(call->app, call->room);
		/* Whatever is said next is a new reply, not an edit of the stopped one. */
		g_clear_pointer(&call->live, live_reply_unref);
		json_object_set_string_member(object(body), "msgtype", "m.notice");
		json_object_set_string_member(object(body), "body", note);
		request(call->app, "PUT", url, TRUE, body, NULL, live_notice_sent, NULL, NULL);
	}
	/* Not from inside the session's own signal: shutdown stops the session. */
	if (g_str_equal(name, "hangup")) {
		GSource *idle = g_idle_source_new();
		g_source_set_callback(idle, hangup_idle, call_ref(call), call_unref);
		g_source_attach(idle, g_main_context_get_thread_default());
		g_source_unref(idle);
	}
}
/* One file per answered call; failure costs the transcript, never the call. */
static void
open_transcript(Call *call)
{
	g_autoptr(GError) error = NULL;
	if (call->app->transcript_dir == NULL || call->transcript != NULL)
		return;
	call->transcript = ai_call_transcript_open(call->app->transcript_dir, call->room,
											   call->began, &error);
	if (call->transcript == NULL)
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript unavailable: %s", error->message);
	else
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript: room=%s path=%s", call->room,
			  ai_call_transcript_get_path(call->transcript));
}
static void
finish_transcript(Call *call)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GError) error = NULL;
	const gchar *path;
	if (call->transcript == NULL)
		return;
	path = ai_call_transcript_get_path(call->transcript);
	if (!ai_call_transcript_close(call->transcript, now, &error)) {
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript not finalized: %s", error->message);
		g_clear_error(&error);
	}
	if (call->app->transcript_hook != NULL &&
		!ai_call_transcript_run_hook(call->app->transcript_hook, path, &error))
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript hook not started: %s",
			  error->message);
}
static void
first_pcm(AiSpeechSynthesizer *synth, GBytes *pcm, guint sample_rate, gpointer data)
{
	Call *call = data;
	gint64 now = g_get_monotonic_time();
	if (call->transcript_at != 0) {
		g_log("ai-call", G_LOG_LEVEL_INFO,
			  "voice timing: final-transcript-to-first-PCM=%" G_GINT64_FORMAT "ms",
			  (now - call->transcript_at) / 1000);
		call->transcript_at = 0;
	} else if (call->answered_at != 0) {
		g_log("ai-call", G_LOG_LEVEL_INFO,
			  "voice timing: membership-to-first-PCM=%" G_GINT64_FORMAT "ms",
			  (now - call->answered_at) / 1000);
		call->answered_at = 0;
	}
}
static void
greet(Call *call)
{
	if (call->greeted || call->voice == NULL || call->closing)
		return;
	call->greeted = TRUE;
	g_log("ai-call", G_LOG_LEVEL_INFO, "Call greeting queued: room=%s", call->room);
	ai_voice_session_say(call->voice, call->opening != NULL ? call->opening
									  : call->outbound		? call->app->outbound_greeting
															: call->app->greeting);
}
static gboolean
humans(JsonNode *root, App *app)
{
	JsonArray *array;
	guint i;
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root))
		return FALSE;
	array = json_node_get_array(root);
	for (i = 0; i < json_array_get_length(array); i++) {
		JsonObject *event = ai_json_array_get_object(array, i);
		JsonObject *content = ai_json_get_object(event, "content");
		if (g_strcmp0(ai_json_get_string(event, "type", NULL), MEMBER) == 0 &&
			g_strcmp0(ai_json_get_string(event, "sender", NULL), app->mxid) != 0 &&
			content != NULL && json_object_get_size(content) != 0)
			return TRUE;
	}
	return FALSE;
}
static gboolean
check_answer(gpointer data);
static void
checked_answer(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	if (call->closing)
		return;
	if (error == NULL && humans(root, app)) {
		greet(call);
		return;
	}
	if (g_get_monotonic_time() >= call->answer_deadline) {
		close_call(call);
		return;
	}
	call->answer_source = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 1, check_answer,
													 call_ref(call), call_unref);
}
static gboolean
check_answer(gpointer data)
{
	Call *call = data;
	g_autofree gchar *url = room_url(call, "state");
	call->answer_source = 0;
	if (!call->closing)
		request(call->app, "GET", url, TRUE, NULL, NULL, checked_answer, call_ref(call),
				call_unref);
	return G_SOURCE_REMOVE;
}
static void
joined_room(GObject *source, GAsyncResult *result, gpointer data)
{
	Call *call = data;
	g_autoptr(GError) error = NULL;
	if (!ai_audio_transport_join_finish(AI_AUDIO_TRANSPORT(source), result, &error))
		failed(call, error);
	else if (!call->closing) {
		call->media_joined = TRUE;
		g_log("ai-call", G_LOG_LEVEL_INFO, "Call media joined: room=%s", call->room);
		if (call->outbound) {
			call->answer_deadline = g_get_monotonic_time() + 30000000;
			check_answer(call);
		} else
			greet(call);
	}
	call_unref(call);
}
static void
maybe_connect(Call *call)
{
	App *app = call->app;
	g_autoptr(GObject) provider = NULL;
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(AiWebsocketRecognizer) stt = NULL;
	g_autoptr(AiHttpSynthesizer) tts = NULL;
	g_autoptr(AiWebrtcVoiceActivity) vad = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *prompt = NULL;
	if (call->closing || call->transport != NULL || call->tx_token == NULL ||
		call->rx_token == NULL || !call->history_ready)
		return;
	provider = ai_provider_factory_new(app->provider, app->config, &error);
	if (provider == NULL) {
		failed(call, error);
		return;
	}
	if (app->model != NULL)
		g_object_set(provider, "model", app->model, NULL);
	conversation = ai_conversation_new(provider);
	prompt = g_strdup_printf(
		"%s\n\nYou are on a live voice call. Speak concise, natural sentences.\nUse "
		"tools when needed. Never claim a failed tool succeeded.\n%s",
		app->identity, call->context->str);
	ai_conversation_set_system_prompt(conversation, prompt);
	if (!AI_IS_CLI_CLIENT(provider))
		ai_conversation_set_local_tools(conversation, TRUE);
	{
		guint agents = 4;
		if (app->call_config != NULL)
			g_object_get(app->call_config, "max-agents", &agents, NULL);
		ai_conversation_enable_background_agents(conversation, agents);
	}
	{
		guint attempts = 3, delay = 500, bitrate = 64000;
		g_autofree gchar *noise = NULL;
		if (app->call_config != NULL)
			g_object_get(app->call_config, "media-reconnect-attempts", &attempts,
						 "media-reconnect-delay-ms", &delay, "opus-bitrate", &bitrate,
						 "noise-suppression", &noise, NULL);
		if (app->local) {
			g_autofree gchar *input = NULL, *output = NULL, *name = NULL;
			if (app->call_config != NULL)
				g_object_get(app->call_config, "local-input", &input, "local-output", &output,
							 "local-speaker-name", &name, NULL);
			call->transport = g_object_new(
				AI_TYPE_LOCAL_AUDIO_TRANSPORT, "input", input, "output", output,
				"speaker-name", name, "echo-cancel", app->local_echo_cancel,
				"noise-suppression", noise, NULL);
		} else
			call->transport =
				g_object_new(AI_TYPE_LIVEKIT_TRANSPORT, "url", app->livekit_url,
							 "receive-token", call->rx_token, "reconnect-attempts", attempts,
							 "reconnect-delay-ms", delay, "opus-bitrate", bitrate,
							 "noise-suppression", noise, NULL);
	}
	stt = ai_websocket_recognizer_new(app->stt_url);
	tts = ai_http_synthesizer_new(app->tts_url);
	vad = ai_webrtc_voice_activity_new();
	call->voice = ai_voice_session_new(call->transport, AI_SPEECH_RECOGNIZER(stt),
									   AI_SPEECH_SYNTHESIZER(tts), AI_VOICE_ACTIVITY(vad),
									   conversation);
	if (app->call_config != NULL) {
		guint timeout, tts_timeout, silence, mode, deadline, barge_in;
		g_autofree gchar *fallback = NULL, *transcription_error = NULL,
						 *synthesis_error = NULL, *empty_reply = NULL, *progress = NULL;
		guint progress_delay, trim_after, stt_frame, repeat_limit;
		g_autofree gchar *repeat_message = NULL, *names = NULL, *mute_message = NULL,
						 *unmute_message = NULL, *soundalikes = NULL;
		g_object_get(app->call_config, "stt-timeout-ms", &timeout, "stt-frame-ms", &stt_frame,
					 "tts-timeout-ms",
					 &tts_timeout, "trailing-silence-ms", &silence, "vad-mode", &mode,
					 "turn-deadline-ms", &deadline, "barge-in-ms", &barge_in,
					 "deadline-message", &fallback, "transcription-error-message",
					 &transcription_error, "synthesis-error-message", &synthesis_error,
					 "empty-reply-message", &empty_reply, "tool-progress-message", &progress,
					 "tool-progress-delay-ms", &progress_delay, "trim-tool-results-after",
					 &trim_after, "repeat-limit", &repeat_limit, "repeat-message",
					 &repeat_message, "assistant-names", &names, "mute-message",
					 &mute_message, "unmute-message", &unmute_message, "assistant-soundalikes",
					 &soundalikes, NULL);
		g_object_set(stt, "timeout-ms", timeout, "frame-ms", stt_frame, NULL);
		g_object_set(tts, "timeout-ms", tts_timeout, NULL);
		g_object_set(vad, "trailing-silence-ms", silence, "mode", mode, NULL);
		g_object_set(call->voice, "turn-deadline-ms", deadline, "deadline-message",
					 fallback, "transcription-error-message", transcription_error,
					 "barge-in-ms", barge_in, "synthesis-error-message", synthesis_error,
					 "empty-reply-message", empty_reply, "tool-progress-message", progress,
					 "tool-progress-delay-ms", progress_delay, "trim-tool-results-after",
					 trim_after, "repeat-limit", repeat_limit, "repeat-message",
					 repeat_message, "speak-code", app->speak_code, "voice-commands",
					 app->voice_commands, "assistant-names", names, "mute-message",
					 mute_message, "unmute-message", unmute_message, "assistant-soundalikes",
					 soundalikes, NULL);
	}
	if (app->speech_cache != NULL) {
		fallback_ready(app->speech_cache, call->voice);
		g_signal_connect_object(app->speech_cache, "ready", G_CALLBACK(fallback_ready),
								call->voice, 0);
	}
	g_signal_connect(call->voice, "error", G_CALLBACK(voice_error), call);
	g_signal_connect(call->transport, "error", G_CALLBACK(terminal_media_error), call);
	g_signal_connect(call->voice, "state-changed", G_CALLBACK(voice_state), call);
	g_signal_connect(call->voice, "transcript", G_CALLBACK(voice_transcript), call);
	g_signal_connect(call->voice, "spoken", G_CALLBACK(voice_spoken), call);
	g_signal_connect(call->voice, "tool", G_CALLBACK(voice_tool), call);
	g_signal_connect(call->voice, "command", G_CALLBACK(voice_command), call);
	g_signal_connect(call->voice, "notify::muted", G_CALLBACK(voice_muted), call);
	g_signal_connect(call->voice, "relevance", G_CALLBACK(voice_relevance), call);
	room_settings(call);
	naming_settings(call);
	open_transcript(call);
	call->synthesizer = AI_SPEECH_SYNTHESIZER(g_object_ref(tts));
	g_signal_connect(tts, "audio", G_CALLBACK(first_pcm), call);
	ai_audio_transport_join_async(call->transport, call->room, call->tx_token, NULL,
								  joined_room, call_ref(call));
}
static void
minted_rx(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	if (call->closing)
		return;
	call->rx_token = g_strdup(ai_json_get_string(object(root), "jwt", NULL));
	if (error != NULL || call->rx_token == NULL) {
		failed(call, error);
		return;
	}
	maybe_connect(call);
}
static void
minted_tx(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	if (call->closing)
		return;
	call->tx_token = g_strdup(ai_json_get_string(object(root), "jwt", NULL));
	if (error != NULL || call->tx_token == NULL) {
		failed(call, error);
		return;
	}
	maybe_connect(call);
}
static void
openid_ready(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	g_autoptr(JsonNode) tx = empty_object(), rx = empty_object();
	g_autofree gchar *receiver = g_strconcat(app->device, "-RX", NULL);
	JsonObject *a = object(tx), *b = object(rx);
	if (call->closing)
		return;
	if (error != NULL || object(root) == NULL) {
		failed(call, error);
		return;
	}
	json_object_set_string_member(a, "room", call->room);
	json_object_set_string_member(a, "device_id", app->device);
	json_object_set_member(a, "openid_token", json_node_copy(root));
	json_object_set_string_member(b, "room", call->room);
	json_object_set_string_member(b, "device_id", receiver);
	json_object_set_member(b, "openid_token", json_node_copy(root));
	request(app, "POST", app->jwt_url, FALSE, tx, NULL, minted_tx, call_ref(call),
			call_unref);
	request(app, "POST", app->jwt_url, FALSE, rx, NULL, minted_rx, call_ref(call),
			call_unref);
}
static void
ignored(App *app, JsonNode *root, const GError *error, gpointer data)
{
	if (error != NULL)
		g_printerr("Matrix notification failed: %s\n", error->message);
}
static void
member_posted(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	g_autofree gchar *user = NULL, *url = NULL;
	g_autoptr(JsonNode) body = empty_object();
	if (call->closing) {
		/* A shutdown can race the original PUT. Clear again after its reply --
		 * unless a new call has the room now, whose membership that would be. */
		Call *current = g_hash_table_lookup(app->calls, call->room);
		if (current != NULL && current != call) {
			call->clearing = FALSE;
			removed_if_done(call);
			return;
		}
		put_member(call, body, cleared);
		return;
	}
	if (error != NULL) {
		failed(call, error);
		return;
	}
	call->answered_at = g_get_monotonic_time();
	if (call->outbound) {
		const gchar *event_id = ai_json_get_string(object(root), "event_id", NULL);
		if (event_id != NULL) {
			g_autoptr(JsonNode) ring = empty_object();
			JsonObject *o = object(ring), *mentions = json_object_new(),
					   *relation = json_object_new();
			JsonArray *users = json_array_new();
			g_autofree gchar *txn = g_uuid_string_random(),
							 *suffix = g_strdup_printf("send/" RING "/%s", txn);
			g_autofree gchar *ring_url = room_url(call, suffix);
			if (call->target != NULL)
				json_array_add_string_element(users, call->target);
			json_object_set_array_member(mentions, "user_ids", users);
			json_object_set_boolean_member(mentions, "room", TRUE);
			json_object_set_object_member(o, "m.mentions", mentions);
			json_object_set_string_member(o, "notification_type", "ring");
			json_object_set_string_member(relation, "event_id", event_id);
			json_object_set_string_member(relation, "rel_type", "m.reference");
			json_object_set_object_member(o, "m.relates_to", relation);
			json_object_set_int_member(o, "sender_ts", g_get_real_time() / 1000);
			json_object_set_int_member(o, "lifetime", 30000);
			json_object_set_string_member(o, "m.call.intent", "audio");
			request(app, "PUT", ring_url, TRUE, ring, NULL, ignored, NULL, NULL);
		}
	}
	user = g_uri_escape_string(app->mxid, NULL, FALSE);
	url = g_strdup_printf("%s/_matrix/client/v3/user/%s/openid/request_token",
						  app->homeserver, user);
	request(app, "POST", url, TRUE, body, NULL, openid_ready, call_ref(call), call_unref);
}
static void
history_ready(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	JsonArray *events = ai_json_get_array(object(root), "chunk");
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);
	guint i, total = 0;
	if (call->closing)
		return;
	if (error == NULL && events != NULL) {
		for (i = 0; i < json_array_get_length(events); i++) {
			JsonObject *event = ai_json_array_get_object(events, i);
			JsonObject *content = ai_json_get_object(event, "content");
			const gchar *body = ai_json_get_string(content, "body", "");
			const gchar *sender = ai_json_get_string(event, "sender", "Someone");
			const gchar *msgtype = ai_json_get_string(content, "msgtype", "m.text");
			g_autofree gchar *clean = NULL, *line = NULL;
			gchar *p;
			if (*body == '\0' || g_str_has_prefix(body, "⏳") ||
				(g_strcmp0(msgtype, "m.text") && g_strcmp0(msgtype, "m.notice") &&
				 g_strcmp0(msgtype, "m.emote")))
				continue;
			clean = g_utf8_substring(body, 0, MIN((glong)240, g_utf8_strlen(body, -1)));
			for (p = clean; *p; p++)
				if (g_ascii_isspace(*p))
					*p = ' ';
			line = g_strdup_printf(
				"%s: %s", g_str_equal(sender, app->mxid) ? "Assistant" : sender, clean);
			if (total + strlen(line) > 2500)
				break;
			total += strlen(line);
			g_ptr_array_add(lines, g_steal_pointer(&line));
		}
		for (i = lines->len; i > 0; i--)
			g_string_append_printf(call->context, "%s\n",
								   (gchar *)g_ptr_array_index(lines, i - 1));
	}
	call->history_ready = TRUE;
	maybe_connect(call);
}
static Call *
start_call(App *app, const gchar *room, const gchar *opening, const gchar *target,
		   gboolean outbound)
{
	Call *call;
	g_autoptr(JsonNode) membership = empty_object();
	JsonObject *o = object(membership), *focus = json_object_new(),
			   *preferred = json_object_new();
	JsonArray *foci = json_array_new();
	g_autofree gchar *membership_id = g_strconcat(app->mxid, ":", app->device, NULL);
	g_autofree gchar *url = NULL;
	if (app->stopping || g_hash_table_contains(app->calls, room)) {
		json_object_unref(focus);
		json_object_unref(preferred);
		json_array_unref(foci);
		return NULL;
	}
	call = g_new0(Call, 1);
	g_ref_count_init(&call->refs);
	call->app = app;
	call->started_at = g_get_monotonic_time();
	call->began = g_date_time_new_now_utc();
	call->room = g_strdup(room);
	call->key = g_strdup_printf("_%s_%s_m.call", app->mxid, app->device);
	call->opening = g_strdup(opening);
	call->target = g_strdup(target);
	call->outbound = outbound;
	call->context = g_string_new("[CALL CONTEXT]\n");
	g_string_append_printf(call->context,
						   "Direction: %s\nRoom: %s\nReason: %s\nRecent chat:\n",
						   outbound ? "outbound" : "inbound", room,
						   opening != NULL ? opening : "incoming call");
	g_hash_table_insert(app->calls, g_strdup(room), call);
	json_object_set_string_member(o, "application", "m.call");
	json_object_set_string_member(o, "call_id", "");
	json_object_set_string_member(o, "scope", "m.room");
	json_object_set_string_member(o, "device_id", app->device);
	json_object_set_string_member(o, "membershipID", membership_id);
	json_object_set_int_member(o, "expires", 14400000);
	json_object_set_string_member(o, "m.call.intent", "audio");
	json_object_set_string_member(focus, "type", "livekit");
	json_object_set_string_member(focus, "focus_selection", "oldest_membership");
	json_object_set_object_member(o, "focus_active", focus);
	json_object_set_string_member(preferred, "livekit_alias", room);
	json_object_set_string_member(preferred, "type", "livekit");
	json_object_set_string_member(preferred, "livekit_service_url", app->foci);
	json_array_add_object_element(foci, preferred);
	json_object_set_array_member(o, "foci_preferred", foci);
	put_member(call, membership, member_posted);
	url = room_url(
		call,
		"messages?dir=b&limit=20&filter=%7B%22types%22%3A%5B%22m.room.message%22%5D%7D");
	request(app, "GET", url, TRUE, NULL, NULL, history_ready, call_ref(call), call_unref);
	return call;
}
/* --local: the session is ready to open its devices at once; there is no
 * room to join, token to mint or chat history to read. */
static Call *
start_local(App *app)
{
	Call *call = g_new0(Call, 1);
	g_ref_count_init(&call->refs);
	call->app = app;
	call->started_at = g_get_monotonic_time();
	call->began = g_date_time_new_now_utc();
	call->room = g_strdup("local");
	call->key = g_strdup("local");
	call->tx_token = g_strdup("local");
	call->rx_token = g_strdup("local");
	call->history_ready = TRUE;
	call->context = g_string_new("[CALL CONTEXT]\nDirection: local device\n");
	g_hash_table_insert(app->calls, g_strdup(call->room), call);
	maybe_connect(call);
	return call;
}
static void
checked_hangup(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Call *call = data;
	if (!call->closing && error == NULL && !humans(root, app))
		close_call(call);
}
static void
process_sync(App *app, JsonNode *root)
{
	JsonObject *rooms =
		ai_json_get_object(ai_json_get_object(object(root), "rooms"), "join");
	GList *keys, *l;
	if (rooms == NULL)
		return;
	keys = json_object_get_members(rooms);
	for (l = keys; l != NULL; l = l->next) {
		const gchar *room = l->data;
		JsonObject *rd = ai_json_get_object(rooms, room);
		JsonArray *events =
			ai_json_get_array(ai_json_get_object(rd, "timeline"), "events");
		guint i;
		if (events == NULL)
			continue;
		for (i = 0; i < json_array_get_length(events); i++) {
			JsonObject *event = ai_json_array_get_object(events, i);
			JsonObject *content = ai_json_get_object(event, "content");
			Call *call;
			if (g_strcmp0(ai_json_get_string(event, "type", NULL), MEMBER) != 0 ||
				g_strcmp0(ai_json_get_string(event, "sender", NULL), app->mxid) == 0)
				continue;
			call = g_hash_table_lookup(app->calls, room);
			if (content != NULL && json_object_get_size(content) != 0) {
				if (call == NULL)
					start_call(app, room, NULL, NULL, FALSE);
				else if (call->outbound && call->voice != NULL)
					greet(call);
			} else if (call != NULL && !call->closing) {
				g_autofree gchar *url = room_url(call, "state");
				request(app, "GET", url, TRUE, NULL, NULL, checked_hangup, call_ref(call),
						call_unref);
			}
		}
	}
	g_list_free(keys);
}
static gboolean
retry_sync(gpointer data)
{
	sync_next(data);
	return G_SOURCE_REMOVE;
}
static void
synced(App *app, JsonNode *root, const GError *error, gpointer data)
{
	const gchar *since;
	if (app->stopping)
		return;
	if (error != NULL ||
		(since = ai_json_get_string(object(root), "next_batch", NULL)) == NULL) {
		g_printerr("Matrix sync failed; retrying\n");
		g_timeout_add_seconds(2, retry_sync, app);
		return;
	}
	g_free(app->since);
	app->since = g_strdup(since);
	process_sync(app, root);
	sync_next(app);
}
static void
sync_next(App *app)
{
	g_autofree gchar *since = NULL, *url = NULL;
	if (app->stopping || !app->primed || app->startup != 0)
		return;
	since = g_uri_escape_string(app->since, NULL, FALSE);
	url = g_strdup_printf("%s/_matrix/client/v3/sync?timeout=30000&since=%s",
						  app->homeserver, since);
	request(app, "GET", url, TRUE, NULL, app->sync_cancel, synced, NULL, NULL);
}
typedef struct {
	App *app;
	gchar *url;
	gboolean state;
} Sweep;
static void
sweep_free(gpointer data)
{
	Sweep *s = data;
	g_free(s->url);
	g_free(s);
}
static gboolean
sweep_retry(gpointer data);
static void
swept(App *app, JsonNode *root, const GError *error, gpointer data)
{
	Sweep *s = data;
	if (error != NULL) {
		Sweep *retry = g_new0(Sweep, 1);
		retry->app = app;
		retry->url = g_strdup(s->url);
		retry->state = s->state;
		g_printerr("Startup membership sweep failed; retrying\n");
		g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 2, sweep_retry, retry, sweep_free);
		return;
	}
	if (s->state && root != NULL && JSON_NODE_HOLDS_ARRAY(root)) {
		JsonArray *events = json_node_get_array(root);
		guint i;
		for (i = 0; i < json_array_get_length(events); i++) {
			JsonObject *event = ai_json_array_get_object(events, i);
			JsonObject *content = ai_json_get_object(event, "content");
			const gchar *key = ai_json_get_string(event, "state_key", NULL);
			if (key != NULL &&
				g_strcmp0(ai_json_get_string(event, "type", NULL), MEMBER) == 0 &&
				g_strcmp0(ai_json_get_string(event, "sender", NULL), app->mxid) == 0 &&
				content != NULL && json_object_get_size(content) != 0) {
				g_autofree gchar *encoded = g_uri_escape_string(key, NULL, FALSE);
				Sweep *clear = g_new0(Sweep, 1);
				clear->app = app;
				clear->url = g_strdup_printf("%s/" MEMBER "/%s", s->url, encoded);
				app->startup++;
				sweep_retry(clear);
				sweep_free(clear);
			}
		}
	}
	app->startup--;
	if (app->startup == 0) {
		app->primed = TRUE;
		sync_next(app);
	}
}
static gboolean
sweep_retry(gpointer data)
{
	Sweep *s = data, *copy = g_new0(Sweep, 1);
	g_autoptr(JsonNode) body = empty_object();
	copy->app = s->app;
	copy->url = g_strdup(s->url);
	copy->state = s->state;
	request(s->app, s->state ? "GET" : "PUT", s->url, TRUE, s->state ? NULL : body, NULL,
			swept, copy, sweep_free);
	return G_SOURCE_REMOVE;
}
static void
primed(App *app, JsonNode *root, const GError *error, gpointer data)
{
	JsonObject *rooms;
	const gchar *since = ai_json_get_string(object(root), "next_batch", NULL);
	GList *keys, *l;
	if (error != NULL || since == NULL) {
		g_printerr("Initial Matrix sync failed\n");
		app->exit_status = 1;
		app->stopping = TRUE;
		if (app->speech_cache != NULL)
			ai_call_speech_cache_cancel(app->speech_cache);
		return;
	}
	app->since = g_strdup(since);
	rooms = ai_json_get_object(ai_json_get_object(object(root), "rooms"), "join");
	keys = rooms != NULL ? json_object_get_members(rooms) : NULL;
	app->startup = 1;
	for (l = keys; l != NULL; l = l->next) {
		g_autofree gchar *room = g_uri_escape_string(l->data, NULL, FALSE);
		Sweep *s = g_new0(Sweep, 1);
		s->app = app;
		s->state = TRUE;
		s->url =
			g_strdup_printf("%s/_matrix/client/v3/rooms/%s/state", app->homeserver, room);
		app->startup++;
		sweep_retry(s);
		sweep_free(s);
	}
	g_list_free(keys);
	app->startup--;
	if (app->startup == 0) {
		app->primed = TRUE;
		sync_next(app);
	}
}
/* Shutdown speech has its own deadline and cancellable: it must never delay
 * membership cleanup while a speech service or media connection is stalled. */
static void
goodbye_write_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Call *call = data;
	g_autoptr(GError) error = NULL;
	ai_audio_transport_write_pcm_finish(AI_AUDIO_TRANSPORT(source), result, &error);
	call->goodbye_writes--;
	if (!call->closing &&
		(error != NULL || (call->goodbye_done && call->goodbye_writes == 0)))
		close_call(call);
	call_unref(call);
}
static void
goodbye_audio(AiSpeechSynthesizer *synth, GBytes *pcm, guint rate, gpointer data)
{
	Call *call = data;
	if (call->closing || !call->goodbye_pending)
		return;
	call->goodbye_writes++;
	ai_audio_transport_write_pcm_async(call->transport, pcm, rate, call->goodbye_cancel,
									   goodbye_write_done, call_ref(call));
}
static gboolean
goodbye_retry(gpointer data);
static void
goodbye_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Call *call = data;
	g_autoptr(GError) error = NULL;
	ai_speech_synthesizer_synthesize_finish(AI_SPEECH_SYNTHESIZER(source), result,
											&error);
	if (!call->closing && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING)) {
		/* Cancellation is asynchronous. Wait for the previous synthesis to
		 * release the service without extending the original shutdown budget. */
		call->goodbye_retry = g_timeout_source_new(25);
		g_source_set_callback(call->goodbye_retry, goodbye_retry, call_ref(call),
							  call_unref);
		g_source_attach(call->goodbye_retry, g_main_context_get_thread_default());
		call_unref(call);
		return;
	}
	call->goodbye_done = TRUE;
	if (!call->closing) {
		if (error != NULL)
			g_log("ai-call", G_LOG_LEVEL_INFO, "Shutdown speech failed: %s",
				  error->message);
		if (error != NULL || call->goodbye_writes == 0)
			close_call(call);
	}
	call_unref(call);
}
static gboolean
goodbye_retry(gpointer data)
{
	Call *call = data;
	g_clear_pointer(&call->goodbye_retry, g_source_unref);
	if (!call->closing)
		ai_speech_synthesizer_synthesize_async(call->synthesizer, call->goodbye_text,
											   call->goodbye_cancel, goodbye_done,
											   call_ref(call));
	return G_SOURCE_REMOVE;
}
static gboolean
goodbye_timeout(gpointer data)
{
	Call *call = data;
	g_clear_pointer(&call->goodbye_source, g_source_unref);
	g_log("ai-call", G_LOG_LEVEL_INFO, "Shutdown speech deadline: room=%s", call->room);
	close_call(call);
	return G_SOURCE_REMOVE;
}
static void
shutdown_call(Call *call)
{
	g_autofree gchar *goodbye = NULL;
	if (call->closing || call->goodbye_pending)
		return;
	if (call->app->call_config != NULL)
		g_object_get(call->app->call_config, "goodbye-message", &goodbye, NULL);
	else
		goodbye = g_strdup("Goodbye.");
	if (!call->media_joined || call->synthesizer == NULL || goodbye == NULL ||
		*goodbye == '\0') {
		close_call(call);
		return;
	}
	call->goodbye_pending = TRUE;
	call->goodbye_text = g_steal_pointer(&goodbye);
	if (call->voice != NULL) {
		g_signal_handlers_disconnect_by_data(call->voice, call);
		ai_voice_session_stop(call->voice);
	}
	g_signal_handlers_disconnect_by_data(call->synthesizer, call);
	call->goodbye_cancel = g_cancellable_new();
	call->goodbye_source = g_timeout_source_new(2000);
	g_source_set_callback(call->goodbye_source, goodbye_timeout, call_ref(call),
						  call_unref);
	g_source_attach(call->goodbye_source, g_main_context_get_thread_default());
	g_signal_connect(call->synthesizer, "audio", G_CALLBACK(goodbye_audio), call);
	g_log("ai-call", G_LOG_LEVEL_INFO, "Shutdown speech queued: room=%s budget-ms=2000",
		  call->room);
	ai_speech_synthesizer_synthesize_async(call->synthesizer, call->goodbye_text,
										   call->goodbye_cancel, goodbye_done,
										   call_ref(call));
}
static gboolean
shutdown_app(gpointer data)
{
	App *app = data;
	GHashTableIter iter;
	gpointer value;
	if (app->stop_signals++ > 0) {
		/* Asked twice: stop waiting on the homeserver and go. */
		g_printerr("Second stop signal; exiting without waiting for cleanup\n");
		g_main_loop_quit(app->loop);
		return G_SOURCE_CONTINUE;
	}
	if (app->stopping)
		return G_SOURCE_CONTINUE;
	app->stopping = TRUE;
	if (app->speech_cache != NULL)
		ai_call_speech_cache_cancel(app->speech_cache);
	g_cancellable_cancel(app->sync_cancel);
	g_hash_table_iter_init(&iter, app->calls);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		shutdown_call(value);
	maybe_exit(app);
	return G_SOURCE_CONTINUE;
}
typedef struct {
	App *app;
	gchar *contents;
} Drop;
static void
drop_deleted(GObject *source, GAsyncResult *result, gpointer data)
{
	Drop *drop = data;
	App *app = drop->app;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	if (g_file_delete_finish(G_FILE(source), result, &error) && !app->stopping &&
		drop->contents != NULL && json_parser_load_from_data(parser, drop->contents, -1, NULL)) {
		JsonObject *o = ai_json_root_object(parser);
		const gchar *room = ai_json_get_string(o, "room_id", NULL);
		if (room != NULL && *room == '!' && strchr(room + 1, ':') != NULL)
			start_call(app, room, ai_json_get_string(o, "message", NULL),
					   ai_json_get_string(o, "target", NULL), TRUE);
	}
	app->drop_busy = FALSE;
	app->pending--;
	g_free(drop->contents);
	g_free(drop);
	maybe_exit(app);
}
static void
drop_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
	App *app = data;
	g_autofree gchar *contents = NULL;
	gsize length = 0;
	if (g_file_load_contents_finish(G_FILE(source), result, &contents, &length, NULL,
									NULL)) {
		Drop *drop = g_new0(Drop, 1);
		/* Consumed either way: left in place, it would be reread every second. */
		if (length > 65536) {
			g_message("Outbound drop file is over 64 KiB; removed unread");
			g_clear_pointer(&contents, g_free);
		}
		drop->app = app;
		drop->contents = g_steal_pointer(&contents);
		g_file_delete_async(G_FILE(source), G_PRIORITY_DEFAULT, NULL, drop_deleted, drop);
	} else {
		app->pending--;
		app->drop_busy = FALSE;
		maybe_exit(app);
	}
}
static gboolean
watch_drop(gpointer data)
{
	App *app = data;
	g_autoptr(GFile) file = NULL;
	if (app->stopping)
		return G_SOURCE_REMOVE;
	if (!app->primed || app->drop_busy)
		return G_SOURCE_CONTINUE;
	file = g_file_new_for_path(app->drop_path);
	app->drop_busy = TRUE;
	app->pending++;
	g_file_load_contents_async(file, NULL, drop_loaded, app);
	return G_SOURCE_CONTINUE;
}

int
main(int argc, char **argv)
{
	App app = {0};
	g_autoptr(GOptionContext) options =
		g_option_context_new("— answer MatrixRTC voice calls");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *credentials = NULL, *config_path = NULL, *identity_file = NULL;
	g_autofree gchar *provider_name = NULL, *model = NULL, *url = NULL;
	gboolean version = FALSE, license = FALSE, list_settings = FALSE;
	g_autofree gchar *enroll_name = NULL, *enroll_file = NULL;
	gint enroll_seconds = 20;
	g_auto(GStrv) call_overrides = NULL;
	g_autoptr(AiCallConfig) call_config = ai_call_config_new();
	guint setting_index;
	JsonObject *creds;
	GOptionEntry entries[] = {
		{"config", 0, 0, G_OPTION_ARG_FILENAME, &config_path,
		 "ai-glib YAML configuration", "PATH"},
		{"identity", 0, 0, G_OPTION_ARG_FILENAME, &identity_file,
		 "Identity/system-prompt file", "PATH"},
		{"credentials", 0, 0, G_OPTION_ARG_FILENAME, &credentials,
		 "Matrix credentials JSON file", "PATH"},
		{"provider", 'p', 0, G_OPTION_ARG_STRING, &provider_name, "Conversation provider",
		 "NAME"},
		{"model", 'm', 0, G_OPTION_ARG_STRING, &model, "Conversation model", "NAME"},
		{"call-set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &call_overrides,
		 "Override a ai_call GObject property", "PROPERTY=VALUE"},
		{"list-call-settings", 0, 0, G_OPTION_ARG_NONE, &list_settings,
		 "List call property names, types and defaults", NULL},
		{"local", 0, 0, G_OPTION_ARG_NONE, &app.local,
		 "Talk through this machine's microphone and speaker instead of answering calls",
		 NULL},
		{"enroll", 0, 0, G_OPTION_ARG_STRING, &enroll_name,
		 "Save NAME's voiceprint from the microphone (or --enroll-file), then exit",
		 "NAME"},
		{"enroll-file", 0, 0, G_OPTION_ARG_FILENAME, &enroll_file,
		 "Enroll from a 16 kHz mono 16-bit WAV or raw PCM file", "PATH"},
		{"enroll-seconds", 0, 0, G_OPTION_ARG_INT, &enroll_seconds,
		 "How long to record for --enroll (default 20)", "N"},
		{"version", 0, 0, G_OPTION_ARG_NONE, &version, "Show version", NULL},
		{"license", 0, 0, G_OPTION_ARG_NONE, &license, "Show license", NULL},
		{NULL}};
	g_option_context_add_main_entries(options, entries, NULL);
	g_option_context_set_description(
		options,
		"Example: ai-call --config config.yaml --identity identity.md\nEndpoints: "
		"AI_VOICE_STT_URL, AI_VOICE_TTS_URL, "
		"AI_VOICE_LIVEKIT_URL.\nSIGINT/SIGTERM clears Matrix membership before exiting.");
	if (!g_option_context_parse(options, &argc, &argv, &error))
		goto fail;
	if (version) {
		g_print("ai-call %d.%d.%d\n", AI_VERSION_MAJOR, AI_VERSION_MINOR,
				AI_VERSION_MICRO);
		return 0;
	}
	if (license) {
		g_print(
			"SPDX-License-Identifier: AGPL-3.0-or-later\nGNU Affero General Public "
			"License version 3 or later.\nhttps://www.gnu.org/licenses/agpl-3.0.html\n");
		return 0;
	}
	if (list_settings) {
		guint count, i;
		GParamSpec **properties =
			g_object_class_list_properties(G_OBJECT_GET_CLASS(call_config), &count);
		for (i = 0; i < count; i++) {
			g_autofree gchar *value =
				g_strdup_value_contents(g_param_spec_get_default_value(properties[i]));
			g_print("%s (%s): %s\n", properties[i]->name,
					g_type_name(G_PARAM_SPEC_VALUE_TYPE(properties[i])), value);
		}
		g_free(properties);
		return 0;
	}

	g_log_set_handler("ai-call", G_LOG_LEVEL_INFO, info_log, NULL);
	g_log_set_handler("ai-glib", G_LOG_LEVEL_INFO, info_log, NULL);
	app.config = ai_config_new();
	if (config_path != NULL && !ai_config_load_from_file(app.config, config_path, &error))
		goto fail;
	{
		g_autofree gchar *user_config = g_build_filename(
			g_get_user_config_dir(), "ai-glib", AI_CONFIG_FILENAME, NULL);
		const gchar *paths[] = {AI_CONFIG_SYSTEM_DIR "/" AI_CONFIG_FILENAME,
								AI_CONFIG_ADMIN_DIR "/" AI_CONFIG_FILENAME, user_config,
								config_path};
		guint i;
		for (i = 0; i < G_N_ELEMENTS(paths); i++)
			if (paths[i] != NULL &&
				(i == 3 || g_file_test(paths[i], G_FILE_TEST_EXISTS)) &&
				!ai_call_config_load(call_config, paths[i], &error))
				goto fail;
	}
	if (!ai_call_config_apply_environment(call_config, &error))
		goto fail;
	for (setting_index = 0;
		 call_overrides != NULL && call_overrides[setting_index] != NULL;
		 setting_index++) {
		g_auto(GStrv) pair = g_strsplit(call_overrides[setting_index], "=", 2);
		if (pair[1] == NULL ||
			!ai_call_config_set_text(call_config, pair[0], pair[1], &error)) {
			if (error == NULL)
				g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
									"--call-set requires PROPERTY=VALUE");
			goto fail;
		}
	}
	app.call_config = call_config;
	g_object_get(app.config, "voice-stt-url", &app.stt_url, "voice-tts-url", &app.tts_url,
				 "voice-livekit-url", &app.livekit_url, NULL);
	if (identity_file == NULL)
		g_object_get(app.config, "voice-identity-file", &identity_file, NULL);
	/* Enrolling talks to nobody: no identity, no Matrix. */
	if (enroll_name != NULL) {
		if (enroll_seconds < 3 || enroll_seconds > 60) {
			g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"--enroll-seconds must be 3 to 60");
			goto fail;
		}
		app.local = TRUE;
		app.identity = g_strdup("");
		identity_file = g_strdup("");
	}
	if (identity_file == NULL) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"Configure voice.identity_file or pass --identity");
		goto fail;
	}
	if (enroll_name == NULL && !g_file_get_contents(identity_file, &app.identity, NULL, &error))
		goto fail;
	if (!ai_provider_factory_resolve_defaults(app.config, "ai-call", provider_name, model,
											  &app.provider, &app.model, &error))
		goto fail;
	if (app.local) {
		app.mxid = g_strdup("assistant");
		goto devices;
	}
	if (credentials == NULL)
		g_object_get(call_config, "credentials", &credentials, NULL);
	if (credentials == NULL) {
		g_set_error_literal(
			&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Configure ai_call.credentials, AI_CALL_CREDENTIALS or --credentials");
		goto fail;
	}
	if (!json_parser_load_from_file(parser, credentials, &error))
		goto fail;
	creds = ai_json_root_object(parser);
	app.mxid = g_strdup(ai_json_get_string(creds, "userId", NULL));
	app.access = g_strdup(ai_json_get_string(creds, "accessToken", NULL));
	app.homeserver = g_strdup(ai_json_get_string(creds, "homeserver", NULL));
	if (app.mxid == NULL || app.access == NULL || app.homeserver == NULL) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Credentials need userId, accessToken and homeserver");
		goto fail;
	}
	while (g_str_has_suffix(app.homeserver, "/"))
		app.homeserver[strlen(app.homeserver) - 1] = '\0';
	if (!ai_livekit_transport_is_available()) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"GStreamer LiveKit plugin unavailable; see docs/voice.org");
		goto fail;
	}
devices:
	g_object_get(call_config, "state-file", &app.state_file, NULL);
	if (app.state_file != NULL && *app.state_file == '\0')
		g_clear_pointer(&app.state_file, g_free);
	g_object_get(call_config, "device", &app.device, "jwt-url", &app.jwt_url, "focus-url",
				 &app.foci, "outbound-path", &app.drop_path, "greeting", &app.greeting,
				 "outbound-greeting", &app.outbound_greeting, "transcript-dir",
				 &app.transcript_dir, "transcript-hook", &app.transcript_hook, NULL);
	/* Unset means the XDG default; an empty string turns transcripts off. */
	if (app.transcript_dir == NULL)
		app.transcript_dir = ai_call_transcript_default_dir();
	else if (*app.transcript_dir == '\0')
		g_clear_pointer(&app.transcript_dir, g_free);
	{
		g_autofree gchar *code = NULL;
		g_object_get(call_config, "speak-code", &code, NULL);
		if (code == NULL || *code == '\0' || g_str_equal(code, "yes"))
			app.speak_code = TRUE;
		else if (g_str_equal(code, "no"))
			app.speak_code = FALSE;
		else {
			g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
						"speak-code must be yes or no, not \"%s\"", code);
			goto fail;
		}
	}
	{
		g_autofree gchar *commands = NULL;
		g_object_get(call_config, "voice-commands", &commands, NULL);
		if (commands == NULL || *commands == '\0' || g_str_equal(commands, "no"))
			app.voice_commands = FALSE;
		else if (g_str_equal(commands, "yes"))
			app.voice_commands = TRUE;
		else {
			g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
						"voice-commands must be yes or no, not \"%s\"", commands);
			goto fail;
		}
	}
	{
		g_autofree gchar *room = NULL;
		g_object_get(call_config, "room-mode", &room, "chime-in-threshold",
					 &app.chime_threshold, "chime-in-cooldown-ms", &app.chime_cooldown_ms,
					 "relevance-provider", &app.relevance_provider, "relevance-model",
					 &app.relevance_model, NULL);
		if (room == NULL || *room == '\0' || g_str_equal(room, "no"))
			app.room_mode = FALSE;
		else if (g_str_equal(room, "yes"))
			app.room_mode = TRUE;
		else {
			g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
						"room-mode must be yes or no, not \"%s\"", room);
			goto fail;
		}
		if (app.relevance_provider != NULL && *app.relevance_provider == '\0')
			g_clear_pointer(&app.relevance_provider, g_free);
		if (app.relevance_model != NULL && *app.relevance_model == '\0')
			g_clear_pointer(&app.relevance_model, g_free);
		g_object_get(call_config, "speaker-url", &app.speaker_url, "voiceprint-dir",
					 &app.voiceprint_dir, "speaker-threshold", &app.speaker_threshold,
					 "unknown-speaker-name", &app.unknown_speaker_name, "guest-threshold",
					 &app.guest_threshold, "naming-timeout-ms", &app.naming_timeout_ms, NULL);
		{
			g_autofree gchar *guests = NULL;
			g_object_get(call_config, "track-guests", &guests, NULL);
			if (guests == NULL || *guests == '\0' || g_str_equal(guests, "no"))
				app.track_guests = FALSE;
			else if (g_str_equal(guests, "yes"))
				app.track_guests = TRUE;
			else {
				g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"track-guests must be yes or no, not \"%s\"", guests);
				goto fail;
			}
		}
		if (app.speaker_url != NULL && *app.speaker_url == '\0')
			g_clear_pointer(&app.speaker_url, g_free);
		if (app.voiceprint_dir == NULL || *app.voiceprint_dir == '\0') {
			g_free(app.voiceprint_dir);
			app.voiceprint_dir =
				g_build_filename(g_get_user_data_dir(), "ai-glib", "voiceprints", NULL);
		}
	}
	{
		g_autofree gchar *live = NULL;
		g_object_get(call_config, "live-text", &live, NULL);
		if (live == NULL || *live == '\0' || g_str_equal(live, "off"))
			app.live_text = 0;
		else if (g_str_equal(live, "replies"))
			app.live_text = 1;
		else if (g_str_equal(live, "both"))
			app.live_text = 2;
		else {
			g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
						"live-text must be off, replies or both, not \"%s\"", live);
			goto fail;
		}
	}
	if (app.transcript_hook != NULL && *app.transcript_hook == '\0')
		g_clear_pointer(&app.transcript_hook, g_free);
	if (app.drop_path == NULL)
		app.drop_path =
			g_build_filename(g_get_user_runtime_dir(), "ai-outbound-call.json", NULL);
	{
		g_autofree gchar *echo = NULL;
		g_object_get(call_config, "local-echo-cancel", &echo, NULL);
		if (echo == NULL || *echo == '\0' || g_str_equal(echo, "no"))
			app.local_echo_cancel = FALSE;
		else if (g_str_equal(echo, "yes"))
			app.local_echo_cancel = TRUE;
		else {
			g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
						"local-echo-cancel must be yes or no, not \"%s\"", echo);
			goto fail;
		}
	}
	/* There is no room to post into. */
	if (app.local)
		app.live_text = 0;
	if (enroll_name != NULL) {
		if (!enroll_voice(&app, enroll_name, enroll_file, (guint)enroll_seconds, &error))
			goto fail;
		g_print("Saved %s's voiceprint in %s\n", enroll_name, app.voiceprint_dir);
		goto cleanup;
	}
	if (!app.local && (app.jwt_url == NULL || *app.jwt_url == '\0' || app.foci == NULL ||
		*app.foci == '\0' || app.device == NULL || *app.device == '\0')) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"Configure ai_call.jwt-url, focus-url and a nonempty device");
		goto fail;
	}
	/* Every request is declared credentialed in request(); the session
	 * guard applies the same rule to anything else it sends
	 * (ai-http-redirect-private.h). */
	app.http = ai_http_session_new();
	g_object_set(app.http, "timeout", 40, NULL);
	app.loop = g_main_loop_new(NULL, FALSE);
	app.sync_cancel = g_cancellable_new();
	app.calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, call_unref);
	g_unix_signal_add(SIGTERM, shutdown_app, &app);
	g_unix_signal_add(SIGINT, shutdown_app, &app);
	g_unix_signal_add(SIGUSR1, toggle_mute, &app);
	if (!app.local)
		g_timeout_add_seconds(1, watch_drop, &app);
	{
		g_autofree gchar *message = NULL;
		g_object_get(app.call_config, "synthesis-error-message", &message, NULL);
		app.speech_cache = ai_call_speech_cache_new(app.tts_url, message);
		g_signal_connect(app.speech_cache, "completed", G_CALLBACK(fallback_completed),
						 &app);
		app.pending++;
		ai_call_speech_cache_start(app.speech_cache);
	}
	if (app.local) {
		g_print("ai-call listening on this machine's microphone\n");
		start_local(&app);
	} else {
		url = g_strconcat(app.homeserver, "/_matrix/client/v3/sync?timeout=0", NULL);
		request(&app, "GET", url, TRUE, NULL, app.sync_cancel, primed, NULL, NULL);
		g_print("ai-call watching as %s (%s)\n", app.mxid, app.device);
	}
	g_main_loop_run(app.loop);
cleanup:
	g_clear_object(&app.speech_cache);
	g_clear_pointer(&app.calls, g_hash_table_unref);
	g_clear_object(&app.http);
	g_clear_object(&app.sync_cancel);
	g_clear_pointer(&app.loop, g_main_loop_unref);
	g_clear_object(&app.config);
	g_free(app.homeserver);
	g_free(app.mxid);
	g_free(app.access);
	g_free(app.device);
	g_free(app.jwt_url);
	g_free(app.foci);
	g_free(app.stt_url);
	g_free(app.tts_url);
	g_free(app.livekit_url);
	g_free(app.identity);
	g_free(app.model);
	g_free(app.since);
	g_free(app.drop_path);
	g_free(app.greeting);
	g_free(app.outbound_greeting);
	g_free(app.transcript_dir);
	g_free(app.transcript_hook);
	g_free(app.state_file);
	g_free(app.relevance_provider);
	g_free(app.relevance_model);
	g_free(app.speaker_url);
	g_free(app.voiceprint_dir);
	g_free(app.unknown_speaker_name);
	return app.exit_status;
fail:
	g_printerr("ai-call: %s\n", error != NULL ? error->message : "startup failed");
	app.exit_status = 1;
	goto cleanup;
}
