/*
 * test-ai-gui-session.c - The ai-gui session model and its store
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * These link gui/ai-gui-session.c and gui/ai-gui-session-store.c
 * directly, without GTK. That is possible because neither of them
 * includes a toolkit header, and it is the reason they do not: a suite
 * that needed a display would pass or fail by whose machine ran it,
 * exactly as one reading the developer's real ~/.claude would.
 *
 * HOME and the working directory are sandboxed here for the same
 * reason. The harness layer scans both.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include <ai-glib.h>

#include "ai-gui-session-store.h"
#include "ai-gui-util.h"
#include "ai-gui-work.h"

typedef struct
{
	gchar *home;
	gchar *sessions;
	gchar *registry;
	gchar *cwd;
	gchar *original_cwd;
} Fixture;

static AiGuiOptions *
fixture_options(Fixture *fixture)
{
	AiGuiOptions *options = g_new0(AiGuiOptions, 1);

	options->provider = g_strdup("ollama");
	options->working_directory = g_strdup(fixture->cwd);
	options->max_tokens = 2048;
	options->stream = TRUE;
	options->expand = TRUE;
	options->agents = FALSE;

	return options;
}

static void
fixture_set_up(
	Fixture       *fixture,
	gconstpointer  data
){
	fixture->original_cwd = g_get_current_dir();
	fixture->home = g_dir_make_tmp("ai-gui-test-XXXXXX", NULL);
	g_assert_nonnull(fixture->home);

	fixture->sessions = g_build_filename(fixture->home, "sessions", NULL);
	fixture->registry = g_build_filename(fixture->home, "registry", NULL);
	fixture->cwd = g_build_filename(fixture->home, "project", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->cwd, 0700), ==, 0);

	g_setenv("HOME", fixture->home, TRUE);
	g_setenv("XDG_DATA_HOME", fixture->home, TRUE);
	g_setenv("XDG_CONFIG_HOME", fixture->home, TRUE);
	g_setenv("XDG_STATE_HOME", fixture->home, TRUE);
	g_assert_cmpint(g_chdir(fixture->cwd), ==, 0);
}

static void
fixture_tear_down(
	Fixture       *fixture,
	gconstpointer  data
){
	if (fixture->original_cwd != NULL)
		g_chdir(fixture->original_cwd);

	g_free(fixture->original_cwd);
	g_free(fixture->home);
	g_free(fixture->sessions);
	g_free(fixture->registry);
	g_free(fixture->cwd);
}

/* ---------------------------------------------------------------- */

static void
test_session_basics(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiOptions) options = fixture_options(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiSession) session = NULL;

	session = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(session);

	g_assert_nonnull(ai_gui_session_get_id(session));
	g_assert_nonnull(ai_gui_session_get_conversation(session));
	g_assert_nonnull(ai_gui_session_get_transcript(session));
	g_assert_nonnull(ai_gui_session_get_completion(session));
	g_assert_nonnull(ai_gui_session_get_commands(session));
	g_assert_false(ai_gui_session_get_busy(session));
	g_assert_cmpuint(ai_gui_session_get_queued(session), ==, 0);
	g_assert_cmpstr(ai_gui_session_get_working_directory(session), ==,
	                fixture->cwd);

	/* A name somebody typed survives; the automatic one would not. */
	ai_gui_session_set_title(session, "Deploy notes");
	g_assert_cmpstr(ai_gui_session_get_title(session), ==, "Deploy notes");
}

/*
 * The saved provider must be the factory's name, not the display name.
 *
 * "Ollama" and "ollama" happen to both resolve, which is precisely why
 * this is asserted on a provider where they differ only in case: the
 * bug it guards against -- saving ai_provider_get_name() -- is invisible
 * until somebody opens a claude-code session and gets the default
 * provider back with no explanation.
 */
