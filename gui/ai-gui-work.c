/*
 * ai-gui-work.c - Project registration, the dashboard's data, and its actions
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <fcntl.h>
#include <string.h>
#include <sys/file.h>

#include <glib/gstdio.h>

#include "core/ai-subprocess-util.h"

#include "ai-gui-work.h"

/* Every subprocess here is a local git or tmux call that should answer
 * immediately. The bound is what stops a wedged one from pinning a
 * worker thread for the life of the process. */
#define AI_GUI_WORK_TIMEOUT_MS 30000

static const gchar *
work_field(
	AiWorkSession *session,
	const gchar   *name
){
	const gchar *value = ai_work_session_get_field(session, name);

	return value != NULL ? value : "";
}

/* ================================================================
 * Project identity
 * ================================================================ */

gchar *
ai_gui_work_project_label(const gchar *project)
{
	g_autofree gchar *trimmed = NULL;
	g_autofree gchar *leaf = NULL;

	if (project == NULL || *project == '\0')
		return g_strdup("Untitled");

	/*
	 * A trailing separator would make g_path_get_basename() answer the
	 * component before it, which is the right answer here -- but only by
	 * accident, and not for a path that is nothing but separators. Trim
	 * first so the two cases are told apart on purpose.
	 */
	trimmed = g_strdup(project);
	{
		gsize len = strlen(trimmed);

		while (len > 1 && G_IS_DIR_SEPARATOR(trimmed[len - 1]))
			trimmed[--len] = '\0';
	}

	leaf = g_path_get_basename(trimmed);

	/* The Git common directory is the identity, so its own basename is
	 * `.git` and the directory holding it is the project. */
	if (g_strcmp0(leaf, ".git") == 0)
	{
		g_autofree gchar *parent = g_path_get_dirname(trimmed);
		g_autofree gchar *name = g_path_get_basename(parent);

		if (name != NULL && *name != '\0' && g_strcmp0(name, ".") != 0)
			return g_steal_pointer(&name);
	}

	if (leaf == NULL || *leaf == '\0' || g_strcmp0(leaf, ".") == 0 ||
	    G_IS_DIR_SEPARATOR(leaf[0]))
	{
		return g_strdup(project);
	}

	return g_steal_pointer(&leaf);
}

gint
ai_gui_work_project_compare(
	const gchar *a,
	const gchar *b
){
	g_autofree gchar *label_a = ai_gui_work_project_label(a);
	g_autofree gchar *label_b = ai_gui_work_project_label(b);
	g_autofree gchar *fold_a = g_utf8_casefold(label_a, -1);
	g_autofree gchar *fold_b = g_utf8_casefold(label_b, -1);
	gint order = g_strcmp0(fold_a, fold_b);

	/* Two checkouts of the same repository share a label. Falling through
	 * to the identity keeps their order stable instead of letting it
	 * depend on which one the sort happened to see first. */
	return order != 0 ? order : g_strcmp0(a, b);
}

/* ================================================================
 * Ordering
 * ================================================================ */

gint
ai_gui_work_priority(const gchar *status)
{
	/*
	 * The order is the answer to "what needs me next", and it is copied
	 * from ai-tui deliberately. Two dashboards reading one registry must
	 * not disagree about which row is at the top.
	 */
	static const gchar *const STATES[] = {
		"INPUT", "ERROR", "WORK", "DONE", "STOPPED", "IDLE", "DISCONNECTED"
	};
	guint i;

	if (status == NULL)
		return (gint)G_N_ELEMENTS(STATES) - 1;

	for (i = 0; i < G_N_ELEMENTS(STATES); i++)
	{
		if (g_str_equal(status, STATES[i]))
			return (gint)i;
	}

	return (gint)G_N_ELEMENTS(STATES) - 1;
}

gint
ai_gui_work_compare(
	gconstpointer a,
	gconstpointer b
){
	AiWorkSession *left = *(AiWorkSession * const *)a;
	AiWorkSession *right = *(AiWorkSession * const *)b;
	gint order;

	order = ai_gui_work_priority(work_field(left, "status"))
		- ai_gui_work_priority(work_field(right, "status"));

	if (order != 0)
		return order;

	order = g_strcmp0(work_field(left, "project"), work_field(right, "project"));

	/* The id last, so the order is total: two rows that tie on both keys
	 * must not swap places between refreshes. */
	return order != 0 ? order
		: g_strcmp0(ai_work_session_get_id(left), ai_work_session_get_id(right));
}

