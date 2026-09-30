/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-work-session.h"
#include "core/ai-json-util.h"

/* Manifest and material are JSON data, never interpolated instructions. */
static JsonObject *
link_record(AiWorkSession *self, const gchar *url)
{
	g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
	gchar *number, *kind;
	JsonObject *record = json_object_new();
	if (g_str_has_suffix(path, "/")) path[strlen(path) - 1] = '\0';
	number = strrchr(path, '/'); *number++ = '\0';
	kind = strrchr(path, '/'); *kind++ = '\0';
	if (g_str_has_suffix(path, "/-")) path[strlen(path) - 2] = '\0';
	json_object_set_string_member(record, "url", url);
	json_object_set_string_member(record, "host", g_uri_get_host(uri));
	json_object_set_string_member(record, "repository", path + 1);
	json_object_set_string_member(record, "id", number);
	json_object_set_string_member(record, "kind", g_str_equal(kind, "issues") ? "issue" : "pull-request");
	json_object_set_string_member(record, "relationship", ai_work_session_get_link_relationship(self, url));
	return record;
}

/**
 * ai_work_session_dup_link_manifest:
 * @self: a session
 *
 * Serializes all associations as a JSON array, including URL, host, repository,
 * item id, kind and relationship. No I/O. Hosts may persist this beside their
 * own conversation metadata; the session registry already preserves links.
 * Returns: (transfer full): manifest JSON (an empty array when no links exist)
 */
gchar *
ai_work_session_dup_link_manifest(AiWorkSession *self)
{
	g_auto(GStrv) links = NULL;
	g_autoptr(JsonNode) root = json_node_new(JSON_NODE_ARRAY);
	JsonArray *array = json_array_new();
	guint i;
	g_return_val_if_fail(AI_IS_WORK_SESSION(self), NULL);
	json_node_take_array(root, array);
	links = ai_work_session_dup_links(self);
	for (i = 0; links[i] != NULL; i++) json_array_add_object_element(array, link_record(self, links[i]));
	return json_to_string(root, FALSE);
}

typedef struct
{
	JsonNode *root;
	GCancellable *cancel;
	GCancellable *parent;
	gulong parent_id;
	GSource *deadline;
	guint pending;
	gboolean timed_out;
} Context;

typedef struct { GTask *task; guint index; } Pending;

static void
context_free(Context *context)
{
	if (context->deadline != NULL)
	{
		g_source_destroy(context->deadline);
		g_source_unref(context->deadline);
	}
	if (context->parent_id != 0) g_cancellable_disconnect(context->parent, context->parent_id);
	g_clear_object(&context->parent);
	g_clear_object(&context->cancel);
	json_node_unref(context->root);
	g_free(context);
}
static void context_cancel(GCancellable *cancel, gpointer data)
{
	(void)cancel;
	g_cancellable_cancel(data);
}
static gboolean context_timeout(gpointer data)
{
	Context *context = data;
	context->timed_out = TRUE;
	g_clear_pointer(&context->deadline, g_source_unref);
	g_cancellable_cancel(context->cancel);
	return G_SOURCE_REMOVE;
}
static const gchar *
error_category(const GError *error, gboolean timed_out)
{
	if (timed_out && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return "timeout";
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) return "connector-unavailable";
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) return "authorization-failed";
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) return "not-found-or-inaccessible";
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE)) return "network-error";
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) return "timeout";
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)) return "invalid-response";
	return "fetch-failed";
}
static void
context_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
	Pending *pending = data;
	g_autoptr(GTask) task = pending->task;
	Context *context = g_task_get_task_data(task);
	JsonObject *record = json_array_get_object_element(json_node_get_array(context->root), pending->index);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = ai_work_session_refresh_link_finish(AI_WORK_SESSION(source), result, &error);
	g_free(pending);
	if (text != NULL)
	{
		/* Bound prompt size per link, preserving every reference and explicitly
		 * identifying truncation rather than silently claiming complete content. */
		gboolean truncated = strlen(text) > 32768;
		if (truncated)
		{
			gchar *end = text + 32768;
			while ((*end & 0xc0) == 0x80) end--;
			*end = '\0';
		}
		json_object_set_string_member(record, "status", "available");
		json_object_set_string_member(record, "content", text);
		json_object_set_boolean_member(record, "truncated", truncated);
	}
	else
	{
		json_object_set_string_member(record, "status", error_category(error, context->timed_out));
		json_object_set_string_member(record, "error", context->timed_out &&
			g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ?
			"Linked work retrieval exceeded the 30 second deadline" : error->message);
	}
	if (--context->pending == 0)
	{
		if (!g_task_return_error_if_cancelled(task))
			g_task_return_pointer(task, json_to_string(context->root, FALSE), g_free);
	}
}

/**
 * ai_work_session_read_context_async:
 * @self: a session
 * @cancellable: (nullable): cancellation
 * @callback: (scope async) (closure user_data): completion callback
 * @user_data: (nullable): callback data
 *
 * Snapshots links and fetches each using the existing authenticated connectors.
 * Results retain registry order and report per-item errors without hiding other
 * items. A 30 second group deadline cancels outstanding fetches. User cancellation
 * fails the whole operation. Content is limited to 32 KiB per link and marked
 * when truncated. No remote writes. No references yields an empty JSON array.
 */
void
ai_work_session_read_context_async(AiWorkSession *self, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *manifest = NULL;
	Context *context;
	JsonArray *array;
	guint i;
	g_return_if_fail(AI_IS_WORK_SESSION(self));
	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, ai_work_session_read_context_async);
	if (g_task_return_error_if_cancelled(task)) return;
	manifest = ai_work_session_dup_link_manifest(self);
	json_parser_load_from_data(parser, manifest, -1, NULL);
	context = g_new0(Context, 1);
	context->root = json_node_copy(json_parser_get_root(parser));
	context->cancel = g_cancellable_new();
	g_task_set_task_data(task, context, (GDestroyNotify)context_free);
	array = json_node_get_array(context->root);
	context->pending = json_array_get_length(array);
	if (context->pending == 0)
	{
		g_task_return_pointer(task, g_strdup("[]"), g_free);
		return;
	}
	if (cancellable != NULL)
	{
		context->parent = g_object_ref(cancellable);
		context->parent_id = g_cancellable_connect(cancellable, G_CALLBACK(context_cancel),
			g_object_ref(context->cancel), g_object_unref);
	}
	context->deadline = g_timeout_source_new_seconds(30);
	g_source_set_callback(context->deadline, context_timeout, context, NULL);
	g_source_attach(context->deadline, g_main_context_get_thread_default());
	for (i = 0; i < context->pending; i++)
	{
		JsonObject *record = json_array_get_object_element(array, i);
		Pending *pending = g_new0(Pending, 1);
		pending->task = g_object_ref(task); pending->index = i;
		ai_work_session_refresh_link_async(self, ai_json_get_string(record, "url", NULL),
			context->cancel, context_fetched, pending);
	}
}

/**
 * ai_work_session_read_context_finish:
 * @self: a session
 * @result: async result
 * @error: return location for cancellation or whole-operation errors
 * Returns: (transfer full) (nullable): JSON array of references and fetch results
 */
gchar *
ai_work_session_read_context_finish(AiWorkSession *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	g_return_val_if_fail(g_async_result_is_tagged(result, ai_work_session_read_context_async), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
