/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Private presentation helpers. Included after App and forward declarations. */
#include "core/ai-subprocess-util.h"
#include <sys/file.h>
#include <fcntl.h>

static const gchar *
work_field(AiWorkSession *work, const gchar *name)
{
	const gchar *value = work != NULL ? ai_work_session_get_field(work, name) : NULL;
	return value != NULL ? value : "";
}

/* No shell interpolation. All tmux requests have a bounded lifetime. */
static gchar *
work_tmux(const gchar *socket, const gchar * const *args)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GSubprocess) child = NULL;
	gchar *output = NULL;
	guint i;
	if (socket == NULL || !g_path_is_absolute(socket)) return NULL;
	g_ptr_array_add(argv, "tmux");
	g_ptr_array_add(argv, "-S");
	g_ptr_array_add(argv, (gpointer)socket);
	for (i = 0; args[i]; i++) g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	child = g_subprocess_newv((const gchar * const *)argv->pdata,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
	if (child == NULL || !ai_subprocess_communicate_utf8_bounded(child, NULL, 1000,
		NULL, &output, NULL, NULL) || !g_subprocess_get_successful(child))
	{
		g_free(output);
		return NULL;
	}
	g_strchomp(output);
	return output;
}

static gboolean
work_pane_valid(const gchar *pane)
{
	const gchar *p;
	if (pane == NULL || pane[0] != '%' || pane[1] == '\0') return FALSE;
	for (p = pane + 1; *p; p++) if (!g_ascii_isdigit(*p)) return FALSE;
	return TRUE;
}

static gint
work_priority(const gchar *state)
{
	const gchar *states[] = {"INPUT", "ERROR", "WORK", "DONE", "STOPPED", "IDLE", "DISCONNECTED"};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(states); i++) if (g_str_equal(state, states[i])) return i;
	return 6;
}

static gint
work_compare(gconstpointer a, gconstpointer b)
{
	AiWorkSession *left = *(AiWorkSession * const *)a;
	AiWorkSession *right = *(AiWorkSession * const *)b;
	gint order = work_priority(work_field(left, "status")) - work_priority(work_field(right, "status"));
	if (order != 0) return order;
	order = g_strcmp0(work_field(left, "project"), work_field(right, "project"));
	return order != 0 ? order : g_strcmp0(ai_work_session_get_id(left), ai_work_session_get_id(right));
}

/* Serialized across ai-tui processes on this machine. The worker never touches
 * App or GObjects owned by the main context. Pane options are the live source
 * for window aggregation, so two panes never race to install different names. */
typedef struct
{
	gchar *socket;
	gchar *pane;
	gchar *state;
	gchar *title;
	gchar *lock;
	gchar *id;
	gboolean titles;
} WorkTitle;

static void
work_title_free(WorkTitle *job)
{
	g_free(job->socket); g_free(job->pane); g_free(job->state);
	g_free(job->title); g_free(job->lock); g_free(job->id); g_free(job);
}

