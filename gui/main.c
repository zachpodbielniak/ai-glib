/*
 * main.c - ai-gui, the GTK4 desktop client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Deliberately thin, like ai-tui's own main: parse the command line,
 * build an AdwApplication, hand the options to a window. Everything the
 * window then does comes out of the library's view and harness layers.
 */

#include <stdlib.h>

#include "ai-gui.h"
#include "ai-gui-settings.h"
#include "ai-gui-style.h"

#include "core/ai-theme.h"

static gchar    *opt_provider = NULL;
static gchar    *opt_model = NULL;
static gchar    *opt_system = NULL;
static gchar    *opt_effort = NULL;
static gchar    *opt_directory = NULL;
static gchar   **opt_set = NULL;
static gint      opt_max_tokens = 4096;
static gboolean  opt_no_stream = FALSE;
static gboolean  opt_continue = FALSE;
static gboolean  opt_skip_permissions = FALSE;
static gboolean  opt_local_tools = FALSE;
static gboolean  opt_yes = FALSE;
static gboolean  opt_no_expand = FALSE;
static gboolean  opt_no_agents = FALSE;
static gboolean  opt_dashboard = FALSE;
static gboolean  opt_no_dashboard = FALSE;
static gchar    *opt_theme = NULL;
static gchar    *opt_color_scheme = NULL;
static gboolean  opt_list_themes = FALSE;
static gchar   **opt_attach = NULL;
static gboolean  opt_version = FALSE;
static gboolean  opt_license = FALSE;

static const GOptionEntry option_entries[] = {
	{ "provider", 'p', 0, G_OPTION_ARG_STRING, &opt_provider,
	  "Provider: claude, openai, gemini, grok, ollama, claude-code, "
	  "claude-tmux, opencode, grok-build, antigravity, cursor, codex-cli "
	  "(omitted: saved ai-gui defaults)", "NAME" },
	{ "model", 'm', 0, G_OPTION_ARG_STRING, &opt_model,
	  "Model id (omitted: the saved ai-gui model, else the native default)",
	  "MODEL" },
	{ "system", 's', 0, G_OPTION_ARG_STRING, &opt_system,
	  "System prompt", "TEXT" },
	{ "effort", 0, 0, G_OPTION_ARG_STRING, &opt_effort,
	  "Reasoning effort: low, medium, high, xhigh, max", "LEVEL" },
	{ "directory", 'C', 0, G_OPTION_ARG_FILENAME, &opt_directory,
	  "Working directory for mentions and tools (default: this one)", "DIR" },
	{ "max-tokens", 0, 0, G_OPTION_ARG_INT, &opt_max_tokens,
	  "Maximum tokens per response (default 4096)", "N" },
	{ "no-stream", 0, 0, G_OPTION_ARG_NONE, &opt_no_stream,
	  "Wait for each whole turn instead of streaming it", NULL },
	{ "continue", 'c', 0, G_OPTION_ARG_NONE, &opt_continue,
	  "Continue the provider's most recent session", NULL },
	{ "skip-permissions", 0, 0, G_OPTION_ARG_NONE, &opt_skip_permissions,
	  "Let a wrapped CLI run its tools without asking", NULL },
	{ "local-tools", 0, 0, G_OPTION_ARG_NONE, &opt_local_tools,
	  "Run tools in this process (HTTP providers only)", NULL },
	{ "yes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes,
	  "Approve every local tool call without asking", NULL },
	{ "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_set,
	  "Set a provider property (repeatable)", "PROP=VALUE" },
	{ "no-expand", 0, 0, G_OPTION_ARG_NONE, &opt_no_expand,
	  "Send input verbatim: no @ mentions, no / commands", NULL },
	{ "no-agents", 0, 0, G_OPTION_ARG_NONE, &opt_no_agents,
	  "Do not let the model start background agents", NULL },
	{ "dashboard", 0, 0, G_OPTION_ARG_NONE, &opt_dashboard,
	  "Open the project dashboard instead of a conversation", NULL },
	{ "no-dashboard", 0, 0, G_OPTION_ARG_NONE, &opt_no_dashboard,
	  "Open a conversation even when the saved preference says otherwise",
	  NULL },
	{ "attach", 'a', 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_attach,
	  "Attach a file to the first message (repeatable)", "FILE" },
	{ "theme", 0, 0, G_OPTION_ARG_STRING, &opt_theme,
	  "Colour theme (overrides AI_GUI_THEME and NO_COLOR)", "NAME" },
	{ "list-themes", 0, 0, G_OPTION_ARG_NONE, &opt_list_themes,
	  "List colour themes and exit", NULL },
	{ "color-scheme", 0, 0, G_OPTION_ARG_STRING, &opt_color_scheme,
	  "Light or dark: system, light, dark", "NAME" },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &opt_version,
	  "Print the version and exit", NULL },
	{ "license", 0, 0, G_OPTION_ARG_NONE, &opt_license,
	  "Print licensing information and exit", NULL },
	{ NULL, 0, 0, 0, NULL, NULL, NULL }
};

