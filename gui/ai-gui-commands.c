/*
 * ai-gui-commands.c - Built-in slash commands, in the desktop client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-commands.h"
#include "ai-gui-work.h"

void
ai_gui_commands_say(
	AiGuiSession *session,
	const gchar  *format,
	...
){
	g_autofree gchar *text = NULL;
	g_autoptr(AiViewBlock) block = NULL;
	va_list args;

	g_return_if_fail(AI_GUI_IS_SESSION(session));

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);

	block = ai_view_status_block_new(AI_VIEW_STATUS_INFO, text);
	ai_view_block_set_complete(block, TRUE);
	ai_transcript_append(ai_gui_session_get_transcript(session), block);
}

static void
commands_error(
	AiGuiSession *session,
	const gchar  *format,
	...
){
	g_autofree gchar *text = NULL;
	g_autoptr(AiViewBlock) block = NULL;
	va_list args;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);

	block = ai_view_status_block_new(AI_VIEW_STATUS_ERROR, text);
	ai_view_block_set_complete(block, TRUE);
	ai_transcript_append(ai_gui_session_get_transcript(session), block);
}

/* A path argument is relative to the session, not to wherever ai-gui was
 * started from: a person typing /cwd src means this conversation's src. */
static gchar *
commands_resolve_path(
	AiGuiSession *session,
	const gchar  *path
){
	if (path == NULL || *path == '\0')
		return NULL;

	if (g_path_is_absolute(path))
		return g_strdup(path);

	if (path[0] == '~')
	{
		g_autofree gchar *rest = g_strdup(path + 1);

		return g_build_filename(g_get_home_dir(),
		                        *rest == '/' ? rest + 1 : rest, NULL);
	}

	return g_build_filename(ai_gui_session_get_working_directory(session),
	                        path, NULL);
}

/* ================================================================
 * Listings
 * ================================================================ */

static void
commands_help(AiGuiSession *session)
{
	AiCommandSet *set = ai_gui_session_get_commands(session);
	g_autoptr(GString) text = g_string_new("Commands\n");
	GList *list;
	GList *iter;

	list = ai_command_set_list(set);

	for (iter = list; iter != NULL; iter = iter->next)
	{
		AiCommand *command = iter->data;
		const gchar *hint = ai_command_get_argument_hint(command);
		const gchar *origin = ai_command_get_origin(command);

		g_string_append_printf(text, "  /%s%s%s — %s%s%s\n",
			ai_command_get_name(command),
			hint != NULL ? " " : "", hint != NULL ? hint : "",
			ai_command_get_description(command) != NULL
				? ai_command_get_description(command) : "",
			origin != NULL && *origin != '\0' ? "  [" : "",
			origin != NULL && *origin != '\0' ? origin : "");

		if (origin != NULL && *origin != '\0')
			g_string_append(text, "]");
	}

	g_list_free_full(list, g_object_unref);
	g_string_append(text,
		"\n@path mentions a file. Ctrl+backslash opens the dashboard.");
	ai_gui_commands_say(session, "%s", text->str);
}

static void
commands_resources(
	AiGuiSession   *session,
	AiResourceKind  kind,
	const gchar    *heading
){
	AiCommandSet *set = ai_gui_session_get_commands(session);
	AiResourceRegistry *registry = ai_command_set_get_registry(set);
	g_autoptr(GString) text = g_string_new(heading);
	GList *list;
	GList *iter;
	guint shown = 0;

	g_string_append_c(text, '\n');
	list = ai_resource_registry_list(registry, kind);

	for (iter = list; iter != NULL; iter = iter->next)
	{
		AiResource *resource = iter->data;
		const gchar *description = ai_resource_get_description(resource);

		g_string_append_printf(text, "  %s%s%s\n",
			ai_resource_get_name(resource),
			description != NULL && *description != '\0' ? " — " : "",
			description != NULL ? description : "");
		shown++;
	}

	g_list_free_full(list, g_object_unref);

	if (shown == 0)
		g_string_append(text, "  (none found on the search paths)");

	ai_gui_commands_say(session, "%s", text->str);
}

static void
commands_tools(AiGuiSession *session)
{
	AiToolExecutor *executor =
		ai_conversation_get_executor(ai_gui_session_get_conversation(session));
	g_autoptr(GString) text = g_string_new("Tools\n");
	GList *tools = ai_tool_executor_get_tools(executor);
	GList *iter;

	for (iter = tools; iter != NULL; iter = iter->next)
	{
		g_string_append_printf(text, "  %s — %s\n",
			ai_tool_get_name(iter->data),
			ai_tool_get_description(iter->data) != NULL
				? ai_tool_get_description(iter->data) : "");
	}

	if (tools == NULL)
	{
		g_string_append(text,
			"  (none — a wrapped CLI runs its own tools in its own process)");
	}

	g_list_free(tools);
	ai_gui_commands_say(session, "%s", text->str);
}

