/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-work-session.h"
#include "core/ai-subprocess-util.h"
#include "core/ai-json-util.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const gchar *field_names[] = {"project", "directory", "branch", "title", "provider", "model", "provider-session", "socket", "pane", "activity", "status"};

struct _AiWorkSession
{
	GObject parent_instance;
	gchar *id;
	gchar *fields[G_N_ELEMENTS(field_names)];
	GPtrArray *links;
	GHashTable *titles;
	GHashTable *states;
};
G_DEFINE_TYPE(AiWorkSession, ai_work_session, G_TYPE_OBJECT)

static gint
field_index(const gchar *name)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(field_names); i++)
		if (g_strcmp0(name, field_names[i]) == 0) return (gint)i;
	return -1;
}

static gchar *
clean_label(const gchar *text)
{
	g_autofree gchar *valid = g_utf8_make_valid(text != NULL ? text : "", -1);
	g_autofree gchar *result = g_utf8_substring(valid, 0, MIN(1024, g_utf8_strlen(valid, -1)));
	gchar *p;
	for (p = result; *p; p++) if ((guchar)*p < 32 || *p == 127) *p = ' ';
	return g_steal_pointer(&result);
}

static void
set_field(AiWorkSession *self, guint index, const gchar *value)
{
	g_autofree gchar *safe = index == 3 || index == 9 || index == 10 ?
		clean_label(value) : g_strdup(value != NULL ? value : "");
	if (g_strcmp0(self->fields[index], safe) == 0) return;
	g_free(self->fields[index]);
	self->fields[index] = g_steal_pointer(&safe);
	g_object_notify(G_OBJECT(self), field_names[index]);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiWorkSession *self = AI_WORK_SESSION(object);
	if (id >= 1 && id <= G_N_ELEMENTS(field_names)) g_value_set_string(value, self->fields[id - 1]);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id >= 1 && id <= G_N_ELEMENTS(field_names)) set_field(AI_WORK_SESSION(object), id - 1, g_value_get_string(value));
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
finalize(GObject *object)
{
	AiWorkSession *self = AI_WORK_SESSION(object);
	guint i;
	g_free(self->id);
	for (i = 0; i < G_N_ELEMENTS(field_names); i++) g_free(self->fields[i]);
	g_ptr_array_unref(self->links);
	g_hash_table_unref(self->titles);
	g_hash_table_unref(self->states);
	G_OBJECT_CLASS(ai_work_session_parent_class)->finalize(object);
}

static void
ai_work_session_class_init(AiWorkSessionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	guint i;
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	for (i = 0; i < G_N_ELEMENTS(field_names); i++)
		g_object_class_install_property(object_class, i + 1,
			g_param_spec_string(field_names[i], field_names[i], field_names[i], "",
				G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));
}

static void
ai_work_session_init(AiWorkSession *self)
{
	guint i;
	self->id = g_uuid_string_random();
	self->links = g_ptr_array_new_with_free_func(g_free);
	self->titles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; i < G_N_ELEMENTS(field_names); i++) self->fields[i] = g_strdup("");
}

static gchar *
git_field(const gchar *directory, const gchar *argument)
{
	g_autoptr(GSubprocess) child = NULL;
	g_autofree gchar *text = NULL;
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		NULL, "git", "-C", directory, "rev-parse", argument, g_str_equal(argument, "--abbrev-ref") ? "HEAD" : NULL, NULL);
	if (child == NULL || !ai_subprocess_communicate_utf8_bounded(child, NULL, 2000, NULL, &text, NULL, NULL) ||
		!g_subprocess_get_successful(child)) return NULL;
	g_strchomp(text);
	return g_steal_pointer(&text);
}

/**
 * ai_work_session_new:
 * @directory: local working directory
 *
 * Creates an independent task identity. Linked worktrees share their canonical
 * Git common directory as project identity; non-Git directories use their path.
 * Git discovery is synchronous and bounded; call from a worker for UI use.
 * Returns: (transfer full): a new session
 */
AiWorkSession *
ai_work_session_new(const gchar *directory)
{
	g_autoptr(AiWorkSession) self = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autofree gchar *cwd = NULL;
	g_autofree gchar *common = NULL;
	g_autofree gchar *project = NULL;
	g_autofree gchar *branch = NULL;
	g_return_val_if_fail(directory != NULL, NULL);
	cwd = g_canonicalize_filename(directory, NULL);
	common = git_field(cwd, "--git-common-dir");
	project = common != NULL ? g_canonicalize_filename(common, cwd) : g_strdup(cwd);
	branch = git_field(cwd, "--abbrev-ref");
	g_object_set(self, "directory", cwd, "project", project, "branch", branch,
		"status", "IDLE", NULL);
	return g_steal_pointer(&self);
}

