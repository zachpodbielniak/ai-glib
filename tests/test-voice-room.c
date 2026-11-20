/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <string.h>

/*
 * The pieces of room mode on their own: what a room remembers and how a
 * turn is written (AiVoiceRoom), who is talking to the assistant
 * (AiNameAddressDetector), and how a relevance answer is read and a judge
 * that keeps failing backs off (AiProviderRelevanceJudge).
 */

/* ---- AiVoiceRoom ---- */

/* A turn is what was heard, then the line that made it; the room then
 * starts over, and the revision moves on every change. */
static void
test_room_turn(void)
{
	g_autoptr(AiVoiceRoom) room = ai_voice_room_new();
	g_autofree gchar *turn = NULL;
	guint64 before;
	ai_voice_room_hear(room, "Sam", "We ship Friday.");
	ai_voice_room_hear(room, "Kim", "Friday is tight.");
	g_assert_cmpuint(ai_voice_room_get_n_lines(room), ==, 2);
	before = ai_voice_room_get_revision(room);
	turn = ai_voice_room_take_turn(room, "Sam", "Ada, what do you think?");
	g_assert_cmpstr(turn, ==,
					"[Sam]: We ship Friday.\n[Kim]: Friday is tight.\n[Sam]: Ada, what do you think?");
	g_assert_cmpuint(ai_voice_room_get_n_lines(room), ==, 0);
	g_assert_cmpuint(ai_voice_room_get_revision(room), !=, before);
	g_clear_pointer(&turn, g_free);
	/* A note of the session's own is not attributed. */
	turn = ai_voice_room_take_turn(room, NULL, "[Note]: chiming in");
	g_assert_cmpstr(turn, ==, "[Note]: chiming in");
}
/* Lines, characters and age each bound what is kept; oldest goes first. */
static void
test_room_limits(void)
{
	g_autoptr(AiVoiceRoom) room = g_object_new(AI_TYPE_VOICE_ROOM, "max-lines", 2, NULL);
	g_auto(GStrv) lines = NULL;
	ai_voice_room_hear(room, "A", "one");
	ai_voice_room_hear(room, "B", "two");
	ai_voice_room_hear(room, "C", "three");
	lines = ai_voice_room_get_lines(room);
	g_assert_cmpuint(g_strv_length(lines), ==, 2);
	g_assert_cmpstr(lines[0], ==, "[B]: two");
	g_clear_pointer(&lines, g_strfreev);

	g_object_set(room, "max-lines", 40, "max-chars", 8, NULL);
	ai_voice_room_clear(room);
	ai_voice_room_hear(room, "A", "12345");
	ai_voice_room_hear(room, "B", "67890");
	lines = ai_voice_room_get_lines(room);
	g_assert_cmpuint(g_strv_length(lines), ==, 1);
	g_assert_cmpstr(lines[0], ==, "[B]: 67890");
	g_clear_pointer(&lines, g_strfreev);

	g_object_set(room, "max-chars", 0, "max-age-ms", 30, NULL);
	ai_voice_room_clear(room);
	ai_voice_room_hear(room, "A", "old news");
	g_usleep(60000);
	ai_voice_room_hear(room, "B", "fresh");
	lines = ai_voice_room_get_lines(room);
	g_assert_cmpuint(g_strv_length(lines), ==, 1);
	g_assert_cmpstr(lines[0], ==, "[B]: fresh");
}
/* The roster says who has spoken lately, addressed or not, and follows a
 * guest who was given a real name. */
