/*
 * test-transcript.c - The buffer, and the folding that fills it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Two things are tested here. AiTranscript's own contract as a GListModel
 * plus its ::block-changed, and AiConversation's folding rules -- the ones
 * that turn a flat stream of events into narration, a collapsed tool group,
 * and more narration.
 */

#include <ai-glib.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	guint  items_changed;
	guint  last_position;
	guint  last_removed;
	guint  last_added;

	guint  block_changed;
	guint  last_changed_position;
} Watch;

static void
on_items_changed(GListModel *model, guint position, guint removed,
                 guint added, gpointer user_data)
{
	Watch *w = user_data;

	(void)model;

	w->items_changed++;
	w->last_position = position;
	w->last_removed = removed;
	w->last_added = added;
}

static void
on_block_changed(AiTranscript *t, guint position, AiViewBlock *block,
                 gpointer user_data)
{
	Watch *w = user_data;

	(void)t;
	(void)block;

	w->block_changed++;
	w->last_changed_position = position;
}

static Watch *
watch(AiTranscript *t)
{
	Watch *w = g_new0(Watch, 1);

	g_signal_connect(t, "items-changed", G_CALLBACK(on_items_changed), w);
	g_signal_connect(t, "block-changed", G_CALLBACK(on_block_changed), w);

	return w;
}

/* ----------------------------------------------------------------
 * GListModel conformance
 * ---------------------------------------------------------------- */

static void
test_is_a_list_model(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();

	g_assert_true(G_IS_LIST_MODEL(t));
	g_assert_cmpuint(g_list_model_get_item_type(G_LIST_MODEL(t)), ==,
	                 AI_TYPE_VIEW_BLOCK);
	g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(t)), ==, 0);
}

static void
test_append_notifies(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) block = ai_view_turn_block_new("hello");
	Watch *w = watch(t);

	ai_transcript_append(t, block);

	g_assert_cmpuint(w->items_changed, ==, 1);
	g_assert_cmpuint(w->last_position, ==, 0);
	g_assert_cmpuint(w->last_removed, ==, 0);
	g_assert_cmpuint(w->last_added, ==, 1);
	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 1);

	g_free(w);
}

static void
test_get_item_out_of_range(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) block = ai_view_turn_block_new("hello");

	ai_transcript_append(t, block);

	g_assert_nonnull(ai_transcript_get_block(t, 0));
	g_assert_null(ai_transcript_get_block(t, 1));
	g_assert_null(g_list_model_get_item(G_LIST_MODEL(t), 99));
}

static void
test_get_last(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) first = ai_view_turn_block_new("one");
	g_autoptr(AiViewBlock) second = ai_view_turn_block_new("two");

	g_assert_null(ai_transcript_get_last(t));

	ai_transcript_append(t, first);
	ai_transcript_append(t, second);

	g_assert_true(ai_transcript_get_last(t) == second);
}

static void
test_block_changed_carries_position(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) a = ai_view_turn_block_new("a");
	g_autoptr(AiViewBlock) b = ai_view_text_block_new();
	Watch *w;

	ai_transcript_append(t, a);
	ai_transcript_append(t, b);

	w = watch(t);
	ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(b), "streaming");

	g_assert_cmpuint(w->block_changed, ==, 1);
	g_assert_cmpuint(w->last_changed_position, ==, 1);

	g_free(w);
}

static void
test_clear(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) a = ai_view_turn_block_new("a");
	g_autoptr(AiViewBlock) b = ai_view_turn_block_new("b");
	Watch *w;

	ai_transcript_append(t, a);
	ai_transcript_append(t, b);

	w = watch(t);
	ai_transcript_clear(t);

	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 0);
	g_assert_cmpuint(w->items_changed, ==, 1);
	g_assert_cmpuint(w->last_removed, ==, 2);
	g_assert_cmpuint(w->last_added, ==, 0);

	g_free(w);
}

static void
test_clear_empty_is_silent(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	Watch *w = watch(t);

	ai_transcript_clear(t);

	g_assert_cmpuint(w->items_changed, ==, 0);

	g_free(w);
}

