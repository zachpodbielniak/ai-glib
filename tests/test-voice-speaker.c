/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

/*
 * Speaker naming by voiceprint: AiVoiceprint itself, the stores, the HTTP
 * embedder against a stand-in service, and AiVoiceprintIdentifier's
 * ordering, guests, promotion and extension points.
 */

/* A voiceprint service stand-in. The first sample of the PCM picks what it
 * does, so a test can say "this is A" without real audio:
 *   1000 A, 2000 B, 1100 A on a different day, 3000 and 3100 one stranger,
 *   4000 HTTP 500, 4100 not JSON, 4200 a value too big for a float,
 *   4300 a zero vector, 4400 a reply over a megabyte, 4500 never answers,
 *   4600 a redirect to another host, 4800 the 3000 stranger after 300 ms,
 *   anything else another stranger.
 * Less than half a second is refused, as the real service does. */
typedef struct {
	SoupServer *server;
	gchar *url;
	gchar *dir;
	const gchar *model;
	guint requests;
	GPtrArray *held; /* paused messages, never answered */
} Service;

static void
reply_json(SoupServerMessage *msg, guint status, const gchar *json)
{
	soup_server_message_set_status(msg, status, NULL);
	soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, json,
									 strlen(json));
}
typedef struct {
	SoupServerMessage *msg;
	gchar *json;
} Late;
static gboolean
late_reply(gpointer data)
{
	Late *l = data;
	reply_json(l->msg, 200, l->json);
	soup_server_message_unpause(l->msg);
	g_object_unref(l->msg);
	g_free(l->json);
	g_free(l);
	return G_SOURCE_REMOVE;
}
static void
embed_handler(SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query,
			  gpointer data)
{
	Service *s = data;
	SoupMessageBody *body = soup_server_message_get_request_body(msg);
	const gchar *vector;
	g_autofree gchar *reply = NULL;
	gint16 first = 0;
	s->requests++;
	if (body->length < 16000) {
		reply_json(msg, 422, "{\"error\":\"need at least 0.5 s of S16LE audio\"}");
		return;
	}
	memcpy(&first, body->data, 2);
	switch (first) {
	case 4000:
		reply_json(msg, 500, "{\"error\":\"model not loaded\"}");
		return;
	case 4100:
		reply_json(msg, 200, "<html>proxy error</html>");
		return;
	case 4200:
		reply_json(msg, 200, "{\"model\":\"test-voices\",\"embedding\":[1e39,0,0,0]}");
		return;
	case 4300:
		reply_json(msg, 200, "{\"model\":\"test-voices\",\"embedding\":[0,0,0,0]}");
		return;
	case 4400: {
		GString *big = g_string_new("{\"model\":\"test-voices\",\"embedding\":[1");
		while (big->len < 1100 * 1024)
			g_string_append(big, ",0");
		g_string_append(big, "]}");
		reply_json(msg, 200, big->str);
		g_string_free(big, TRUE);
		return;
	}
	case 4500:
		soup_server_message_pause(msg);
		g_ptr_array_add(s->held, g_object_ref(msg));
		return;
	case 4600:
		soup_server_message_set_redirect(msg, 302, "http://elsewhere.invalid/embed");
		return;
	case 4800: {
		Late *l = g_new0(Late, 1);
		l->msg = g_object_ref(msg);
		l->json = g_strdup_printf("{\"model\":\"%s\",\"embedding\":[0,0,1,0]}", s->model);
		soup_server_message_pause(msg);
		g_timeout_add(300, late_reply, l);
		return;
	}
	default:
		break;
	}
	vector = first == 1000	 ? "[1,0,0,0]"
			 : first == 2000 ? "[0,1,0,0]"
			 : first == 1100 ? "[0.9949,0.0995,0,0]"
			 : first == 3000 ? "[0,0,1,0]"
			 : first == 3100 ? "[0,0,0.9949,0.0995]"
							 : "[0,0,0,1]";
	reply = g_strdup_printf("{\"model\":\"%s\",\"dim\":4,\"embedding\":%s}", s->model, vector);
	reply_json(msg, 200, reply);
}
static void
service_start(Service *s)
{
	GSList *uris;
	g_autofree gchar *base = NULL;
	s->model = "test-voices";
	s->held = g_ptr_array_new_with_free_func(g_object_unref);
	s->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(s->server, "/embed", embed_handler, s, NULL);
	g_assert_true(soup_server_listen_local(s->server, 0, 0, NULL));
	uris = soup_server_get_uris(s->server);
	base = g_uri_to_string(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	s->url = g_strconcat(base, "embed", NULL);
	s->dir = g_dir_make_tmp("voiceprints-XXXXXX", NULL);
}
static void
remove_tree(const gchar *dir)
{
	GDir *d = g_dir_open(dir, 0, NULL);
	const gchar *name;
	while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, name, NULL);
		g_unlink(path);
	}
	if (d != NULL)
		g_dir_close(d);
	g_rmdir(dir);
}
static void
drain(void)
{
	while (g_main_context_iteration(NULL, FALSE))
		;
}
static void
service_stop(Service *s)
{
	guint i;
	for (i = 0; i < s->held->len; i++) {
		reply_json(g_ptr_array_index(s->held, i), 503, "{}");
		soup_server_message_unpause(g_ptr_array_index(s->held, i));
	}
	drain();
	g_ptr_array_unref(s->held);
	soup_server_disconnect(s->server);
	g_object_unref(s->server);
	remove_tree(s->dir);
	g_free(s->dir);
	g_free(s->url);
}
static GBytes *
voice(gint16 first, gdouble seconds)
{
	gsize n = (gsize)(seconds * 16000);
	gint16 *samples = g_new0(gint16, n);
	samples[0] = first;
	return g_bytes_new_take(samples, n * 2);
}