static void
commands_todos(AiGuiSession *session)
{
	AiToolExecutor *executor =
		ai_conversation_get_executor(ai_gui_session_get_conversation(session));
	g_autoptr(GString) text = g_string_new("Todos\n");
	guint n = ai_tool_executor_get_n_todos(executor);
	guint i;

	for (i = 0; i < n; i++)
	{
		const gchar *label = NULL;
		AiTodoState state = AI_TODO_PENDING;
		const gchar *mark;

		ai_tool_executor_get_todo_fields(executor, i, &label, &state);
		mark = state == AI_TODO_COMPLETED ? "x"
			: (state == AI_TODO_IN_PROGRESS ? ">" : " ");
		g_string_append_printf(text, "  [%s] %s\n", mark,
		                       label != NULL ? label : "");
	}

	if (n == 0)
		g_string_append(text, "  (the model has not written a plan)");

	ai_gui_commands_say(session, "%s", text->str);
}

static void
commands_running(AiGuiSession *session)
{
	AiBrigade *brigade =
		ai_conversation_get_brigade(ai_gui_session_get_conversation(session));
	g_autoptr(GString) text = g_string_new("Background agents\n");
	GList *list;
	GList *iter;

	if (brigade == NULL)
	{
		ai_gui_commands_say(session,
			"Background agents are off for this session.");
		return;
	}

	list = ai_brigade_list(brigade);

	for (iter = list; iter != NULL; iter = iter->next)
	{
		g_string_append_printf(text, "  %s — %s, %" G_GINT64_FORMAT "s\n",
			ai_agent_get_id(iter->data),
			ai_agent_state_to_string(ai_agent_get_state(iter->data)),
			ai_agent_get_elapsed_ms(iter->data) / 1000);
	}

	if (list == NULL)
		g_string_append(text, "  (none started)");

	g_list_free(list);
	ai_gui_commands_say(session, "%s", text->str);
}

static void
commands_kill(
	AiGuiSession *session,
	const gchar  *arguments
){
	AiBrigade *brigade =
		ai_conversation_get_brigade(ai_gui_session_get_conversation(session));
	AiAgent *agent;

	if (brigade == NULL)
	{
		ai_gui_commands_say(session,
			"Background agents are off for this session.");
		return;
	}

	if (arguments == NULL || *arguments == '\0')
	{
		commands_error(session, "/kill needs an agent id, or all.");
		return;
	}

	if (g_strcmp0(arguments, "all") == 0)
	{
		ai_gui_commands_say(session, "Stopped %u agent(s).",
		                    ai_brigade_cancel_all(brigade));
		return;
	}

	agent = ai_brigade_get(brigade, arguments);

	if (agent == NULL)
	{
		commands_error(session, "No agent named '%s'.", arguments);
		return;
	}

	ai_agent_cancel(agent);
	ai_gui_commands_say(session, "Stopping %s.", arguments);
}

/* ================================================================
 * Linked work
 * ================================================================ */

typedef struct
{
	AiGuiSession *session;
	gboolean      assign;
} LinkFetch;

static void
on_link_fetched(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	LinkFetch *fetch = user_data;
	g_autoptr(AiGuiSession) session = fetch->session;
	gboolean assign = fetch->assign;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *material = NULL;

	g_free(fetch);

	material = ai_work_session_refresh_link_finish(AI_WORK_SESSION(source),
	                                               result, &error);

	if (material == NULL)
	{
		/*
		 * A failed fetch never submits an empty assignment: sending the
		 * model "here is your task:" with nothing after it is worse than
		 * saying the fetch did not work.
		 */
		commands_error(session, "%s", error != NULL
			? error->message : "Could not read that link.");
		return;
	}

	ai_gui_session_publish_work(session);

	if (!assign)
		return;

	{
		g_autoptr(GError) send_error = NULL;

		if (!ai_gui_session_send(session, material, NULL, &send_error))
		{
			commands_error(session, "%s", send_error != NULL
				? send_error->message : "Could not queue the assignment.");
		}
	}
}

static void
commands_fetch_link(
	AiGuiSession *session,
	const gchar  *url,
	gboolean      assign
){
	AiWorkSession *work = ai_gui_session_get_work(session);
	LinkFetch *fetch;

	if (work == NULL)
		return;

	fetch = g_new0(LinkFetch, 1);
	fetch->session = g_object_ref(session);
	fetch->assign = assign;

	ai_work_session_refresh_link_async(work, url, NULL, on_link_fetched,
	                                   fetch);
}