/**
 * ai_work_session_get_id:
 * @self: a session
 * Returns: (transfer none): stable UUID, independent of the provider
 */
const gchar *
ai_work_session_get_id(AiWorkSession *self)
{
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), NULL);
	return self->id;
}

/**
 * ai_work_session_get_field:
 * @self: a session
 * @field: string property name
 * Returns: (transfer none) (nullable): value, or %NULL for an unknown field
 */
const gchar *
ai_work_session_get_field(AiWorkSession *self, const gchar *field)
{
	gint i;
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), NULL);
	i = field_index(field);
	return i >= 0 ? self->fields[i] : NULL;
}

/**
 * ai_work_session_update:
 * @self: a session
 * @busy: foreground work remains
 * @attention: explicit input required
 * @background: number of active background agents
 * @outcome: (nullable): DONE, ERROR, STOPPED, or %NULL for idle
 *
 * Input takes precedence over work; active work takes precedence over outcome.
 */
void
ai_work_session_update(AiWorkSession *self, gboolean busy, gboolean attention,
					  guint background, const gchar *outcome)
{
	const gchar *status = "IDLE";
	g_return_if_fail(AI_IS_WORK_SESSION(self));
	if (attention) status = "INPUT";
	else if (busy || background != 0) status = "WORK";
	else if (g_strcmp0(outcome, "DONE") == 0 || g_strcmp0(outcome, "ERROR") == 0 ||
	         g_strcmp0(outcome, "STOPPED") == 0) status = outcome;
	g_object_set(self, "status", status, NULL);
}

/**
 * ai_work_session_add_link:
 * @self: a session
 * @url: absolute HTTP(S) issue, pull request or merge request URL
 * @error: return location for an error
 *
 * Stores an explicit association, never changes a remote assignment. Duplicate
 * links are idempotent. Credentials, query strings and fragments are rejected.
 * Returns: whether the URL is valid and linked
 */
gboolean
ai_work_session_add_link(AiWorkSession *self, const gchar *url, GError **error)
{
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GRegex) pattern = NULL;
	guint i;
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), FALSE);
	if (url == NULL || strlen(url) > 4096 || !g_utf8_validate(url, -1, NULL)) goto invalid;
	for (i = 0; url[i]; i++) if ((guchar)url[i] <= 32 || url[i] == 127) goto invalid;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || (g_strcmp0(g_uri_get_scheme(uri), "https") != 0 &&
		g_strcmp0(g_uri_get_scheme(uri), "http") != 0) || g_uri_get_host(uri) == NULL ||
		g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL ||
		g_uri_get_fragment(uri) != NULL) goto invalid;
	pattern = g_regex_new("^/[^/]+/.+/(issues|pull|pulls|merge_requests)/[1-9][0-9]*/?$", 0, 0, NULL);
	if (!g_regex_match(pattern, g_uri_get_path(uri), 0, NULL)) goto invalid;
	for (i = 0; i < self->links->len; i++)
		if (g_str_equal(url, g_ptr_array_index(self->links, i))) return TRUE;
	if (self->links->len >= 32) goto invalid;
	g_ptr_array_add(self->links, g_strdup(url));
	return TRUE;
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		"Use a full issue or PR URL without credentials, query or fragment (maximum 32 links)");
	return FALSE;
}

/**
 * ai_work_session_remove_link:
 * @self: a session
 * @url: previously linked URL
 * Returns: whether a link was removed
 */
gboolean
ai_work_session_remove_link(AiWorkSession *self, const gchar *url)
{
	guint i;
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), FALSE);
	for (i = 0; i < self->links->len; i++)
		if (g_strcmp0(url, g_ptr_array_index(self->links, i)) == 0)
			{ g_ptr_array_remove_index(self->links, i); return TRUE; }
	return FALSE;
}

/**
 * ai_work_session_dup_links:
 * @self: a session
 * Returns: (transfer full) (array zero-terminated=1): linked URLs
 */
