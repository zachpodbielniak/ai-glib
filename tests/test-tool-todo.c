/*
 * test-tool-todo.c - The todo_write tool
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Everything this tool reads is model output, which puts it in the same
 * category as subprocess stdout: untrusted, and not allowed to abort a run
 * with G_DEBUG=fatal-warnings on. So the malformed cases below are not
 * defensive padding -- a json-glib critical on a type mismatch would kill
 * the process, and a model writing "done" instead of "completed" must cost
 * nothing at all.
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

/* Run todo_write with a raw JSON parameter object. */
static gchar *
call_todo_write(
	AiToolExecutor  *executor,
	const gchar     *json,
	GError         **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(AiToolUse)  use = NULL;
	GError               *parse_error = NULL;

	g_assert_true(json_parser_load_from_data(parser, json, -1, &parse_error));
	g_assert_no_error(parse_error);

	use = ai_tool_use_new("id-1", "todo_write",
	                      json_parser_get_root(parser));

	return ai_tool_executor_execute(executor, use, NULL, error);
}

typedef struct
{
	guint changed;
} Watcher;

static void
on_todos_changed(AiToolExecutor *executor, gpointer user_data)
{
	Watcher *w = user_data;

	w->changed++;
}

/* ----------------------------------------------------------------
 * The ordinary path
 * ---------------------------------------------------------------- */

static void
test_writes_a_list(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	const gchar              *label = NULL;
	AiTodoState               state = AI_TODO_COMPLETED;
	GError                   *error = NULL;

	result = call_todo_write(executor,
		"{\"todos\":["
		"{\"content\":\"Add the parser\",\"active_form\":\"Adding the parser\","
		"\"status\":\"pending\"},"
		"{\"content\":\"Wire the registry\","
		"\"active_form\":\"Wiring the registry\","
		"\"status\":\"in_progress\"},"
		"{\"content\":\"Read the files\",\"status\":\"completed\"}"
		"]}", &error);

	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 3);

	/* Order is the model's, and is preserved. */
	g_assert_true(ai_tool_executor_get_todo_fields(executor, 0, &label,
	                                               &state));
	g_assert_cmpstr(label, ==, "Add the parser");
	g_assert_cmpint(state, ==, AI_TODO_PENDING);

	/* The in-progress item reads in its active phrasing, and the rule
	 * lives here rather than in every renderer. */
	g_assert_true(ai_tool_executor_get_todo_fields(executor, 1, &label,
	                                               &state));
	g_assert_cmpstr(label, ==, "Wiring the registry");
	g_assert_cmpint(state, ==, AI_TODO_IN_PROGRESS);

	g_assert_true(ai_tool_executor_get_todo_fields(executor, 2, &label,
	                                               &state));
	g_assert_cmpstr(label, ==, "Read the files");
	g_assert_cmpint(state, ==, AI_TODO_COMPLETED);

	/* The model gets the list back, so it can see what it just said. */
	g_assert_nonnull(strstr(result, "Add the parser"));
	g_assert_nonnull(strstr(result, "[in_progress]"));
}

static void
test_replaces_rather_than_merges(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *first = NULL;
	g_autofree gchar         *second = NULL;
	const gchar              *label = NULL;

	first = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"One\",\"status\":\"pending\"},"
		"{\"content\":\"Two\",\"status\":\"pending\"}]}", NULL);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 2);

	second = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"Three\",\"status\":\"pending\"}]}", NULL);

	/*
	 * The model resends the whole list every time, which is what keeps
	 * this honest: there is no partial-update protocol to get wrong and
	 * no way for the two sides to drift apart.
	 */
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 1);
	g_assert_true(ai_tool_executor_get_todo_fields(executor, 0, &label, NULL));
	g_assert_cmpstr(label, ==, "Three");
	g_assert_nonnull(second);
}