static void
work_title_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	WorkTitle *job = data;
	g_autofree gchar *meta = NULL;
	g_autofree gchar *rows = NULL;
	g_auto(GStrv) values = NULL;
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *seen = g_strdup_printf("%" G_GINT64_FORMAT, g_get_real_time() / G_USEC_PER_SEC);
	const gchar *best = "";
	gint fd;
	guint i;
	const gchar *query[] = {"display-message", "-p", "-t", job->pane,
		"#{window_id}\t#{window_name}\t#{@ai_base}\t#{@ai_last}\t#{automatic-rename}\t#{@ai_auto}\t#{@ai_session}\tEND", NULL};
	const gchar *pane_state[] = {"set-option", "-p", "-t", job->pane, "@ai_state", job->state, NULL};
	const gchar *pane_seen[] = {"set-option", "-p", "-t", job->pane, "@ai_seen", seen, NULL};
	const gchar *pane_title[] = {"select-pane", "-t", job->pane, "-T", job->title, NULL};
	(void)source; (void)cancel;
	fd = g_open(job->lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) { g_task_return_boolean(task, FALSE); return; }
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); g_task_return_boolean(task, FALSE); return; }
	meta = work_tmux(job->socket, query);
	if (meta == NULL) goto done;
	values = g_strsplit(meta, "\t", -1);
	if (g_strv_length(values) != 8) goto done;
	if (!*job->state && *values[6] && !g_str_equal(values[6], job->id)) goto done;
	{
		const gchar *owner[] = {"set-option", "-p", "-t", job->pane, "@ai_session", *job->state ? job->id : "", NULL};
		g_autofree gchar *ignored_owner = work_tmux(job->socket, owner);
	}
	if (!job->titles) goto done;
	{
		g_autofree gchar *ignored = work_tmux(job->socket, pane_state);
		g_autofree gchar *ignored_seen = work_tmux(job->socket, pane_seen);
		g_autofree gchar *ignored_title = work_tmux(job->socket, pane_title);
		const gchar *list[] = {"list-panes", "-t", values[0], "-F", "#{@ai_state}\t#{@ai_seen}", NULL};
		rows = work_tmux(job->socket, list);
	}
	if (rows == NULL) goto done;
	lines = g_strsplit(rows, "\n", -1);
	for (i = 0; lines[i]; i++)
	{
		gchar *stamp = strchr(lines[i], '\t');
		gint64 age;
		if (stamp == NULL) continue;
		*stamp++ = '\0';
		age = g_get_real_time() / G_USEC_PER_SEC - g_ascii_strtoll(stamp, NULL, 10);
		if (age >= 0 && age <= 15 && *lines[i] && (!*best || work_priority(lines[i]) < work_priority(best))) best = lines[i];
	}
	/* Respect a user rename made since our last write. */
	if (*values[3] && !g_str_equal(values[1], values[3]))
	{
		if (!*best)
		{
			const gchar *clear[] = {"set-option", "-wu", "-t", values[0], "@ai_base", ";",
				"set-option", "-wu", "-t", values[0], "@ai_last", NULL};
			g_autofree gchar *ignored = work_tmux(job->socket, clear);
		}
		goto done;
	}
	if (!*values[2] && *best)
	{
		const gchar *save[] = {"set-option", "-w", "-t", values[0], "@ai_base", values[1], NULL};
		const gchar *automatic[] = {"set-option", "-w", "-t", values[0], "@ai_auto", values[4], NULL};
		g_autofree gchar *a = work_tmux(job->socket, save);
		g_autofree gchar *b = work_tmux(job->socket, automatic);
		const gchar *disable[] = {"set-option", "-w", "-t", values[0], "automatic-rename", "off", NULL};
		g_autofree gchar *c = work_tmux(job->socket, disable);
	}
	name = *best && !g_str_equal(best, "IDLE") ?
		g_strdup_printf("%s: %s", best, *values[2] ? values[2] : values[1]) :
		g_strdup(*values[2] ? values[2] : values[1]);
	{
		const gchar *rename[] = {"rename-window", "-t", values[0], name, NULL};
		const gchar *remember[] = {"set-option", "-w", "-t", values[0], "@ai_last", *best ? name : "", NULL};
		g_autofree gchar *a = g_strcmp0(name, values[1]) != 0 ? work_tmux(job->socket, rename) : NULL;
		g_autofree gchar *b = work_tmux(job->socket, remember);
		if (!*best && *values[2])
		{
			const gchar *restore[] = {"set-option", "-w", "-t", values[0], "automatic-rename", *values[5] ? values[5] : "off", NULL};
			const gchar *clear[] = {"set-option", "-wu", "-t", values[0], "@ai_base", NULL};
			g_autofree gchar *c = work_tmux(job->socket, restore);
			g_autofree gchar *d = work_tmux(job->socket, clear);
		}
	}
done:
	flock(fd, LOCK_UN); close(fd);
	g_task_return_boolean(task, TRUE);
}

static void
work_title_done(GObject *source, GAsyncResult *result, gpointer data)
{
	App *app = data;
	(void)source;
	g_task_propagate_boolean(G_TASK(result), NULL);
	app->title_pending = FALSE;
}

static void
work_title_update(App *app, gboolean release)
{
	g_autoptr(GTask) task = NULL;
	WorkTitle *job;
	const gchar *state;
	if (app->work == NULL || app->title_pending ||
		!work_pane_valid(work_field(app->work, "pane")) || !*work_field(app->work, "socket")) return;
	state = release ? "" : work_field(app->work, "status");
	job = g_new0(WorkTitle, 1);
	job->socket = g_strdup(work_field(app->work, "socket"));
	job->pane = g_strdup(work_field(app->work, "pane"));
	job->state = g_strdup(state);
	job->id = g_strdup(ai_work_session_get_id(app->work));
	job->titles = !opt_no_tmux_titles;
	job->title = release ? g_strdup(app->original_pane_title != NULL ? app->original_pane_title : "") :
		g_strdup_printf("%s: %s", state, *work_field(app->work, "title") ? work_field(app->work, "title") : "ai-tui");
	job->lock = g_build_filename(app->work_directory, "tmux.lock", NULL);
	task = g_task_new(NULL, NULL, work_title_done, app);
	g_task_set_task_data(task, job, (GDestroyNotify)work_title_free);
	app->title_pending = TRUE;
	g_task_run_in_thread(task, work_title_thread);
}