gchar **
ai_work_session_dup_links(AiWorkSession *self)
{
	g_auto(GStrv) result = NULL;
	guint i;
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), NULL);
	result = g_new0(gchar *, self->links->len + 1);
	for (i = 0; i < self->links->len; i++) result[i] = g_strdup(g_ptr_array_index(self->links, i));
	return g_steal_pointer(&result);
}

/**
 * ai_work_session_default_directory:
 * Returns: (transfer full): user state directory for task records
 */
gchar *
ai_work_session_default_directory(void)
{
	return g_build_filename(g_get_user_state_dir(), "ai-glib", "sessions", NULL);
}

/**
 * ai_work_session_save:
 * @self: a session owned by this process
 * @directory: registry directory
 * @live: whether this process still owns a live conversation
 * @error: return location for an error
 *
 * Atomically replaces this session's record, mode 0600. Each process writes only
 * its own UUID. Metadata survives exit; liveness expires after fifteen seconds.
 * Returns: whether the snapshot was saved
 */
gboolean
ai_work_session_save(AiWorkSession *self, const gchar *directory, gboolean live, GError **error)
{
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_autofree gchar *path = NULL;
	g_autofree gchar *data = NULL;
	g_auto(GStrv) links = NULL;
	gsize length;
	guint i;
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), FALSE);
	g_return_val_if_fail(directory != NULL, FALSE);
	if (g_mkdir_with_parents(directory, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create session registry: %s", g_strerror(errno));
		return FALSE;
	}
	for (i = 0; i < G_N_ELEMENTS(field_names); i++)
		g_key_file_set_string(file, "session", field_names[i], self->fields[i]);
	links = ai_work_session_dup_links(self);
	g_key_file_set_string_list(file, "session", "links", (const gchar * const *)links, g_strv_length(links));
	for (i = 0; links[i] != NULL; i++)
	{
		g_autofree gchar *group = g_strdup_printf("link-%u", i);
		const gchar *title = g_hash_table_lookup(self->titles, links[i]);
		const gchar *state = g_hash_table_lookup(self->states, links[i]);
		if (title != NULL) g_key_file_set_string(file, group, "title", title);
		if (state != NULL) g_key_file_set_string(file, group, "state", state);
	}
	g_key_file_set_integer(file, "session", "version", 1);
	g_key_file_set_int64(file, "session", "heartbeat", g_get_real_time());
	g_key_file_set_boolean(file, "session", "live", live);
	data = g_key_file_to_data(file, &length, NULL);
	if (length > 512 * 1024)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Session metadata exceeds 512 KiB");
		return FALSE;
	}
	path = g_build_filename(directory, self->id, NULL);
	return g_file_set_contents_full(path, data, length,
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

/**
 * ai_work_session_list:
 * @directory: registry directory
 * @error: return location for directory errors
 *
 * Reads bounded, versioned snapshots. Malformed entries are ignored. Expired or
 * released sessions report DISCONNECTED; their links and resume IDs survive.
 * Free the returned list with g_list_free_full(list, g_object_unref). An empty
 * registry returns NULL without setting @error.
 * Returns: (transfer full) (element-type AiWorkSession) (nullable): sessions
 */
GList *
ai_work_session_list(const gchar *directory, GError **error)
{
	g_autolist(AiWorkSession) result = NULL;
	g_autoptr(GDir) dir = NULL;
	const gchar *name;
	g_return_val_if_fail(directory != NULL, NULL);
	if (!g_file_test(directory, G_FILE_TEST_EXISTS)) return g_steal_pointer(&result);
	dir = g_dir_open(directory, 0, error);
	if (dir == NULL) return NULL;
	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = NULL;
		g_autoptr(GKeyFile) file = g_key_file_new();
		g_autoptr(AiWorkSession) item = NULL;
		g_auto(GStrv) links = NULL;
		GStatBuf st;
		gint64 age;
		guint i;
		gboolean valid = TRUE;
		if (!g_uuid_string_is_valid(name)) continue;
		path = g_build_filename(directory, name, NULL);
		if (g_lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 512 * 1024 ||
			!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL) ||
			g_key_file_get_integer(file, "session", "version", NULL) != 1) continue;
		item = g_object_new(AI_TYPE_WORK_SESSION, NULL);
		g_free(item->id);
		item->id = g_strdup(name);
		for (i = 0; i < G_N_ELEMENTS(field_names); i++)
		{
			g_autofree gchar *value = g_key_file_get_string(file, "session", field_names[i], NULL);
			if (value == NULL) { valid = FALSE; break; }
			set_field(item, i, value);
		}
		if (!valid) continue;
		links = g_key_file_get_string_list(file, "session", "links", NULL, NULL);
		for (i = 0; links != NULL && links[i] != NULL; i++)
			if (!ai_work_session_add_link(item, links[i], NULL)) { valid = FALSE; break; }
		if (!valid) continue;
		for (i = 0; links != NULL && links[i] != NULL; i++)
		{
			g_autofree gchar *group = g_strdup_printf("link-%u", i);
			g_autofree gchar *title = g_key_file_get_string(file, group, "title", NULL);
			g_autofree gchar *state = g_key_file_get_string(file, group, "state", NULL);
			if (title != NULL) g_hash_table_replace(item->titles, g_strdup(links[i]), clean_label(title));
			if (state != NULL) g_hash_table_replace(item->states, g_strdup(links[i]), clean_label(state));
		}
		age = g_get_real_time() - g_key_file_get_int64(file, "session", "heartbeat", NULL);
		if (age < 0 || age > 15 * G_USEC_PER_SEC || !g_key_file_get_boolean(file, "session", "live", NULL))
			g_object_set(item, "status", "DISCONNECTED", NULL);
		result = g_list_prepend(result, g_steal_pointer(&item));
	}
	return g_steal_pointer(&result);
}

