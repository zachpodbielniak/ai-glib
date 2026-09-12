/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_COMPILATION)
#error "This is a private ai-glib header."
#endif
#include "core/ai-image-generator.h"
#include "core/ai-video-generator.h"
#include "core/ai-cli-client.h"
void ai_grok_media_image_init(AiImageGeneratorInterface *iface);
void ai_grok_media_video_init(AiVideoGeneratorInterface *iface);
gboolean ai_grok_media_parse_event(AiCliClient *client, JsonObject *event, GError **error);
