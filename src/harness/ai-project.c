/*
 * ai-project.c - The work registry, grouped by project
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A project has no record of its own. It is what the #AiWorkSession
 * records in the registry have in common: their `project` field, the
 * canonical Git common directory (or the directory itself outside Git).
 * Deriving it on every read, instead of keeping a second store, means
 * there is nothing to fall out of step --- a project exists for exactly
 * as long as something in the registry says it does.
 *
 * The label, the grouping and the urgency order live here so ai-tui,
 * ai-gui and `ai project` give one answer. There were two copies of the
 * label before, and they had drifted: one trimmed trailing separators and
 * had an "Untitled" fallback, the other did neither.
 */

#include <string.h>

#include "harness/ai-project.h"
#include "harness/ai-work-session.h"
#include "core/ai-error.h"

struct _AiProject
{
	GObject    parent_instance;
	gchar     *id;
	gchar     *name;
	gchar     *root;
	GPtrArray *sessions;   /* (element-type AiWorkSession), sorted */
	guint      live;
	guint      busy;
	guint      attention;
};

G_DEFINE_TYPE(AiProject, ai_project, G_TYPE_OBJECT)

enum
{
	PROP_0,
	PROP_ID,
	PROP_NAME,
	PROP_ROOT,
	PROP_STATUS,
	PROP_SESSION_COUNT,
	PROP_LIVE_COUNT,
	PROP_BUSY_COUNT,
	PROP_ATTENTION_COUNT,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_project_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiProject *self = AI_PROJECT(object);

