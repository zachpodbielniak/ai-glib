/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * ai-loop-cli.h - `ai loop ...` and `ai goal ...`
 *
 * Reads and edits the same per-session schedule files ai-tui and ai-gui
 * run, under $XDG_STATE_HOME/ai-glib/sessions/loops. A session that is
 * running picks an edit up on its next tick (within a second); one that
 * is not applies it when it is resumed.
 *
 * Every word shown -- state names, "next in 4m", "turn 3/20" -- comes
 * from the library, so this prints what the TUI panel and the GUI view
 * print. And every edit goes through the same parser as the slash
 * command, so `ai loop edit ID --every 10m` means exactly what
 * `/loop edit ID --every 10m` does.
 *
 * Subcommands rather than flags, following `ai decide`: a prompt that is
 * literally "loop" still works as `ai -- loop`.
 */
#ifndef AI_LOOP_CLI_H
#define AI_LOOP_CLI_H

#include <string.h>
#include <unistd.h>

#include <ai-glib.h>
#include <json-glib/json-glib.h>

typedef struct
{
	gchar          *owner;
	gchar          *path;
	AiLoopSchedule *schedule;
	gboolean        running;
	gchar          *directory;
} LoopCliSession;

static void
loop_cli_session_free(gpointer data)
{
	LoopCliSession *session = data;

	g_free(session->owner);
	g_free(session->path);
	g_free(session->directory);
	g_clear_object(&session->schedule);
	g_free(session);
}

static void
loop_cli_usage(const gchar *kind, FILE *stream)
{
	gboolean goal = g_strcmp0(kind, "goal") == 0;

	fprintf(stream,
		"Usage:\n"
		"  ai %s list [--json] [--session ID]\n"
		"  ai %s show ID\n"
		"  ai %s pause|resume|run|delete ID|all [--session ID]\n",
		kind, kind, kind);

	if (goal)
	{
		fprintf(stream,
			"  ai goal stop ID|all [--session ID]\n"
			"  ai goal edit ID [--turns N] [--time SPAN] [--condition TEXT]\n"
			"  ai goal add --session ID CONDITION [--turns N] [--time SPAN]\n"
			"\n"
			"A goal keeps taking turns in its session until the condition holds,\n"
			"for at most --turns (default 20, max 200) and --time (default 2h, max 7d).\n"
			"States: active, paused, met, failed, stopped, expired.\n");
	}
	else
	{
		fprintf(stream,
			"  ai loop edit ID [--every INTERVAL | --self-paced] [--prompt TEXT]\n"
			"  ai loop add --session ID [INTERVAL] PROMPT|/command\n"
			"\n"
			"A loop repeats a prompt or /command in its session: every INTERVAL\n"
			"(1m floor, rounded to what cron can express) or at a pace the model\n"
			"picks. Loops expire 7 days after they are created.\n");
	}

	fprintf(stream,
		"\n"
		"ID is an id or an unambiguous prefix of one. `all` needs --session.\n"
		"A running session picks an edit up within a second; a closed one when\n"
		"it is resumed. --json prints {\"sessions\": [{\"session\", \"running\",\n"
		"\"directory\", \"entries\": [...]}]}.\n");
}

static gboolean
loop_cli_is_running(const gchar *store, const gchar *owner)
{
	g_autoptr(GError) error = NULL;
	gint              fd = ai_loop_store_claim(store, owner, &error);

	if (fd >= 0)
	{
		close(fd);
		return FALSE;
	}

	return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY);
}

/* The directory a session was working in, from the dashboard registry,
 * when a front-end published which schedule it owns. */
static gchar *
loop_cli_directory(GList *records, const gchar *owner)
{
	GList *link;

	for (link = records; link != NULL; link = link->next)
	{
		AiWorkSession *work = link->data;

		if (g_strcmp0(ai_work_session_get_field(work, "loop-owner"), owner) == 0)
		{
			const gchar *directory = ai_work_session_get_field(work, "directory");
			return directory != NULL && directory[0] != '\0' ? g_strdup(directory) : NULL;
		}
	}

	return NULL;
}