static void
test_room_roster(void)
{
	g_autoptr(AiVoiceRoom) room = g_object_new(AI_TYPE_VOICE_ROOM, "announce-roster", TRUE, NULL);
	g_autofree gchar *turn = NULL;
	g_auto(GStrv) present = NULL;
	ai_voice_room_hear(room, "Guest 1", "Hello.");
	ai_voice_room_note_speaker(room, "Kim");
	ai_voice_room_rename_speaker(room, "Guest 1", "Lee");
	turn = ai_voice_room_take_turn(room, "Sam", "Ada, who is here?");
	g_assert_cmpstr(turn, ==, "[Present: Lee, Kim, Sam]\n[Lee]: Hello.\n[Sam]: Ada, who is here?");
	g_object_set(room, "roster-window-ms", 1, NULL);
	g_usleep(5000);
	present = ai_voice_room_get_present(room);
	g_assert_cmpuint(g_strv_length(present), ==, 0);
}
/* How talk reaches the model is the room's to decide; a subclass may write
 * it however it likes. */
typedef struct {
	AiVoiceRoom parent;
} Plain;
typedef struct {
	AiVoiceRoomClass parent_class;
} PlainClass;
G_DEFINE_TYPE(Plain, plain, AI_TYPE_VOICE_ROOM)
static gchar *
plain_format_line(AiVoiceRoom *self, const gchar *speaker, const gchar *text)
{
	return g_strdup_printf("%s said: %s", speaker, text);
}
static void
plain_class_init(PlainClass *klass)
{
	AI_VOICE_ROOM_CLASS(klass)->format_line = plain_format_line;
}
static void
plain_init(Plain *self)
{
}
static void
test_room_subclass(void)
{
	g_autoptr(AiVoiceRoom) room = g_object_new(plain_get_type(), NULL);
	g_autofree gchar *turn = NULL;
	ai_voice_room_hear(room, "Sam", "hi");
	turn = ai_voice_room_take_turn(room, "Kim", "Ada?");
	g_assert_cmpstr(turn, ==, "Sam said: hi\nKim said: Ada?");
}

/* ---- AiNameAddressDetector ---- */

static void
test_name_detector(void)
{
	g_autoptr(AiNameAddressDetector) d = ai_name_address_detector_new("Ada, Kay", "say");
	AiAddressDetector *a = AI_ADDRESS_DETECTOR(d);
	g_autoptr(AiNameAddressDetector) open = ai_name_address_detector_new(NULL, NULL);
	g_assert_true(ai_address_detector_is_addressed(a, "Sam", "Ada, what do you think?"));
	g_assert_true(ai_address_detector_is_addressed(a, "Sam", "what does ada think"));
	g_assert_true(ai_address_detector_is_addressed(a, "Sam", "Hey Kay."));
	g_assert_false(ai_address_detector_is_addressed(a, "Sam", "Adams is late"));
	/* A sound-alike counts only in a name's place. */
	g_assert_true(ai_address_detector_is_addressed(a, "Sam", "What do you think, say?"));
	g_assert_true(ai_address_detector_is_addressed(a, "Sam", "Hi, say."));
	g_assert_true(ai_address_detector_is_addressed(a, "Sam", "Say, could you help?"));
	g_assert_false(ai_address_detector_is_addressed(a, "Sam", "I say we ship it"));
	g_assert_false(ai_address_detector_is_addressed(a, "Sam", "Say that again"));
	/* No names: everything is for the assistant. */
	g_assert_true(ai_address_detector_is_addressed(AI_ADDRESS_DETECTOR(open), "Sam", "anything"));
}

/* ---- relevance ---- */