static void
test_empty_list_clears(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *first = NULL;
	g_autofree gchar         *second = NULL;

	first = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"One\",\"status\":\"pending\"}]}", NULL);
	second = call_todo_write(executor, "{\"todos\":[]}", NULL);

	g_assert_nonnull(first);
	g_assert_nonnull(second);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 0);
	g_assert_nonnull(strstr(second, "0 items"));
}

static void
test_changed_fires_once_per_call(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	Watcher                   w = { 0 };

	g_signal_connect(executor, "todos-changed",
	                 G_CALLBACK(on_todos_changed), &w);

	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"a\",\"status\":\"pending\"},"
		"{\"content\":\"b\",\"status\":\"pending\"},"
		"{\"content\":\"c\",\"status\":\"pending\"}]}", NULL);

	/* Three items, one notification: a frontend redrawing the list wants
	 * one redraw per call. */
	g_assert_nonnull(result);
	g_assert_cmpuint(w.changed, ==, 1);
}

static void
test_clear_todos(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	Watcher                   w = { 0 };

	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"a\",\"status\":\"pending\"}]}", NULL);
	g_assert_nonnull(result);

	g_signal_connect(executor, "todos-changed",
	                 G_CALLBACK(on_todos_changed), &w);

	ai_tool_executor_clear_todos(executor);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 0);
	g_assert_cmpuint(w.changed, ==, 1);

	/* Clearing an empty list is a no-op, not a second notification. */
	ai_tool_executor_clear_todos(executor);
	g_assert_cmpuint(w.changed, ==, 1);
}

/* ----------------------------------------------------------------
 * Model output that is not what the schema asked for
 * ---------------------------------------------------------------- */

static void
test_missing_todos_parameter(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	GError                   *error = NULL;
	gchar                    *result;

	result = call_todo_write(executor, "{\"other\":1}", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
}

static void
test_todos_wrong_type(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	GError                   *error = NULL;
	gchar                    *result;

	/* A string where an array was asked for. Reported, never a critical:
	 * under fatal warnings a critical would kill the process. */
	result = call_todo_write(executor, "{\"todos\":\"not an array\"}", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_entries_that_are_not_objects_are_skipped(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	GError                   *error = NULL;

	result = call_todo_write(executor,
		"{\"todos\":[\"a string\",42,null,"
		"{\"content\":\"real\",\"status\":\"pending\"}]}", &error);

	/* One malformed entry costs itself, not the list. */
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 1);
}

static void
test_entry_without_content_is_skipped(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;

	result = call_todo_write(executor,
		"{\"todos\":[{\"status\":\"pending\"},"
		"{\"content\":\"real\",\"status\":\"pending\"}]}", NULL);

	g_assert_nonnull(result);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 1);
}

static void
test_content_of_the_wrong_type_is_skipped(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;

	/* The case that would be a json-glib critical if the member were
	 * read through get_string_member_with_default(). */
	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":7,\"status\":\"pending\"},"
		"{\"content\":\"real\",\"status\":\"pending\"}]}", NULL);

	g_assert_nonnull(result);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 1);
}

static void
test_unknown_status_is_pending(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	AiTodoState               state = AI_TODO_COMPLETED;

	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"x\",\"status\":\"whatever\"}]}", NULL);

	/* A run must not stop because a model invented a status word. */
	g_assert_nonnull(result);
	g_assert_true(ai_tool_executor_get_todo_fields(executor, 0, NULL, &state));
	g_assert_cmpint(state, ==, AI_TODO_PENDING);
}

static void
test_status_synonyms(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	AiTodoState               state = AI_TODO_PENDING;

	/* "done" and "in-progress" are what models actually write when they
	 * have not read the schema carefully. */
	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"a\",\"status\":\"done\"},"
		"{\"content\":\"b\",\"status\":\"in-progress\"}]}", NULL);

	g_assert_nonnull(result);
	g_assert_true(ai_tool_executor_get_todo_fields(executor, 0, NULL, &state));
	g_assert_cmpint(state, ==, AI_TODO_COMPLETED);
	g_assert_true(ai_tool_executor_get_todo_fields(executor, 1, NULL, &state));
	g_assert_cmpint(state, ==, AI_TODO_IN_PROGRESS);
}

