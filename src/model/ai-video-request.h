/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "core/ai-enums.h"
#include "model/ai-image.h"
G_BEGIN_DECLS
#define AI_TYPE_VIDEO_REQUEST (ai_video_request_get_type())
typedef struct _AiVideoRequest AiVideoRequest;
GType ai_video_request_get_type(void);
AiVideoRequest *ai_video_request_new(const gchar *prompt);
AiVideoRequest *ai_video_request_copy(const AiVideoRequest *self);
void ai_video_request_free(AiVideoRequest *self);
const gchar *
ai_video_request_get_prompt(const AiVideoRequest *self);
void
ai_video_request_set_prompt(AiVideoRequest *self, const gchar * value);
const gchar *
ai_video_request_get_model(const AiVideoRequest *self);
void
ai_video_request_set_model(AiVideoRequest *self, const gchar * value);
const gchar *
ai_video_request_get_operation(const AiVideoRequest *self);
void
ai_video_request_set_operation(AiVideoRequest *self, const gchar * value);
const gchar *
ai_video_request_get_aspect_ratio(const AiVideoRequest *self);
void
ai_video_request_set_aspect_ratio(AiVideoRequest *self, const gchar * value);
const gchar *
ai_video_request_get_resolution(const AiVideoRequest *self);
void
ai_video_request_set_resolution(AiVideoRequest *self, const gchar * value);
gint
ai_video_request_get_duration(const AiVideoRequest *self);
void
ai_video_request_set_duration(AiVideoRequest *self, gint value);
AiTriState
ai_video_request_get_generate_audio(const AiVideoRequest *self);
void
ai_video_request_set_generate_audio(AiVideoRequest *self, AiTriState value);
guint
ai_video_request_get_poll_interval_ms(const AiVideoRequest *self);
void
ai_video_request_set_poll_interval_ms(AiVideoRequest *self, guint value);
guint
ai_video_request_get_timeout_ms(const AiVideoRequest *self);
void
ai_video_request_set_timeout_ms(AiVideoRequest *self, guint value);
AiImage *ai_video_request_get_image(const AiVideoRequest *self);
void ai_video_request_set_image(AiVideoRequest *self, AiImage *image);
void ai_video_request_add_reference_image(AiVideoRequest *self, AiImage *image);
GList *ai_video_request_get_reference_images(const AiVideoRequest *self);
guint ai_video_request_get_reference_image_count(const AiVideoRequest *self);
const gchar * const *ai_video_request_get_voices(const AiVideoRequest *self);
void ai_video_request_set_voices(AiVideoRequest *self, const gchar * const *voices);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiVideoRequest, ai_video_request_free)
G_END_DECLS