static void
test_removed_block_stops_reporting(void)
{
	/*
	 * A caller holding a reference may go on mutating a block after it
	 * leaves the transcript. It must not still be reporting those changes
	 * to a transcript it is no longer part of.
	 */
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) block = ai_view_text_block_new();
	Watch *w;

	ai_transcript_append(t, block);
	w = watch(t);

	ai_transcript_clear(t);
	g_assert_cmpuint(w->block_changed, ==, 0);

	ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block), "after removal");
	g_assert_cmpuint(w->block_changed, ==, 0);

	g_free(w);
}

static void
test_find_block(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) a = ai_view_turn_block_new("a");
	g_autoptr(AiViewBlock) b = ai_view_turn_block_new("b");
	g_autoptr(AiViewBlock) orphan = ai_view_turn_block_new("orphan");
	guint position = 999;

	ai_transcript_append(t, a);
	ai_transcript_append(t, b);

	g_assert_true(ai_transcript_find_block(t, b, &position));
	g_assert_cmpuint(position, ==, 1);
	g_assert_false(ai_transcript_find_block(t, orphan, NULL));
}

static void
test_to_text(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autoptr(AiViewBlock) turn = ai_view_turn_block_new("hi");
	g_autoptr(AiViewBlock) text = ai_view_text_block_new();
	g_autofree gchar *out = NULL;

	ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(text), "hello back");

	ai_transcript_append(t, turn);
	ai_transcript_append(t, text);

	out = ai_transcript_to_text(t, 0);
	g_assert_cmpstr(out, ==, "> hi\n\nhello back\n");
}

static void
test_to_text_empty(void)
{
	g_autoptr(AiTranscript) t = ai_transcript_new();
	g_autofree gchar *out = ai_transcript_to_text(t, 0);

	g_assert_cmpstr(out, ==, "");
}

static void
test_block_ids_are_unique(void)
{
	g_autoptr(AiViewBlock) a = ai_view_turn_block_new("a");
	g_autoptr(AiViewBlock) b = ai_view_turn_block_new("b");

	g_assert_cmpuint(ai_view_block_get_id(a), !=, 0);
	g_assert_cmpuint(ai_view_block_get_id(a), !=, ai_view_block_get_id(b));
}

/* ----------------------------------------------------------------
 * Folding: the rhythm of a session
 * ---------------------------------------------------------------- */

/* Feed one event straight into a conversation's folding. */
static void
emit(AiMockProvider *mock, AiEvent *event)
{
	ai_event_source_emit(AI_EVENT_SOURCE(mock), event);
}

static AiConversation *
conversation_for(AiMockProvider *mock)
{
	return ai_conversation_new(G_OBJECT(mock));
}

static AiViewBlockKind
kind_at(AiConversation *c, guint i)
{
	return ai_view_block_get_kind(
		ai_transcript_get_block(ai_conversation_get_transcript(c), i));
}

static void
test_text_deltas_collapse_into_one_block(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	g_autofree gchar *text = NULL;

	{
		g_autoptr(AiEvent) e1 = ai_event_new_text_delta("Hello ");
		g_autoptr(AiEvent) e2 = ai_event_new_text_delta("there");

		emit(mock, e1);
		emit(mock, e2);
	}

	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 1);
	g_assert_cmpint(kind_at(c, 0), ==, AI_VIEW_BLOCK_TEXT);

	text = ai_view_block_render_text(ai_transcript_get_block(t, 0), 0);
	g_assert_cmpstr(text, ==, "Hello there");
}

static void
test_tool_interrupts_prose(void)
{
	/*
	 * The whole visual effect: narration, a tool group, then *new*
	 * narration rather than a continuation of the first paragraph.
	 */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);

	{
		g_autoptr(AiEvent) before = ai_event_new_text_delta("Now the registry.");
		g_autoptr(AiToolUse) tu =
			ai_tool_use_new_from_json_string("t1", "Write",
			                                 "{\"file_path\": \"a.c\"}");
		g_autoptr(AiEvent) tool = ai_event_new_tool_started(tu);
		g_autoptr(AiEvent) after = ai_event_new_text_delta("Cleaning up.");

		emit(mock, before);
		emit(mock, tool);
		emit(mock, after);
	}

	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 3);
	g_assert_cmpint(kind_at(c, 0), ==, AI_VIEW_BLOCK_TEXT);
	g_assert_cmpint(kind_at(c, 1), ==, AI_VIEW_BLOCK_TOOL);
	g_assert_cmpint(kind_at(c, 2), ==, AI_VIEW_BLOCK_TEXT);
}