static void
test_camel_case_active_form(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	const gchar              *label = NULL;

	/* claude-code spells it activeForm. A model that has seen either
	 * spelling should get the same behaviour. */
	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"Add it\",\"activeForm\":\"Adding it\","
		"\"status\":\"in_progress\"}]}", NULL);

	g_assert_nonnull(result);
	g_assert_true(ai_tool_executor_get_todo_fields(executor, 0, &label, NULL));
	g_assert_cmpstr(label, ==, "Adding it");
}

static void
test_two_in_progress_is_allowed_with_a_message(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;

	/*
	 * A model doing two things at once is a model being imprecise, not
	 * this program being broken. g_message, so it is said out loud
	 * without aborting a fatal-warnings run -- and this test would fail
	 * loudly if it were ever raised to g_warning.
	 */
	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
	                      "*2 items marked in progress*");

	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"a\",\"status\":\"in_progress\"},"
		"{\"content\":\"b\",\"status\":\"in_progress\"}]}", NULL);

	g_test_assert_expected_messages();

	g_assert_nonnull(result);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 2);
}

/* ----------------------------------------------------------------
 * Accessors
 * ---------------------------------------------------------------- */

static void
test_accessor_bounds(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	const gchar              *label = (const gchar *)0x1;

	result = call_todo_write(executor,
		"{\"todos\":[{\"content\":\"only\",\"status\":\"pending\"}]}", NULL);
	g_assert_nonnull(result);

	g_assert_null(ai_tool_executor_get_todo(executor, 1));
	g_assert_nonnull(ai_tool_executor_get_todo(executor, 0));

	/* Out of range leaves the outputs untouched. */
	g_assert_false(ai_tool_executor_get_todo_fields(executor, 99, &label,
	                                                NULL));
	g_assert_true(label == (const gchar *)0x1);

	g_assert_true(ai_tool_executor_get_todo_fields(executor, 0, NULL, NULL));

	g_assert_cmpuint(ai_tool_executor_get_todos(executor)->len, ==, 1);
}

static void
test_todo_boxed_type(void)
{
	g_autoptr(AiTodo) todo = ai_todo_new("Do it", "Doing it",
	                                     AI_TODO_IN_PROGRESS);
	g_autoptr(AiTodo) copy = ai_todo_copy(todo);

	g_assert_cmpstr(copy->content, ==, "Do it");
	g_assert_cmpstr(copy->active_form, ==, "Doing it");
	g_assert_cmpint(copy->state, ==, AI_TODO_IN_PROGRESS);
	g_assert_cmpstr(ai_todo_get_label(copy), ==, "Doing it");

	g_assert_null(ai_todo_copy(NULL));
	ai_todo_free(NULL);
	g_assert_true(AI_TYPE_TODO != G_TYPE_INVALID);
}

static void
test_label_falls_back_to_content(void)
{
	g_autoptr(AiTodo) no_active = ai_todo_new("Do it", NULL,
	                                          AI_TODO_IN_PROGRESS);
	g_autoptr(AiTodo) empty_active = ai_todo_new("Do it", "",
	                                             AI_TODO_IN_PROGRESS);
	g_autoptr(AiTodo) pending = ai_todo_new("Do it", "Doing it",
	                                        AI_TODO_PENDING);

	/* A model that omits the active form still renders. */
	g_assert_cmpstr(ai_todo_get_label(no_active), ==, "Do it");
	g_assert_cmpstr(ai_todo_get_label(empty_active), ==, "Do it");

	/* And the active phrasing only applies while it is active. */
	g_assert_cmpstr(ai_todo_get_label(pending), ==, "Do it");
}