static void
test_session_provider_is_canonical(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiOptions) options = fixture_options(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiSession) session = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;

	session = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);

	node = ai_gui_session_to_json(session);
	object = json_node_get_object(node);

	g_assert_cmpstr(json_object_get_string_member(object, "provider"), ==,
	                "ollama");
	g_assert_cmpstr(json_object_get_string_member(object, "provider-display"),
	                ==, ai_gui_session_get_provider_name(session));
}

/*
 * A turn restored from disk must read exactly as it was typed.
 *
 * A turn renders as "> text", so persisting the rendering and restoring
 * it through ai_view_turn_block_new() gains one "> " per save-and-load
 * cycle -- a transcript that decays a little every time the application
 * is closed.
 */
static void
test_session_round_trip(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiOptions) options = fixture_options(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiSessionStore) store = NULL;
	g_autoptr(AiGuiSessionStore) reloaded = NULL;
	g_autoptr(AiGuiSession) session = NULL;
	AiGuiSession *restored;
	AiTranscript *transcript;
	AiViewBlock *block;
	g_autofree gchar *text = NULL;

	session = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);
	ai_gui_session_set_title(session, "Round trip");

	transcript = ai_gui_session_get_transcript(session);

	{
		g_autoptr(AiViewBlock) turn = ai_view_turn_block_new("hello there");
		g_autoptr(AiViewBlock) answer = ai_view_text_block_new();

		ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(answer), "general kenobi");
		ai_transcript_append(transcript, turn);
		ai_transcript_append(transcript, answer);
	}

	store = ai_gui_session_store_new(fixture->sessions);
	ai_gui_session_store_add(store, session);
	g_assert_cmpuint(ai_gui_session_store_save_all(store), ==, 1);

	reloaded = ai_gui_session_store_new(fixture->sessions);
	g_assert_cmpuint(ai_gui_session_store_load(reloaded, options), ==, 1);

	restored = ai_gui_session_store_get(reloaded, 0);
	g_assert_nonnull(restored);
	g_assert_cmpstr(ai_gui_session_get_title(restored), ==, "Round trip");
	g_assert_cmpstr(ai_gui_session_get_id(restored), ==,
	                ai_gui_session_get_id(session));

	transcript = ai_gui_session_get_transcript(restored);

	/* Two blocks plus the note saying this came off disk. */
	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 3);

	block = ai_transcript_get_block(transcript, 0);
	g_assert_cmpint(ai_view_block_get_kind(block), ==, AI_VIEW_BLOCK_TURN);
	g_assert_cmpstr(ai_view_turn_block_get_text(AI_VIEW_TURN_BLOCK(block)), ==,
	                "hello there");

	block = ai_transcript_get_block(transcript, 1);
	g_assert_cmpint(ai_view_block_get_kind(block), ==, AI_VIEW_BLOCK_TEXT);
	g_assert_cmpstr(ai_view_text_block_get_text(AI_VIEW_TEXT_BLOCK(block)), ==,
	                "general kenobi");

	/* Restored blocks never stream again. */
	g_assert_true(ai_view_block_get_complete(block));
}

/*
 * One unreadable file costs itself and nothing else.
 *
 * The same rule the harness layer applies to resource files: a session
 * truncated by a crash must not hide the sixteen that survived it, and
 * it must not abort a fatal-warnings run either.
 */
