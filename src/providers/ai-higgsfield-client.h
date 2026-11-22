/*
 * ai-higgsfield-client.h - Higgsfield image and video generation API
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <gio/gio.h>

#include "core/ai-media-generator.h"

G_BEGIN_DECLS

/**
 * AI_HIGGSFIELD_DEFAULT_BASE_URL:
 *
 * The API's base URL, as the official documentation gives it.
 */
#define AI_HIGGSFIELD_DEFAULT_BASE_URL "https://api.higgsfield.ai"

/* A few model endpoints, for convenience; the catalog is much larger. */
#define AI_HIGGSFIELD_MODEL_SOUL_V2 "higgsfield-ai/soul/v2/standard"
#define AI_HIGGSFIELD_MODEL_SOUL "higgsfield-ai/soul/standard"
#define AI_HIGGSFIELD_MODEL_KLING_2_5_TURBO_PRO_I2V "kling-video/v2.5-turbo/pro/image-to-video"
#define AI_HIGGSFIELD_MODEL_KLING_2_5_TURBO_PRO_T2V "kling-video/v2.5-turbo/pro/text-to-video"
#define AI_HIGGSFIELD_MODEL_HAILUO_2_3_I2V "minimax/hailuo-2.3/standard/image-to-video"
#define AI_HIGGSFIELD_MODEL_HAILUO_2_3_T2V "minimax/hailuo-2.3/standard/text-to-video"

#define AI_TYPE_HIGGSFIELD_CLIENT (ai_higgsfield_client_get_type())

G_DECLARE_FINAL_TYPE(AiHiggsfieldClient, ai_higgsfield_client, AI, HIGGSFIELD_CLIENT, GObject)

AiHiggsfieldClient *
ai_higgsfield_client_new(void);

AiHiggsfieldClient *
ai_higgsfield_client_new_with_credentials(
	const gchar *key_id,
	const gchar *key_secret
);

void
ai_higgsfield_client_set_credentials(
	AiHiggsfieldClient *self,
	const gchar        *key_id,
	const gchar        *key_secret
);

gboolean
ai_higgsfield_client_has_credentials(
	AiHiggsfieldClient *self
);

const gchar *
ai_higgsfield_client_get_base_url(
	AiHiggsfieldClient *self
);

void
ai_higgsfield_client_set_base_url(
	AiHiggsfieldClient *self,
	const gchar        *base_url
);

guint
ai_higgsfield_client_get_timeout(
	AiHiggsfieldClient *self
);

void
ai_higgsfield_client_set_timeout(
	AiHiggsfieldClient *self,
	guint               seconds
);

void
ai_higgsfield_client_upload_async(
	AiHiggsfieldClient  *self,
	GBytes              *data,
	const gchar         *content_type,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gchar *
ai_higgsfield_client_upload_finish(
	AiHiggsfieldClient  *self,
	GAsyncResult        *result,
	GError             **error
);

G_END_DECLS