/* CLI adapters reuse the user's configured authentication. Explicit URL repo
 * selection prevents a link from being fetched in the current repo by accident. */
typedef struct
{
	gchar *url;
	gchar *title;
	gchar *state;
} LinkFetch;

static void
link_fetch_free(LinkFetch *fetch)
{
	g_free(fetch->url); g_free(fetch->title); g_free(fetch->state); g_free(fetch);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(LinkFetch, link_fetch_free)

static gchar *
link_tea_login(GUri *uri, GCancellable *cancel, GError **error)
{
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *output = NULL;
	g_autofree gchar *match = NULL;
	JsonNode *root;
	JsonArray *accounts;
	guint i;
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		error, "tea", "login", "list", "--output", "json", NULL);
	if (child == NULL || !ai_subprocess_communicate_utf8_bounded(child, NULL, 5000, cancel, &output, NULL, error)) return NULL;
	if (!g_subprocess_get_successful(child) || !json_parser_load_from_data(parser, output, -1, NULL)) goto invalid;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root)) goto invalid;
	accounts = json_node_get_array(root);
	for (i = 0; i < json_array_get_length(accounts); i++)
	{
		JsonNode *node = json_array_get_element(accounts, i);
		JsonObject *account;
		const gchar *url, *name;
		g_autoptr(GUri) host = NULL;
		if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
		account = json_node_get_object(node);
		url = ai_json_get_string(account, "url", NULL);
		name = ai_json_get_string(account, "name", NULL);
		if (url == NULL || name == NULL) continue;
		host = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
		if (host == NULL || g_strcmp0(g_uri_get_host(host), g_uri_get_host(uri)) != 0 ||
			g_strcmp0(g_uri_get_scheme(host), g_uri_get_scheme(uri)) != 0 ||
			g_uri_get_port(host) != g_uri_get_port(uri)) continue;
		if (match != NULL) goto invalid;
		match = g_strdup(name);
	}
	if (match != NULL) return g_steal_pointer(&match);
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		"Link metadata needs one authenticated tea account matching this host");
	return NULL;
}