GPtrArray *
ai_gui_work_list(
	const gchar  *directory,
	GError      **error
){
	g_autofree gchar *fallback = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GList *list;
	GList *iter;

	if (directory == NULL)
	{
		fallback = ai_work_session_default_directory();
		directory = fallback;
	}

	list = ai_work_session_list(directory, error);

	if (list == NULL && error != NULL && *error != NULL)
		return NULL;

	rows = g_ptr_array_new_with_free_func(g_object_unref);

	for (iter = list; iter != NULL; iter = iter->next)
		g_ptr_array_add(rows, iter->data);

	g_list_free(list);
	g_ptr_array_sort(rows, ai_gui_work_compare);

	return g_steal_pointer(&rows);
}

gboolean
ai_gui_work_is_live(AiWorkSession *session)
{
	g_return_val_if_fail(AI_IS_WORK_SESSION(session), FALSE);

	return !g_str_equal(work_field(session, "status"), "DISCONNECTED");
}

gboolean
ai_gui_work_can_resume(AiWorkSession *session)
{
	g_return_val_if_fail(AI_IS_WORK_SESSION(session), FALSE);

	/*
	 * Both halves matter. A live record is already being driven by
	 * somebody, and a disconnected one with no native id has nothing to
	 * resume *from* — its transcript was never the provider's to keep.
	 */
	return !ai_gui_work_is_live(session)
		&& *work_field(session, "provider-session") != '\0';
}

/* ================================================================
 * Construction off the main thread
 * ================================================================ */

static void
work_new_thread(
	GTask        *task,
	gpointer      source,
	gpointer      data,
	GCancellable *cancellable
){
	AiWorkSession *session = ai_work_session_new(data);

	if (session == NULL)
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
		                        "Cannot identify the project directory");
		return;
	}

	g_task_return_pointer(task, session, g_object_unref);
}

void
ai_gui_work_new_async(
	const gchar         *directory,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;

	g_return_if_fail(directory != NULL);

	task = g_task_new(NULL, cancellable, callback, user_data);
	g_task_set_task_data(task, g_strdup(directory), g_free);
	g_task_run_in_thread(task, work_new_thread);
}

AiWorkSession *
ai_gui_work_new_finish(
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ================================================================
 * Opening a URL
 * ================================================================ */

gboolean
ai_gui_work_open_url(
	const gchar  *url,
	GError      **error
){
	g_autoptr(GSubprocess) child = NULL;

	g_return_val_if_fail(url != NULL, FALSE);

	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
	                         G_SUBPROCESS_FLAGS_STDERR_SILENCE,
	                         error, "xdg-open", url, NULL);

	return child != NULL;
}

/* ================================================================
 * tmux
 * ================================================================ */

static gchar *
work_tmux(
	const gchar        *socket,
	const gchar *const *args,
	GCancellable       *cancellable
){
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	guint i;

	if (socket == NULL || *socket == '\0')
		return NULL;

	g_ptr_array_add(argv, (gpointer)"tmux");
	g_ptr_array_add(argv, (gpointer)"-S");
	g_ptr_array_add(argv, (gpointer)socket);

	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);

	g_ptr_array_add(argv, NULL);

	child = g_subprocess_newv((const gchar * const *)argv->pdata,
	                          G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                          G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);

	if (child == NULL)
		return NULL;

	if (!ai_subprocess_communicate_utf8_bounded(child, NULL,
		AI_GUI_WORK_TIMEOUT_MS, cancellable, &output, NULL, &error))
	{
		return NULL;
	}

	if (!g_subprocess_get_successful(child))
		return NULL;

	return output != NULL ? g_strdup(g_strstrip(output)) : g_strdup("");
}

/* A tmux pane id is `%` and digits. Anything else is not a target we
 * will hand to tmux. */
static gboolean
work_pane_valid(const gchar *pane)
{
	const gchar *p;

	if (pane == NULL || pane[0] != '%' || pane[1] == '\0')
		return FALSE;

	for (p = pane + 1; *p != '\0'; p++)
	{
		if (!g_ascii_isdigit(*p))
			return FALSE;
	}

	return TRUE;
}

typedef struct
{
	gchar *socket;
	gchar *pane;
	gchar *id;
} WorkFocus;

static void
work_focus_free(gpointer data)
{
	WorkFocus *focus = data;

	g_free(focus->socket);
	g_free(focus->pane);
	g_free(focus->id);
	g_free(focus);
}

