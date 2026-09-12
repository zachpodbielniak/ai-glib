/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_VIDEO_RESPONSE (ai_video_response_get_type())
typedef struct _AiVideoResponse AiVideoResponse;
GType ai_video_response_get_type(void);
AiVideoResponse *ai_video_response_new(const gchar *url, const gchar *model);
AiVideoResponse *ai_video_response_copy(const AiVideoResponse *self);
void ai_video_response_free(AiVideoResponse *self);
const gchar *ai_video_response_get_url(const AiVideoResponse *self);
const gchar *ai_video_response_get_model(const AiVideoResponse *self);
const gchar *ai_video_response_get_request_id(const AiVideoResponse *self);
void ai_video_response_set_request_id(AiVideoResponse *self, const gchar *request_id);
gdouble ai_video_response_get_duration(const AiVideoResponse *self);
void ai_video_response_set_duration(AiVideoResponse *self, gdouble duration);
gboolean ai_video_response_save_to_file(AiVideoResponse *self, const gchar *path, GCancellable *cancellable, GError **error);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiVideoResponse, ai_video_response_free)
G_END_DECLS