static GPtrArray *
loop_cli_sessions(const gchar *store, const gchar *only, gint64 now)
{
	GPtrArray         *sessions = g_ptr_array_new_with_free_func(loop_cli_session_free);
	g_auto(GStrv)      owners = ai_loop_store_list_owners(store);
	g_autofree gchar  *registry = ai_work_session_default_directory();
	GList             *records = ai_work_session_list(registry, NULL);
	guint              i;

	/* --session is an exact id, or a prefix when nothing matches exactly. */
	if (only != NULL && g_strv_contains((const gchar *const *)owners, only))
	{
		g_strfreev(owners);
		owners = g_new0(gchar *, 2);
		owners[0] = g_strdup(only);
	}

	for (i = 0; owners[i] != NULL; i++)
	{
		LoopCliSession   *session;
		g_autoptr(GError) error = NULL;

		if (only != NULL && !g_str_has_prefix(owners[i], only))
		{
			continue;
		}

		session = g_new0(LoopCliSession, 1);
		session->owner = g_strdup(owners[i]);
		session->path = ai_loop_store_path(store, owners[i]);
		session->schedule = ai_loop_schedule_new();

		if (ai_loop_schedule_load(session->schedule, session->path, now, &error) < 0)
		{
			/* One unreadable file costs itself; the rest still list. */
			g_printerr("ai: skipping session %s: %s\n", owners[i], error->message);
			loop_cli_session_free(session);
			continue;
		}

		session->running = loop_cli_is_running(store, owners[i]);
		session->directory = loop_cli_directory(records, owners[i]);
		g_ptr_array_add(sessions, session);
	}

	g_list_free_full(records, g_object_unref);
	return sessions;
}

static guint
loop_cli_count(AiLoopSchedule *schedule, AiLoopKind kind)
{
	guint i;
	guint n = 0;

	for (i = 0; i < ai_loop_schedule_get_n_tasks(schedule); i++)
	{
		if (ai_loop_schedule_get_kind(schedule, i) == kind)
		{
			n++;
		}
	}

	return n;
}

static int
loop_cli_list(GPtrArray *sessions, AiLoopKind kind, gboolean json, gint64 now)
{
	guint i;

	if (json)
	{
		g_autoptr(JsonBuilder)   builder = json_builder_new();
		g_autoptr(JsonGenerator) generator = json_generator_new();
		g_autoptr(JsonNode)      root = NULL;
		g_autofree gchar        *text = NULL;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, ai_loop_kind_to_string(kind));
		json_builder_set_member_name(builder, "sessions");
		json_builder_begin_array(builder);

		for (i = 0; i < sessions->len; i++)
		{
			LoopCliSession       *session = g_ptr_array_index(sessions, i);
			g_autofree gchar     *entries = ai_loop_schedule_dup_json(session->schedule, now);
			g_autoptr(JsonParser) parser = json_parser_new();
			JsonArray            *array;
			guint                 j;

			if (loop_cli_count(session->schedule, kind) == 0 ||
			    !json_parser_load_from_data(parser, entries, -1, NULL))
			{
				continue;
			}

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "session");
			json_builder_add_string_value(builder, session->owner);
			json_builder_set_member_name(builder, "running");
			json_builder_add_boolean_value(builder, session->running);
			json_builder_set_member_name(builder, "directory");

			if (session->directory != NULL)
				json_builder_add_string_value(builder, session->directory);
			else
				json_builder_add_null_value(builder);

			json_builder_set_member_name(builder, "entries");
			json_builder_begin_array(builder);
			array = json_node_get_array(json_parser_get_root(parser));

			for (j = 0; j < json_array_get_length(array); j++)
			{
				JsonNode   *node = json_array_get_element(array, j);
				JsonObject *entry = json_node_get_object(node);

				if (g_strcmp0(json_object_get_string_member(entry, "kind"),
				              ai_loop_kind_to_string(kind)) == 0)
				{
					json_builder_add_value(builder, json_node_copy(node));
				}
			}

			json_builder_end_array(builder);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
		json_builder_end_object(builder);
		root = json_builder_get_root(builder);
		json_generator_set_pretty(generator, TRUE);
		json_generator_set_root(generator, root);
		text = json_generator_to_data(generator, NULL);
		g_print("%s\n", text);
		return 0;
	}

	{
		gboolean any = FALSE;

		for (i = 0; i < sessions->len; i++)
		{
			LoopCliSession *session = g_ptr_array_index(sessions, i);
			guint           j;

			if (loop_cli_count(session->schedule, kind) == 0)
			{
				continue;
			}

			g_print("%sSession %s (%s)%s%s\n", any ? "\n" : "", session->owner,
			        session->running ? "running" : "not running",
			        session->directory != NULL ? "  " : "",
			        session->directory != NULL ? session->directory : "");
			any = TRUE;

			for (j = 0; j < ai_loop_schedule_get_n_tasks(session->schedule); j++)
			{
				g_autofree gchar *line = NULL;

				if (ai_loop_schedule_get_kind(session->schedule, j) != kind)
				{
					continue;
				}

				line = ai_loop_schedule_dup_line(session->schedule, j, now);
				g_print("  %s\n", line);
			}
		}

		if (!any)
		{
			g_print("%s\n", kind == AI_LOOP_KIND_GOAL ? "No goals." : "No loops.");
		}
	}

	return 0;
}