static void
test_consecutive_tools_group(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	AiViewBlock *block;
	g_autofree gchar *summary = NULL;

	{
		guint i;

		for (i = 0; i < 3; i++)
		{
			g_autofree gchar *id = g_strdup_printf("t%u", i);
			g_autofree gchar *input =
				g_strdup_printf("{\"command\": \"echo %u\"}", i);
			g_autoptr(AiToolUse) tu =
				ai_tool_use_new_from_json_string(id, "Bash", input);
			g_autoptr(AiEvent) e = ai_event_new_tool_started(tu);

			emit(mock, e);
		}
	}

	/* One block, three calls -- that is the grouping. */
	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 1);

	block = ai_transcript_get_block(t, 0);
	g_assert_cmpuint(
		ai_view_tool_block_get_n_calls(AI_VIEW_TOOL_BLOCK(block)), ==, 3);

	summary = ai_view_tool_block_get_summary(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran 3 commands");
}

static void
test_tool_finished_updates_the_call(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	AiToolCall *call;

	{
		g_autoptr(AiToolUse) tu =
			ai_tool_use_new_from_json_string("t1", "Bash",
			                                 "{\"command\": \"ls\"}");
		g_autoptr(AiToolResult) res = ai_tool_result_new("t1", "a.c", FALSE);
		g_autoptr(AiEvent) started = ai_event_new_tool_started(tu);
		g_autoptr(AiEvent) finished = ai_event_new_tool_finished(tu, res);

		emit(mock, started);

		call = ai_view_tool_block_find_call(
			AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(t, 0)), "t1");
		g_assert_cmpint(ai_tool_call_get_state(call), ==, AI_TOOL_CALL_PENDING);

		emit(mock, finished);
	}

	g_assert_cmpint(ai_tool_call_get_state(call), ==, AI_TOOL_CALL_OK);
	g_assert_cmpstr(ai_tool_call_get_result(call), ==, "a.c");
}

static void
test_tool_finished_after_new_prose(void)
{
	/*
	 * A result can arrive after prose has already started a new paragraph,
	 * so the call it answers is in a group that is no longer open. It still
	 * has to be found.
	 */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	AiToolCall *call;

	{
		g_autoptr(AiToolUse) tu =
			ai_tool_use_new_from_json_string("t1", "Bash",
			                                 "{\"command\": \"ls\"}");
		g_autoptr(AiToolResult) res = ai_tool_result_new("t1", "done", FALSE);
		g_autoptr(AiEvent) started = ai_event_new_tool_started(tu);
		g_autoptr(AiEvent) prose = ai_event_new_text_delta("meanwhile");
		g_autoptr(AiEvent) finished = ai_event_new_tool_finished(NULL, res);

		emit(mock, started);
		emit(mock, prose);
		emit(mock, finished);
	}

	/* No third block was invented for the stray result. */
	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 2);

	call = ai_view_tool_block_find_call(
		AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(t, 0)), "t1");
	g_assert_cmpint(ai_tool_call_get_state(call), ==, AI_TOOL_CALL_OK);
}

static void
test_unmatched_tool_result_is_still_shown(void)
{
	/*
	 * Dropping it would leave real work invisible; a group of its own is
	 * the honest answer.
	 */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);

	{
		g_autoptr(AiToolResult) res =
			ai_tool_result_new("never-announced", "surprise", FALSE);
		g_autoptr(AiEvent) e = ai_event_new_tool_finished(NULL, res);

		emit(mock, e);
	}

	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 1);
	g_assert_cmpint(kind_at(c, 0), ==, AI_VIEW_BLOCK_TOOL);
}

static void
test_thinking_gets_its_own_block(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);

	{
		g_autoptr(AiEvent) a = ai_event_new_text_delta("before");
		g_autoptr(AiEvent) think = ai_event_new_thinking_delta("hmm");
		g_autoptr(AiEvent) b = ai_event_new_text_delta("after");

		emit(mock, a);
		emit(mock, think);
		emit(mock, b);
	}

	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 3);
	g_assert_cmpint(kind_at(c, 1), ==, AI_VIEW_BLOCK_THINKING);

	/* Collapsed by default: reasoning is available, not imposed. */
	g_assert_false(ai_view_block_get_expanded(ai_transcript_get_block(t, 1)));
}