typedef struct {
	gboolean done;
	AiSpeakerMatch *match;
	AiVoiceprint *print;
	GError *error;
} Result;
static void
result_clear(Result *r)
{
	g_clear_object(&r->match);
	g_clear_object(&r->print);
	g_clear_error(&r->error);
	r->done = FALSE;
}
static void
enrolled(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->print = ai_voiceprint_identifier_enroll_finish(AI_VOICEPRINT_IDENTIFIER(source), result,
													  &r->error);
	r->done = TRUE;
}
static void
identified(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->match = ai_speaker_identifier_identify_finish(AI_SPEAKER_IDENTIFIER(source), result,
													 &r->error);
	r->done = TRUE;
}
static void
embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->print = ai_speaker_embedder_embed_finish(AI_SPEAKER_EMBEDDER(source), result, &r->error);
	r->done = TRUE;
}
static gboolean
tick(gpointer data)
{
	return G_SOURCE_CONTINUE;
}
/* Run the main context until *FLAG is set, for at most five seconds; a tick
 * keeps a blocking iteration from waiting forever on nothing. */
static void
wait_flag(const gboolean *flag)
{
	gint64 limit = g_get_monotonic_time() + 5000000;
	guint ticker = g_timeout_add(50, tick, NULL);
	while (!*flag && g_get_monotonic_time() < limit)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(ticker);
}
static void
wait_done(Result *r)
{
	wait_flag(&r->done);
	g_assert_true(r->done);
}
static void
enroll(AiVoiceprintIdentifier *id, const gchar *name, gint16 first)
{
	Result r = {0};
	g_autoptr(GBytes) pcm = voice(first, 2.0);
	ai_voiceprint_identifier_enroll_async(id, name, pcm, NULL, enrolled, &r);
	wait_done(&r);
	g_assert_no_error(r.error);
	g_assert_nonnull(r.print);
	result_clear(&r);
}
static Result
identify(AiVoiceprintIdentifier *id, gint16 first, gdouble seconds)
{
	Result r = {0};
	g_autoptr(GBytes) pcm = voice(first, seconds);
	ai_speaker_identifier_identify_async(AI_SPEAKER_IDENTIFIER(id), pcm, NULL, identified, &r);
	wait_done(&r);
	return r;
}
static const gchar *
name_of(Result *r)
{
	g_assert_no_error(r->error);
	g_assert_nonnull(r->match);
	return ai_speaker_match_get_name(r->match);
}

/* ---- AiVoiceprint ---- */