static void
commands_links(
	AiGuiSession *session,
	const gchar  *name,
	const gchar  *arguments
){
	AiWorkSession *work = ai_gui_session_get_work(session);
	g_auto(GStrv) parts = NULL;
	g_autoptr(GError) error = NULL;

	if (work == NULL)
	{
		ai_gui_commands_say(session,
			"This session has not registered with the dashboard yet.");
		return;
	}

	parts = g_strsplit(arguments != NULL ? arguments : "", " ", 2);

	if (g_strcmp0(parts[0], "link") == 0 && parts[1] != NULL)
	{
		g_autofree gchar *url = g_strdup(g_strstrip(parts[1]));

		if (!ai_work_session_add_link(work, url, &error))
		{
			commands_error(session, "%s", error->message);
			return;
		}

		ai_gui_commands_say(session, "Linked %s", url);
		commands_fetch_link(session, url, FALSE);
		ai_gui_session_publish_work(session);
		return;
	}

	if (g_strcmp0(parts[0], "unlink") == 0 && parts[1] != NULL)
	{
		g_autofree gchar *url = g_strdup(g_strstrip(parts[1]));

		ai_gui_commands_say(session, "%s",
			ai_work_session_remove_link(work, url)
				? "Link removed." : "Link not found.");
		ai_gui_session_publish_work(session);
		return;
	}

	{
		g_auto(GStrv) links = ai_work_session_dup_links(work);
		g_autoptr(GString) text = g_string_new("Linked work\n");
		guint i;

		for (i = 0; links[i] != NULL; i++)
		{
			const gchar *state = ai_work_session_get_link_state(work, links[i]);
			const gchar *title = ai_work_session_get_link_title(work, links[i]);

			g_string_append_printf(text, "  %u: %s%s%s%s%s\n", i + 1,
				links[i],
				state != NULL && *state != '\0' ? "  (" : "",
				state != NULL ? state : "",
				state != NULL && *state != '\0' ? ")" : "",
				title != NULL && *title != '\0' ? "" : "");

			if (title != NULL && *title != '\0')
				g_string_append_printf(text, "     %s\n", title);
		}

		if (links[0] == NULL)
		{
			g_string_append(text,
				"  (none) — /issue link URL or /pr link URL");
		}

		(void)name;
		ai_gui_commands_say(session, "%s", text->str);
	}
}

/* ================================================================
 * Entry point
 * ================================================================ */