static void
loop_cli_where(LoopCliSession *session)
{
	if (session->running)
	{
		g_print("Session %s is running; it picks this up within a second.\n", session->owner);
	}
	else
	{
		g_print("Session %s is not running; this takes effect when it is resumed.\n",
		        session->owner);
	}
}

/* Find the one session holding an id of @kind, by exact id or prefix. */
static LoopCliSession *
loop_cli_find(GPtrArray *sessions, AiLoopKind kind, const gchar *text, gchar **full)
{
	g_autoptr(GString) matches = g_string_new(NULL);
	g_autoptr(GString) valid = g_string_new(NULL);
	g_autofree gchar  *wanted = g_ascii_strdown(text, -1);
	LoopCliSession    *found = NULL;
	guint              n = 0;
	guint              i;

	for (i = 0; i < sessions->len; i++)
	{
		LoopCliSession *session = g_ptr_array_index(sessions, i);
		guint           j;

		for (j = 0; j < ai_loop_schedule_get_n_tasks(session->schedule); j++)
		{
			const gchar *id = ai_loop_schedule_get_id(session->schedule, j);

			if (ai_loop_schedule_get_kind(session->schedule, j) != kind)
			{
				continue;
			}

			g_string_append_printf(valid, "%s%s", valid->len > 0 ? ", " : "", id);

			if (g_strcmp0(id, wanted) == 0 ||
			    (strlen(wanted) >= 3 && g_str_has_prefix(id, wanted)))
			{
				g_string_append_printf(matches, "%s%s:%s", matches->len > 0 ? ", " : "",
				                       session->owner, id);
				g_free(*full);
				*full = g_strdup(id);
				found = session;
				n++;
			}
		}
	}

	if (n == 1)
	{
		return found;
	}

	g_clear_pointer(full, g_free);

	if (n > 1)
	{
		g_printerr("ai: '%s' matches more than one: %s. Give more of the id, or --session.\n",
		           text, matches->str);
	}
	else
	{
		g_printerr("ai: No %s '%s'. Valid ids: %s.\n", ai_loop_kind_to_string(kind), text,
		           valid->len > 0 ? valid->str : "none");
	}

	return NULL;
}

static gchar *
loop_cli_apply(AiLoopSchedule *schedule, AiLoopKind kind, const gchar *text, gint64 now, GError **error)
{
	if (kind == AI_LOOP_KIND_GOAL)
	{
		return ai_loop_schedule_goal_command(schedule, text, now, error);
	}

	return ai_loop_schedule_command(schedule, text, NULL, g_get_user_config_dir(),
	                                g_get_home_dir(), now, error);
}

static int
loop_cli_add(const gchar *store, const gchar *owner, AiLoopKind kind, const gchar *text, gint64 now)
{
	g_autofree gchar         *path = ai_loop_store_path(store, owner);
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	g_autofree gchar         *notice = NULL;
	guint                     before;
	LoopCliSession            where = { 0 };

	if (path == NULL)
	{
		g_printerr("ai: '%s' is not a session id.\n", owner);
		return 1;
	}

	if (ai_loop_schedule_load(schedule, path, now, &error) < 0)
	{
		g_printerr("ai: %s\n", error->message);
		return 1;
	}

	before = ai_loop_schedule_get_n_tasks(schedule);
	notice = loop_cli_apply(schedule, kind, text, now, &error);

	if (notice == NULL)
	{
		g_printerr("ai: %s\n", error->message);
		return 1;
	}

	if (ai_loop_schedule_get_n_tasks(schedule) <= before)
	{
		g_printerr("ai: nothing was added: '%s' is a subcommand, not a %s.\n", text,
		           kind == AI_LOOP_KIND_GOAL ? "condition" : "prompt");
		return 1;
	}

	if (!ai_loop_schedule_save(schedule, path, &error))
	{
		g_printerr("ai: %s\n", error->message);
		return 1;
	}

	g_print("%s\n", notice);
	where.owner = (gchar *)owner;
	where.running = loop_cli_is_running(store, owner);
	loop_cli_where(&where);
	return 0;
}