static const gchar *description_text =
	"\n"
	"Examples:\n"
	"  # Open on the saved ai-gui defaults\n"
	"  ai-gui\n"
	"\n"
	"  # A specific provider and model\n"
	"  ai-gui -p claude-code -m claude-opus-5\n"
	"\n"
	"  # Resume whatever the wrapped CLI was last doing\n"
	"  ai-gui -p codex-cli --continue\n"
	"\n"
	"  # An HTTP provider running this process's own tools, unattended\n"
	"  ai-gui -p claude --local-tools --yes\n"
	"\n"
	"  # Any provider property, the same names `ai --set` takes\n"
	"  ai-gui -p grok-build --set reasoning-effort=high\n"
	"\n"
	"  # Every ai-gui and ai-tui session on this machine, at a glance\n"
	"  ai-gui --dashboard\n"
	"\n"
	"  # The same palettes ai-tui draws, and the desktop's own\n"
	"  ai-gui --theme nord\n"
	"  ai-gui --theme terminal --color-scheme dark\n"
	"\n"
	"Sessions are kept under $XDG_DATA_HOME/ai-glib/gui/sessions and\n"
	"restored on the next start. A restored transcript is a record to\n"
	"read; where the provider keeps its own session, that session is\n"
	"resumed and carries the context.\n";

static const gchar *license_text =
	"ai-gui, part of ai-glib.\n"
	"Copyright (C) 2026\n"
	"\n"
	"This program is free software: you can redistribute it and/or modify\n"
	"it under the terms of the GNU Affero General Public License as\n"
	"published by the Free Software Foundation, either version 3 of the\n"
	"License, or (at your option) any later version.\n"
	"\n"
	"This program is distributed in the hope that it will be useful, but\n"
	"WITHOUT ANY WARRANTY; without even the implied warranty of\n"
	"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU\n"
	"Affero General Public License for more details.\n"
	"\n"
	"You should have received a copy of the GNU Affero General Public\n"
	"License along with this program. If not, see\n"
	"<https://www.gnu.org/licenses/>.\n";

/*
 * The appearance this run starts in.
 *
 * Resolved once in main() rather than read again in on_activate(),
 * because a second activation raises the window that is already open
 * and must not re-theme it out from under somebody who has since
 * changed it.
 */
static gchar *startup_theme;
static gchar *startup_color_scheme;

/*
 * --theme beats AI_GUI_THEME beats the saved choice beats the default,
 * and NO_COLOR only gets a say when none of the three above spoke. That
 * is the order ai-tui uses, including the part where naming a theme
 * explicitly overrides NO_COLOR: somebody who typed `--theme nord`
 * asked for colour.
 */
static void
resolve_appearance(void)
{
	g_autoptr(AiGuiSettings) saved = ai_gui_settings_load(NULL);
	const gchar *requested = opt_theme != NULL ? opt_theme
		: g_getenv("AI_GUI_THEME");

	if (requested != NULL && *requested != '\0' &&
	    ai_theme_find(requested, NULL) == NULL)
	{
		/*
		 * g_message, not a failure: the window still opens. Refusing to
		 * start over a misspelled theme would be a poor trade, and the
		 * line is printed so the misspelling is not a mystery.
		 */
		g_message("ai-gui: unknown theme '%s'; use --list-themes", requested);
		requested = NULL;
	}

	if (requested == NULL || *requested == '\0')
	{
		requested = g_getenv("NO_COLOR") != NULL ? "monochrome"
			: saved->theme;
	}

	startup_theme = g_strdup(requested);
	startup_color_scheme = g_strdup(opt_color_scheme != NULL
		? opt_color_scheme : saved->color_scheme);
}