static void
test_usage_and_error_become_status(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	g_autofree gchar *usage_text = NULL;

	{
		g_autoptr(AiUsage) usage = ai_usage_new(120, 45);
		g_autoptr(AiEvent) u = ai_event_new_usage(usage, 3400);
		g_autoptr(GError) err =
			g_error_new(AI_ERROR, AI_ERROR_RATE_LIMITED, "slow down");
		g_autoptr(AiEvent) e = ai_event_new_error(err);

		emit(mock, u);
		emit(mock, e);
	}

	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 2);
	g_assert_cmpint(kind_at(c, 0), ==, AI_VIEW_BLOCK_STATUS);
	g_assert_cmpint(
		ai_view_status_block_get_status_kind(
			AI_VIEW_STATUS_BLOCK(ai_transcript_get_block(t, 1))),
		==, AI_VIEW_STATUS_ERROR);

	usage_text = ai_view_block_render_text(ai_transcript_get_block(t, 0), 0);
	g_assert_true(strstr(usage_text, "120 in / 45 out") != NULL);
	g_assert_true(strstr(usage_text, "$0.0034") != NULL);
}

static void
test_unpriced_usage_omits_cost(void)
{
	/* Claiming a turn was free is worse than saying nothing about it. */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	g_autofree gchar *text = NULL;

	{
		g_autoptr(AiUsage) usage = ai_usage_new(1, 2);
		g_autoptr(AiEvent) u = ai_event_new_usage(usage, -1);

		emit(mock, u);
	}

	text = ai_view_block_render_text(ai_transcript_get_block(t, 0), 0);
	g_assert_null(strchr(text, '$'));
}

static void
test_stream_end_completes_blocks(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);

	{
		g_autoptr(AiEvent) delta = ai_event_new_text_delta("partial");
		g_autoptr(AiEvent) end = ai_event_new(AI_EVENT_STREAM_END);

		emit(mock, delta);
		g_assert_false(ai_view_block_get_complete(ai_transcript_get_block(t, 0)));

		emit(mock, end);
	}

	g_assert_true(ai_view_block_get_complete(ai_transcript_get_block(t, 0)));
}

static void
test_input_delta_is_not_folded(void)
{
	/* A consumer wanting whole calls loses nothing by ignoring these. */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);

	{
		g_autoptr(AiEvent) e = ai_event_new_tool_input_delta("t1", "{\"pa");

		emit(mock, e);
	}

	g_assert_cmpuint(ai_transcript_get_n_blocks(t), ==, 0);
}

static void
test_conversation_defaults(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);

	g_assert_nonnull(ai_conversation_get_transcript(c));
	g_assert_nonnull(ai_conversation_get_executor(c));
	g_assert_true(ai_conversation_get_provider(c) == G_OBJECT(mock));
	g_assert_true(ai_conversation_get_stream(c));
	g_assert_false(ai_conversation_get_local_tools(c));
	g_assert_false(ai_conversation_get_busy(c));
	g_assert_cmpint(ai_conversation_get_max_tokens(c), ==, 4096);
}

static void
test_local_tools_refused_for_cli_providers(void)
{
	/*
	 * The CLI wrappers ignore the tools argument and run their own, so an
	 * executor pointed at one would advertise tools the model never sees.
	 */
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	g_autoptr(AiConversation) c = ai_conversation_new(G_OBJECT(cli));

	ai_conversation_set_local_tools(c, TRUE);
	g_assert_false(ai_conversation_get_local_tools(c));
}

static void
test_local_tools_allowed_for_http_providers(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);

	ai_conversation_set_local_tools(c, TRUE);
	g_assert_true(ai_conversation_get_local_tools(c));
}

