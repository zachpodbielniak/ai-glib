/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include "core/ai-video-generator.h"
#include "core/ai-error.h"

static void ai_video_generator_default_init(AiVideoGeneratorInterface *iface);
G_DEFINE_INTERFACE(AiVideoGenerator, ai_video_generator, G_TYPE_OBJECT)

/* Implementations own progress semantics; the common contract returns an artifact. */
static void
ai_video_generator_default_init(AiVideoGeneratorInterface *iface)
{
	(void)iface;
}

/**
 * ai_video_generator_generate_video_async:
 * @self: a video generator
 * @request: generation parameters
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async) (nullable): completion callback
 * @user_data: callback data
 *
 * Starts generation and provider polling. Providers copy inputs before return.
 * Completion is delivered on the initiating thread-default main context.
 * Cancellation stops local work; a submitted remote job may still be billed.
 */
void
ai_video_generator_generate_video_async(
	AiVideoGenerator *self,
	AiVideoRequest *request,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data
){
	AiVideoGeneratorInterface *iface;
	const gchar *prompt;
	g_autoptr(GTask) task = NULL;
	g_return_if_fail(AI_IS_VIDEO_GENERATOR(self));
	g_return_if_fail(request != NULL);
	iface = AI_VIDEO_GENERATOR_GET_IFACE(self);
	prompt = ai_video_request_get_prompt(request);

	/* Return recoverable errors through the async contract, including an
	 * incomplete third-party implementation, rather than abandoning callbacks. */
	if (iface->generate_video_async == NULL || iface->generate_video_finish == NULL ||
	    prompt == NULL || prompt[0] == '\0' ||
	    ai_video_request_get_poll_interval_ms(request) == 0 ||
	    ai_video_request_get_timeout_ms(request) == 0)
	{
		task = g_task_new(self, cancellable, callback, user_data);
		g_task_set_source_tag(task, ai_video_generator_generate_video_async);
		if (iface->generate_video_async == NULL || iface->generate_video_finish == NULL)
			g_task_return_new_error(task, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
			                        "Provider does not implement video generation");
		else
			g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                        "Video prompt and positive polling/timeout values are required");
		return;
	}
	iface->generate_video_async(self, request, cancellable, callback, user_data);
}

/**
 * ai_video_generator_generate_video_finish:
 * @self: a video generator
 * @result: asynchronous result
 * @error: (out) (optional): return location for an error
 *
 * Completes generation and transfers the completed artifact to the caller.
 *
 * Returns: (transfer full) (nullable): the completed video, or NULL on error
 */
AiVideoResponse *
ai_video_generator_generate_video_finish(
	AiVideoGenerator *self,
	GAsyncResult *result,
	GError **error
){
	AiVideoGeneratorInterface *iface;
	g_return_val_if_fail(AI_IS_VIDEO_GENERATOR(self), NULL);
	g_return_val_if_fail(G_IS_ASYNC_RESULT(result), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);
	if (g_async_result_is_tagged(result, ai_video_generator_generate_video_async))
	{
		g_return_val_if_fail(g_task_is_valid(result, self), NULL);
		return (AiVideoResponse *)g_task_propagate_pointer(G_TASK(result), error);
	}
	iface = AI_VIDEO_GENERATOR_GET_IFACE(self);
	if (iface->generate_video_finish == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		                    "Provider does not implement video completion");
		return NULL;
	}
	return iface->generate_video_finish(self, result, error);
}

/* Sync state belongs to the private loop and cannot re-enter a caller's UI. */
typedef struct
{
	GMainLoop *loop;
	AiVideoResponse *response;
	GError *error;
	gboolean done;
} VideoSync;

static void
video_sync_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	VideoSync *state = (VideoSync *)user_data;
	state->response = ai_video_generator_generate_video_finish(
		AI_VIDEO_GENERATOR(source), result, &state->error);
	state->done = TRUE;
	g_main_loop_quit(state->loop);
}

/**
 * ai_video_generator_generate_video:
 * @self: a video generator
 * @request: generation parameters
 * @cancellable: (nullable): cancellation token
 * @error: (out) (optional): return location for an error
 *
 * Waits for completion on a private main context. Unrelated sources on the
 * caller's context are not dispatched. Prefer the async API in user interfaces.
 *
 * Returns: (transfer full) (nullable): a completed video, or NULL on error
 */
AiVideoResponse *
ai_video_generator_generate_video(
	AiVideoGenerator *self,
	AiVideoRequest *request,
	GCancellable *cancellable,
	GError **error
){
	g_autoptr(GMainContext) context = NULL;
	VideoSync state = { NULL, NULL, NULL, FALSE };
	g_return_val_if_fail(AI_IS_VIDEO_GENERATOR(self), NULL);
	g_return_val_if_fail(request != NULL, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);
	context = g_main_context_new();
	g_main_context_push_thread_default(context);
	state.loop = g_main_loop_new(context, FALSE);
	ai_video_generator_generate_video_async(self, request, cancellable,
	                                       video_sync_done, &state);
	if (!state.done)
		g_main_loop_run(state.loop);
	g_main_loop_unref(state.loop);
	g_main_context_pop_thread_default(context);
	if (state.error != NULL)
	{
		g_propagate_error(error, state.error);
		return NULL;
	}
	return state.response;
}

/**
 * ai_video_generator_get_default_model:
 * @self: a video generator
 *
 * Returns the default media model independently of chat model selection.
 *
 * Returns: (transfer none) (nullable): the default model identifier
 */
const gchar *
ai_video_generator_get_default_model(AiVideoGenerator *self)
{
	AiVideoGeneratorInterface *iface;
	g_return_val_if_fail(AI_IS_VIDEO_GENERATOR(self), NULL);
	iface = AI_VIDEO_GENERATOR_GET_IFACE(self);
	return iface->get_default_model != NULL ? iface->get_default_model(self) : NULL;
}