static void
on_activate(
	GApplication *app,
	gpointer      user_data
){
	AiGuiOptions *options = user_data;
	GtkWindow *existing = gtk_application_get_active_window(
		GTK_APPLICATION(app));

	/*
	 * A second launch raises the window that is already open rather than
	 * opening another. Two windows would each hold their own store and
	 * write over each other's session files at shutdown.
	 */
	if (existing != NULL)
	{
		gtk_window_present(existing);
		return;
	}

	ai_gui_style_init(startup_theme, startup_color_scheme);

	{
		AiGuiWindow *window = ai_gui_window_new(ADW_APPLICATION(app), options);

		gtk_window_present(GTK_WINDOW(window));

		/*
		 * Attached after the window is up, so a file that cannot be
		 * read reports itself in the window rather than on a terminal
		 * nobody launched this from.
		 */
		ai_gui_window_attach_files(window,
		                           (const gchar * const *)opt_attach);
	}
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(AdwApplication) app = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGuiOptions) options = NULL;
	gint status;

	context = g_option_context_new("- a desktop front-end for ai-glib");
	g_option_context_add_main_entries(context, option_entries, NULL);
	g_option_context_set_description(context, description_text);

	if (!g_option_context_parse(context, &argc, &argv, &error))
	{
		g_printerr("ai-gui: %s\n", error->message);
		return 2;
	}

	if (opt_version)
	{
		g_print("ai-gui %s (ai-glib)\n", AI_GLIB_VERSION_STRING);
		return 0;
	}

	if (opt_license)
	{
		g_print("%s", license_text);
		return 0;
	}

	if (opt_list_themes)
	{
		guint i;

		for (i = 0; i < ai_theme_count(); i++)
			g_print("%s%s\n", ai_theme_get(i)->name, i == 0 ? " (default)" : "");

		g_print("\n`terminal` (also spelled `system`) follows the desktop "
		        "theme and\n--color-scheme; the named palettes carry their "
		        "own light or dark.\n");
		return 0;
	}

	options = g_new0(AiGuiOptions, 1);
	options->provider = g_strdup(opt_provider);
	options->model = g_strdup(opt_model);
	options->system = g_strdup(opt_system);
	options->effort = g_strdup(opt_effort);
	options->working_directory = opt_directory != NULL
		? g_strdup(opt_directory) : g_get_current_dir();
	options->sets = opt_set != NULL ? g_strdupv(opt_set) : NULL;
	options->max_tokens = opt_max_tokens;
	options->stream = !opt_no_stream;
	options->continue_session = opt_continue;
	options->skip_permissions = opt_skip_permissions;
	options->local_tools = opt_local_tools;
	options->approve_all = opt_yes;
	options->expand = !opt_no_expand;
	options->agents = !opt_no_agents;

	/*
	 * The saved preference applies only to a bare launch.
	 *
	 * --continue or an explicit `--set session-id=` means somebody has
	 * already said which conversation they want, and opening a list of
	 * all of them instead would be answering a question they did not
	 * ask. --dashboard says it outright and wins either way.
	 */
	{
		g_autoptr(AiConfig) config = ai_config_new();
		gboolean explicit_session = opt_continue;
		gsize i;

		for (i = 0; opt_set != NULL && opt_set[i] != NULL; i++)
		{
			if (g_str_has_prefix(opt_set[i], "session-id=") ||
			    g_str_has_prefix(opt_set[i], "continue-session"))
			{
				explicit_session = TRUE;
			}
		}

		options->dashboard = opt_dashboard ||
			(ai_config_get_app_dashboard(config, "ai-gui") &&
			 !opt_no_dashboard && !explicit_session);
	}

	resolve_appearance();

	app = adw_application_new(AI_GUI_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), options);

	/*
	 * Options are parsed here rather than handed to GApplication, so the
	 * argv the toolkit sees is empty. A leftover word would otherwise be
	 * treated as a file to open, which this application does not do.
	 */
	status = g_application_run(G_APPLICATION(app), 0, NULL);

	g_free(opt_provider);
	g_free(opt_model);
	g_free(opt_system);
	g_free(opt_effort);
	g_free(opt_directory);
	g_free(opt_theme);
	g_free(opt_color_scheme);
	g_strfreev(opt_attach);
	g_free(startup_theme);
	g_free(startup_color_scheme);
	g_strfreev(opt_set);

	return status;
}