	switch (prop_id)
	{
		case PROP_ID:
			g_value_set_string(value, self->id);
			break;
		case PROP_NAME:
			g_value_set_string(value, self->name);
			break;
		case PROP_ROOT:
			g_value_set_string(value, self->root);
			break;
		case PROP_STATUS:
			g_value_set_string(value, ai_project_get_status(self));
			break;
		case PROP_SESSION_COUNT:
			g_value_set_uint(value, self->sessions->len);
			break;
		case PROP_LIVE_COUNT:
			g_value_set_uint(value, self->live);
			break;
		case PROP_BUSY_COUNT:
			g_value_set_uint(value, self->busy);
			break;
		case PROP_ATTENTION_COUNT:
			g_value_set_uint(value, self->attention);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void
ai_project_finalize(GObject *object)
{
	AiProject *self = AI_PROJECT(object);

	g_free(self->id);
	g_free(self->name);
	g_free(self->root);
	g_ptr_array_unref(self->sessions);

	G_OBJECT_CLASS(ai_project_parent_class)->finalize(object);
}

static void
ai_project_class_init(AiProjectClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	const GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

	object_class->get_property = ai_project_get_property;
	object_class->finalize = ai_project_finalize;

	/**
	 * AiProject:id:
	 *
	 * The identity every session in the project shares: the canonical
	 * Git common directory, or the directory itself outside Git.
	 */
	properties[PROP_ID] = g_param_spec_string("id", "Id",
		"Canonical project identity", NULL, flags);

	/**
	 * AiProject:name:
	 *
	 * What to call the project, from ai_project_label_for_path().
	 */
	properties[PROP_NAME] = g_param_spec_string("name", "Name",
		"Display label", NULL, flags);

	/**
	 * AiProject:root:
	 *
	 * The main checkout: the directory holding a `.git` common directory,
	 * or #AiProject:id itself for a bare repository or a plain directory.
	 */
	properties[PROP_ROOT] = g_param_spec_string("root", "Root",
		"Main checkout directory", NULL, flags);

	/**
	 * AiProject:status:
	 *
	 * The most urgent status among the project's sessions, ranked by
	 * ai_work_session_status_priority().
	 */
	properties[PROP_STATUS] = g_param_spec_string("status", "Status",
		"Most urgent session status", NULL, flags);

	/**
	 * AiProject:session-count:
	 *
	 * Every session recorded for the project, disconnected ones included.
	 */
	properties[PROP_SESSION_COUNT] = g_param_spec_uint("session-count",
		"Session count", "Recorded sessions", 0, G_MAXUINT, 0, flags);

	/**
	 * AiProject:live-count:
	 *
	 * Sessions whose process is still running (not DISCONNECTED).
	 */
	properties[PROP_LIVE_COUNT] = g_param_spec_uint("live-count",
		"Live count", "Running sessions", 0, G_MAXUINT, 0, flags);

	/**
	 * AiProject:busy-count:
	 *
	 * Sessions in the middle of a turn (WORK).
	 */
	properties[PROP_BUSY_COUNT] = g_param_spec_uint("busy-count",
		"Busy count", "Sessions working", 0, G_MAXUINT, 0, flags);

	/**
	 * AiProject:attention-count:
	 *
	 * Sessions waiting on a person (INPUT or ERROR).
	 */
	properties[PROP_ATTENTION_COUNT] = g_param_spec_uint("attention-count",
		"Attention count", "Sessions needing a person", 0, G_MAXUINT, 0, flags);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_project_init(AiProject *self)
{
	self->sessions = g_ptr_array_new_with_free_func(g_object_unref);
}

/* ================================================================
 * Identity
 * ================================================================ */

/* A copy of @path with trailing separators removed, but never to empty. */
static gchar *
trim_separators(const gchar *path)
{
	gchar *trimmed = g_strdup(path);
	gsize len = strlen(trimmed);

	while (len > 1 && G_IS_DIR_SEPARATOR(trimmed[len - 1]))
		trimmed[--len] = '\0';

	return trimmed;
}

/**
 * ai_project_label_for_path:
 * @project: (nullable): a project identity, usually a work session's
 *   `project` field
 *
 * The one name every front-end shows for a project.
 *
 * The identity of a Git project is its common directory, whose own
 * basename is `.git`, so a literal basename would call every repository
 * "git". The directory holding it is the name instead. A path with no
 * usable basename is shown whole rather than invented, and an empty one
 * is "Untitled".
 *
 * Returns: (transfer full): the label
 */
gchar *
ai_project_label_for_path(const gchar *project)
{
	g_autofree gchar *trimmed = NULL;
	g_autofree gchar *leaf = NULL;

	if (project == NULL || *project == '\0')
		return g_strdup("Untitled");

	trimmed = trim_separators(project);
	leaf = g_path_get_basename(trimmed);

	if (g_strcmp0(leaf, ".git") == 0)
	{
		g_autofree gchar *parent = g_path_get_dirname(trimmed);
		g_autofree gchar *name = g_path_get_basename(parent);

		if (*name != '\0' && g_strcmp0(name, ".") != 0 && !G_IS_DIR_SEPARATOR(name[0]))
			return g_steal_pointer(&name);

		return g_strdup(trimmed);
	}

	if (*leaf == '\0' || g_strcmp0(leaf, ".") == 0 || G_IS_DIR_SEPARATOR(leaf[0]))
		return g_strdup(trimmed);

	return g_steal_pointer(&leaf);
}

/**
 * ai_project_compare_paths:
 * @a: (nullable): a project identity
 * @b: (nullable): another project identity
 *
 * Orders two projects by label, case-insensitively, then by identity.
 *
 * Two checkouts of one repository name can share a label; falling back
 * to the identity keeps them apart and keeps their order stable rather
 * than dependent on which one a sort saw first.
 *
 * Returns: negative, zero or positive, as strcmp()
 */
gint
ai_project_compare_paths(
	const gchar *a,
	const gchar *b
){
	g_autofree gchar *label_a = ai_project_label_for_path(a);
	g_autofree gchar *label_b = ai_project_label_for_path(b);
	g_autofree gchar *fold_a = g_utf8_casefold(label_a, -1);
	g_autofree gchar *fold_b = g_utf8_casefold(label_b, -1);
	gint order = g_strcmp0(fold_a, fold_b);

	return order != 0 ? order : g_strcmp0(a, b);
}

/* The main checkout for an identity: the parent of a `.git` directory. */
static gchar *
root_for(const gchar *id)
{
	g_autofree gchar *trimmed = trim_separators(id);
	g_autofree gchar *leaf = g_path_get_basename(trimmed);

	if (g_strcmp0(leaf, ".git") == 0)
	{
		g_autofree gchar *parent = g_path_get_dirname(trimmed);

		if (g_strcmp0(parent, ".") != 0)
			return g_steal_pointer(&parent);
	}

	return g_steal_pointer(&trimmed);
}

/* A session with no project field groups under its own directory, so a
 * record written by something older is still somewhere rather than lost. */
static const gchar *
session_project(AiWorkSession *session)
{
	const gchar *project = ai_work_session_get_field(session, "project");

	return project != NULL && *project != '\0'
		? project : ai_work_session_get_field(session, "directory");
}

static gint
compare_sessions(gconstpointer a, gconstpointer b)
{
	return ai_work_session_compare(*(AiWorkSession * const *)a, *(AiWorkSession * const *)b);
}

static gint
compare_projects(gconstpointer a, gconstpointer b)
{
	AiProject *left = (AiProject *)a;
	AiProject *right = (AiProject *)b;
	gint order = ai_work_session_status_priority(ai_project_get_status(left))
		- ai_work_session_status_priority(ai_project_get_status(right));

	return order != 0 ? order : ai_project_compare_paths(left->id, right->id);
}

/* ================================================================
 * Listing
 * ================================================================ */

/**
 * ai_project_list:
 * @directory: the work registry, usually ai_work_session_default_directory()
 * @error: return location for a #GError
 *
 * Groups the registry's sessions by project.
 *
 * The order is "what needs me next": by the most urgent session in each
 * project, then by label. A project's sessions are in
 * ai_work_session_compare() order. Everything ai_work_session_list()
 * tolerates is tolerated here --- a corrupt record costs itself only ---
 * and a registry that does not exist yet is an empty answer.
 *
 * Returns: (transfer full) (element-type AiProject) (nullable): the
 *   projects, or %NULL when there are none or on error. Free with
 *   g_list_free_full(list, g_object_unref).
 */
GList *
ai_project_list(
	const gchar  *directory,
	GError      **error
){
	g_autoptr(GHashTable) by_id = NULL;
	GList *sessions;
	GList *result = NULL;
	GList *l;
	GHashTableIter iter;
	gpointer value;
	GError *local = NULL;

	g_return_val_if_fail(directory != NULL, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	sessions = ai_work_session_list(directory, &local);

	if (local != NULL)
	{
		g_propagate_error(error, local);
		return NULL;
	}

	by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_object_unref);

	for (l = sessions; l != NULL; l = l->next)
	{
		AiWorkSession *session = l->data;
		const gchar *id = session_project(session);
		const gchar *status = ai_work_session_get_field(session, "status");
		AiProject *project;

		if (id == NULL || *id == '\0')
			continue;

		project = g_hash_table_lookup(by_id, id);

		if (project == NULL)
		{
			project = g_object_new(AI_TYPE_PROJECT, NULL);
			project->id = g_strdup(id);
			project->name = ai_project_label_for_path(id);
			project->root = root_for(id);
			g_hash_table_insert(by_id, project->id, project);
		}

		g_ptr_array_add(project->sessions, g_object_ref(session));

		if (g_strcmp0(status, "DISCONNECTED") != 0)
			project->live++;

		if (g_strcmp0(status, "WORK") == 0)
			project->busy++;

		if (g_strcmp0(status, "INPUT") == 0 || g_strcmp0(status, "ERROR") == 0)
			project->attention++;
	}

	g_list_free_full(sessions, g_object_unref);

	g_hash_table_iter_init(&iter, by_id);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		AiProject *project = value;

		g_ptr_array_sort(project->sessions, compare_sessions);
		result = g_list_prepend(result, g_object_ref(project));
	}

	return g_list_sort(result, compare_projects);
}

/* ================================================================
 * Lookup
 * ================================================================ */

/* Whether @path is @dir or somewhere below it --- a separator boundary,
 * so "/r/alphabet" is not inside "/r/alpha". */
static gboolean
path_within(const gchar *path, const gchar *dir)
{
	g_autofree gchar *base = NULL;
	gsize len;

	if (dir == NULL || *dir == '\0')
		return FALSE;

	base = trim_separators(dir);
	len = strlen(base);

	if (strncmp(path, base, len) != 0)
		return FALSE;

	return path[len] == '\0' || G_IS_DIR_SEPARATOR(path[len]) ||
	       (len == 1 && G_IS_DIR_SEPARATOR(base[0]));
}

/* How closely @path belongs to @project: the length of the longest
 * directory of it that contains @path, or 0. Longest wins, so a worktree
 * nested inside another project's checkout still resolves to its own. */
static gsize
path_score(AiProject *project, const gchar *path)
{
	gsize best = 0;
	guint i;

	if (path_within(path, project->id))
		best = MAX(best, strlen(project->id));

	if (path_within(path, project->root))
		best = MAX(best, strlen(project->root));

	for (i = 0; i < project->sessions->len; i++)
	{
		const gchar *dir = ai_work_session_get_field(
			g_ptr_array_index(project->sessions, i), "directory");

		if (path_within(path, dir))
			best = MAX(best, strlen(dir));
	}

	return best;
}

/**
 * ai_project_find:
 * @projects: (element-type AiProject) (nullable): from ai_project_list()
 * @query: a project name, an identity, or a path in or below a project
 * @error: return location for a #GError
 *
 * Resolves what a person typed to one project.
 *
 * A path --- anything containing a separator, or "." or ".." --- matches the project
 * whose identity, main checkout or any session directory contains it,
 * the deepest one winning. Otherwise @query is a name, compared
 * case-insensitively. A name two projects share is an error that lists
 * both, never a guess: acting on the wrong repository is worse than
 * asking. Paths are compared as written, without touching the disk.
 *
 * Returns: (transfer full) (nullable): the project, or %NULL with
 *   @error set (%AI_ERROR_INVALID_REQUEST) when nothing or more than one
 *   thing matches
 */
AiProject *
ai_project_find(
	GList        *projects,
	const gchar  *query,
	GError      **error
){
	g_autoptr(GPtrArray) named = g_ptr_array_new();
	g_autofree gchar *fold = NULL;
	AiProject *best = NULL;
	gsize best_score = 0;
	GList *l;

	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (query == NULL || *query == '\0')
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			"Give a project name or path");
		return NULL;
	}