static void
work_publish(App *app)
{
	GObject *provider;
	g_autoptr(GList) agents = NULL;
	GList *iter;
	guint active = 0, completed = 0, total = 0, todo_index;
	gboolean attention = app->approval_prompt != NULL;
	const gchar *model;
	g_autofree gchar *session = NULL;
	g_autofree gchar *activity = NULL;
	g_autoptr(GError) error = NULL;
	if (app->work == NULL || !app->work_registered) return;
	provider = ai_conversation_get_provider(app->conversation);
	if (ai_conversation_get_brigade(app->conversation) != NULL)
		agents = ai_brigade_list(ai_conversation_get_brigade(app->conversation));
	for (iter = agents; iter != NULL; iter = iter->next)
	{
		AiAgentState state = ai_agent_get_state(iter->data);
		if (state >= AI_AGENT_STATE_QUEUED && state <= AI_AGENT_STATE_BLOCKED) active++;
		if (state == AI_AGENT_STATE_WAITING_INPUT) attention = TRUE;
	}
	active += app->side_questions != NULL ? app->side_questions->len : 0;
	model = AI_IS_CLIENT(provider) ? ai_client_get_model(AI_CLIENT(provider)) : ai_cli_client_get_model(AI_CLI_CLIENT(provider));
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(provider), "session-id") != NULL)
		g_object_get(provider, "session-id", &session, NULL);
	total = ai_tool_executor_get_n_todos(ai_conversation_get_executor(app->conversation));
	for (todo_index = 0; todo_index < total; todo_index++)
	{
		const gchar *label = NULL;
		AiTodoState state;
		ai_tool_executor_get_todo_fields(ai_conversation_get_executor(app->conversation), todo_index, &label, &state);
		if (state == AI_TODO_COMPLETED) completed++;
	}
	activity = g_strdup_printf("%s%s%u background / %u/%u todos / %" G_GINT64_FORMAT "s", ai_conversation_get_activity(app->conversation) != NULL ?
		ai_conversation_get_activity(app->conversation) : "Ready", " / ", active, completed, total, ai_conversation_get_activity_elapsed(app->conversation) / G_USEC_PER_SEC);
	g_object_set(app->work, "provider", ai_provider_type_to_string(ai_provider_get_provider_type(AI_PROVIDER(provider))), "model", model,
		"provider-session", session, "activity", activity, NULL);
	ai_work_session_update(app->work, app->sending || ai_conversation_get_busy(app->conversation) ||
		ai_prompt_queue_get_length(app->send_queue) != 0, attention, active, app->work_outcome);
	if (!ai_work_session_save(app->work, app->work_directory, TRUE, &error))
	{
		g_free(app->work_notice); app->work_notice = g_strdup(error->message);
	}
	work_title_update(app, FALSE);
}

static void
work_refresh(App *app)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = ai_work_session_list(app->work_directory, &error);
	g_autofree gchar *selected = NULL;
	guint i;
	if (app->work_rows != NULL && app->work_selected < app->work_rows->len)
		selected = g_strdup(ai_work_session_get_id(g_ptr_array_index(app->work_rows, app->work_selected)));
	if (rows == NULL)
	{
		g_free(app->work_notice); app->work_notice = g_strdup(error->message); return;
	}
	g_ptr_array_sort(rows, work_compare);
	g_clear_pointer(&app->work_rows, g_ptr_array_unref);
	app->work_rows = g_steal_pointer(&rows);
	app->work_selected = MIN(app->work_selected, app->work_rows->len ? app->work_rows->len - 1 : 0);
	for (i = 0; selected != NULL && i < app->work_rows->len; i++)
		if (g_str_equal(selected, ai_work_session_get_id(g_ptr_array_index(app->work_rows, i)))) app->work_selected = i;
}

typedef struct
{
	App *app;
	guint generation;
	gboolean assign;
} WorkFetch;

