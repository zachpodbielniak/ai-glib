/*
 * test-view-agent-block.c - The background-agent panel
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Rendered strings are asserted literally, as in test-view-tool-block.c
 * and test-view-todo-block.c: these are what a reader sees, so a change
 * of wording should be a change somebody made on purpose.
 *
 * The rule with the most behind it is the in-place update. An agent
 * changes state several times over its life, and a transcript that grew
 * a block per change would bury the conversation the panel belongs to.
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static AiAgent *
agent(const gchar *id, const gchar *description, AiAgentState state)
{
	AiAgent *self = ai_agent_new(id, NULL);

	ai_agent_set_description(self, description);
	ai_agent_set_state(self, state);

	return self;
}

static gchar *
render(AiViewAgentBlock *block)
{
	g_autoptr(AiRenderedText) text =
		ai_view_block_render(AI_VIEW_BLOCK(block), 0);

	return g_strdup(ai_rendered_text_get_text(text));
}

typedef struct
{
	guint changed;
} Counter;

static void
on_changed(AiViewBlock *block, gpointer user_data)
{
	Counter *counter = user_data;

	(void)block;
	counter->changed++;
}

/* ----------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------- */

static void
test_empty_says_so(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autofree gchar           *text = render(block);

	g_assert_cmpstr(text, ==, "No background agents");
	g_assert_cmpuint(ai_view_agent_block_get_n_agents(block), ==, 0);
}

static void
test_one_running_agent(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          one =
		agent("a1", "audit the search code", AI_AGENT_STATE_RUNNING);
	g_autoptr(GList)            agents = g_list_append(NULL, one);
	g_autofree gchar           *text = NULL;

	ai_view_agent_block_set_agents(block, agents);
	text = render(block);

	g_assert_cmpstr(text, ==,
	                "▸ a1  audit the search code  running 0s\n"
	                "1 running, 1 total");
}

static void
test_markers_differ_by_state(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          done = agent("a1", "one", AI_AGENT_STATE_DONE);
	g_autoptr(AiAgent)          failed =
		agent("a2", "two", AI_AGENT_STATE_FAILED);
	g_autoptr(AiAgent)          stopped =
		agent("a3", "three", AI_AGENT_STATE_CANCELLED);
	g_autoptr(AiAgent)          queued =
		agent("a4", "four", AI_AGENT_STATE_QUEUED);
	GList                      *agents = NULL;
	g_autofree gchar           *text = NULL;

	agents = g_list_append(agents, done);
	agents = g_list_append(agents, failed);
	agents = g_list_append(agents, stopped);
	agents = g_list_append(agents, queued);

	ai_view_agent_block_set_agents(block, agents);
	text = render(block);
	g_list_free(agents);

	/*
	 * One glyph each, distinguishable with no colour at all --- a
	 * terminal without it, or a reader who cannot tell red from green,
	 * still gets the difference.
	 */
	g_assert_nonnull(strstr(text, "✔ a1"));
	g_assert_nonnull(strstr(text, "✘ a2"));
	g_assert_nonnull(strstr(text, "⊘ a3"));
	g_assert_nonnull(strstr(text, "· a4"));
}

static void
test_tally_counts_only_the_live(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          one =
		agent("a1", "one", AI_AGENT_STATE_RUNNING);
	g_autoptr(AiAgent)          two = agent("a2", "two", AI_AGENT_STATE_DONE);
	g_autoptr(AiAgent)          three =
		agent("a3", "three", AI_AGENT_STATE_QUEUED);
	GList                      *agents = NULL;
	g_autofree gchar           *text = NULL;

	agents = g_list_append(agents, one);
	agents = g_list_append(agents, two);
	agents = g_list_append(agents, three);

	ai_view_agent_block_set_agents(block, agents);
	text = render(block);
	g_list_free(agents);

	/*
	 * Queued does not count as running, because it is not: "live" means
	 * occupying a concurrency slot, and a queued agent is waiting for
	 * one. It is still in the total, which is what tells a reader there
	 * is more coming.
	 */
	g_assert_nonnull(strstr(text, "1 running, 3 total"));
	g_assert_cmpuint(ai_view_agent_block_get_n_live(block), ==, 1);
}

static void
test_no_description_still_renders(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          one = agent("a1", NULL, AI_AGENT_STATE_RUNNING);
	g_autoptr(GList)            agents = g_list_append(NULL, one);
	g_autofree gchar           *text = NULL;

	ai_view_agent_block_set_agents(block, agents);
	text = render(block);

	g_assert_cmpstr(text, ==, "▸ a1  running 0s\n1 running, 1 total");
}

static void
test_multibyte_description(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          one =
		agent("a1", "レビューする", AI_AGENT_STATE_RUNNING);
	g_autoptr(GList)            agents = g_list_append(NULL, one);
	g_autofree gchar           *text = NULL;

	ai_view_agent_block_set_agents(block, agents);
	text = render(block);

	g_assert_true(g_utf8_validate(text, -1, NULL));
	g_assert_nonnull(strstr(text, "レビューする"));
}

/* ----------------------------------------------------------------
 * Spans
 * ---------------------------------------------------------------- */

/*
 * Every span is a byte range into valid UTF-8, sorted and not
 * overlapping --- the invariant the whole style layer rests on, and the
 * one Emacs converts with byte-to-position.
 */
