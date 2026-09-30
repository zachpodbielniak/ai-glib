/*
 * ai-gui-agents.c - What the background agents are doing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-gui-agents.h"

typedef struct
{
	AiGuiSession *session;
	GtkWidget    *group;
	GPtrArray    *rows;
	GSource      *tick;
} AiGuiAgents;

static void agents_rebuild(AiGuiAgents *agents);

static void
agents_free(
	gpointer  data,
	GClosure *closure
){
	AiGuiAgents *agents = data;

	if (agents->tick != NULL)
	{
		/* Through the GSource, never an id: see the main-context note in
		 * AGENTS.md for why an id from one context names a different
		 * source in another. */
		g_source_destroy(agents->tick);
		g_clear_pointer(&agents->tick, g_source_unref);
	}

	g_clear_object(&agents->session);
	g_clear_pointer(&agents->rows, g_ptr_array_unref);
	g_free(agents);
}

/*
 * The rows this panel added, remembered rather than rediscovered.
 *
 * An #AdwPreferencesGroup does not hold its rows as its own children --
 * gtk_widget_get_first_child() returns the group's internal box -- so
 * walking the children to clear it removes nothing at all, and a
 * once-a-second refresh then stacks a fresh copy of the list on top of
 * the last one until the dialog is a wall of identical rows.
 */
static void
agents_clear_rows(AiGuiAgents *agents)
{
	guint i;

	for (i = 0; i < agents->rows->len; i++)
	{
		adw_preferences_group_remove(ADW_PREFERENCES_GROUP(agents->group),
		                             g_ptr_array_index(agents->rows, i));
	}

	g_ptr_array_set_size(agents->rows, 0);
}

static void
agents_add_row(
	AiGuiAgents *agents,
	GtkWidget   *row
){
	g_ptr_array_add(agents->rows, row);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(agents->group), row);
}

static void
on_cancel_agent(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiAgents *agents = user_data;
	const gchar *id = g_object_get_data(G_OBJECT(button), "ai-agent-id");
	AiBrigade *brigade;
	AiAgent *agent;

	brigade = ai_conversation_get_brigade(
		ai_gui_session_get_conversation(agents->session));

	if (brigade == NULL || id == NULL)
		return;

	agent = ai_brigade_get(brigade, id);

	if (agent != NULL)
		ai_agent_cancel(agent);

	agents_rebuild(agents);
}

static void
on_cancel_all(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiAgents *agents = user_data;
	AiBrigade *brigade = ai_conversation_get_brigade(
		ai_gui_session_get_conversation(agents->session));

	if (brigade != NULL)
		ai_brigade_cancel_all(brigade);

	agents_rebuild(agents);
}

static void
agents_rebuild(AiGuiAgents *agents)
{
	AiBrigade *brigade;
	GList *list;
	GList *iter;
	guint shown = 0;

	agents_clear_rows(agents);

	brigade = ai_conversation_get_brigade(
		ai_gui_session_get_conversation(agents->session));

	if (brigade == NULL)
	{
		GtkWidget *row = adw_action_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
			"Background agents are off for this session");
		agents_add_row(agents, row);
		return;
	}

	list = ai_brigade_list(brigade);

	for (iter = list; iter != NULL; iter = iter->next)
	{
		AiAgent *agent = iter->data;
		GtkWidget *row = adw_action_row_new();
		g_autofree gchar *subtitle = NULL;
		const gchar *description = ai_agent_get_description(agent);
		AiAgentState state = ai_agent_get_state(agent);

		subtitle = g_strdup_printf("%s · %" G_GINT64_FORMAT " s%s%s",
			ai_agent_state_to_string(state),
			ai_agent_get_elapsed_ms(agent) / 1000,
			description != NULL && *description != '\0' ? " · " : "",
			description != NULL ? description : "");

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              ai_agent_get_id(agent));
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

		if (state == AI_AGENT_STATE_QUEUED ||
		    state == AI_AGENT_STATE_STARTING ||
		    state == AI_AGENT_STATE_RUNNING ||
		    state == AI_AGENT_STATE_WAITING_INPUT ||
		    state == AI_AGENT_STATE_BLOCKED)
		{
			GtkWidget *cancel =
				gtk_button_new_from_icon_name("process-stop-symbolic");

			gtk_widget_add_css_class(cancel, "flat");
			gtk_widget_set_valign(cancel, GTK_ALIGN_CENTER);
			g_object_set_data_full(G_OBJECT(cancel), "ai-agent-id",
			                       g_strdup(ai_agent_get_id(agent)), g_free);
			g_signal_connect(cancel, "clicked",
			                 G_CALLBACK(on_cancel_agent), agents);
			adw_action_row_add_suffix(ADW_ACTION_ROW(row), cancel);
		}

		agents_add_row(agents, row);
		shown++;
	}

	g_list_free(list);

	if (shown == 0)
	{
		GtkWidget *row = adw_action_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              "No agents have been started");
		agents_add_row(agents, row);
	}
}

static gboolean
on_tick(gpointer user_data)
{
	agents_rebuild(user_data);
	return G_SOURCE_CONTINUE;
}

static void
on_closed(
	AdwDialog *dialog,
	gpointer   user_data
){
}

void
ai_gui_agents_present(
	GtkWidget    *parent,
	AiGuiSession *session
){
	AiGuiAgents *agents;
	AdwDialog *dialog;
	AdwPreferencesPage *page;
	GtkWidget *toolbar;

	g_return_if_fail(AI_GUI_IS_SESSION(session));

	agents = g_new0(AiGuiAgents, 1);
	agents->session = g_object_ref(session);
	agents->rows = g_ptr_array_new();

	dialog = adw_dialog_new();
	adw_dialog_set_title(dialog, "Background agents");
	adw_dialog_set_content_width(dialog, 560);
	adw_dialog_set_content_height(dialog, 480);

	page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
	agents->group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(agents->group),
	                                "Agents");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(agents->group),
		"A spawned agent cannot spawn further agents: one level of "
		"fan-out is delegation, unattended recursion is a fork bomb "
		"that bills.");

	{
		GtkWidget *cancel = gtk_button_new_with_label("Stop all");

		gtk_widget_add_css_class(cancel, "destructive-action");
		gtk_widget_set_valign(cancel, GTK_ALIGN_CENTER);
		g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_all), agents);
		adw_preferences_group_set_header_suffix(
			ADW_PREFERENCES_GROUP(agents->group), cancel);
	}

	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(agents->group));

	toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
	                             adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(page));
	adw_dialog_set_child(dialog, toolbar);

	agents_rebuild(agents);

	/* Polled rather than driven by a signal: the brigade reports a
	 * finish, not each turn an agent takes, and elapsed time moves on
	 * its own. One second is slower than anybody reads. */
	agents->tick = g_timeout_source_new_seconds(1);
	g_source_set_callback(agents->tick, on_tick, agents, NULL);
	g_source_attach(agents->tick, g_main_context_get_thread_default());

	g_signal_connect_data(dialog, "closed", G_CALLBACK(on_closed), agents,
	                      agents_free, 0);

	adw_dialog_present(dialog, parent);
}