static void
work_focus_thread(
	GTask        *task,
	gpointer      source,
	gpointer      data,
	GCancellable *cancellable
){
	WorkFocus *focus = data;
	const gchar *query[] = {
		"display-message", "-p", "-t", focus->pane, "#{@ai_session}", NULL
	};
	const gchar *switch_args[] = {
		"switch-client", "-t", focus->pane, ";",
		"select-pane", "-t", focus->pane, NULL
	};
	g_autofree gchar *owner = NULL;
	g_autofree gchar *result = NULL;

	owner = work_tmux(focus->socket, query, cancellable);

	if (g_strcmp0(owner, focus->id) != 0)
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"That pane no longer belongs to this session. "
			"Resume it instead, or open the project again.");
		return;
	}

	result = work_tmux(focus->socket, switch_args, cancellable);

	if (result == NULL)
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
		                        "tmux would not switch to that pane");
		return;
	}

	g_task_return_boolean(task, TRUE);
}

void
ai_gui_work_focus_pane_async(
	AiWorkSession       *session,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;
	WorkFocus *focus;

	g_return_if_fail(AI_IS_WORK_SESSION(session));

	task = g_task_new(NULL, cancellable, callback, user_data);

	if (!work_pane_valid(work_field(session, "pane")) ||
	    *work_field(session, "socket") == '\0')
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"That session is not in a tmux pane on this machine.");
		return;
	}

	if (!ai_gui_work_is_live(session))
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"That session has disconnected.");
		return;
	}

	focus = g_new0(WorkFocus, 1);
	focus->socket = g_strdup(work_field(session, "socket"));
	focus->pane = g_strdup(work_field(session, "pane"));
	focus->id = g_strdup(ai_work_session_get_id(session));

	g_task_set_task_data(task, focus, work_focus_free);
	g_task_run_in_thread(task, work_focus_thread);
}

gboolean
ai_gui_work_focus_pane_finish(
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

/* ================================================================
 * Worktrees
 * ================================================================ */

static void
work_worktree_thread(
	GTask        *task,
	gpointer      source,
	gpointer      data,
	GCancellable *cancellable
){
	const gchar *directory = data;
	g_autofree gchar *uuid = g_uuid_string_random();
	g_autofree gchar *name = g_path_get_basename(directory);
	g_autofree gchar *branch = g_strconcat("ai/", uuid, NULL);
	g_autofree gchar *parent = g_path_get_dirname(directory);
	g_autofree gchar *leaf = g_strdup_printf("%s-%s", name, uuid);
	g_autofree gchar *root = g_build_filename(parent, ".ai-worktrees", NULL);
	g_autofree gchar *path = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;

	if (g_mkdir_with_parents(root, 0700) != 0)
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
		                        "Cannot create %s", root);
		return;
	}

	path = g_build_filename(root, leaf, NULL);

	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
	                         G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
	                         "git", "-C", directory, "worktree", "add",
	                         "-b", branch, path, "HEAD", NULL);

	if (child == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}

	{
		g_autofree gchar *stderr_text = NULL;

		if (!ai_subprocess_communicate_utf8_bounded(child, NULL,
			AI_GUI_WORK_TIMEOUT_MS, cancellable, NULL, &stderr_text, &error))
		{
			g_task_return_error(task, g_steal_pointer(&error));
			return;
		}

		if (!g_subprocess_get_successful(child))
		{
			/*
			 * Git's own words rather than a summary. "fatal: invalid
			 * reference: HEAD" on a repository with no commits is
			 * actionable; "could not create a worktree" is not.
			 */
			g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
				"git worktree add failed: %s",
				stderr_text != NULL && *g_strstrip(stderr_text) != '\0'
					? g_strstrip(stderr_text) : "no detail reported");
			return;
		}
	}

	g_task_return_pointer(task, g_steal_pointer(&path), g_free);
}

void
ai_gui_work_create_worktree_async(
	const gchar         *directory,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;

	g_return_if_fail(directory != NULL);

	task = g_task_new(NULL, cancellable, callback, user_data);
	g_task_set_task_data(task, g_strdup(directory), g_free);
	g_task_run_in_thread(task, work_worktree_thread);
}

gchar *
ai_gui_work_create_worktree_finish(
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ================================================================
 * The advisory lock
 * ================================================================ */

gint
ai_gui_work_claim(
	const gchar  *directory,
	const gchar  *id,
	GError      **error
){
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *path = NULL;
	gint fd;

	g_return_val_if_fail(id != NULL, -1);

	if (directory == NULL)
	{
		fallback = ai_work_session_default_directory();
		directory = fallback;
	}

	if (g_mkdir_with_parents(directory, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot create %s", directory);
		return -1;
	}

	name = g_strconcat(id, ".lock", NULL);
	path = g_build_filename(directory, name, NULL);
	fd = g_open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);

	if (fd < 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot open %s", path);
		return -1;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != 0)
	{
		g_close(fd, NULL);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
		            "Another ai-glib front-end already owns that session.");
		return -1;
	}

	return fd;
}