/* Every shape a model answers in is read; no score is a zero. */
static void
test_parse_reply(void)
{
	struct {
		const gchar *text;
		guint score;
		const gchar *reason;
	} cases[] = {
		{"SCORE: 8\nREASON: a question went unanswered", 8, "a question went unanswered"},
		{"**Score**: 7/10\n**Reason**: wrong date", 7, "wrong date"},
		{"I would give it a score of 6 out of 10.", 6, ""},
		{"```json\n{\"score\": 9, \"reason\": \"stuck\"}\n```", 9, "stuck"},
		{"{\"score\": 42}", 10, ""},
		{"SCORE: 15", 10, ""},
		{"no idea", 0, ""},
		{"", 0, ""},
		{NULL, 0, ""},
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *reason = NULL;
		guint score = ai_relevance_judge_parse_reply(cases[i].text, &reason);
		g_assert_cmpuint(score, ==, cases[i].score);
		g_assert_cmpstr(reason, ==, cases[i].reason);
	}
}
typedef struct {
	gboolean done;
	gint score;
	gchar *reason;
	GError *error;
} Judged;
static void
judged(GObject *source, GAsyncResult *result, gpointer data)
{
	Judged *j = data;
	j->score = ai_relevance_judge_judge_finish(AI_RELEVANCE_JUDGE(source), result, &j->reason,
											   &j->error);
	j->done = TRUE;
}
static Judged
judge(AiRelevanceJudge *judge)
{
	static const gchar *const lines[] = {"[Sam]: When is the release?", NULL};
	Judged j = {0};
	gint64 limit = g_get_monotonic_time() + 3000000;
	ai_relevance_judge_judge_async(judge, "Ada", lines, NULL, judged, &j);
	while (!j.done && g_get_monotonic_time() < limit) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	g_assert_true(j.done);
	return j;
}
static void
on_failed(AiProviderRelevanceJudge *judge, const GError *error, gpointer data)
{
	(*(guint *)data)++;
}
/* The judge asks with the assistant's name and the talk; a run of failures
 * pauses it instead of costing one request per line forever. */
static void
test_provider_judge(void)
{
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(AiProviderRelevanceJudge) j =
		ai_provider_relevance_judge_new(AI_PROVIDER(provider));
	guint failures = 0;
	Judged r;
	g_signal_connect(j, "failed", G_CALLBACK(on_failed), &failures);
	ai_mock_provider_push_text(provider, "SCORE: 7\nREASON: unanswered");
	r = judge(AI_RELEVANCE_JUDGE(j));
	g_assert_no_error(r.error);
	g_assert_cmpint(r.score, ==, 7);
	g_assert_cmpstr(r.reason, ==, "unanswered");
	g_assert_nonnull(strstr(ai_mock_provider_get_last_system_prompt(provider), "named Ada"));
	g_free(r.reason);

	g_object_set(j, "failure-limit", 2, "backoff-ms", 60000, NULL);
	ai_mock_provider_push_error(provider, "key revoked");
	r = judge(AI_RELEVANCE_JUDGE(j));
	g_assert_nonnull(r.error);
	g_assert_cmpint(r.score, ==, -1);
	g_clear_error(&r.error);
	g_free(r.reason);
	ai_mock_provider_push_error(provider, "key revoked");
	r = judge(AI_RELEVANCE_JUDGE(j));
	g_clear_error(&r.error);
	g_free(r.reason);
	g_assert_cmpuint(failures, ==, 2);
	/* Paused: the provider is not asked. */
	{
		guint calls = ai_mock_provider_get_call_count(provider);
		r = judge(AI_RELEVANCE_JUDGE(j));
		g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_BUSY);
		g_assert_cmpuint(ai_mock_provider_get_call_count(provider), ==, calls);
		g_clear_error(&r.error);
		g_free(r.reason);
	}
	/* A new provider starts afresh. */
	g_object_set(j, "provider", provider, NULL);
	ai_mock_provider_push_text(provider, "{\"score\": 3, \"reason\": \"chat\"}");
	r = judge(AI_RELEVANCE_JUDGE(j));
	g_assert_no_error(r.error);
	g_assert_cmpint(r.score, ==, 3);
	g_free(r.reason);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/room/turn", test_room_turn);
	g_test_add_func("/voice/room/limits", test_room_limits);
	g_test_add_func("/voice/room/roster", test_room_roster);
	g_test_add_func("/voice/room/subclass", test_room_subclass);
	g_test_add_func("/voice/room/name-detector", test_name_detector);
	g_test_add_func("/voice/room/parse-reply", test_parse_reply);
	g_test_add_func("/voice/room/provider-judge", test_provider_judge);
	return g_test_run();
}