	/* "." and ".." have no separator but are paths all the same:
	 * `ai project show .` means this directory, not a project called ".". */
	if (strchr(query, G_DIR_SEPARATOR) != NULL ||
	    g_str_equal(query, ".") || g_str_equal(query, ".."))
	{
		g_autofree gchar *path = g_canonicalize_filename(query, NULL);

		for (l = projects; l != NULL; l = l->next)
		{
			gsize score = path_score(l->data, path);

			if (score > best_score)
			{
				best = l->data;
				best_score = score;
			}
		}

		if (best != NULL)
			return g_object_ref(best);

		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			"No recorded project contains %s", path);
		return NULL;
	}

	fold = g_utf8_casefold(query, -1);

	for (l = projects; l != NULL; l = l->next)
	{
		AiProject *project = l->data;
		g_autofree gchar *name = g_utf8_casefold(project->name, -1);

		if (g_strcmp0(name, fold) == 0)
			g_ptr_array_add(named, project);
	}

	if (named->len == 1)
		return g_object_ref(g_ptr_array_index(named, 0));

	if (named->len == 0)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			"No recorded project named '%s'", query);
		return NULL;
	}

	{
		g_autoptr(GString) roots = g_string_new(NULL);
		guint i;

		for (i = 0; i < named->len; i++)
			g_string_append_printf(roots, "%s%s", i > 0 ? ", " : "",
				((AiProject *)g_ptr_array_index(named, i))->root);

		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			"%u projects are named '%s' (%s); give a path instead",
			named->len, query, roots->str);
	}

	return NULL;
}