static void
test_state_names_round_trip(void)
{
	g_assert_cmpstr(ai_todo_state_to_string(AI_TODO_PENDING), ==, "pending");
	g_assert_cmpstr(ai_todo_state_to_string(AI_TODO_IN_PROGRESS), ==,
	                "in_progress");
	g_assert_cmpstr(ai_todo_state_to_string(AI_TODO_COMPLETED), ==,
	                "completed");
	g_assert_nonnull(ai_todo_state_to_string((AiTodoState)99));

	g_assert_cmpint(ai_todo_state_from_string("pending"), ==, AI_TODO_PENDING);
	g_assert_cmpint(ai_todo_state_from_string("in_progress"), ==,
	                AI_TODO_IN_PROGRESS);
	g_assert_cmpint(ai_todo_state_from_string("completed"), ==,
	                AI_TODO_COMPLETED);
	g_assert_cmpint(ai_todo_state_from_string(NULL), ==, AI_TODO_PENDING);
}

static void
test_tool_is_advertised_with_a_schema(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	GList                    *iter;
	AiTool                   *todo_tool = NULL;

	for (iter = ai_tool_executor_get_tools(executor); iter != NULL;
	     iter = iter->next)
	{
		if (g_strcmp0(ai_tool_get_name(iter->data), "todo_write") == 0)
		{
			todo_tool = iter->data;
			break;
		}
	}

	g_assert_nonnull(todo_tool);

	{
		g_autoptr(JsonNode) schema =
			ai_tool_get_parameters_json(todo_tool);
		g_autofree gchar   *text = json_to_string(schema, FALSE);

		/*
		 * A bare {"type":"array"} is not a schema any provider can act
		 * on -- Gemini rejects it outright and the others guess. The
		 * items block is what makes the tool usable.
		 */
		g_assert_nonnull(strstr(text, "\"items\""));
		g_assert_nonnull(strstr(text, "in_progress"));
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/todo/writes", test_writes_a_list);
	g_test_add_func("/ai-glib/todo/replaces", test_replaces_rather_than_merges);
	g_test_add_func("/ai-glib/todo/empty-clears", test_empty_list_clears);
	g_test_add_func("/ai-glib/todo/changed-once",
	                test_changed_fires_once_per_call);
	g_test_add_func("/ai-glib/todo/clear", test_clear_todos);

	g_test_add_func("/ai-glib/todo/missing-parameter",
	                test_missing_todos_parameter);
	g_test_add_func("/ai-glib/todo/wrong-type", test_todos_wrong_type);
	g_test_add_func("/ai-glib/todo/bad-entries",
	                test_entries_that_are_not_objects_are_skipped);
	g_test_add_func("/ai-glib/todo/no-content",
	                test_entry_without_content_is_skipped);
	g_test_add_func("/ai-glib/todo/content-wrong-type",
	                test_content_of_the_wrong_type_is_skipped);
	g_test_add_func("/ai-glib/todo/unknown-status",
	                test_unknown_status_is_pending);
	g_test_add_func("/ai-glib/todo/status-synonyms", test_status_synonyms);
	g_test_add_func("/ai-glib/todo/camel-active-form",
	                test_camel_case_active_form);
	g_test_add_func("/ai-glib/todo/two-in-progress",
	                test_two_in_progress_is_allowed_with_a_message);

	g_test_add_func("/ai-glib/todo/accessor-bounds", test_accessor_bounds);
	g_test_add_func("/ai-glib/todo/boxed", test_todo_boxed_type);
	g_test_add_func("/ai-glib/todo/label-fallback",
	                test_label_falls_back_to_content);
	g_test_add_func("/ai-glib/todo/state-names", test_state_names_round_trip);
	g_test_add_func("/ai-glib/todo/schema",
	                test_tool_is_advertised_with_a_schema);

	return g_test_run();
}
