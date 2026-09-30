/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-decider.h"
#include "ai-error.h"

static void ai_decider_default_init(AiDeciderInterface *iface) { (void)iface; }
G_DEFINE_INTERFACE(AiDecider, ai_decider, G_TYPE_OBJECT)

/**
 * ai_decider_decide_async:
 * @self: a decision provider
 * @request: typed questions and input text
 * @cancellable: (nullable): cancellation
 * @callback: (scope async) (closure user_data): completion callback
 * @user_data: (nullable): callback data
 *
 * Validates and snapshots the request before dispatch. Providers must retain
 * the snapshot if needed after their vfunc returns. Completion uses the
 * calling thread's thread-default context.
 */
void
ai_decider_decide_async(AiDecider *self, AiDecisionRequest *request,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(AiDecisionRequest) snapshot = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(GError) error = NULL;
	AiDeciderInterface *iface;
	g_return_if_fail(AI_IS_DECIDER(self));
	g_return_if_fail(AI_IS_DECISION_REQUEST(request));
	iface = AI_DECIDER_GET_IFACE(self);
	json = ai_decision_request_dup_json(request);
	snapshot = ai_decision_request_new_from_json(json, &error);
	if (snapshot == NULL || iface->decide_async == NULL || iface->decide_finish == NULL)
	{
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_set_source_tag(task, ai_decider_decide_async);
		if (error != NULL) g_task_return_error(task, g_steal_pointer(&error));
		else g_task_return_new_error(task, AI_ERROR, AI_ERROR_NOT_SUPPORTED, "Provider does not implement decisions");
		return;
	}
	iface->decide_async(self, snapshot, cancellable, callback, user_data);
}

/**
 * ai_decider_decide_finish:
 * @self: a decision provider
 * @result: asynchronous result
 * @error: return location for an error
 * Returns: (transfer full) (nullable): validated decision results
 */
AiDecisionResponse *
ai_decider_decide_finish(AiDecider *self, GAsyncResult *result, GError **error)
{
	AiDeciderInterface *iface;
	g_return_val_if_fail(AI_IS_DECIDER(self), NULL);
	g_return_val_if_fail(G_IS_ASYNC_RESULT(result), NULL);
	if (g_async_result_is_tagged(result, ai_decider_decide_async))
	{
		g_return_val_if_fail(g_task_is_valid(result, self), NULL);
		return g_task_propagate_pointer(G_TASK(result), error);
	}
	iface = AI_DECIDER_GET_IFACE(self);
	if (iface->decide_finish == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED, "Provider does not implement decisions");
		return NULL;
	}
	return iface->decide_finish(self, result, error);
}

typedef struct { GMainLoop *loop; AiDecisionResponse *response; GError *error; } Sync;
static void
sync_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Sync *sync = user_data;
	sync->response = ai_decider_decide_finish(AI_DECIDER(source), result, &sync->error);
	g_main_loop_quit(sync->loop);
}

/**
 * ai_decider_decide:
 * @self: a decision provider
 * @request: typed questions and text
 * @cancellable: (nullable): cancellation
 * @error: return location for an error
 *
 * Blocks using a private main context. A server on the caller's normal
 * context must run on another thread to service this call.
 * Returns: (transfer full) (nullable): validated results
 */
AiDecisionResponse *
ai_decider_decide(AiDecider *self, AiDecisionRequest *request, GCancellable *cancellable, GError **error)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GMainLoop) loop = g_main_loop_new(context, FALSE);
	Sync sync = { NULL, NULL, NULL };
	g_return_val_if_fail(AI_IS_DECIDER(self), NULL);
	g_return_val_if_fail(AI_IS_DECISION_REQUEST(request), NULL);
	sync.loop = loop;
	g_main_context_push_thread_default(context);
	ai_decider_decide_async(self, request, cancellable, sync_done, &sync);
	g_main_loop_run(loop);
	g_main_context_pop_thread_default(context);
	if (sync.error != NULL) g_propagate_error(error, sync.error);
	return sync.response;
}
