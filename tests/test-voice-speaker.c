/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

/* A voiceprint service stand-in. The first sample of the PCM picks the
 * voice, so a test can say "this is A" without real audio: 1000 is A, 2000
 * is B, 1100 is A on a different day, anything else a stranger. Less than
 * half a second is refused, as the real service does. */
typedef struct {
	SoupServer *server;
	gchar *url;
	gchar *dir;
	const gchar *model;
	guint requests;
} Service;

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
		const gchar *err = "{\"error\":\"need at least 0.5 s of S16LE audio\"}";
		soup_server_message_set_status(msg, 422, NULL);
		soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, err,
										 strlen(err));
		return;
	}
	memcpy(&first, body->data, 2);
	vector = first == 1000	 ? "[1,0,0,0]"
			 : first == 2000 ? "[0,1,0,0]"
			 : first == 1100 ? "[0.9949,0.0995,0,0]"
			 : first == 3000 ? "[0,0,1,0]"
			 : first == 3100 ? "[0,0,0.9949,0.0995]"
							 : "[0,0,0,1]";
	reply = g_strdup_printf("{\"model\":\"%s\",\"dim\":4,\"embedding\":%s}", s->model, vector);
	soup_server_message_set_status(msg, 200, NULL);
	soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, reply,
									 strlen(reply));
}
static void
service_start(Service *s)
{
	GSList *uris;
	g_autofree gchar *base = NULL;
	s->model = "test-voices";
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
service_stop(Service *s)
{
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
	gchar *name;
	gdouble score;
	GError *error;
} Result;
static void
enrolled(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	ai_voiceprint_identifier_enroll_finish(AI_VOICEPRINT_IDENTIFIER(source), result, &r->error);
	r->done = TRUE;
}
static void
identified(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->name = ai_speaker_identifier_identify_finish(AI_SPEAKER_IDENTIFIER(source), result,
													&r->score, &r->error);
	r->done = TRUE;
}
static void
wait_done(Result *r)
{
	gint64 limit = g_get_monotonic_time() + 5000000;
	while (!r->done && g_get_monotonic_time() < limit)
		g_main_context_iteration(NULL, TRUE);
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

	r = identify(id, 1100, 1.0);
	g_assert_no_error(r.error);
	g_assert_cmpstr(r.name, ==, "Ada");
	g_assert_cmpfloat(r.score, >, 0.9);
	g_free(r.name);

	r = identify(id, 2000, 1.0);
	g_assert_cmpstr(r.name, ==, "Sam");
	g_free(r.name);

	/* Nobody enrolled: no name, and not an error. */
	r = identify(id, 7, 1.0);
	g_assert_no_error(r.error);
	g_assert_null(r.name);
	g_assert_cmpfloat(r.score, <, 0.5);
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
	g_assert_no_error(r.error);
	g_assert_cmpstr(r.name, ==, "Ada");
	g_free(r.name);
	service_stop(&s);
}
/* Enrolling again averages into the voiceprint rather than replacing it. */
static void
test_reenroll_averages(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autofree gchar *file = NULL, *contents = NULL;
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	enroll(id, "Ada", 1000);
	enroll(id, "Ada", 1100);
	file = g_build_filename(s.dir, "Ada.json", NULL);
	g_assert_true(g_file_get_contents(file, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "\"count\":2"));
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 1);
	service_stop(&s);
}
/* Too short to tell is an error the caller can recognise, not "a stranger";
 * a name that would leave the directory is refused before any request. */
static void
test_short_and_bad_names(void)
{
	Service s = {0};
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autoptr(GBytes) pcm = voice(1000, 2.0);
	Result r;
	const gchar *bad[] = {"", "../escape", "a/b", ".hidden", NULL};
	guint i;
	service_start(&s);
	id = ai_voiceprint_identifier_new(s.url, s.dir);
	enroll(id, "Ada", 1000);
	r = identify(id, 1000, 0.2);
	g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_assert_null(r.name);
	g_clear_error(&r.error);
	for (i = 0; bad[i] != NULL; i++) {
		guint before = s.requests;
		Result e = {0};
		ai_voiceprint_identifier_enroll_async(id, bad[i], pcm, NULL, enrolled, &e);
		wait_done(&e);
		g_assert_error(e.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_assert_cmpuint(s.requests, ==, before);
		g_clear_error(&e.error);
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
	g_assert_no_error(r.error);
	g_assert_cmpstr(r.name, ==, "Guest 1");
	g_free(r.name);
	r = identify(id, 3100, 1.0);
	g_assert_cmpstr(r.name, ==, "Guest 1");
	g_free(r.name);
	r = identify(id, 7, 2.0);
	g_assert_cmpstr(r.name, ==, "Guest 2");
	g_free(r.name);
	r = identify(id, 1100, 1.0);
	g_assert_cmpstr(r.name, ==, "Ada");
	g_free(r.name);
	r = identify(id, 3000, 1.0);
	g_assert_cmpstr(r.name, ==, "Guest 1");
	g_free(r.name);
	/* A third stranger, but only a second of them: not enough to be sure. */
	{
		g_autoptr(AiVoiceprintIdentifier) fresh = ai_voiceprint_identifier_new(s.url, s.dir);
		g_object_set(fresh, "track-guests", TRUE, NULL);
		r = identify(fresh, 7, 1.0);
		g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_assert_null(r.name);
		g_clear_error(&r.error);
	}
	/* Guests are never written to disk. */
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 1);
	service_stop(&s);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/speaker/enroll-and-identify", test_enroll_and_identify);
	g_test_add_func("/voice/speaker/persisted-and-filtered", test_persisted_and_filtered);
	g_test_add_func("/voice/speaker/reenroll-averages", test_reenroll_averages);
	g_test_add_func("/voice/speaker/short-and-bad-names", test_short_and_bad_names);
	g_test_add_func("/voice/speaker/guests-tracked", test_guests_tracked);
	return g_test_run();
}