static void
test_spans_are_well_formed(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          one =
		agent("a1", "レビューする", AI_AGENT_STATE_RUNNING);
	g_autoptr(AiAgent)          two = agent("a2", "two", AI_AGENT_STATE_DONE);
	GList                      *agents = NULL;
	g_autoptr(AiRenderedText)   text = NULL;
	const gchar                *raw;
	guint                       i;
	guint                       n;
	guint                       previous_end = 0;

	agents = g_list_append(agents, one);
	agents = g_list_append(agents, two);

	ai_view_agent_block_set_agents(block, agents);
	text = ai_view_block_render(AI_VIEW_BLOCK(block), 0);
	g_list_free(agents);

	raw = ai_rendered_text_get_text(text);
	n = ai_rendered_text_get_n_spans(text);
	g_assert_cmpuint(n, >, 0);

	for (i = 0; i < n; i++)
	{
		guint      start = 0;
		guint      len = 0;
		guint      end;
		AiStyleTag tag = AI_STYLE_DEFAULT;

		g_assert_true(ai_rendered_text_get_span(text, i, &start, &len, &tag));
		end = start + len;

		g_assert_cmpuint(len, >, 0);                /* never empty */
		g_assert_cmpuint(start, >=, previous_end);  /* sorted, disjoint */
		g_assert_cmpuint(end, <=, strlen(raw));

		/* Never splits a character. */
		g_assert_true((raw[start] & 0xC0) != 0x80);
		if (raw[end] != '\0') g_assert_true((raw[end] & 0xC0) != 0x80);

		previous_end = end;
	}
}

/* ----------------------------------------------------------------
 * In place
 * ---------------------------------------------------------------- */

/*
 * Updating rewrites the block; it does not add one.
 *
 * Asserted through the transcript, because that is where the difference
 * shows: ::block-changed redraws one panel, ::items-changed would append
 * a second copy of it.
 */
static void
test_updating_is_in_place(void)
{
	g_autoptr(AiTranscript)     transcript = ai_transcript_new();
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          one =
		agent("a1", "the work", AI_AGENT_STATE_RUNNING);
	g_autoptr(GList)            agents = g_list_append(NULL, one);
	Counter                     counter = { 0 };
	g_autofree gchar           *after = NULL;

	ai_transcript_append(transcript, AI_VIEW_BLOCK(block));
	g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(transcript)), ==, 1);

	g_signal_connect(block, "changed", G_CALLBACK(on_changed), &counter);

	ai_view_agent_block_set_agents(block, agents);
	ai_agent_set_state(one, AI_AGENT_STATE_DONE);
	ai_view_agent_block_set_agents(block, agents);

	g_assert_cmpuint(counter.changed, ==, 2);
	g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(transcript)), ==, 1);

	after = render(block);
	g_assert_nonnull(strstr(after, "✔ a1"));
}

/*
 * The block keeps rendering after the agents are gone.
 *
 * A brigade forgets an agent the moment its answer is collected, and a
 * panel that emptied itself as each result came back would be at its
 * least informative exactly when somebody looks at it. So the rows are
 * copies, not references.
 */
static void
test_details_survive_the_agents(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autofree gchar           *text = NULL;

	{
		g_autoptr(AiAgent) one =
			agent("a1", "the work", AI_AGENT_STATE_DONE);
		g_autoptr(GList)   agents = g_list_append(NULL, one);

		ai_view_agent_block_set_agents(block, agents);
	}

	text = render(block);

	g_assert_nonnull(strstr(text, "a1"));
	g_assert_nonnull(strstr(text, "the work"));
}

static void
test_setting_null_empties_it(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();
	g_autoptr(AiAgent)          one =
		agent("a1", "the work", AI_AGENT_STATE_RUNNING);
	g_autoptr(GList)            agents = g_list_append(NULL, one);
	g_autofree gchar           *text = NULL;

	ai_view_agent_block_set_agents(block, agents);
	ai_view_agent_block_set_agents(block, NULL);

	text = render(block);
	g_assert_cmpstr(text, ==, "No background agents");
}

static void
test_kind_is_reported(void)
{
	g_autoptr(AiViewAgentBlock) block = ai_view_agent_block_new();

	g_assert_cmpint(ai_view_block_get_kind(AI_VIEW_BLOCK(block)), ==,
	                AI_VIEW_BLOCK_AGENT);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/view-agent/empty", test_empty_says_so);
	g_test_add_func("/ai-glib/view-agent/one-running", test_one_running_agent);
	g_test_add_func("/ai-glib/view-agent/markers",
	                test_markers_differ_by_state);
	g_test_add_func("/ai-glib/view-agent/tally",
	                test_tally_counts_only_the_live);
	g_test_add_func("/ai-glib/view-agent/no-description",
	                test_no_description_still_renders);
	g_test_add_func("/ai-glib/view-agent/multibyte",
	                test_multibyte_description);

	g_test_add_func("/ai-glib/view-agent/spans", test_spans_are_well_formed);

	g_test_add_func("/ai-glib/view-agent/in-place", test_updating_is_in_place);
	g_test_add_func("/ai-glib/view-agent/details-survive",
	                test_details_survive_the_agents);
	g_test_add_func("/ai-glib/view-agent/null-empties",
	                test_setting_null_empties_it);
	g_test_add_func("/ai-glib/view-agent/kind", test_kind_is_reported);

	return g_test_run();
}
