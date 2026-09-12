/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
#include "model/ai-video-request.h"
#include "model/ai-video-response.h"
G_BEGIN_DECLS
#define AI_TYPE_VIDEO_GENERATOR (ai_video_generator_get_type())
G_DECLARE_INTERFACE(AiVideoGenerator, ai_video_generator, AI, VIDEO_GENERATOR, GObject)
/**
 * AiVideoGeneratorInterface:
 * @parent_iface: parent interface
 * @generate_video_async: starts generation, copying request inputs
 * @generate_video_finish: completes generation
 * @get_default_model: returns the provider's video model
 * @_reserved: reserved ABI slots
 *
 * Providers return completed artifacts after any required polling.
 */
struct _AiVideoGeneratorInterface
{
	GTypeInterface parent_iface;
	void (*generate_video_async)(AiVideoGenerator *self, AiVideoRequest *request,
	                             GCancellable *cancellable,
	                             GAsyncReadyCallback callback, gpointer user_data);
	AiVideoResponse *(*generate_video_finish)(AiVideoGenerator *self,
	                                         GAsyncResult *result, GError **error);
	const gchar *(*get_default_model)(AiVideoGenerator *self);
	gpointer _reserved[8];
};
void ai_video_generator_generate_video_async(AiVideoGenerator *self,
	AiVideoRequest *request, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data);
AiVideoResponse *ai_video_generator_generate_video_finish(AiVideoGenerator *self,
	GAsyncResult *result, GError **error);
AiVideoResponse *ai_video_generator_generate_video(AiVideoGenerator *self,
	AiVideoRequest *request, GCancellable *cancellable, GError **error);
const gchar *ai_video_generator_get_default_model(AiVideoGenerator *self);
G_END_DECLS