static void
test_clear_empties_everything(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);

	{
		g_autoptr(AiEvent) e = ai_event_new_text_delta("something");
		emit(mock, e);
	}

	g_assert_cmpuint(
		ai_transcript_get_n_blocks(ai_conversation_get_transcript(c)), ==, 1);

	ai_conversation_clear(c);

	g_assert_cmpuint(
		ai_transcript_get_n_blocks(ai_conversation_get_transcript(c)), ==, 0);
	g_assert_null(ai_conversation_get_messages(c));

	/* The open blocks were forgotten too, so the next delta starts fresh. */
	{
		g_autoptr(AiEvent) e = ai_event_new_text_delta("again");
		emit(mock, e);
	}

	g_assert_cmpuint(
		ai_transcript_get_n_blocks(ai_conversation_get_transcript(c)), ==, 1);
}

/* ----------------------------------------------------------------
 * Sending
 * ---------------------------------------------------------------- */

typedef struct
{
	GMainLoop *loop;
	gboolean   ok;
	GError    *error;
} SendRun;

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
	SendRun *run = user_data;

	run->ok = ai_conversation_send_finish(AI_CONVERSATION(source), result,
	                                      &run->error);
	g_main_loop_quit(run->loop);
}

static SendRun *
send_and_wait(AiConversation *c, const gchar *text)
{
	SendRun *run = g_new0(SendRun, 1);

	run->loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_async(c, text, NULL, on_sent, run);
	g_main_loop_run(run->loop);

	return run;
}

static void
send_run_free(SendRun *run)
{
	g_main_loop_unref(run->loop);
	g_clear_error(&run->error);
	g_free(run);
}

static void
test_send_builds_turn_and_reply(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	SendRun *run;
	g_autofree gchar *dump = NULL;

	ai_mock_provider_push_text(mock, "the answer");

	run = send_and_wait(c, "the question");

	g_assert_true(run->ok);
	g_assert_no_error(run->error);
	g_assert_false(ai_conversation_get_busy(c));

	g_assert_cmpint(kind_at(c, 0), ==, AI_VIEW_BLOCK_TURN);

	dump = ai_transcript_to_text(t, 0);
	g_assert_true(strstr(dump, "> the question") != NULL);
	g_assert_true(strstr(dump, "the answer") != NULL);

	/* History round-trips: the user turn and the reply. */
	g_assert_cmpuint(g_list_length(ai_conversation_get_messages(c)), ==, 2);

	send_run_free(run);
}

static void
test_send_empty_is_refused(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	SendRun *run = send_and_wait(c, "");

	g_assert_false(run->ok);
	g_assert_error(run->error, AI_ERROR, AI_ERROR_INVALID_REQUEST);

	send_run_free(run);
}

static void
test_send_while_busy_is_refused(void)
{
	/*
	 * Interleaving two turns would produce a transcript describing neither.
	 */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	SendRun *first = g_new0(SendRun, 1);
	SendRun *second = g_new0(SendRun, 1);

	ai_mock_provider_set_delay_ms(mock, 50);
	ai_mock_provider_push_text(mock, "slow answer");

	first->loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_async(c, "one", NULL, on_sent, first);

	g_assert_true(ai_conversation_get_busy(c));

	second->loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_async(c, "two", NULL, on_sent, second);
	g_main_loop_run(second->loop);

	g_assert_false(second->ok);
	g_assert_error(second->error, AI_ERROR, AI_ERROR_INVALID_REQUEST);

	g_main_loop_run(first->loop);
	g_assert_true(first->ok);

	send_run_free(first);
	send_run_free(second);
}

static void
test_provider_error_becomes_a_status_block(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	SendRun *run;
	guint n;

	ai_mock_provider_push_error(mock, "the model refused");

	run = send_and_wait(c, "please");

	g_assert_false(run->ok);
	g_assert_nonnull(run->error);

	/* busy clears even on failure, or the frontend locks up. */
	g_assert_false(ai_conversation_get_busy(c));

	n = ai_transcript_get_n_blocks(t);
	g_assert_cmpuint(n, >=, 2);
	g_assert_cmpint(
		ai_view_status_block_get_status_kind(
			AI_VIEW_STATUS_BLOCK(ai_transcript_get_block(t, n - 1))),
		==, AI_VIEW_STATUS_ERROR);

	send_run_free(run);
}

