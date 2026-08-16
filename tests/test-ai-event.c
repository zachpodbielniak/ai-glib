/*
 * test-ai-event.c - Tests for AiEvent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <ai-glib.h>

/* Every kind, so the loops below cannot silently skip a new one. */
static const AiEventKind ALL_KINDS[] = {
	AI_EVENT_STREAM_START,
	AI_EVENT_TEXT_DELTA,
	AI_EVENT_THINKING_DELTA,
	AI_EVENT_TOOL_STARTED,
	AI_EVENT_TOOL_INPUT_DELTA,
	AI_EVENT_TOOL_FINISHED,
	AI_EVENT_USAGE,
	AI_EVENT_STATUS,
	AI_EVENT_ERROR,
	AI_EVENT_STREAM_END
};

static AiToolUse *
make_tool_use(const gchar *id, const gchar *name)
{
	return ai_tool_use_new_from_json_string(id, name, "{\"command\": \"ls\"}");
}

static void
test_event_new_kind_only(void)
{
	g_autoptr(AiEvent) event = ai_event_new(AI_EVENT_STREAM_START);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_STREAM_START);
	g_assert_null(ai_event_get_text(event));
	g_assert_null(ai_event_get_tool_use(event));
	g_assert_null(ai_event_get_tool_result(event));
	g_assert_null(ai_event_get_tool_use_id(event));
	g_assert_null(ai_event_get_usage(event));
	g_assert_null(ai_event_get_error(event));
	g_assert_null(ai_event_get_source(event));

	/* Unpriced by default -- 0 would read as "free", which is a lie. */
	g_assert_cmpint(ai_event_get_cost_micros(event), ==, -1);
}

static void
test_event_text_delta(void)
{
	g_autoptr(AiEvent) event = ai_event_new_text_delta("hello");

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_TEXT_DELTA);
	g_assert_cmpstr(ai_event_get_text(event), ==, "hello");

	/* A text delta must not look like anything else. */
	g_assert_null(ai_event_get_tool_use(event));
	g_assert_null(ai_event_get_usage(event));
	g_assert_null(ai_event_get_error(event));
}

static void
test_event_text_delta_null(void)
{
	g_autoptr(AiEvent) event = ai_event_new_text_delta(NULL);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_TEXT_DELTA);
	g_assert_null(ai_event_get_text(event));
}

static void
test_event_text_delta_empty(void)
{
	/*
	 * An empty delta is not the same as no delta: providers emit them at
	 * block boundaries and a consumer may legitimately count them.
	 */
	g_autoptr(AiEvent) event = ai_event_new_text_delta("");

	g_assert_nonnull(ai_event_get_text(event));
	g_assert_cmpstr(ai_event_get_text(event), ==, "");
}

static void
test_event_thinking_delta(void)
{
	g_autoptr(AiEvent) event = ai_event_new_thinking_delta("hmm");

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_THINKING_DELTA);
	g_assert_cmpstr(ai_event_get_text(event), ==, "hmm");
}

static void
test_event_tool_started(void)
{
	g_autoptr(AiToolUse) tu = make_tool_use("toolu_01", "bash");
	g_autoptr(AiEvent) event = ai_event_new_tool_started(tu);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_TOOL_STARTED);
	g_assert_true(ai_event_get_tool_use(event) == tu);
	g_assert_cmpstr(ai_event_get_tool_use_id(event), ==, "toolu_01");
	g_assert_null(ai_event_get_tool_result(event));
}

static void
test_event_tool_started_null(void)
{
	g_autoptr(AiEvent) event = ai_event_new_tool_started(NULL);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_TOOL_STARTED);
	g_assert_null(ai_event_get_tool_use(event));
	g_assert_null(ai_event_get_tool_use_id(event));
}

static void
test_event_tool_started_holds_ref(void)
{
	AiToolUse *tu = make_tool_use("toolu_ref", "bash");
	AiEvent *event = ai_event_new_tool_started(tu);

	/* Drop the caller's ref; the event must still own one. */
	g_object_unref(tu);

	g_assert_true(AI_IS_TOOL_USE(ai_event_get_tool_use(event)));
	g_assert_cmpstr(ai_tool_use_get_name(ai_event_get_tool_use(event)), ==, "bash");

	ai_event_unref(event);
}