static void
test_store_skips_bad_files(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiOptions) options = fixture_options(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiSessionStore) store = NULL;
	g_autoptr(AiGuiSessionStore) reloaded = NULL;
	g_autoptr(AiGuiSession) session = NULL;
	g_autofree gchar *truncated = NULL;
	g_autofree gchar *not_an_object = NULL;
	g_autofree gchar *ignored = NULL;

	g_assert_cmpint(g_mkdir_with_parents(fixture->sessions, 0700), ==, 0);

	truncated = g_build_filename(fixture->sessions, "broken.json", NULL);
	g_assert_true(g_file_set_contents(truncated, "{\"id\": \"a\", ", -1, NULL));

	not_an_object = g_build_filename(fixture->sessions, "null.json", NULL);
	g_assert_true(g_file_set_contents(not_an_object, "null", -1, NULL));

	ignored = g_build_filename(fixture->sessions, "notes.txt", NULL);
	g_assert_true(g_file_set_contents(ignored, "not a session", -1, NULL));

	session = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);

	store = ai_gui_session_store_new(fixture->sessions);
	ai_gui_session_store_add(store, session);
	g_assert_true(ai_gui_session_store_save(store, session, &error));
	g_assert_no_error(error);

	reloaded = ai_gui_session_store_new(fixture->sessions);
	g_assert_cmpuint(ai_gui_session_store_load(reloaded, options), ==, 1);
}

static void
test_store_remove_deletes_the_file(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiOptions) options = fixture_options(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiSessionStore) store = NULL;
	g_autoptr(AiGuiSession) session = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *path = NULL;

	session = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);

	store = ai_gui_session_store_new(fixture->sessions);
	ai_gui_session_store_add(store, session);
	g_assert_true(ai_gui_session_store_save(store, session, &error));

	name = g_strconcat(ai_gui_session_get_id(session), ".json", NULL);
	path = g_build_filename(fixture->sessions, name, NULL);
	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

	g_assert_true(ai_gui_session_store_remove(store, session));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_cmpuint(ai_gui_session_store_get_n_sessions(store), ==, 0);
}

static void
test_store_is_a_list_model(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiOptions) options = fixture_options(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiSessionStore) store = ai_gui_session_store_new(fixture->sessions);
	g_autoptr(AiGuiSession) first = NULL;
	g_autoptr(AiGuiSession) second = NULL;
	g_autoptr(AiGuiSession) item = NULL;

	first = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);
	second = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);

	ai_gui_session_store_add(store, first);
	ai_gui_session_store_add(store, second);

	g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 2);
	g_assert_cmpint(g_list_model_get_item_type(G_LIST_MODEL(store)), ==,
	                AI_GUI_TYPE_SESSION);

	/* Newest first: the sidebar reads top to bottom. */
	item = g_list_model_get_item(G_LIST_MODEL(store), 0);
	g_assert_true(item == second);
}

/*
 * A session registers with the shared dashboard registry, and what it
 * publishes is what ai-tui's dashboard will read.
 *
 * Registration runs git in a worker, so the loop is pumped until the
 * record arrives rather than assumed present -- which is also the
 * contract ai_gui_session_get_work() states.
 */
static void
test_session_registers_with_the_dashboard(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiOptions) options = fixture_options(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiSession) session = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	AiWorkSession *work = NULL;
	gint64 deadline;

	session = ai_gui_session_new(options, "ollama", NULL, &error);
	g_assert_no_error(error);

	/*
	 * Set before the loop is ever iterated. The registration worker's
	 * completion callback is dispatched on the main context, so it
	 * cannot have run yet -- this is ordering, not a race.
	 */
	ai_gui_session_set_work_directory(session, fixture->registry);

	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while ((work = ai_gui_session_get_work(session)) == NULL &&
	       g_get_monotonic_time() < deadline)
	{
		g_main_context_iteration(NULL, FALSE);
	}

	g_assert_nonnull(work);

	ai_gui_session_publish_work(session);

	rows = ai_gui_work_list(fixture->registry, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 1);

	{
		AiWorkSession *row = g_ptr_array_index(rows, 0);

		/* The canonical provider name, as everywhere else: the factory
		 * wants `ollama`, never the display name. */
		g_assert_cmpstr(ai_work_session_get_field(row, "provider"), ==,
		                "ollama");
		g_assert_cmpstr(ai_work_session_get_field(row, "directory"), ==,
		                fixture->cwd);
		g_assert_cmpstr(ai_work_session_get_field(row, "status"), ==, "IDLE");
		g_assert_true(ai_gui_work_is_live(row));
	}

	/* A normal exit keeps the metadata and drops liveness, so the row
	 * stays visible and resumable rather than vanishing. */
	ai_gui_session_release_work(session);
	g_clear_pointer(&rows, g_ptr_array_unref);

	rows = ai_gui_work_list(fixture->registry, NULL);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_false(ai_gui_work_is_live(g_ptr_array_index(rows, 0)));
}