static void
work_link_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
	WorkFetch *fetch = data;
	App *app = fetch->app;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = ai_work_session_refresh_link_finish(AI_WORK_SESSION(source), result, &error);
	app->link_pending--;
	if (app->running && fetch->generation == app->link_generation)
	{
		if (text != NULL && fetch->assign)
		{
			if (ai_prompt_queue_push(app->send_queue, text, NULL, TRUE, &error))
			{
				g_auto(GStrv) first = g_strsplit(text, "\n", 2);
				g_object_set(app->work, "title", first[0], NULL);
				app_flush_send_queue(app);
			}
		}
		g_free(app->work_notice);
		app->work_notice = error != NULL ? g_strdup(error->message) : NULL;
		work_publish(app);
		app_schedule_redraw(app);
	}
	g_free(fetch);
}

static void
work_fetch_link(App *app, const gchar *url, gboolean assign)
{
	WorkFetch *fetch = g_new0(WorkFetch, 1);
	fetch->app = app;
	fetch->generation = app->link_generation;
	fetch->assign = assign;
	app->link_pending++;
	ai_work_session_refresh_link_async(app->work, url, app->link_cancel, work_link_fetched, fetch);
}

static gboolean
work_tick(gpointer data)
{
	App *app = data;
	work_publish(app);
	if (app->work_registered && app->link_pending == 0 &&
		g_get_monotonic_time() - app->link_refreshed > 60 * G_USEC_PER_SEC)
	{
		g_auto(GStrv) links = ai_work_session_dup_links(app->work);
		app->link_refreshed = g_get_monotonic_time();
		if (links[0] != NULL) work_fetch_link(app, links[app->link_refresh_cursor++ % g_strv_length(links)], FALSE);
	}
	if (app->dashboard) { work_refresh(app); app_schedule_redraw(app); }
	return G_SOURCE_CONTINUE;
}

static void
work_toggle(App *app)
{
	app->dashboard = !app->dashboard;
	if (app->dashboard) work_refresh(app);
	else app->work_registered = TRUE;
	clearok(curscr, TRUE);
	app_schedule_redraw(app);
}

static void
work_open_url(App *app, const gchar *url)
{
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	/* Browser launch is an explicit user action, never an automatic fetch. */
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		&error, "xdg-open", url, NULL);
	if (child == NULL) { g_free(app->work_notice); app->work_notice = g_strdup(error->message); }
}

static void
work_draw(App *app)
{
	guint i, first;
	gint y = 3;
	erase();
	chrome_text(0, 1, COLS - 2, "PROJECT DASHBOARD", A_BOLD);
	chrome_text(1, 1, COLS - 2, "State       Project / branch / task", A_DIM);
	first = app->work_selected >= (guint)MAX(1, LINES - 7) ? app->work_selected - MAX(1, LINES - 7) + 1 : 0;
	for (i = first; app->work_rows != NULL && i < app->work_rows->len && y < LINES - 4; i++, y++)
	{
		AiWorkSession *row = g_ptr_array_index(app->work_rows, i);
		g_autofree gchar *project_path = g_str_has_suffix(work_field(row, "project"), "/.git") ?
			g_path_get_dirname(work_field(row, "project")) : g_strdup(work_field(row, "project"));
		g_autofree gchar *project = g_path_get_basename(project_path);
		g_auto(GStrv) links = ai_work_session_dup_links(row);
		g_autoptr(GString) badges = g_string_new("");
		g_autofree gchar *line = NULL;
		guint link_index;
		for (link_index = 0; links[link_index] != NULL && link_index < 2; link_index++)
		{
			const gchar *number = strrchr(links[link_index], '/');
			g_string_append_printf(badges, " %s#%s", strstr(links[link_index], "/issues/") != NULL ? "ISSUE" : "PR", number != NULL ? number + 1 : "?");
		}
		if (g_strv_length(links) > 2) g_string_append_printf(badges, " +%u", g_strv_length(links) - 2);
		line = g_strdup_printf("%-12s %s%s / %s / %s", work_field(row, "status"), project, badges->str,
			work_field(row, "branch"), *work_field(row, "title") ? work_field(row, "title") : work_field(row, "provider"));
		chrome_text(y, 1, COLS - 2, line, i == app->work_selected ? A_REVERSE : A_NORMAL);
	}
	if (app->work_rows == NULL || app->work_rows->len == 0)
		chrome_text(3, 1, COLS - 2, "No sessions yet. Press n to start here.", A_NORMAL);
	else if (app->work_selected < app->work_rows->len)
	{
		AiWorkSession *row = g_ptr_array_index(app->work_rows, app->work_selected);
		g_auto(GStrv) links = ai_work_session_dup_links(row);
		chrome_text(LINES - 4, 1, COLS - 2, work_field(row, "activity"), A_DIM);
		if (links[0] != NULL) chrome_text(LINES - 3, 1, COLS - 2, links[app->work_link % g_strv_length(links)], A_UNDERLINE);
	}
	if (app->work_notice != NULL) chrome_text(LINES - 2, 1, COLS - 2, app->work_notice, A_BOLD);
	chrome_text(LINES - 1, 0, COLS, "Enter jump | n new | w worktree | r resume | o link | Tab next link | ^\\ back | q quit", A_DIM);
	curs_set(0);
	refresh();
}