/* ================================================================
 * Accessors
 * ================================================================ */

/**
 * ai_project_get_id:
 * @self: a project
 *
 * Returns: (transfer none): see #AiProject:id
 */
const gchar *
ai_project_get_id(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), NULL);
	return self->id;
}

/**
 * ai_project_get_name:
 * @self: a project
 *
 * Returns: (transfer none): see #AiProject:name
 */
const gchar *
ai_project_get_name(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), NULL);
	return self->name;
}

/**
 * ai_project_get_root:
 * @self: a project
 *
 * Returns: (transfer none): see #AiProject:root
 */
const gchar *
ai_project_get_root(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), NULL);
	return self->root;
}

/**
 * ai_project_get_status:
 * @self: a project
 *
 * Returns: (transfer none): see #AiProject:status; "DISCONNECTED" for a
 *   project with no sessions
 */
const gchar *
ai_project_get_status(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), NULL);

	/* Sessions are kept sorted, so the first is the most urgent. */
	if (self->sessions->len == 0)
		return "DISCONNECTED";

	return ai_work_session_get_field(g_ptr_array_index(self->sessions, 0), "status");
}

/**
 * ai_project_get_session_count:
 * @self: a project
 *
 * Returns: see #AiProject:session-count
 */
guint
ai_project_get_session_count(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), 0);
	return self->sessions->len;
}

/**
 * ai_project_get_live_count:
 * @self: a project
 *
 * Returns: see #AiProject:live-count
 */
guint
ai_project_get_live_count(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), 0);
	return self->live;
}

/**
 * ai_project_get_busy_count:
 * @self: a project
 *
 * Returns: see #AiProject:busy-count
 */
guint
ai_project_get_busy_count(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), 0);
	return self->busy;
}

/**
 * ai_project_get_attention_count:
 * @self: a project
 *
 * Returns: see #AiProject:attention-count
 */
guint
ai_project_get_attention_count(AiProject *self)
{
	g_return_val_if_fail(AI_IS_PROJECT(self), 0);
	return self->attention;
}

/**
 * ai_project_dup_sessions:
 * @self: a project
 *
 * The project's sessions, most urgent first.
 *
 * Returns: (transfer container) (element-type AiWorkSession): a new
 *   array holding references; freeing it releases them
 */
GPtrArray *
ai_project_dup_sessions(AiProject *self)
{
	GPtrArray *copy;
	guint i;

	g_return_val_if_fail(AI_IS_PROJECT(self), NULL);

	copy = g_ptr_array_new_full(self->sessions->len, g_object_unref);

	for (i = 0; i < self->sessions->len; i++)
		g_ptr_array_add(copy, g_object_ref(g_ptr_array_index(self->sessions, i)));

	return copy;
}