static void
test_event_tool_input_delta(void)
{
	g_autoptr(AiEvent) event =
		ai_event_new_tool_input_delta("toolu_02", "{\"comm");

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_TOOL_INPUT_DELTA);
	g_assert_cmpstr(ai_event_get_tool_use_id(event), ==, "toolu_02");

	/* The fragment rides in the text field and need not parse on its own. */
	g_assert_cmpstr(ai_event_get_text(event), ==, "{\"comm");
	g_assert_null(ai_event_get_tool_use(event));
}

static void
test_event_tool_finished(void)
{
	g_autoptr(AiToolUse) tu = make_tool_use("toolu_03", "bash");
	g_autoptr(AiToolResult) res =
		ai_tool_result_new_with_name("toolu_03", "bash", "a.c b.c", FALSE);
	g_autoptr(AiEvent) event = ai_event_new_tool_finished(tu, res);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_TOOL_FINISHED);
	g_assert_true(ai_event_get_tool_use(event) == tu);
	g_assert_true(ai_event_get_tool_result(event) == res);
	g_assert_cmpstr(ai_event_get_tool_use_id(event), ==, "toolu_03");
}

static void
test_event_tool_finished_id_from_result(void)
{
	/*
	 * Several providers report a result without repeating the request.  The
	 * id must still come through, or nothing downstream can match the result
	 * to the call it answers.
	 */
	g_autoptr(AiToolResult) res =
		ai_tool_result_new("toolu_04", "done", FALSE);
	g_autoptr(AiEvent) event = ai_event_new_tool_finished(NULL, res);

	g_assert_null(ai_event_get_tool_use(event));
	g_assert_cmpstr(ai_event_get_tool_use_id(event), ==, "toolu_04");
}

static void
test_event_tool_finished_both_null(void)
{
	g_autoptr(AiEvent) event = ai_event_new_tool_finished(NULL, NULL);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_TOOL_FINISHED);
	g_assert_null(ai_event_get_tool_use_id(event));
}

static void
test_event_tool_finished_error_result(void)
{
	g_autoptr(AiToolResult) res =
		ai_tool_result_new("toolu_05", "No such file", TRUE);
	g_autoptr(AiEvent) event = ai_event_new_tool_finished(NULL, res);

	g_assert_true(ai_tool_result_get_is_error(ai_event_get_tool_result(event)));
}

static void
test_event_usage(void)
{
	g_autoptr(AiUsage) usage = ai_usage_new(120, 45);
	g_autoptr(AiEvent) event = ai_event_new_usage(usage, 3400);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_USAGE);
	g_assert_cmpint(ai_usage_get_input_tokens(ai_event_get_usage(event)), ==, 120);
	g_assert_cmpint(ai_usage_get_output_tokens(ai_event_get_usage(event)), ==, 45);
	g_assert_cmpint(ai_event_get_cost_micros(event), ==, 3400);

	/* A copy, not the caller's instance -- the caller may free theirs. */
	g_assert_true(ai_event_get_usage(event) != usage);
}

static void
test_event_usage_null(void)
{
	g_autoptr(AiEvent) event = ai_event_new_usage(NULL, -1);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_USAGE);
	g_assert_null(ai_event_get_usage(event));
	g_assert_cmpint(ai_event_get_cost_micros(event), ==, -1);
}

static void
test_event_usage_survives_caller_free(void)
{
	AiUsage *usage = ai_usage_new(7, 8);
	AiEvent *event = ai_event_new_usage(usage, 0);

	ai_usage_free(usage);

	g_assert_cmpint(ai_usage_get_total_tokens(ai_event_get_usage(event)), ==, 15);

	/* Zero cost is a real answer and must not be mistaken for "unpriced". */
	g_assert_cmpint(ai_event_get_cost_micros(event), ==, 0);

	ai_event_unref(event);
}

static void
test_event_status(void)
{
	g_autoptr(AiEvent) event = ai_event_new_status("resuming session abc");

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_STATUS);
	g_assert_cmpstr(ai_event_get_text(event), ==, "resuming session abc");
}