/* A vector with no direction or a non-finite value is never a voiceprint. */
static void
test_voiceprint_validation(void)
{
	static const gfloat good[] = {3, 4};
	static const gfloat zero[] = {0, 0};
	gfloat bad[2] = {1, 0};
	g_autoptr(GError) error = NULL;
	g_autoptr(AiVoiceprint) v = ai_voiceprint_new("Ada", "m", good, 2, &error);
	const gfloat *e;
	gsize dims;
	g_assert_no_error(error);
	e = ai_voiceprint_get_embedding(v, &dims);
	g_assert_cmpuint(dims, ==, 2);
	g_assert_cmpfloat_with_epsilon(e[0], 0.6, 1e-6);
	g_assert_cmpfloat_with_epsilon(e[1], 0.8, 1e-6);
	g_assert_null(ai_voiceprint_new("Ada", "m", zero, 2, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error(&error);
	bad[1] = NAN;
	g_assert_null(ai_voiceprint_new("Ada", "m", bad, 2, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error(&error);
	bad[1] = INFINITY;
	g_assert_null(ai_voiceprint_new("Ada", "m", bad, 2, &error));
	g_clear_error(&error);
	g_assert_null(ai_voiceprint_new("Ada", NULL, good, 2, &error));
	g_clear_error(&error);
	g_assert_null(ai_voiceprint_new("Ada", "m", good, 0, &error));
	g_clear_error(&error);
	g_assert_null(ai_voiceprint_new("[Ada]", "m", good, 2, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}
/* JSON round-trips; similarity is only between the same model; absorbing
 * weights by count and caps it. */
static void
test_voiceprint_json_and_math(void)
{
	static const gfloat a[] = {1, 0, 0};
	static const gfloat b[] = {0, 1, 0};
	g_autoptr(GError) error = NULL;
	g_autoptr(AiVoiceprint) va = ai_voiceprint_new("Ada", "m1", a, 3, &error);
	g_autoptr(AiVoiceprint) vb = ai_voiceprint_new(NULL, "m1", b, 3, &error);
	g_autoptr(AiVoiceprint) other = ai_voiceprint_new(NULL, "m2", a, 3, &error);
	g_autoptr(AiVoiceprint) back = NULL;
	g_autoptr(JsonNode) node = NULL;
	const gfloat *e;
	g_assert_no_error(error);
	ai_voiceprint_set_count(va, 3);
	node = ai_voiceprint_to_json(va);
	back = ai_voiceprint_new_from_json(node, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(ai_voiceprint_get_name(back), ==, "Ada");
	g_assert_cmpstr(ai_voiceprint_get_model(back), ==, "m1");
	g_assert_cmpuint(ai_voiceprint_get_count(back), ==, 3);
	g_assert_cmpfloat_with_epsilon(ai_voiceprint_similarity(va, back), 1.0, 1e-6);
	g_assert_cmpfloat_with_epsilon(ai_voiceprint_similarity(va, vb), 0.0, 1e-6);
	g_assert_false(ai_voiceprint_is_comparable(va, other));
	g_assert_cmpfloat(ai_voiceprint_similarity(va, other), ==, -1.0);
	/* Three parts A to one part B, capped at 2 recordings of history. */
	ai_voiceprint_absorb(va, vb, 2);
	e = ai_voiceprint_get_embedding(va, NULL);
	g_assert_cmpfloat_with_epsilon(e[0] / e[1], 2.0, 1e-5);
	g_assert_cmpuint(ai_voiceprint_get_count(va), ==, 2);
	/* Incomparable: left alone. */
	ai_voiceprint_absorb(va, other, 0);
	g_assert_cmpfloat_with_epsilon(e[0] / e[1], 2.0, 1e-5);
}
/* A name is a file name and a prompt prefix: nothing that escapes either. */
static void
test_voiceprint_names(void)
{
	const gchar *good[] = {"Ada", "Kim Lee", "José", "O'Brien", NULL};
	const gchar *bad[] = {"",		  ".hidden", "a/b",	"..\\x",	"[Sam]", "Sam: hi",
						  "Sam\nKim", " Sam",	 "Sam ", "Guest 3", "guest 12", NULL};
	guint i, n = 0;
	g_autofree gchar *long_name = g_strnfill(AI_VOICEPRINT_NAME_MAX + 1, 'a');
	for (i = 0; good[i] != NULL; i++)
		g_assert_true(ai_voiceprint_name_is_valid(good[i]));
	for (i = 0; bad[i] != NULL; i++)
		g_assert_false(ai_voiceprint_name_is_valid(bad[i]));
	g_assert_false(ai_voiceprint_name_is_valid(long_name));
	g_assert_false(ai_voiceprint_name_is_valid(NULL));
	g_assert_true(ai_voiceprint_name_is_guest("Guest 12", &n));
	g_assert_cmpuint(n, ==, 12);
	g_assert_false(ai_voiceprint_name_is_guest("Guest", NULL));
	g_assert_false(ai_voiceprint_name_is_guest("Guest 0", NULL));
	g_assert_false(ai_voiceprint_name_is_guest("Guest 2b", NULL));
}

/* ---- the stores ---- */

static void
on_changed(AiVoiceprintStore *store, gpointer data)
{
	(*(guint *)data)++;
}
static AiVoiceprint *
print_of(const gchar *name, gfloat x, gfloat y)
{
	gfloat v[2];
	v[0] = x;
	v[1] = y;
	return ai_voiceprint_new(name, "m", v, 2, NULL);
}
/* Both stores keep the same contract: save, look up, list sorted, rename to
 * a free name only, forget, and say each time that something changed. */
static void
check_store_contract(AiVoiceprintStore *store)
{
	g_autoptr(AiVoiceprint) kim = print_of("Kim", 1, 0);
	g_autoptr(AiVoiceprint) ada = print_of("Ada", 0, 1);
	g_autoptr(AiVoiceprint) found = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GError) error = NULL;
	guint changes = 0;
	g_signal_connect(store, "changed", G_CALLBACK(on_changed), &changes);
	g_assert_true(ai_voiceprint_store_save(store, kim, &error));
	g_assert_true(ai_voiceprint_store_save(store, ada, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(changes, >=, 2);
	all = ai_voiceprint_store_list(store);
	g_assert_cmpuint(all->len, ==, 2);
	g_assert_cmpstr(ai_voiceprint_get_name(g_ptr_array_index(all, 0)), ==, "Ada");
	found = ai_voiceprint_store_lookup(store, "Kim");
	g_assert_nonnull(found);
	g_assert_null(ai_voiceprint_store_lookup(store, "Nobody"));
	g_assert_null(ai_voiceprint_store_lookup(store, "../etc"));
	g_assert_false(ai_voiceprint_store_rename(store, "Kim", "Ada", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error(&error);
	g_assert_false(ai_voiceprint_store_rename(store, "Nobody", "Sam", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_true(ai_voiceprint_store_rename(store, "Kim", "Kimberly", &error));
	g_clear_object(&found);
	found = ai_voiceprint_store_lookup(store, "Kimberly");
	g_assert_nonnull(found);
	g_assert_cmpstr(ai_voiceprint_get_name(found), ==, "Kimberly");
	g_assert_null(ai_voiceprint_store_lookup(store, "Kim"));
	g_assert_true(ai_voiceprint_store_remove(store, "Ada", &error));
	g_assert_false(ai_voiceprint_store_remove(store, "Ada", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(ai_voiceprint_store_remove(store, "a/b", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_signal_handlers_disconnect_by_data(store, &changes);
}
static void
test_memory_store(void)
{
	g_autoptr(AiMemoryVoiceprintStore) store = ai_memory_voiceprint_store_new();
	check_store_contract(AI_VOICEPRINT_STORE(store));
}
static void
test_file_store_contract(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("voiceprints-XXXXXX", NULL);
	g_autofree gchar *nested = g_build_filename(dir, "store", NULL);
	g_autoptr(AiFileVoiceprintStore) store = ai_file_voiceprint_store_new(nested);
	g_autofree gchar *file = NULL;
	GStatBuf st;
	check_store_contract(AI_VOICEPRINT_STORE(store));
	/* Created private, and so is every file. */
	g_assert_cmpint(g_stat(nested, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	file = g_build_filename(nested, "Kimberly.json", NULL);
	g_assert_cmpint(g_stat(file, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	remove_tree(nested);
	g_rmdir(dir);
}
/* What the file store will not read: a symlink, a file far too large, a
 * file whose name is not its own, junk. A readable-by-others file or
 * directory is tightened, not trusted as it is. */
static void
test_file_store_hardening(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("voiceprints-XXXXXX", NULL);
	g_autofree gchar *target = g_build_filename(dir, "target.txt", NULL);
	g_autofree gchar *link = g_build_filename(dir, "Linked.json", NULL);
	g_autofree gchar *huge = g_build_filename(dir, "Huge.json", NULL);
	g_autofree gchar *liar = g_build_filename(dir, "Liar.json", NULL);
	g_autofree gchar *open_file = g_build_filename(dir, "Open.json", NULL);
	g_autofree gchar *big = g_strnfill(AI_FILE_VOICEPRINT_STORE_MAX_FILE + 10, ' ');
	g_autoptr(AiFileVoiceprintStore) store = NULL;
	g_autoptr(AiVoiceprint) v = print_of("Ada", 1, 0);
	g_autoptr(GPtrArray) all = NULL;
	GStatBuf st;
	const gchar *print = "{\"name\":\"Open\",\"model\":\"m\",\"embedding\":[1,0]}";
	g_assert_true(g_file_set_contents(target,
									  "{\"name\":\"Linked\",\"model\":\"m\",\"embedding\":[1,0]}",
									  -1, NULL));
	g_assert_cmpint(symlink(target, link), ==, 0);
	g_assert_true(g_file_set_contents(huge, big, -1, NULL));
	g_assert_true(g_file_set_contents(
		liar, "{\"name\":\"Someone Else\",\"model\":\"m\",\"embedding\":[1,0]}", -1, NULL));
	g_assert_true(g_file_set_contents(open_file, print, -1, NULL));
	g_assert_cmpint(g_chmod(open_file, 0644), ==, 0);
	g_assert_cmpint(g_chmod(dir, 0755), ==, 0);
	store = ai_file_voiceprint_store_new(dir);
	all = ai_voiceprint_store_list(AI_VOICEPRINT_STORE(store));
	g_assert_cmpuint(all->len, ==, 1);
	g_assert_cmpstr(ai_voiceprint_get_name(g_ptr_array_index(all, 0)), ==, "Open");
	g_assert_cmpint(g_stat(open_file, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	/* Saving tightens the directory. */
	g_assert_true(ai_voiceprint_store_save(AI_VOICEPRINT_STORE(store), v, NULL));
	g_assert_cmpint(g_stat(dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);
	remove_tree(dir);
}
static void
count_change(AiVoiceprintStore *store, gpointer data)
{
	(*(guint *)data)++;
}
/* Another process enrolling reaches a running identifier. */
static void
test_file_store_monitor(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("voiceprints-XXXXXX", NULL);
	g_autofree gchar *file = g_build_filename(dir, "Ada.json", NULL);
	g_autoptr(AiFileVoiceprintStore) store = NULL;
	g_autoptr(AiHttpSpeakerEmbedder) embedder = ai_http_speaker_embedder_new("http://127.0.0.1:1/");
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	guint changes = 0;
	gint64 limit;
	store = g_object_new(AI_TYPE_FILE_VOICEPRINT_STORE, "directory", dir, "monitor", TRUE, NULL);
	g_signal_connect(store, "changed", G_CALLBACK(count_change), &changes);
	id = ai_voiceprint_identifier_new_full(AI_SPEAKER_EMBEDDER(embedder),
										   AI_VOICEPRINT_STORE(store));
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 0);
	g_assert_true(g_file_set_contents_full(
		file, "{\"name\":\"Ada\",\"model\":\"m\",\"embedding\":[1,0]}", -1,
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL));
	limit = g_get_monotonic_time() + 5000000;
	while (changes == 0 && g_get_monotonic_time() < limit) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	g_assert_cmpuint(changes, >, 0);
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 1);
	g_signal_handlers_disconnect_by_data(store, &changes);
	remove_tree(dir);
}

/* ---- the HTTP embedder ---- */

static Result
embed_with(AiHttpSpeakerEmbedder *embedder, gint16 first, gdouble seconds)
{
	Result r = {0};
	g_autoptr(GBytes) pcm = voice(first, seconds);
	ai_speaker_embedder_embed_async(AI_SPEAKER_EMBEDDER(embedder), pcm, NULL, embedded, &r);
	wait_done(&r);
	return r;
}
/* Every way a service can fail is an error that says so, and none of them
 * is a voiceprint. */
static void
test_http_embedder_failures(void)
{
	Service s = {0};
	g_autoptr(AiHttpSpeakerEmbedder) embedder = NULL;
	Result r;
	service_start(&s);
	embedder = ai_http_speaker_embedder_new(s.url);
	g_object_set(embedder, "timeout-ms", 300, NULL);
	r = embed_with(embedder, 1000, 1.0);
	g_assert_no_error(r.error);
	g_assert_cmpstr(ai_voiceprint_get_model(r.print), ==, "test-voices");
	g_assert_null(ai_voiceprint_get_name(r.print));
	result_clear(&r);
	r = embed_with(embedder, 4000, 1.0);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_FAILED);
	g_assert_nonnull(strstr(r.error->message, "model not loaded"));
	result_clear(&r);
	r = embed_with(embedder, 4100, 1.0);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	result_clear(&r);
	r = embed_with(embedder, 4200, 1.0);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	result_clear(&r);
	r = embed_with(embedder, 4300, 1.0);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	result_clear(&r);
	r = embed_with(embedder, 4400, 1.0);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
	result_clear(&r);
	r = embed_with(embedder, 4500, 1.0);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
	result_clear(&r);
	r = embed_with(embedder, 4600, 1.0);
	g_assert_nonnull(r.error);
	g_assert_null(r.print);
	result_clear(&r);
	/* Too short never reaches the service. */
	{
		guint before = s.requests;
		r = embed_with(embedder, 1000, 0.2);
		g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_assert_cmpuint(s.requests, ==, before);
		result_clear(&r);
	}
	service_stop(&s);
}

/* ---- the identifier ---- */

/* Enrolled voices are told apart, a stranger is nobody, and what is on disk
 * is private to its owner. */
static void
test_enroll_and_identify(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autofree gchar *file = NULL;
	GStatBuf st;
	Result r;
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	enroll(id, "Ada", 1000);
	enroll(id, "Sam", 2000);
	file = g_build_filename(s.dir, "Ada.json", NULL);
	g_assert_cmpint(g_stat(file, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 2);
	g_assert_true(ai_speaker_identifier_has_name(AI_SPEAKER_IDENTIFIER(id), "Ada"));
	g_assert_false(ai_speaker_identifier_has_name(AI_SPEAKER_IDENTIFIER(id), "Kim"));

	r = identify(id, 1100, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Ada");
	g_assert_cmpint(ai_speaker_match_get_kind(r.match), ==, AI_SPEAKER_MATCH_ENROLLED);
	g_assert_cmpfloat(ai_speaker_match_get_score(r.match), >, 0.9);
	g_assert_nonnull(ai_speaker_match_get_voiceprint(r.match));
	result_clear(&r);

	r = identify(id, 2000, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Sam");
	result_clear(&r);

	/* Nobody enrolled: no name, and not an error. */
	r = identify(id, 7, 1.0);
	g_assert_null(name_of(&r));
	g_assert_cmpint(ai_speaker_match_get_kind(r.match), ==, AI_SPEAKER_MATCH_UNKNOWN);
	g_assert_cmpfloat(ai_speaker_match_get_score(r.match), <, 0.5);
	result_clear(&r);
	service_stop(&s);
}
/* Voiceprints outlive the process; a file from another model, or one that is
 * not a voiceprint at all, costs itself and nothing else. */
static void
test_persisted_and_filtered(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) first = NULL, second = NULL;
	g_autofree gchar *other = NULL, *junk = NULL;
	Result r;
	service_start(&s);
	first = ai_voiceprint_identifier_new(s.url, s.dir);
	enroll(first, "Ada", 1000);
	other = g_build_filename(s.dir, "Old.json", NULL);
	g_assert_true(g_file_set_contents(
		other, "{\"name\":\"Old\",\"model\":\"another-model\",\"count\":1,"
			   "\"embedding\":[0.9949,0.0995,0,0]}",
		-1, NULL));
	junk = g_build_filename(s.dir, "Junk.json", NULL);
	g_assert_true(g_file_set_contents(junk, "{\"name\":7,\"embedding\":\"no\"}", -1, NULL));

	second = ai_voiceprint_identifier_new(s.url, s.dir);
	r = identify(second, 1100, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Ada");
	result_clear(&r);
	service_stop(&s);
}
/* Enrolling again averages into the voiceprint rather than replacing it, and
 * the closest other voice is reported for an enrolment warning. */
static void
test_reenroll_averages(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autofree gchar *file = NULL, *contents = NULL, *near = NULL;
	g_autoptr(AiVoiceprint) heard = NULL;
	gdouble score = 0;
	static const gfloat almost_a[] = {0.99f, 0.1f, 0, 0};
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	enroll(id, "Ada", 1000);
	enroll(id, "Ada", 1100);
	enroll(id, "Sam", 2000);
	file = g_build_filename(s.dir, "Ada.json", NULL);
	g_assert_true(g_file_get_contents(file, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "\"count\":2"));
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 2);
	heard = ai_voiceprint_new(NULL, "test-voices", almost_a, 4, NULL);
	near = ai_voiceprint_identifier_closest(id, heard, "Sam", &score);
	g_assert_cmpstr(near, ==, "Ada");
	g_assert_cmpfloat(score, >, 0.9);
	g_clear_pointer(&near, g_free);
	near = ai_voiceprint_identifier_closest(id, heard, "Ada", &score);
	g_assert_cmpstr(near, ==, "Sam");
	service_stop(&s);
}
/* Too short to tell is an error the caller can recognise, not "a stranger";
 * a name that would leave the directory or fake a speaker is refused before
 * any request. */
static void
test_short_and_bad_names(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autoptr(GBytes) pcm = voice(1000, 2.0);
	Result r;
	const gchar *bad[] = {"", "../escape", "a/b", ".hidden", "[Ada]", "Guest 1", "x\ty", NULL};
	guint i;
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	enroll(id, "Ada", 1000);
	r = identify(id, 1000, 0.2);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_assert_null(r.match);
	result_clear(&r);
	for (i = 0; bad[i] != NULL; i++) {
		guint before = s.requests;
		Result e = {0};
		ai_voiceprint_identifier_enroll_async(id, bad[i], pcm, NULL, enrolled, &e);
		wait_done(&e);
		g_assert_error(e.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_assert_cmpuint(s.requests, ==, before);
		result_clear(&e);
	}
	service_stop(&s);
}

/* Everyone is tracked, enrolled or not: a voice that matches nobody enrolled
 * becomes a numbered guest for as long as this identifier lives, and later
 * utterances in that voice are the same guest. An unknown voice too short to
 * start a guest on is "too short to tell", not a new person. */
static void
test_guests_tracked(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	Result r;
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	g_object_set(id, "track-guests", TRUE, NULL);
	enroll(id, "Ada", 1000);

	r = identify(id, 3000, 2.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 1");
	g_assert_cmpint(ai_speaker_match_get_kind(r.match), ==, AI_SPEAKER_MATCH_GUEST);
	g_assert_cmpuint(ai_speaker_match_get_guest_number(r.match), ==, 1);
	result_clear(&r);
	r = identify(id, 3100, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 1");
	result_clear(&r);
	r = identify(id, 7, 2.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 2");
	result_clear(&r);
	r = identify(id, 1100, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Ada");
	result_clear(&r);
	r = identify(id, 3000, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 1");
	result_clear(&r);
	/* A third stranger, but only a second of them: not enough to be sure. */
	{
		g_autoptr(AiVoiceprintIdentifier) fresh = ai_voiceprint_identifier_new(s.url, s.dir);
		g_object_set(fresh, "track-guests", TRUE, NULL);
		r = identify(fresh, 7, 1.0);
		g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		result_clear(&r);
	}
	/* Guests are never written to disk. */
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 1);
	{
		g_autoptr(GPtrArray) guests = ai_voiceprint_identifier_list_guests(id);
		g_assert_cmpuint(guests->len, ==, 2);
		g_assert_cmpstr(ai_voiceprint_get_name(g_ptr_array_index(guests, 0)), ==, "Guest 1");
	}
	ai_voiceprint_identifier_forget_guests(id);
	r = identify(id, 7, 2.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 1");
	result_clear(&r);
	service_stop(&s);
}
/* Guests are numbered in the order they were heard, not the order their
 * voiceprints came back: the first stranger's slow reply is still Guest 1. */
static void
test_guests_in_heard_order(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autoptr(GBytes) slow = voice(4800, 2.0);
	g_autoptr(GBytes) fast = voice(7, 2.0);
	Result first = {0}, second = {0};
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	g_object_set(id, "track-guests", TRUE, NULL);
	ai_speaker_identifier_identify_async(AI_SPEAKER_IDENTIFIER(id), slow, NULL, identified,
										 &first);
	ai_speaker_identifier_identify_async(AI_SPEAKER_IDENTIFIER(id), fast, NULL, identified,
										 &second);
	wait_done(&second);
	g_assert_true(first.done);
	g_assert_cmpstr(name_of(&first), ==, "Guest 1");
	g_assert_cmpstr(name_of(&second), ==, "Guest 2");
	result_clear(&first);
	result_clear(&second);
	service_stop(&s);
}
/* A cancelled identification leaves the queue at once: one stuck embedding
 * never holds the utterances after it, and a cancelled one grows no guest. */
static void
test_cancel_unblocks_queue(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autoptr(GBytes) stuck = voice(4500, 2.0);
	g_autoptr(GBytes) next = voice(3000, 2.0);
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	Result a = {0}, b = {0};
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	g_object_set(id, "track-guests", TRUE, NULL);
	ai_speaker_identifier_identify_async(AI_SPEAKER_IDENTIFIER(id), stuck, cancel, identified,
										 &a);
	ai_speaker_identifier_identify_async(AI_SPEAKER_IDENTIFIER(id), next, NULL, identified, &b);
	g_cancellable_cancel(cancel);
	wait_done(&a);
	g_assert_error(a.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	wait_done(&b);
	g_assert_cmpstr(name_of(&b), ==, "Guest 1");
	result_clear(&a);
	result_clear(&b);
	service_stop(&s);
}
/* At the guest cap a new stranger is unknown -- never folded into a guest it
 * does not sound like. */
static void
test_guest_cap(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	Result r;
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	g_object_set(id, "track-guests", TRUE, "max-guests", 1, NULL);
	r = identify(id, 3000, 2.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 1");
	result_clear(&r);
	r = identify(id, 7, 2.0);
	g_assert_null(name_of(&r));
	g_assert_cmpint(ai_speaker_match_get_kind(r.match), ==, AI_SPEAKER_MATCH_UNKNOWN);
	result_clear(&r);
	service_stop(&s);
}
static void
on_promoted(AiVoiceprintIdentifier *id, const gchar *guest, const gchar *name, gpointer data)
{
	*(gchar **)data = g_strdup_printf("%s=%s", guest, name);
}
/* A guest's voice can be saved under a name nobody has; a name already
 * enrolled is refused unless merging is asked for explicitly. */
static void
test_promote_guest(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;
	AiSpeakerIdentifier *si;
	Result r;
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	si = AI_SPEAKER_IDENTIFIER(id);
	g_object_set(id, "track-guests", TRUE, NULL);
	g_signal_connect(id, "promoted", G_CALLBACK(on_promoted), &seen);
	enroll(id, "Ada", 1000);
	r = identify(id, 3000, 2.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 1");
	result_clear(&r);
	r = identify(id, 7, 2.0);
	g_assert_cmpstr(name_of(&r), ==, "Guest 2");
	result_clear(&r);

	g_assert_false(ai_speaker_identifier_promote(si, "Guest 1", "Ada", FALSE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error(&error);
	g_assert_false(ai_speaker_identifier_promote(si, "Guest 9", "Kim", FALSE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(ai_speaker_identifier_promote(si, "Guest 1", "Guest 5", FALSE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_null(seen);

	g_assert_true(ai_speaker_identifier_promote(si, "guest 1", "Kim", FALSE, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(seen, ==, "Guest 1=Kim");
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 2);
	r = identify(id, 3100, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Kim");
	g_assert_cmpint(ai_speaker_match_get_kind(r.match), ==, AI_SPEAKER_MATCH_ENROLLED);
	result_clear(&r);
	/* Merging into an enrolled name is an explicit choice. */
	g_assert_true(ai_speaker_identifier_promote(si, "Guest 2", "Ada", TRUE, &error));
	g_assert_no_error(error);
	/* Numbers are not reused after a promotion. */
	r = identify(id, 4900, 2.0);
	g_assert_nonnull(r.match);
	result_clear(&r);
	service_stop(&s);
}

/* An application can change how voices compare without touching the rest. */
typedef struct {
	AiVoiceprintIdentifier parent;
} Lenient;
typedef struct {
	AiVoiceprintIdentifierClass parent_class;
} LenientClass;
G_DEFINE_TYPE(Lenient, lenient, AI_TYPE_VOICEPRINT_IDENTIFIER)
static gdouble
lenient_score(AiVoiceprintIdentifier *self, AiVoiceprint *known, AiVoiceprint *heard)
{
	/* Everyone sounds exactly like whoever is enrolled. */
	return 1.0;
}
static void
lenient_class_init(LenientClass *klass)
{
	AI_VOICEPRINT_IDENTIFIER_CLASS(klass)->score = lenient_score;
}
static void
lenient_init(Lenient *self)
{
}
static void
test_subclass_score(void)
{
	Service s = {0};
	g_autoptr(AiHttpSpeakerEmbedder) embedder = NULL;
	g_autoptr(AiMemoryVoiceprintStore) store = ai_memory_voiceprint_store_new();
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	Result r;
	service_start(&s);
	embedder = ai_http_speaker_embedder_new(s.url);
	id = g_object_new(lenient_get_type(), "embedder", embedder, "store", store, NULL);
	enroll(id, "Ada", 1000);
	r = identify(id, 2000, 1.0);
	g_assert_cmpstr(name_of(&r), ==, "Ada");
	result_clear(&r);
	service_stop(&s);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/speaker/voiceprint/validation", test_voiceprint_validation);
	g_test_add_func("/voice/speaker/voiceprint/json-and-math", test_voiceprint_json_and_math);
	g_test_add_func("/voice/speaker/voiceprint/names", test_voiceprint_names);
	g_test_add_func("/voice/speaker/store/memory", test_memory_store);
	g_test_add_func("/voice/speaker/store/file-contract", test_file_store_contract);
	g_test_add_func("/voice/speaker/store/file-hardening", test_file_store_hardening);
	g_test_add_func("/voice/speaker/store/file-monitor", test_file_store_monitor);
	g_test_add_func("/voice/speaker/http/failures", test_http_embedder_failures);
	g_test_add_func("/voice/speaker/enroll-and-identify", test_enroll_and_identify);
	g_test_add_func("/voice/speaker/persisted-and-filtered", test_persisted_and_filtered);
	g_test_add_func("/voice/speaker/reenroll-averages", test_reenroll_averages);
	g_test_add_func("/voice/speaker/short-and-bad-names", test_short_and_bad_names);
	g_test_add_func("/voice/speaker/guests-tracked", test_guests_tracked);
	g_test_add_func("/voice/speaker/guests-in-heard-order", test_guests_in_heard_order);
	g_test_add_func("/voice/speaker/cancel-unblocks-queue", test_cancel_unblocks_queue);
	g_test_add_func("/voice/speaker/guest-cap", test_guest_cap);
	g_test_add_func("/voice/speaker/promote-guest", test_promote_guest);
	g_test_add_func("/voice/speaker/subclass-score", test_subclass_score);
	return g_test_run();
}
