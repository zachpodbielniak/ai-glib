/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib-unix.h>
#include <libsoup/soup.h>

/*
 * ai-voiceprint-server: a speaker-embedding model behind the HTTP protocol
 * AiHttpSpeakerEmbedder speaks, so a small device (a Pi Zero running
 * ai-call) can have its voiceprints made on a bigger machine.
 *
 *   POST /embed   raw 16 kHz mono S16LE  ->  {"model":"...","dim":N,"embedding":[...]}
 *                 under half a second    ->  422 {"error":"..."}
 *                 over --max-seconds     ->  413 {"error":"..."}
 *   GET  /health                         ->  {"model":"...","dim":N}
 *
 * It keeps nothing: audio and embeddings live only for the request. It
 * listens on loopback unless told otherwise, and has no authentication of
 * its own; put it behind something that does before exposing it.
 */
typedef struct {
	AiOnnxSpeakerEmbedder *embedder;
	gsize max_bytes;
	guint requests;
} Server;

static void
reply(SoupServerMessage *msg, guint status, JsonNode *body)
{
	g_autofree gchar *text = json_to_string(body, FALSE);
	soup_server_message_set_status(msg, status, NULL);
	soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, text,
									 strlen(text));
}
static void
reply_error(SoupServerMessage *msg, guint status, const gchar *message)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "error");
	json_builder_add_string_value(b, message);
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	reply(msg, status, root);
}