/* ---------------------------------------------------------------- */

static void
test_value_from_string(void)
{
	g_autoptr(AiOllamaClient) client = ai_ollama_client_new();
	GParamSpec *pspec;
	GValue value = G_VALUE_INIT;

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(client), "model");
	g_assert_nonnull(pspec);
	g_assert_true(ai_gui_value_from_string(&value, pspec, "llama3.2"));
	g_assert_cmpstr(g_value_get_string(&value), ==, "llama3.2");
	g_value_unset(&value);

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(client),
	                                     "max-tokens");
	g_assert_nonnull(pspec);
	g_assert_true(ai_gui_value_from_string(&value, pspec, "512"));
	g_assert_cmpint(g_value_get_int(&value), ==, 512);
	g_value_unset(&value);

	/* Trailing rubbish is a refusal, not a silently truncated number. */
	g_assert_false(ai_gui_value_from_string(&value, pspec, "512x"));
	g_value_unset(&value);
}

/*
 * Elided on characters, not bytes.
 *
 * A title cut mid-sequence is invalid UTF-8, which GTK renders as a run
 * of replacement glyphs rather than the words somebody typed.
 */
static void
test_summarise_prompt(void)
{
	g_autofree gchar *empty = ai_gui_summarise_prompt(NULL);
	g_autofree gchar *blank = ai_gui_summarise_prompt("\n\n   \n");
	g_autofree gchar *first = ai_gui_summarise_prompt("  first line\nsecond");
	g_autofree gchar *wide = NULL;
	g_autofree gchar *long_utf8 = NULL;
	gsize i;
	GString *builder = g_string_new(NULL);

	g_assert_cmpstr(empty, ==, "New session");
	g_assert_cmpstr(blank, ==, "New session");
	g_assert_cmpstr(first, ==, "first line");

	for (i = 0; i < 100; i++)
		g_string_append(builder, "\xe6\xbc\xa2");

	long_utf8 = g_string_free(builder, FALSE);
	wide = ai_gui_summarise_prompt(long_utf8);

	g_assert_true(g_utf8_validate(wide, -1, NULL));
	g_assert_cmpint(g_utf8_strlen(wide, -1), ==, 61);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/ai-gui/session/basics", Fixture, NULL,
	           fixture_set_up, test_session_basics, fixture_tear_down);
	g_test_add("/ai-gui/session/provider-is-canonical", Fixture, NULL,
	           fixture_set_up, test_session_provider_is_canonical,
	           fixture_tear_down);
	g_test_add("/ai-gui/session/round-trip", Fixture, NULL,
	           fixture_set_up, test_session_round_trip, fixture_tear_down);
	g_test_add("/ai-gui/store/skips-bad-files", Fixture, NULL,
	           fixture_set_up, test_store_skips_bad_files, fixture_tear_down);
	g_test_add("/ai-gui/store/remove-deletes-the-file", Fixture, NULL,
	           fixture_set_up, test_store_remove_deletes_the_file,
	           fixture_tear_down);
	g_test_add("/ai-gui/store/list-model", Fixture, NULL,
	           fixture_set_up, test_store_is_a_list_model, fixture_tear_down);
	g_test_add("/ai-gui/session/registers-with-the-dashboard", Fixture, NULL,
	           fixture_set_up, test_session_registers_with_the_dashboard,
	           fixture_tear_down);

	g_test_add_func("/ai-gui/util/value-from-string", test_value_from_string);
	g_test_add_func("/ai-gui/util/summarise-prompt", test_summarise_prompt);

	return g_test_run();
}