static void
work_focus(App *app, AiWorkSession *row)
{
	g_autofree gchar *result = NULL;
	const gchar *pane = work_field(row, "pane");
	const gchar *args[] = {"switch-client", "-t", pane, ";", "select-pane", "-t", pane, NULL};
	if (g_str_equal(ai_work_session_get_id(row), ai_work_session_get_id(app->work)))
	{
		work_toggle(app); return;
	}
	if (work_pane_valid(pane) && !g_str_equal(work_field(row, "status"), "DISCONNECTED"))
	{
		const gchar *query[] = {"display-message", "-p", "-t", pane, "#{@ai_session}", NULL};
		g_autofree gchar *owner = work_tmux(work_field(row, "socket"), query);
		if (g_strcmp0(owner, ai_work_session_get_id(row)) == 0)
			result = work_tmux(work_field(row, "socket"), args);
	}
	g_free(app->work_notice);
	app->work_notice = result == NULL ? g_strdup("Session unavailable in tmux; use r to resume a disconnected CLI session.") : NULL;
}

typedef struct
{
	gchar *socket;
	gchar *directory;
	gchar *executable;
	gchar *session;
	gchar *provider;
	gchar *model;
	gboolean worktree;
	gchar **environment;
} WorkLaunch;

static void
work_launch_free(WorkLaunch *job)
{
	g_free(job->socket); g_free(job->directory); g_free(job->executable);
	g_free(job->session); g_free(job->provider); g_free(job->model); g_strfreev(job->environment); g_free(job);
}

static void
work_launch_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	WorkLaunch *job = data;
	g_autofree gchar *path = g_strdup(job->directory);
	g_autofree gchar *name = g_path_get_basename(path);
	g_autofree gchar *result = NULL;
	g_autoptr(GPtrArray) args = g_ptr_array_new();
	guint i;
	(void)source;
	if (job->worktree)
	{
		g_autofree gchar *uuid = g_uuid_string_random();
		g_autofree gchar *branch = g_strconcat("ai/", uuid, NULL);
		g_autofree gchar *parent = g_path_get_dirname(job->directory);
		g_autofree gchar *leaf = g_strdup_printf("%s-%s", name, uuid);
		g_autofree gchar *root = g_build_filename(parent, ".ai-worktrees", NULL);
		g_autoptr(GSubprocess) child = NULL;
		g_autoptr(GError) error = NULL;
		if (g_mkdir_with_parents(root, 0700) != 0)
		{
			g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create worktree directory"); return;
		}
		g_free(path); path = g_build_filename(root, leaf, NULL);
		child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
			&error, "git", "-C", job->directory, "worktree", "add", "-b", branch, path, "HEAD", NULL);
		if (child == NULL || !ai_subprocess_communicate_utf8_bounded(child, NULL, 30000, cancel, NULL, NULL, &error))
		{
			g_task_return_error(task, g_steal_pointer(&error)); return;
		}
		if (!g_subprocess_get_successful(child))
		{
			g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Git could not create a worktree at %s", path); return;
		}
	}
	g_ptr_array_add(args, "new-window");
	for (i = 0; job->environment[i] != NULL; i++)
	{
		g_ptr_array_add(args, "-e"); g_ptr_array_add(args, job->environment[i]);
	}
	g_ptr_array_add(args, "-c"); g_ptr_array_add(args, path);
	g_ptr_array_add(args, "-n"); g_ptr_array_add(args, name);
	g_ptr_array_add(args, job->executable); g_ptr_array_add(args, "--no-dashboard");
	if (job->session != NULL)
	{
		g_ptr_array_add(args, "--workspace-session"); g_ptr_array_add(args, job->session);
	}
	else if (*job->provider)
	{
		g_ptr_array_add(args, "-p"); g_ptr_array_add(args, job->provider);
		if (*job->model) { g_ptr_array_add(args, "-m"); g_ptr_array_add(args, job->model); }
	}
	g_ptr_array_add(args, NULL);
	result = work_tmux(job->socket, (const gchar * const *)args->pdata);
	if (result == NULL)
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Cannot open tmux window. Working directory retained at %s", path);
	else g_task_return_boolean(task, TRUE);
}