static void
test_event_error(void)
{
	g_autoptr(GError) error =
		g_error_new(AI_ERROR, AI_ERROR_RATE_LIMITED, "slow down");
	g_autoptr(AiEvent) event = ai_event_new_error(error);
	const GError *held;

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_ERROR);

	held = ai_event_get_error(event);
	g_assert_nonnull(held);
	g_assert_cmpint(held->domain, ==, AI_ERROR);
	g_assert_cmpint(held->code, ==, AI_ERROR_RATE_LIMITED);
	g_assert_cmpstr(held->message, ==, "slow down");

	/* A copy, so the emitter's g_autoptr may unwind out from under us. */
	g_assert_true(held != error);

	/* The message is mirrored into text so text-only frontends still work. */
	g_assert_cmpstr(ai_event_get_text(event), ==, "slow down");
}

static void
test_event_error_survives_caller_free(void)
{
	GError *error = g_error_new(AI_ERROR, AI_ERROR_TIMEOUT, "took too long");
	AiEvent *event = ai_event_new_error(error);

	g_error_free(error);

	g_assert_cmpint(ai_event_get_error(event)->code, ==, AI_ERROR_TIMEOUT);
	g_assert_cmpstr(ai_event_get_error(event)->message, ==, "took too long");

	ai_event_unref(event);
}

static void
test_event_error_null(void)
{
	g_autoptr(AiEvent) event = ai_event_new_error(NULL);

	g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_ERROR);
	g_assert_null(ai_event_get_error(event));
	g_assert_null(ai_event_get_text(event));
}

static void
test_event_refcount(void)
{
	AiEvent *event = ai_event_new_text_delta("shared");
	AiEvent *second;

	second = ai_event_ref(event);
	g_assert_true(second == event);

	/* One unref must not free it while the other reference lives. */
	ai_event_unref(event);
	g_assert_cmpstr(ai_event_get_text(second), ==, "shared");

	ai_event_unref(second);
}

static void
test_event_ref_null(void)
{
	g_assert_null(ai_event_ref(NULL));

	/* Documented as a no-op rather than a crash. */
	ai_event_unref(NULL);
}

static void
test_event_boxed_copy_is_ref(void)
{
	/*
	 * G_DEFINE_BOXED_TYPE wires copy to ref, so anything that boxes an event
	 * -- a GValue, a signal closure -- shares the instance instead of
	 * duplicating it.  That is only safe because events are immutable.
	 */
	AiEvent *event = ai_event_new_text_delta("boxed");
	AiEvent *copy;

	copy = (AiEvent *)g_boxed_copy(AI_TYPE_EVENT, event);
	g_assert_true(copy == event);

	g_boxed_free(AI_TYPE_EVENT, copy);
	g_assert_cmpstr(ai_event_get_text(event), ==, "boxed");

	ai_event_unref(event);
}

static void
test_event_gvalue_roundtrip(void)
{
	g_autoptr(AiEvent) event = ai_event_new_status("via gvalue");
	GValue value = G_VALUE_INIT;
	AiEvent *out;

	g_value_init(&value, AI_TYPE_EVENT);
	g_value_set_boxed(&value, event);

	out = (AiEvent *)g_value_get_boxed(&value);
	g_assert_cmpstr(ai_event_get_text(out), ==, "via gvalue");

	g_value_unset(&value);
}

static void
test_event_source_label(void)
{
	g_autoptr(AiEvent) event = ai_event_new_text_delta("x");

	g_assert_null(ai_event_get_source(event));

	ai_event_set_source(event, "AiGrokBuildClient");
	g_assert_cmpstr(ai_event_get_source(event), ==, "AiGrokBuildClient");

	/* Overwriting must free the old label, not leak it. */
	ai_event_set_source(event, "executor");
	g_assert_cmpstr(ai_event_get_source(event), ==, "executor");

	ai_event_set_source(event, NULL);
	g_assert_null(ai_event_get_source(event));
}

static void
test_event_timestamp(void)
{
	g_autoptr(AiEvent) first = ai_event_new(AI_EVENT_STREAM_START);
	g_autoptr(AiEvent) second = ai_event_new(AI_EVENT_STREAM_END);

	g_assert_cmpint(ai_event_get_timestamp(first), >, 0);

	/* Monotonic: a later event never stamps earlier than an older one. */
	g_assert_cmpint(ai_event_get_timestamp(second), >=,
	                ai_event_get_timestamp(first));
}

