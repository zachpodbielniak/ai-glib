/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * ai-project-cli.h - `ai project ...`
 *
 * Reads the work registry ai-tui and ai-gui publish to, grouped by
 * AiProject. Every word it prints --- the project name, the order, the
 * status --- comes from the library, so a shell, the TUI dashboard and
 * the GUI sidebar name and rank the same projects the same way.
 *
 * Read-only: nothing here writes the registry. `--registry DIR` exists
 * for inspecting somebody else's state directory.
 *
 * A subcommand rather than a flag, following `ai loop`: a prompt that is
 * literally "project" still works as `ai -- project`.
 */
#ifndef AI_PROJECT_CLI_H
#define AI_PROJECT_CLI_H

#include <string.h>

#include <ai-glib.h>
#include <json-glib/json-glib.h>

static void
project_cli_usage(FILE *out)
{
	fputs("Usage:\n"
	      "  ai project [list] [--json] [--registry DIR]\n"
	      "  ai project show [NAME|PATH] [--json] [--registry DIR]\n"
	      "\n"
	      "Projects are the work sessions ai-tui and ai-gui record, grouped by\n"
	      "repository, worktrees included. list puts the project that needs you\n"
	      "most first. show takes a name, or any path inside a project; with\n"
	      "neither it shows the project of the current directory.\n", out);
}

/* The counts a person acts on, in words: what needs them first. */
static gchar *
project_cli_summary(AiProject *project)
{
	guint sessions = ai_project_get_session_count(project);
	guint attention = ai_project_get_attention_count(project);
	guint busy = ai_project_get_busy_count(project);
	g_autoptr(GString) text = g_string_new(NULL);

	g_string_append_printf(text, "%u session%s", sessions, sessions == 1 ? "" : "s");
	if (attention > 0) g_string_append_printf(text, ", %u needs you", attention);
	if (busy > 0) g_string_append_printf(text, ", %u working", busy);
	return g_string_free(g_steal_pointer(&text), FALSE);
}

static void
project_cli_add_project(JsonBuilder *builder, AiProject *project, gboolean sessions)
{
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, ai_project_get_id(project));
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, ai_project_get_name(project));
	json_builder_set_member_name(builder, "root");
	json_builder_add_string_value(builder, ai_project_get_root(project));
	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, ai_project_get_status(project));
	json_builder_set_member_name(builder, "session_count");
	json_builder_add_int_value(builder, ai_project_get_session_count(project));
	json_builder_set_member_name(builder, "live_count");
	json_builder_add_int_value(builder, ai_project_get_live_count(project));
	json_builder_set_member_name(builder, "busy_count");
	json_builder_add_int_value(builder, ai_project_get_busy_count(project));
	json_builder_set_member_name(builder, "attention_count");
	json_builder_add_int_value(builder, ai_project_get_attention_count(project));
	if (sessions)
	{
		g_autoptr(GPtrArray) rows = ai_project_dup_sessions(project);
		const gchar *fields[] = { "title", "status", "directory", "branch", "provider", "model", NULL };
		guint i, f;

		json_builder_set_member_name(builder, "sessions");
		json_builder_begin_array(builder);
		for (i = 0; i < rows->len; i++)
		{
			AiWorkSession *row = g_ptr_array_index(rows, i);

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "id");
			json_builder_add_string_value(builder, ai_work_session_get_id(row));
			for (f = 0; fields[f] != NULL; f++)
			{
				json_builder_set_member_name(builder, fields[f]);
				json_builder_add_string_value(builder, ai_work_session_get_field(row, fields[f]));
			}
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
	}
	json_builder_end_object(builder);
}

static void
project_cli_print_json(JsonBuilder *builder)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = json_builder_get_root(builder);
	g_autofree gchar *text = NULL;

	json_generator_set_pretty(generator, TRUE);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);
	g_print("%s\n", text);
}