static void
work_launch_done(GObject *source, GAsyncResult *result, gpointer data)
{
	App *app = data;
	g_autoptr(GError) error = NULL;
	(void)source;
	g_task_propagate_boolean(G_TASK(result), &error);
	app->launch_pending--;
	g_free(app->work_notice);
	app->work_notice = error != NULL ? g_strdup(error->message) : NULL;
	if (app->running) app_schedule_redraw(app);
}

static void
work_launch_full(App *app, AiWorkSession *row, gboolean resume, gboolean worktree)
{
	g_autoptr(GTask) task = NULL;
	WorkLaunch *job;
	if (resume && (!g_str_equal(work_field(row, "status"), "DISCONNECTED") || !*work_field(row, "provider-session")))
	{
		g_free(app->work_notice); app->work_notice = g_strdup("Resume requires a disconnected session with a native provider session ID."); return;
	}
	if (!*work_field(app->work, "socket"))
	{
		g_free(app->work_notice); app->work_notice = g_strdup("Open ai-tui in tmux to create project windows. ^\\ opens a prompt here."); return;
	}
	job = g_new0(WorkLaunch, 1);
	job->socket = g_strdup(work_field(app->work, "socket"));
	job->directory = g_strdup(work_field(row, "directory"));
	job->executable = g_file_read_link("/proc/self/exe", NULL);
	job->session = resume ? g_strdup(ai_work_session_get_id(row)) : NULL;
	job->provider = g_strdup(work_field(row, "provider"));
	job->model = g_strdup(work_field(row, "model"));
	job->worktree = worktree;
	{
		const gchar *names[] = {"HOME", "PATH", "XDG_STATE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
			"GROK_PATH", "CLAUDE_CODE_PATH", "OPENCODE_PATH", "CURSOR_AGENT_PATH", "AGY_PATH", "CODEX_PATH", NULL};
		GPtrArray *environment = g_ptr_array_new_with_free_func(g_free);
		guint i;
		for (i = 0; names[i] != NULL; i++)
			if (g_getenv(names[i]) != NULL)
				g_ptr_array_add(environment, g_strdup_printf("%s=%s", names[i], g_getenv(names[i])));
		g_ptr_array_add(environment, NULL);
		job->environment = (gchar **)g_ptr_array_free(environment, FALSE);
	}
	if (job->executable == NULL) { work_launch_free(job); return; }
	app->launch_pending++;
	task = g_task_new(NULL, app->link_cancel, work_launch_done, app);
	g_task_set_task_data(task, job, (GDestroyNotify)work_launch_free);
	g_task_run_in_thread(task, work_launch_thread);
}

static void
work_launch(App *app, AiWorkSession *row, gboolean resume)
{
	work_launch_full(app, row, resume, FALSE);
}

static void
work_key(App *app, gint ch)
{
	AiWorkSession *row = app->work_rows != NULL && app->work_selected < app->work_rows->len ?
		g_ptr_array_index(app->work_rows, app->work_selected) : NULL;
	if (ch == KEY_UP && app->work_selected > 0) app->work_selected--;
	else if (ch == KEY_DOWN && app->work_rows != NULL && app->work_selected + 1 < app->work_rows->len) app->work_selected++;
	else if (ch == '\t') app->work_link++;
	else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && row != NULL) work_focus(app, row);
	else if (ch == 'r' && row != NULL) work_launch(app, row, TRUE);
	else if (ch == 'w' && row != NULL) work_launch_full(app, row, FALSE, TRUE);
	else if (ch == 'n')
	{
		if (!app->work_registered) work_toggle(app);
		else work_launch(app, row != NULL ? row : app->work, FALSE);
	}
	else if (ch == 'o' && row != NULL)
	{
		g_auto(GStrv) links = ai_work_session_dup_links(row);
		if (links[0] != NULL) work_open_url(app, links[app->work_link % g_strv_length(links)]);
	}
	else if (ch == 'q') { app->running = FALSE; ai_conversation_cancel(app->conversation); g_main_loop_quit(app->loop); }
	app_schedule_redraw(app);
}