static void
test_non_streaming_provider_folds_the_response(void)
{
	/*
	 * The claude-tmux path: no events at all, so the finished response is
	 * where the blocks must come from.
	 */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	AiTranscript *t = ai_conversation_get_transcript(c);
	SendRun *run;
	g_autofree gchar *dump = NULL;

	ai_conversation_set_stream(c, FALSE);
	ai_mock_provider_push_text(mock, "one-piece answer");

	run = send_and_wait(c, "hello");

	g_assert_true(run->ok);

	dump = ai_transcript_to_text(t, 0);
	g_assert_true(strstr(dump, "one-piece answer") != NULL);

	send_run_free(run);
}

static void
test_multi_turn_history(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) c = conversation_for(mock);
	SendRun *a;
	SendRun *b;

	ai_mock_provider_push_text(mock, "first reply");
	ai_mock_provider_push_text(mock, "second reply");

	a = send_and_wait(c, "first");
	b = send_and_wait(c, "second");

	g_assert_true(a->ok);
	g_assert_true(b->ok);

	/* Two turns, two replies. */
	g_assert_cmpuint(g_list_length(ai_conversation_get_messages(c)), ==, 4);

	send_run_free(a);
	send_run_free(b);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/transcript/list-model", test_is_a_list_model);
	g_test_add_func("/ai-glib/transcript/append-notifies", test_append_notifies);
	g_test_add_func("/ai-glib/transcript/range", test_get_item_out_of_range);
	g_test_add_func("/ai-glib/transcript/get-last", test_get_last);
	g_test_add_func("/ai-glib/transcript/block-changed",
	                test_block_changed_carries_position);
	g_test_add_func("/ai-glib/transcript/clear", test_clear);
	g_test_add_func("/ai-glib/transcript/clear-empty", test_clear_empty_is_silent);
	g_test_add_func("/ai-glib/transcript/removed-block-silent",
	                test_removed_block_stops_reporting);
	g_test_add_func("/ai-glib/transcript/find-block", test_find_block);
	g_test_add_func("/ai-glib/transcript/to-text", test_to_text);
	g_test_add_func("/ai-glib/transcript/to-text-empty", test_to_text_empty);
	g_test_add_func("/ai-glib/transcript/block-ids", test_block_ids_are_unique);

	g_test_add_func("/ai-glib/fold/text-collapses",
	                test_text_deltas_collapse_into_one_block);
	g_test_add_func("/ai-glib/fold/tool-interrupts-prose", test_tool_interrupts_prose);
	g_test_add_func("/ai-glib/fold/tools-group", test_consecutive_tools_group);
	g_test_add_func("/ai-glib/fold/tool-finished", test_tool_finished_updates_the_call);
	g_test_add_func("/ai-glib/fold/tool-finished-late",
	                test_tool_finished_after_new_prose);
	g_test_add_func("/ai-glib/fold/unmatched-result",
	                test_unmatched_tool_result_is_still_shown);
	g_test_add_func("/ai-glib/fold/thinking", test_thinking_gets_its_own_block);
	g_test_add_func("/ai-glib/fold/usage-and-error", test_usage_and_error_become_status);
	g_test_add_func("/ai-glib/fold/unpriced", test_unpriced_usage_omits_cost);
	g_test_add_func("/ai-glib/fold/stream-end", test_stream_end_completes_blocks);
	g_test_add_func("/ai-glib/fold/input-delta", test_input_delta_is_not_folded);

	g_test_add_func("/ai-glib/conversation/defaults", test_conversation_defaults);
	g_test_add_func("/ai-glib/conversation/local-tools-cli",
	                test_local_tools_refused_for_cli_providers);
	g_test_add_func("/ai-glib/conversation/local-tools-http",
	                test_local_tools_allowed_for_http_providers);
	g_test_add_func("/ai-glib/conversation/clear", test_clear_empties_everything);
	g_test_add_func("/ai-glib/conversation/send", test_send_builds_turn_and_reply);
	g_test_add_func("/ai-glib/conversation/send-empty", test_send_empty_is_refused);
	g_test_add_func("/ai-glib/conversation/send-busy", test_send_while_busy_is_refused);
	g_test_add_func("/ai-glib/conversation/provider-error",
	                test_provider_error_becomes_a_status_block);
	g_test_add_func("/ai-glib/conversation/non-streaming",
	                test_non_streaming_provider_folds_the_response);
	g_test_add_func("/ai-glib/conversation/multi-turn", test_multi_turn_history);

	return g_test_run();
}
