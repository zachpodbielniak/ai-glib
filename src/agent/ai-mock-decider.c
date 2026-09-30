/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-mock-decider.h"
struct _AiMockDecider { GObject parent_instance; gchar *json; GError *error; };
static void decider_init(AiDeciderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiMockDecider, ai_mock_decider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_DECIDER, decider_init))
static void
mock_async(AiDecider *decider, AiDecisionRequest *request, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	AiMockDecider *self = AI_MOCK_DECIDER(decider);
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiDecisionResponse) response = NULL;
	g_task_set_source_tag(task, mock_async);
	if (g_task_return_error_if_cancelled(task)) return;
	if (self->error != NULL) { g_task_return_error(task, g_error_copy(self->error)); return; }
	response = ai_decision_response_new_from_json(request, self->json, 0, &error);
	if (response == NULL) g_task_return_error(task, g_steal_pointer(&error));
	else g_task_return_pointer(task, g_steal_pointer(&response), g_object_unref);
}
static AiDecisionResponse *mock_finish(AiDecider *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	g_return_val_if_fail(g_async_result_is_tagged(result, mock_async), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
static void decider_init(AiDeciderInterface *iface)
{
	iface->decide_async = mock_async; iface->decide_finish = mock_finish;
}
static void ai_mock_decider_finalize(GObject *object)
{
	AiMockDecider *self = AI_MOCK_DECIDER(object);
	g_free(self->json); g_clear_error(&self->error);
	G_OBJECT_CLASS(ai_mock_decider_parent_class)->finalize(object);
}
static void ai_mock_decider_class_init(AiMockDeciderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ai_mock_decider_finalize;
}
static void ai_mock_decider_init(AiMockDecider *self) { (void)self; }
/**
 * ai_mock_decider_new:
 * @response_json: canned response, validated against each request
 * Returns: (transfer full): an in-memory decision provider
 */
AiMockDecider *ai_mock_decider_new(const gchar *response_json)
{
	g_autoptr(AiMockDecider) self = g_object_new(AI_TYPE_MOCK_DECIDER, NULL);
	self->json = g_strdup(response_json);
	return g_steal_pointer(&self);
}
/**
 * ai_mock_decider_set_error:
 * @self: a mock provider
 * @error: (nullable): error to copy, or NULL to restore canned responses
 */
void ai_mock_decider_set_error(AiMockDecider *self, const GError *error)
{
	g_return_if_fail(AI_IS_MOCK_DECIDER(self));
	g_clear_error(&self->error);
	self->error = error != NULL ? g_error_copy(error) : NULL;
}