typedef struct {
	SoupServerMessage *msg;
	gint64 started;
} Pending;
/* The embedding is back from the worker thread: answer and resume. */
static void
embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	Pending *p = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiVoiceprint) print =
		ai_speaker_embedder_embed_finish(AI_SPEAKER_EMBEDDER(source), result, &error);
	if (print == NULL) {
		gboolean short_audio = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_log("ai-voiceprint-server", G_LOG_LEVEL_INFO, "embed failed: %s", error->message);
		reply_error(p->msg, short_audio ? 422 : 500, error->message);
	} else {
		g_autoptr(JsonNode) node = ai_voiceprint_to_json(print);
		gsize dims = 0;
		ai_voiceprint_get_embedding(print, &dims);
		/* The service's answer has no name, count or time: only the model,
		 * the length and the vector. */
		json_object_remove_member(json_node_get_object(node), "count");
		json_object_remove_member(json_node_get_object(node), "updated");
		json_object_set_int_member(json_node_get_object(node), "dim", (gint64)dims);
		reply(p->msg, 200, node);
		g_log("ai-voiceprint-server", G_LOG_LEVEL_DEBUG, "embedded in %" G_GINT64_FORMAT " ms",
			  (g_get_monotonic_time() - p->started) / 1000);
	}
	soup_server_message_unpause(p->msg);
	g_object_unref(p->msg);
	g_free(p);
}
static void
embed_handler(SoupServer *soup, SoupServerMessage *msg, const gchar *path, GHashTable *query,
			  gpointer data)
{
	Server *server = data;
	SoupMessageBody *body;
	g_autoptr(GBytes) pcm = NULL;
	Pending *p;
	if (g_strcmp0(soup_server_message_get_method(msg), SOUP_METHOD_POST) != 0) {
		reply_error(msg, 405, "POST raw 16 kHz mono S16LE audio");
		return;
	}
	body = soup_server_message_get_request_body(msg);
	if ((gsize)body->length > server->max_bytes) {
		reply_error(msg, 413, "too much audio");
		return;
	}
	if (body->length < (goffset)AI_SPEAKER_EMBEDDER_DEFAULT_MIN_BYTES || body->length % 2 != 0) {
		reply_error(msg, 422, "need at least 0.5 s of S16LE audio");
		return;
	}
	server->requests++;
	pcm = soup_message_body_flatten(body);
	p = g_new0(Pending, 1);
	p->msg = g_object_ref(msg);
	p->started = g_get_monotonic_time();
	soup_server_message_pause(msg);
	ai_speaker_embedder_embed_async(AI_SPEAKER_EMBEDDER(server->embedder), pcm, NULL, embedded,
									p);
}
static void
health_handler(SoupServer *soup, SoupServerMessage *msg, const gchar *path, GHashTable *query,
			   gpointer data)
{
	Server *server = data;
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "model");
	json_builder_add_string_value(b, ai_onnx_speaker_embedder_get_model_id(server->embedder));
	json_builder_set_member_name(b, "dim");
	json_builder_add_int_value(b, ai_onnx_speaker_embedder_get_dimensions(server->embedder));
	json_builder_set_member_name(b, "requests");
	json_builder_add_int_value(b, server->requests);
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	reply(msg, 200, root);
}
static gboolean
quit(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
	g_autofree gchar *model = NULL, *listen = NULL;
	gint port = 8095, threads = 1, max_seconds = 30;
	gboolean license = FALSE, version = FALSE;
	g_autoptr(GOptionContext) options = g_option_context_new("- serve speaker voiceprints");
	g_autoptr(GError) error = NULL;
	g_autoptr(SoupServer) soup = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autoptr(GInetAddress) address = NULL;
	g_autoptr(GSocketAddress) socket_address = NULL;
	Server server = {0};
	GOptionEntry entries[] = {
		{"model", 'm', 0, G_OPTION_ARG_FILENAME, &model, "Speaker-embedding ONNX model", "PATH"},
		{"listen", 'l', 0, G_OPTION_ARG_STRING, &listen, "Address to listen on (127.0.0.1)",
		 "ADDRESS"},
		{"port", 'p', 0, G_OPTION_ARG_INT, &port, "Port (8095)", "PORT"},
		{"threads", 't', 0, G_OPTION_ARG_INT, &threads, "Threads per embedding (1)", "N"},
		{"max-seconds", 0, 0, G_OPTION_ARG_INT, &max_seconds,
		 "Longest audio accepted, in seconds (30)", "N"},
		{"version", 0, 0, G_OPTION_ARG_NONE, &version, "Show version", NULL},
		{"license", 0, 0, G_OPTION_ARG_NONE, &license, "Show license", NULL},
		{NULL}};
	g_option_context_add_main_entries(options, entries, NULL);
	g_option_context_set_summary(
		options, "Turns 16 kHz mono S16LE audio into a speaker voiceprint with an ONNX model,\n"
				 "for ai-call's speaker-url (or any AiHttpSpeakerEmbedder). Keeps nothing.");
	g_option_context_set_description(
		options,
		"Examples:\n"
		"  ai-voiceprint-server --model nemo_en_titanet_small.onnx\n"
		"  ai-voiceprint-server -m wespeaker_en_voxceleb_resnet34.onnx -l 0.0.0.0 -p 9000 -t 2\n"
		"  curl --data-binary @speech.raw http://127.0.0.1:8095/embed\n"
		"\n"
		"Models: the speaker-recognition ONNX exports from sherpa-onnx (NeMo TitaNet,\n"
		"wespeaker, 3D-Speaker); the front-end is read from the model's metadata.\n"
		"There is no authentication: bind to a trusted network only.");
	if (!g_option_context_parse(options, &argc, &argv, &error)) {
		g_printerr("ai-voiceprint-server: %s\n", error->message);
		return 2;
	}
	if (version) {
		g_print("ai-voiceprint-server %d.%d.%d\n", AI_VERSION_MAJOR, AI_VERSION_MINOR,
				AI_VERSION_MICRO);
		return 0;
	}
	if (license) {
		g_print("SPDX-License-Identifier: AGPL-3.0-or-later\nGNU Affero General Public "
				"License version 3 or later.\nhttps://www.gnu.org/licenses/agpl-3.0.html\n");
		return 0;
	}
	if (model == NULL) {
		g_printerr("ai-voiceprint-server: --model is required (see --help)\n");
		return 2;
	}
	if (port < 1 || port > 65535 || threads < 1 || threads > 64 || max_seconds < 1 ||
		max_seconds > 600) {
		g_printerr("ai-voiceprint-server: --port, --threads or --max-seconds out of range\n");
		return 2;
	}
	server.max_bytes = (gsize)max_seconds * 16000 * 2;
	server.embedder = g_initable_new(AI_TYPE_ONNX_SPEAKER_EMBEDDER, NULL, &error, "model-path",
									 model, "threads", (guint)threads, NULL);
	if (server.embedder == NULL) {
		g_printerr("ai-voiceprint-server: %s\n", error->message);
		return 1;
	}
	address = g_inet_address_new_from_string(listen != NULL ? listen : "127.0.0.1");
	if (address == NULL) {
		g_printerr("ai-voiceprint-server: not an address: %s\n", listen);
		g_object_unref(server.embedder);
		return 2;
	}
	socket_address = g_inet_socket_address_new(address, (guint16)port);
	soup = soup_server_new("server-header", "ai-voiceprint-server", NULL);
	soup_server_add_handler(soup, "/embed", embed_handler, &server, NULL);
	soup_server_add_handler(soup, "/health", health_handler, &server, NULL);
	if (!soup_server_listen(soup, socket_address, 0, &error)) {
		g_printerr("ai-voiceprint-server: %s\n", error->message);
		g_object_unref(server.embedder);
		return 1;
	}
	g_log("ai-voiceprint-server", G_LOG_LEVEL_MESSAGE, "%s on http://%s:%d/embed",
		  ai_onnx_speaker_embedder_get_model_id(server.embedder),
		  listen != NULL ? listen : "127.0.0.1", port);
	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, quit, loop);
	g_unix_signal_add(SIGTERM, quit, loop);
	g_main_loop_run(loop);
	soup_server_disconnect(soup);
	g_object_unref(server.embedder);
	return 0;
}