static void
test_event_kind_to_string(void)
{
	g_autoptr(GHashTable) seen =
		g_hash_table_new(g_str_hash, g_str_equal);
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(ALL_KINDS); i++)
	{
		const gchar *name = ai_event_kind_to_string(ALL_KINDS[i]);

		g_assert_nonnull(name);
		g_assert_cmpstr(name, !=, "");
		g_assert_cmpstr(name, !=, "unknown");

		/* Names must be distinct, or a log cannot be read back. */
		g_assert_false(g_hash_table_contains(seen, name));
		g_hash_table_add(seen, (gpointer)name);
	}

	g_assert_cmpuint(g_hash_table_size(seen), ==, G_N_ELEMENTS(ALL_KINDS));
}

static void
test_event_kind_to_string_out_of_range(void)
{
	g_assert_cmpstr(ai_event_kind_to_string((AiEventKind)9999), ==, "unknown");
}

static void
test_event_null_getters(void)
{
	/*
	 * Every getter guards with g_return_val_if_fail.  GTest makes criticals
	 * fatal, so each one is expected explicitly -- that is the assertion.
	 */
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*self != NULL*");
	g_assert_null(ai_event_get_text(NULL));
	g_test_assert_expected_messages();

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*self != NULL*");
	g_assert_null(ai_event_get_tool_use(NULL));
	g_test_assert_expected_messages();

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*self != NULL*");
	g_assert_cmpint(ai_event_get_cost_micros(NULL), ==, -1);
	g_test_assert_expected_messages();

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*self != NULL*");
	g_assert_cmpint(ai_event_get_timestamp(NULL), ==, 0);
	g_test_assert_expected_messages();
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/event/new-kind-only", test_event_new_kind_only);
	g_test_add_func("/ai-glib/event/text-delta", test_event_text_delta);
	g_test_add_func("/ai-glib/event/text-delta-null", test_event_text_delta_null);
	g_test_add_func("/ai-glib/event/text-delta-empty", test_event_text_delta_empty);
	g_test_add_func("/ai-glib/event/thinking-delta", test_event_thinking_delta);
	g_test_add_func("/ai-glib/event/tool-started", test_event_tool_started);
	g_test_add_func("/ai-glib/event/tool-started-null", test_event_tool_started_null);
	g_test_add_func("/ai-glib/event/tool-started-holds-ref",
	                test_event_tool_started_holds_ref);
	g_test_add_func("/ai-glib/event/tool-input-delta", test_event_tool_input_delta);
	g_test_add_func("/ai-glib/event/tool-finished", test_event_tool_finished);
	g_test_add_func("/ai-glib/event/tool-finished-id-from-result",
	                test_event_tool_finished_id_from_result);
	g_test_add_func("/ai-glib/event/tool-finished-both-null",
	                test_event_tool_finished_both_null);
	g_test_add_func("/ai-glib/event/tool-finished-error-result",
	                test_event_tool_finished_error_result);
	g_test_add_func("/ai-glib/event/usage", test_event_usage);
	g_test_add_func("/ai-glib/event/usage-null", test_event_usage_null);
	g_test_add_func("/ai-glib/event/usage-survives-caller-free",
	                test_event_usage_survives_caller_free);
	g_test_add_func("/ai-glib/event/status", test_event_status);
	g_test_add_func("/ai-glib/event/error", test_event_error);
	g_test_add_func("/ai-glib/event/error-survives-caller-free",
	                test_event_error_survives_caller_free);
	g_test_add_func("/ai-glib/event/error-null", test_event_error_null);
	g_test_add_func("/ai-glib/event/refcount", test_event_refcount);
	g_test_add_func("/ai-glib/event/ref-null", test_event_ref_null);
	g_test_add_func("/ai-glib/event/boxed-copy-is-ref", test_event_boxed_copy_is_ref);
	g_test_add_func("/ai-glib/event/gvalue-roundtrip", test_event_gvalue_roundtrip);
	g_test_add_func("/ai-glib/event/source-label", test_event_source_label);
	g_test_add_func("/ai-glib/event/timestamp", test_event_timestamp);
	g_test_add_func("/ai-glib/event/kind-to-string", test_event_kind_to_string);
	g_test_add_func("/ai-glib/event/kind-to-string-out-of-range",
	                test_event_kind_to_string_out_of_range);
	g_test_add_func("/ai-glib/event/null-getters", test_event_null_getters);

	return g_test_run();
}