void
ai_gui_commands_handle(
	AiGuiWindow     *window,
	AiGuiSession    *session,
	AiCommandResult *command
){
	const gchar *name;
	const gchar *arguments;

	g_return_if_fail(AI_GUI_IS_WINDOW(window));
	g_return_if_fail(AI_GUI_IS_SESSION(session));
	g_return_if_fail(command != NULL);

	name = ai_command_result_get_name(command);
	arguments = ai_command_result_get_arguments(command);

	if (name == NULL)
		return;

	if (g_strcmp0(name, "help") == 0 || g_strcmp0(name, "commands") == 0)
		commands_help(session);
	else if (g_strcmp0(name, "skills") == 0)
		commands_resources(session, AI_RESOURCE_SKILL, "Skills");
	else if (g_strcmp0(name, "agents") == 0)
		commands_resources(session, AI_RESOURCE_AGENT, "Agents");
	else if (g_strcmp0(name, "tools") == 0)
		commands_tools(session);
	else if (g_strcmp0(name, "todos") == 0)
		commands_todos(session);
	else if (g_strcmp0(name, "running") == 0)
		commands_running(session);
	else if (g_strcmp0(name, "kill") == 0)
		commands_kill(session, arguments);
	else if (g_strcmp0(name, "clear") == 0 || g_strcmp0(name, "reset") == 0)
	{
		ai_gui_session_clear(session);
		ai_gui_commands_say(session, "Conversation cleared.");
	}
	else if (g_strcmp0(name, "quit") == 0 || g_strcmp0(name, "exit") == 0)
	{
		gtk_window_close(GTK_WINDOW(window));
	}
	else if (g_strcmp0(name, "reload") == 0)
	{
		ai_resource_registry_scan(
			ai_command_set_get_registry(ai_gui_session_get_commands(session)));
		ai_gui_commands_say(session, "Rescanned the command paths.");
	}
	else if (g_strcmp0(name, "cwd") == 0)
	{
		g_autofree gchar *path = commands_resolve_path(session, arguments);

		if (path == NULL)
		{
			ai_gui_commands_say(session, "%s",
				ai_gui_session_get_working_directory(session));
		}
		else if (!g_file_test(path, G_FILE_TEST_IS_DIR))
		{
			commands_error(session, "No such directory: %s", arguments);
		}
		else
		{
			ai_gui_session_set_working_directory(session, path);
			ai_gui_commands_say(session, "Working directory: %s", path);
		}
	}
	else if (g_strcmp0(name, "context") == 0)
	{
		AiConversation *conversation =
			ai_gui_session_get_conversation(session);
		const gchar *carried =
			ai_conversation_get_carried_context(conversation);

		if (g_strcmp0(arguments, "clear") == 0)
		{
			ai_conversation_clear_carried_context(conversation);
			ai_gui_commands_say(session, "Carried context dropped.");
		}
		else if (carried != NULL && *carried != '\0')
		{
			ai_gui_commands_say(session, "Carried context\n%s", carried);
		}
		else
		{
			ai_gui_commands_say(session,
				"No context is being carried from a previous provider.");
		}
	}
	else if (g_strcmp0(name, "expand") == 0)
	{
		g_autoptr(AiCommandResult) resolved = NULL;
		g_autoptr(GError) error = NULL;

		resolved = ai_conversation_resolve_input(
			ai_gui_session_get_conversation(session),
			arguments != NULL ? arguments : "", NULL, &error);

		if (error != NULL)
			commands_error(session, "%s", error->message);
		else if (resolved != NULL && ai_command_result_get_outcome(resolved) ==
		         AI_COMMAND_OUTCOME_BUILTIN)
		{
			ai_gui_commands_say(session,
				"/%s is a built-in; nothing would be sent.",
				ai_command_result_get_name(resolved));
		}
		else
		{
			const gchar *prompt = resolved != NULL
				? ai_command_result_get_prompt(resolved) : arguments;

			ai_gui_commands_say(session, "%s",
			                    prompt != NULL ? prompt : "");
		}
	}
	else if (g_strcmp0(name, "links") == 0 || g_strcmp0(name, "issue") == 0 ||
	         g_strcmp0(name, "pr") == 0)
	{
		commands_links(session, name, arguments);
		ai_gui_window_refresh_links(window);
	}
	else if (g_strcmp0(name, "work") == 0)
	{
		AiWorkSession *work = ai_gui_session_get_work(session);
		g_autoptr(GError) error = NULL;

		if (work == NULL)
		{
			ai_gui_commands_say(session,
				"This session has not registered with the dashboard yet.");
		}
		else if (arguments == NULL || *arguments == '\0')
		{
			commands_error(session, "/work needs an issue or PR URL.");
		}
		else if (!ai_work_session_add_link(work, arguments, &error))
		{
			commands_error(session, "%s", error->message);
		}
		else
		{
			commands_fetch_link(session, arguments, TRUE);
			ai_gui_window_refresh_links(window);
			ai_gui_commands_say(session,
				"Loading the assignment; your draft is preserved.");
		}
	}
	else if (g_strcmp0(name, "dashboard") == 0)
	{
		ai_gui_window_show_dashboard(window,
			!ai_gui_window_get_dashboard(window));
	}
	else if (g_strcmp0(name, "project") == 0)
	{
		if (arguments == NULL || *arguments == '\0')
		{
			ai_gui_window_show_dashboard(window, TRUE);
		}
		else
		{
			g_autofree gchar *path = commands_resolve_path(session, arguments);

			if (path != NULL && g_file_test(path, G_FILE_TEST_IS_DIR))
				ai_gui_window_open_project(window, path);
			else
				commands_error(session, "No such directory: %s", arguments);
		}
	}
	else if (g_strcmp0(name, "save") == 0 || g_strcmp0(name, "export") == 0)
	{
		ai_gui_window_export(window, arguments);
	}
	else if (g_strcmp0(name, "model") == 0 || g_strcmp0(name, "provider") == 0 ||
	         g_strcmp0(name, "effort") == 0)
	{
		/*
		 * The pickers below the composer are how these are chosen here,
		 * and the preferences dialog is where effort lives. Saying so is
		 * more use than reimplementing a list somebody would then have
		 * to type an exact id into.
		 */
		ai_gui_commands_say(session,
			"Provider and model are the two menus under the message box; "
			"they apply to your next question. Effort and every other "
			"provider setting are in Preferences (Ctrl+comma). "
			"This session: %s · %s",
			ai_gui_session_get_provider_id(session),
			ai_gui_session_get_model(session) != NULL
				? ai_gui_session_get_model(session) : "default");
	}
	else if (g_strcmp0(name, "btw") == 0)
	{
		ai_gui_commands_say(session,
			"/btw is an ai-tui feature. Open a second session (Ctrl+N) to "
			"ask something alongside this one.");
	}
	else
	{
		ai_gui_commands_say(session, "/%s is not available here yet.", name);
	}
}