static int
loop_cli_main(gint argc, gchar **argv)
{
	const gchar         *kind_name = argv[0];
	AiLoopKind           kind = g_strcmp0(kind_name, "goal") == 0 ? AI_LOOP_KIND_GOAL : AI_LOOP_KIND_LOOP;
	g_autofree gchar    *store = ai_loop_store_default_directory();
	g_autoptr(GPtrArray) words = g_ptr_array_new();
	g_autoptr(GPtrArray) sessions = NULL;
	const gchar         *session_id = NULL;
	gboolean             json = FALSE;
	gint64               now = g_get_real_time();
	const gchar         *verb;
	gint                 i;

	for (i = 1; i < argc; i++)
	{
		if (g_strcmp0(argv[i], "--json") == 0)
			json = TRUE;
		else if (g_strcmp0(argv[i], "--session") == 0 && i + 1 < argc)
			session_id = argv[++i];
		else if (g_str_has_prefix(argv[i], "--session="))
			session_id = argv[i] + strlen("--session=");
		else if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0)
		{
			loop_cli_usage(kind_name, stdout);
			return 0;
		}
		else
			g_ptr_array_add(words, argv[i]);
	}

	verb = words->len > 0 ? g_ptr_array_index(words, 0) : "list";

	if (g_strcmp0(verb, "add") == 0)
	{
		g_autofree gchar *text = NULL;

		if (session_id == NULL || words->len < 2)
		{
			g_printerr("ai: %s add needs --session ID and a %s.\n", kind_name,
			           kind == AI_LOOP_KIND_GOAL ? "condition" : "prompt");
			return 2;
		}

		g_ptr_array_add(words, NULL);
		text = g_strjoinv(" ", (gchar **)words->pdata + 1);
		return loop_cli_add(store, session_id, kind, text, now);
	}

	sessions = loop_cli_sessions(store, session_id, now);

	if (g_strcmp0(verb, "list") == 0)
	{
		return loop_cli_list(sessions, kind, json, now);
	}

	if (words->len < 2 ||
	    (g_strcmp0(verb, "show") != 0 && g_strcmp0(verb, "pause") != 0 &&
	     g_strcmp0(verb, "resume") != 0 && g_strcmp0(verb, "run") != 0 &&
	     g_strcmp0(verb, "run-now") != 0 && g_strcmp0(verb, "delete") != 0 &&
	     g_strcmp0(verb, "cancel") != 0 && g_strcmp0(verb, "edit") != 0 &&
	     (kind != AI_LOOP_KIND_GOAL || g_strcmp0(verb, "stop") != 0)))
	{
		loop_cli_usage(kind_name, stderr);
		return 2;
	}

	{
		const gchar      *which = g_ptr_array_index(words, 1);
		LoopCliSession   *session = NULL;
		g_autofree gchar *full = NULL;
		g_autofree gchar *rest = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *notice = NULL;
		g_autoptr(GError) error = NULL;

		if (g_strcmp0(which, "all") == 0)
		{
			if (session_id == NULL || g_strcmp0(verb, "edit") == 0 || g_strcmp0(verb, "show") == 0)
			{
				g_printerr("ai: `%s %s all` needs --session ID; `all` never reaches across sessions.\n",
				           kind_name, verb);
				return 2;
			}

			if (sessions->len != 1)
			{
				g_printerr("ai: --session %s names %u sessions.\n", session_id, sessions->len);
				return 1;
			}

			session = g_ptr_array_index(sessions, 0);
			full = g_strdup("all");
		}
		else if ((session = loop_cli_find(sessions, kind, which, &full)) == NULL)
		{
			return 1;
		}

		g_ptr_array_add(words, NULL);
		rest = words->len > 3 ? g_strjoinv(" ", (gchar **)words->pdata + 2) : g_strdup("");
		text = g_strdup_printf("%s %s%s%s", verb, full, rest[0] != '\0' ? " " : "", rest);
		notice = loop_cli_apply(session->schedule, kind, text, now, &error);

		if (notice == NULL)
		{
			g_printerr("ai: %s\n", error->message);
			return 1;
		}

		if (g_strcmp0(verb, "show") != 0)
		{
			if (!ai_loop_schedule_save(session->schedule, session->path, &error))
			{
				g_printerr("ai: %s\n", error->message);
				return 1;
			}
		}

		g_print("%s\n", notice);

		if (g_strcmp0(verb, "show") != 0)
		{
			loop_cli_where(session);
		}
	}

	return 0;
}

#endif /* AI_LOOP_CLI_H */
