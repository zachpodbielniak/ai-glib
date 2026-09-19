/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib-unix.h>
#include <libsoup/soup.h>
#include "core/ai-json-util.h"
#include "call/ai-call-config.h"

#define MEMBER "org.matrix.msc3401.call.member"
#define RING "org.matrix.msc4075.rtc.notification"
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AiVoiceAgent/1.0"

typedef struct _App App;
typedef struct _Call Call;
typedef void (*Reply)(App *, JsonNode *, const GError *, gpointer);
struct _App {
	GMainLoop *loop;
	SoupSession *http;
	AiConfig *config;
	AiCallConfig *call_config;
	GHashTable *calls;
	GCancellable *sync_cancel;
	gchar *homeserver, *mxid, *access, *device, *jwt_url, *foci;
	gchar *stt_url, *tts_url, *livekit_url, *identity, *model;
	gchar *since, *drop_path, *greeting, *outbound_greeting;
	AiProviderType provider;
	guint pending, startup;
	gint exit_status;
	gboolean stopping, primed, drop_busy;
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
	guint retry_source, answer_source;
	gint64 answer_deadline, answered_at, transcript_at;
};
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
maybe_connect(Call *call);
static void
maybe_exit(App *app)
{
	if (app->stopping && app->pending == 0 && g_hash_table_size(app->calls) == 0)
		g_main_loop_quit(app->loop);
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
	g_clear_object(&call->voice);
	g_clear_object(&call->transport);
	g_clear_object(&call->synthesizer);
	g_free(call->room);
	g_free(call->key);
	g_free(call->tx_token);
	g_free(call->rx_token);
	g_free(call->opening);
	g_free(call->target);
	g_string_free(call->context, TRUE);
	g_free(call);
}
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
		if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(r->message)))
			g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u",
						soup_message_get_status(r->message));
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
	r->message = soup_message_new(method, url);
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
	if (call->left && !call->clearing)
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
static void
close_call(Call *call)
{
	g_autoptr(JsonNode) body = empty_object();
	if (call->closing)
		return;
	call->closing = TRUE;
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
	/* Membership removal runs independently of media teardown and uses a fresh
	 * cancellable: SIGTERM must not cancel its own cleanup request. */
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
	g_printerr("Voice: %s\n", error->message);
}
static void
voice_state(AiVoiceSession *voice, AiVoiceState state, gpointer data)
{
	Call *call = data;
	GEnumClass *states = g_type_class_ref(AI_TYPE_VOICE_STATE);
	GEnumValue *value = g_enum_get_value(states, state);
	g_log("ai-call", G_LOG_LEVEL_INFO, "Voice state: room=%s state=%s", call->room,
		  value != NULL ? value->value_nick : "unknown");
	g_type_class_unref(states);
}
static void
info_log(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer data)
{
	/* INFO lifecycle messages are visible without G_MESSAGES_DEBUG. */
	g_printerr("%s INFO: %s\n", domain, message);
}
static void
voice_transcript(AiVoiceSession *voice, const gchar *speaker, const gchar *text,
				 gboolean final, gpointer data)
{
	Call *call = data;
	if (final) {
		call->transcript_at = g_get_monotonic_time();
		g_print("Transcript [%s]: %s\n", speaker, text);
	}
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
	call->transport =
		AI_AUDIO_TRANSPORT(ai_livekit_transport_new(app->livekit_url, call->rx_token));
	stt = ai_websocket_recognizer_new(app->stt_url);
	tts = ai_http_synthesizer_new(app->tts_url);
	vad = ai_webrtc_voice_activity_new();
	call->voice = ai_voice_session_new(call->transport, AI_SPEECH_RECOGNIZER(stt),
									   AI_SPEECH_SYNTHESIZER(tts), AI_VOICE_ACTIVITY(vad),
									   conversation);
	if (app->call_config != NULL) {
		guint timeout, silence, mode, deadline, barge_in;
		g_autofree gchar *fallback = NULL, *transcription_error = NULL;
		g_object_get(app->call_config, "stt-timeout-ms", &timeout, "trailing-silence-ms",
					 &silence, "vad-mode", &mode, "turn-deadline-ms", &deadline,
					 "barge-in-ms", &barge_in, "deadline-message", &fallback,
					 "transcription-error-message", &transcription_error, NULL);
		g_object_set(stt, "timeout-ms", timeout, NULL);
		g_object_set(vad, "trailing-silence-ms", silence, "mode", mode, NULL);
		g_object_set(call->voice, "turn-deadline-ms", deadline, "deadline-message",
					 fallback, "transcription-error-message", transcription_error,
					 "barge-in-ms", barge_in, NULL);
	}
	g_signal_connect(call->voice, "error", G_CALLBACK(voice_error), call);
	g_signal_connect(call->voice, "state-changed", G_CALLBACK(voice_state), call);
	g_signal_connect(call->voice, "transcript", G_CALLBACK(voice_transcript), call);
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
		/* A shutdown can race the original PUT. Clear again after its reply. */
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
static gboolean
shutdown_app(gpointer data)
{
	App *app = data;
	GHashTableIter iter;
	gpointer value;
	if (app->stopping)
		return G_SOURCE_CONTINUE;
	app->stopping = TRUE;
	g_cancellable_cancel(app->sync_cancel);
	g_hash_table_iter_init(&iter, app->calls);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		close_call(value);
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
		json_parser_load_from_data(parser, drop->contents, -1, NULL)) {
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
									NULL) &&
		length <= 65536) {
		Drop *drop = g_new0(Drop, 1);
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
	if (identity_file == NULL) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"Configure voice.identity_file or pass --identity");
		goto fail;
	}
	if (!g_file_get_contents(identity_file, &app.identity, NULL, &error))
		goto fail;
	if (!ai_provider_factory_resolve_defaults(app.config, "ai-call", provider_name, model,
											  &app.provider, &app.model, &error))
		goto fail;
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
	g_object_get(call_config, "device", &app.device, "jwt-url", &app.jwt_url, "focus-url",
				 &app.foci, "outbound-path", &app.drop_path, "greeting", &app.greeting,
				 "outbound-greeting", &app.outbound_greeting, NULL);
	if (app.drop_path == NULL)
		app.drop_path =
			g_build_filename(g_get_user_runtime_dir(), "ai-outbound-call.json", NULL);
	if (app.jwt_url == NULL || *app.jwt_url == '\0' || app.foci == NULL ||
		*app.foci == '\0' || app.device == NULL || *app.device == '\0') {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"Configure ai_call.jwt-url, focus-url and a nonempty device");
		goto fail;
	}
	app.http = soup_session_new_with_options("timeout", 40, NULL);
	app.loop = g_main_loop_new(NULL, FALSE);
	app.sync_cancel = g_cancellable_new();
	app.calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, call_unref);
	g_unix_signal_add(SIGTERM, shutdown_app, &app);
	g_unix_signal_add(SIGINT, shutdown_app, &app);
	g_timeout_add_seconds(1, watch_drop, &app);
	url = g_strconcat(app.homeserver, "/_matrix/client/v3/sync?timeout=0", NULL);
	request(&app, "GET", url, TRUE, NULL, app.sync_cancel, primed, NULL, NULL);
	g_print("ai-call watching as %s (%s)\n", app.mxid, app.device);
	g_main_loop_run(app.loop);
cleanup:
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
	return app.exit_status;
fail:
	g_printerr("ai-call: %s\n", error != NULL ? error->message : "startup failed");
	app.exit_status = 1;
	goto cleanup;
}
