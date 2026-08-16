/*
 * background-agents.c - Starting work that outlives the turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Runs three agents concurrently against AiMockProvider, watches them,
 * cancels one, and collects the rest --- the whole background-agent path
 * with no network, no API key and no bill.
 *
 * It exists because the arrangement is easier to understand as forty
 * lines that run than as four pages that do not, and because the Emacs
 * side will do exactly this: build a brigade, connect two signals, and
 * read state out of AiAgent.
 *
 *   make examples
 *   ./build/release/examples/background-agents
 *
 * To watch a real model do it instead:
 *
 *   ai-tui -p claude --local-tools
 *   > spawn three agents to each summarise a different file, then compare
 */

#include <ai-glib.h>

#include <stdlib.h>

typedef struct
{
    AiBrigade *brigade;
    GMainLoop *loop;
    guint      outstanding;
} Demo;

/*
 * Progress, as it happens.
 *
 * A frontend draws from exactly this: ::agent-state-changed to update a
 * panel, ::agent-finished to say something. Neither waits for a model
 * turn --- the person watching should not have to send a message to find
 * out that the thing they started has finished.
 */
static void
on_state_changed(
    AiBrigade   *brigade,
    const gchar *agent_id,
    gint         state,
    gpointer     user_data
){
    AiAgent *agent;

    (void)user_data;

    agent = ai_brigade_get(brigade, agent_id);

    g_print("  %-12s %-10s %s\n", agent_id,
            ai_agent_state_to_string((AiAgentState)state),
            agent != NULL && ai_agent_get_description(agent) != NULL
                ? ai_agent_get_description(agent)
                : "");
}

static void
on_agent_finished(
    AiBrigade   *brigade,
    const gchar *agent_id,
    gint         state,
    gpointer     user_data
){
    Demo *demo = user_data;

    (void)brigade;
    (void)state;
    (void)agent_id;

    if (--demo->outstanding == 0)
    {
        g_main_loop_quit(demo->loop);
    }
}

/*
 * Stop an agent that is taking too long.
 *
 * Cancelling is always allowed --- a caller who wants something stopped
 * must never be refused --- and a cancelled agent is `cancelled', never
 * `failed'. Somebody deciding whether to retry needs to know which
 * happened, so the distinction survives to the status listing.
 */
static gboolean
give_up_on_it(gpointer user_data)
{
    ai_agent_cancel(user_data);
    return G_SOURCE_REMOVE;
}

/*
 * One agent, on its own provider.
 *
 * The provider is the whole reason this is interesting: nothing ties an
 * agent to whoever asked for it, so a conversation held with one model
 * can start an agent on another. Here they are mocks; in a real program
 * ai_provider_factory_new_from_string("claude-code", NULL, &error) does
 * the same job.
 */
static AiAgent *
make_agent(
    const gchar *id,
    const gchar *description,
    const gchar *reply,
    guint        delay_ms
){
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    AiAgent                  *agent;

    ai_mock_provider_push_text(mock, reply);
    ai_mock_provider_set_delay_ms(mock, delay_ms);

    agent = ai_agent_new(id, AI_PROVIDER(mock));
    ai_agent_set_description(agent, description);

    return agent;
}

int
main(int argc, char *argv[])
{
    g_autoptr(AiBrigade)     brigade = ai_brigade_new();
    g_autoptr(AiLocalWorker) worker  = ai_local_worker_new();
    g_autoptr(GMainLoop)     loop    = g_main_loop_new(NULL, FALSE);
    g_autoptr(AiAgent)       reviewer = NULL;
    g_autoptr(AiAgent)       auditor  = NULL;
    g_autoptr(AiAgent)       loiterer = NULL;
    Demo                     demo = { 0 };

    (void)argc;
    (void)argv;

    /*
     * A brigade needs a worker to run anything. AiLocalWorker is the one
     * the library ships: it runs agents in this process, asynchronously,
     * on the thread-default main context. Nothing blocks --- an agent
     * started here is a set of callbacks the loop will get round to.
     */
    ai_brigade_set_worker(brigade, AI_AGENT_WORKER(worker));
    ai_brigade_set_max_concurrent(brigade, 2);

    demo.brigade = brigade;
    demo.loop = loop;
    demo.outstanding = 3;

    g_signal_connect(brigade, "agent-state-changed",
                     G_CALLBACK(on_state_changed), &demo);
    g_signal_connect(brigade, "agent-finished",
                     G_CALLBACK(on_agent_finished), &demo);

    reviewer = make_agent("reviewer-1", "read the diff", "Looks fine.", 40);
    auditor  = make_agent("auditor-1", "check the search code",
                          "Two issues.", 80);
    loiterer = make_agent("loiterer-1", "take far too long",
                          "never gets here", 5000);

    g_print("Starting three agents, at most two at a time:\n\n");

    ai_brigade_start(brigade, reviewer, "review it", NULL);
    ai_brigade_start(brigade, auditor, "audit it", NULL);
    ai_brigade_start(brigade, loiterer, "dawdle", NULL);

    /*
     * The third is queued, not dropped. Agents beyond the limit wait
     * their turn and are dispatched in arrival order as slots free.
     */
    g_print("\n%u live, %s is waiting for a slot.\n\n",
            ai_brigade_count_live(brigade),
            ai_agent_state_to_string(ai_agent_get_state(loiterer)));

    /* Give up on the slow one after a moment, the way agent_cancel does. */
    g_timeout_add(400, give_up_on_it, loiterer);

    g_main_loop_run(loop);

    /*
     * Collecting an answer is what ends an agent's life: reap returns the
     * text and forgets the record, so a long session does not accumulate
     * every agent it ever ran.
     */
    g_print("\nResults:\n\n");

    {
        const gchar *ids[] = { "reviewer-1", "auditor-1", "loiterer-1" };
        gsize        i;

        for (i = 0; i < G_N_ELEMENTS(ids); i++)
        {
            g_autoptr(GError) error = NULL;
            g_autofree gchar *text =
                ai_brigade_reap(brigade, ids[i], &error);

            g_print("  %-12s %s\n", ids[i],
                    text != NULL ? text : error->message);
        }
    }

    g_print("\n%u agents left registered.\n",
            g_list_length(ai_brigade_list(brigade)));

    return EXIT_SUCCESS;
}