static void
link_fetch_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	LinkFetch *fetch = data;
	g_autoptr(GUri) uri = g_uri_parse(fetch->url, G_URI_FLAGS_NONE, NULL);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *login = NULL;
	g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
	gchar *number = strrchr(path, '/');
	gchar *kind;
	const gchar *title, *state, *body;
	JsonObject *object;
	gboolean github = g_str_equal(g_uri_get_host(uri), "github.com");
	(void)source;
	if (number != NULL && number[1] == '\0') { *number = '\0'; number = strrchr(path, '/'); }
	*number++ = '\0';
	kind = strrchr(path, '/');
	*kind++ = '\0';
	repo = g_strdup(path + 1);
	if (!github)
	{
		login = link_tea_login(uri, cancel, &error);
		if (login == NULL) { g_task_return_error(task, g_steal_pointer(&error)); return; }
	}
	if (github)
		child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
			&error, "gh", g_str_equal(kind, "issues") ? "issue" : "pr", "view", fetch->url,
			"--json", "title,body,state,url", NULL);
	else
		child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
			&error, "tea", "issue", "--login", login, "--repo", repo, "--output", "json", number, NULL);
	if (child == NULL || !ai_subprocess_communicate_utf8_bounded(child, NULL, 15000, cancel, &output, NULL, &error))
	{
		g_task_return_error(task, g_steal_pointer(&error)); return;
	}
	if (!g_subprocess_get_successful(child) || output == NULL || strlen(output) > 1024 * 1024 ||
		!json_parser_load_from_data(parser, output, -1, NULL) ||
		(object = ai_json_root_object(parser)) == NULL)
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Could not fetch link details; authenticate %s for this host, or open the URL", github ? "gh" : "tea"); return;
	}
	title = ai_json_get_string(object, "title", NULL);
	state = ai_json_get_string(object, "state", NULL);
	body = ai_json_get_string(object, "body", "");
	if (title == NULL || state == NULL)
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Link response has no title or state"); return;
	}
	fetch->title = clean_label(title);
	fetch->state = clean_label(state);
	g_task_return_pointer(task, g_strdup_printf("Assigned work: %s\nSource: %s\nState: %s\n\n%s", title, fetch->url, state, body), g_free);
}

/**
 * ai_work_session_refresh_link_async:
 * @self: a session
 * @url: previously linked URL
 * @cancellable: (nullable): cancellation
 * @callback: (scope async) (closure user_data): completion callback
 * @user_data: (nullable): callback data
 *
 * Fetches title, state and assignment text using gh for github.com, or tea for
 * Forgejo/Gitea hosts. No remote mutation occurs. Other forges can retain links
 * and use their browser; fetching requires a supported authenticated adapter.
 */
void
ai_work_session_refresh_link_async(AiWorkSession *self, const gchar *url,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;
	g_autoptr(LinkFetch) fetch = NULL;
	guint i;
	g_return_if_fail(AI_IS_WORK_SESSION(self));
	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, ai_work_session_refresh_link_async);
	for (i = 0; i < self->links->len; i++) if (g_strcmp0(url, g_ptr_array_index(self->links, i)) == 0) break;
	if (i == self->links->len)
	{
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Link is not associated with this session"); return;
	}
	fetch = g_new0(LinkFetch, 1);
	fetch->url = g_strdup(url);
	g_task_set_task_data(task, g_steal_pointer(&fetch), (GDestroyNotify)link_fetch_free);
	g_task_run_in_thread(task, link_fetch_thread);
}

/**
 * ai_work_session_refresh_link_finish:
 * @self: a session
 * @result: asynchronous result
 * @error: return location for errors
 * Returns: (transfer full) (nullable): assignment text; title/state are cached
 */
gchar *
ai_work_session_refresh_link_finish(AiWorkSession *self, GAsyncResult *result, GError **error)
{
	LinkFetch *fetch;
	g_autofree gchar *text = NULL;
	guint i;
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	g_return_val_if_fail(g_async_result_is_tagged(result, ai_work_session_refresh_link_async), NULL);
	text = g_task_propagate_pointer(G_TASK(result), error);
	if (text == NULL) return NULL;
	fetch = g_task_get_task_data(G_TASK(result));
	for (i = 0; i < self->links->len; i++)
		if (g_str_equal(fetch->url, g_ptr_array_index(self->links, i)))
		{
			g_hash_table_replace(self->titles, g_strdup(fetch->url), g_strdup(fetch->title));
			g_hash_table_replace(self->states, g_strdup(fetch->url), g_strdup(fetch->state));
			break;
		}
	return g_steal_pointer(&text);
}

/**
 * ai_work_session_get_link_title:
 * @self: a session
 * @url: linked URL
 * Returns: (transfer none) (nullable): last fetched title
 */
const gchar *
ai_work_session_get_link_title(AiWorkSession *self, const gchar *url)
{
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), NULL);
	return g_hash_table_lookup(self->titles, url);
}

/**
 * ai_work_session_get_link_state:
 * @self: a session
 * @url: linked URL
 * Returns: (transfer none) (nullable): last fetched remote state
 */
const gchar *
ai_work_session_get_link_state(AiWorkSession *self, const gchar *url)
{
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), NULL);
	return g_hash_table_lookup(self->states, url);
}