static gint
project_cli_list(GList *projects, gboolean json)
{
	GList *l;

	if (json)
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "schema_version");
		json_builder_add_int_value(builder, 1);
		json_builder_set_member_name(builder, "projects");
		json_builder_begin_array(builder);
		for (l = projects; l != NULL; l = l->next)
			project_cli_add_project(builder, l->data, FALSE);
		json_builder_end_array(builder);
		json_builder_end_object(builder);
		project_cli_print_json(builder);
		return 0;
	}
	if (projects == NULL)
	{
		g_print("No projects yet. Sessions from ai-tui and ai-gui appear here.\n");
		return 0;
	}
	for (l = projects; l != NULL; l = l->next)
	{
		g_autofree gchar *summary = project_cli_summary(l->data);

		g_print("%-20s %-12s %-32s %s\n", ai_project_get_name(l->data),
			ai_project_get_status(l->data), summary, ai_project_get_root(l->data));
	}
	return 0;
}

static gint
project_cli_show(AiProject *project, gboolean json)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *summary = NULL;
	guint i;

	if (json)
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "schema_version");
		json_builder_add_int_value(builder, 1);
		json_builder_set_member_name(builder, "project");
		project_cli_add_project(builder, project, TRUE);
		json_builder_end_object(builder);
		project_cli_print_json(builder);
		return 0;
	}
	summary = project_cli_summary(project);
	g_print("%s  %s\n%s, %s\n\n", ai_project_get_name(project), ai_project_get_root(project),
		ai_project_get_status(project), summary);
	rows = ai_project_dup_sessions(project);
	for (i = 0; i < rows->len; i++)
	{
		AiWorkSession *row = g_ptr_array_index(rows, i);
		const gchar *title = ai_work_session_get_field(row, "title");
		const gchar *model = ai_work_session_get_field(row, "model");

		g_print("  %-12s %s\n               %s (%s)  %s%s%s\n", ai_work_session_get_field(row, "status"),
			*title ? title : ai_work_session_get_field(row, "provider"),
			ai_work_session_get_field(row, "directory"), ai_work_session_get_field(row, "branch"),
			ai_work_session_get_field(row, "provider"), *model ? "/" : "", model);
	}
	return 0;
}

static gint
project_cli_main(gint argc, gchar **argv)
{
	g_autofree gchar *registry = NULL;
	g_autoptr(GPtrArray) words = g_ptr_array_new();
	g_autolist(AiProject) projects = NULL;
	g_autoptr(GError) error = NULL;
	gboolean json = FALSE;
	const gchar *verb;
	gint i;

	for (i = 1; i < argc; i++)
	{
		if (g_strcmp0(argv[i], "--json") == 0)
			json = TRUE;
		else if (g_strcmp0(argv[i], "--registry") == 0 && i + 1 < argc)
		{
			g_free(registry); registry = g_strdup(argv[++i]);
		}
		else if (g_str_has_prefix(argv[i], "--registry="))
		{
			g_free(registry); registry = g_strdup(argv[i] + strlen("--registry="));
		}
		else if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0)
		{
			project_cli_usage(stdout);
			return 0;
		}
		else
			g_ptr_array_add(words, argv[i]);
	}
	verb = words->len > 0 ? g_ptr_array_index(words, 0) : "list";
	if ((g_strcmp0(verb, "list") != 0 && g_strcmp0(verb, "show") != 0) ||
	    (g_strcmp0(verb, "list") == 0 && words->len > 1) || words->len > 2)
	{
		project_cli_usage(stderr);
		return 2;
	}
	if (registry == NULL)
		registry = ai_work_session_default_directory();
	projects = ai_project_list(registry, &error);
	if (error != NULL)
	{
		g_printerr("ai: cannot read the work registry %s: %s\n", registry, error->message);
		return 1;
	}
	if (g_strcmp0(verb, "list") == 0)
		return project_cli_list(projects, json);
	{
		/* No argument: the project of the directory the shell is in. */
		g_autofree gchar *here = words->len > 1 ? NULL : g_get_current_dir();
		const gchar *query = words->len > 1 ? g_ptr_array_index(words, 1) : here;
		g_autoptr(AiProject) project = ai_project_find(projects, query, &error);

		if (project == NULL)
		{
			g_printerr("ai: %s\n", error->message);
			return 1;
		}
		return project_cli_show(project, json);
	}
}

#endif /* AI_PROJECT_CLI_H */
