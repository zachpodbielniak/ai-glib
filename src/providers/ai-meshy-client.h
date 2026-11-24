/*
 * ai-meshy-client.h - Meshy 3D generation API
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
#include <json-glib/json-glib.h>

#include "core/ai-media-generator.h"

G_BEGIN_DECLS

/**
 * AI_MESHY_DEFAULT_BASE_URL:
 *
 * The API's origin.  Paths under it begin with `openapi/`.
 */
#define AI_MESHY_DEFAULT_BASE_URL "https://api.meshy.ai"

/**
 * AI_MESHY_MAX_IMAGE_BYTES:
 *
 * The largest input image Meshy accepts, as a URL or a data URI: 20 MB.
 */
#define AI_MESHY_MAX_IMAGE_BYTES (20 * 1024 * 1024)

/* The operations, named by the request's `operation` parameter or its model. */
#define AI_MESHY_OPERATION_TEXT_TO_3D "text-to-3d"
#define AI_MESHY_OPERATION_IMAGE_TO_3D "image-to-3d"
#define AI_MESHY_OPERATION_MULTI_IMAGE_TO_3D "multi-image-to-3d"
#define AI_MESHY_OPERATION_RETEXTURE "retexture"
#define AI_MESHY_OPERATION_REMESH "remesh"
#define AI_MESHY_OPERATION_RIG "rig"
#define AI_MESHY_OPERATION_ANIMATE "animate"

#define AI_TYPE_MESHY_CLIENT (ai_meshy_client_get_type())

G_DECLARE_FINAL_TYPE(AiMeshyClient, ai_meshy_client, AI, MESHY_CLIENT, GObject)

AiMeshyClient *
ai_meshy_client_new(void);

AiMeshyClient *
ai_meshy_client_new_with_api_key(
	const gchar *api_key
);

void
ai_meshy_client_set_api_key(
	AiMeshyClient *self,
	const gchar   *api_key
);

gboolean
ai_meshy_client_has_api_key(
	AiMeshyClient *self
);

const gchar *
ai_meshy_client_get_base_url(
	AiMeshyClient *self
);

void
ai_meshy_client_set_base_url(
	AiMeshyClient *self,
	const gchar   *base_url
);

guint
ai_meshy_client_get_timeout(
	AiMeshyClient *self
);

void
ai_meshy_client_set_timeout(
	AiMeshyClient *self,
	guint          seconds
);

void
ai_meshy_client_balance_async(
	AiMeshyClient       *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gboolean
ai_meshy_client_balance_finish(
	AiMeshyClient  *self,
	GAsyncResult   *result,
	gint64         *balance,
	gint64         *monthly_credits,
	gint64         *permanent_credits,
	GError        **error
);

void
ai_meshy_client_list_animations_async(
	AiMeshyClient       *self,
	const gchar         *category,
	const gchar         *search,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

JsonNode *
ai_meshy_client_list_animations_finish(
	AiMeshyClient  *self,
	GAsyncResult   *result,
	GError        **error
);

gint64
ai_meshy_estimate_credits(
	const gchar     *operation,
	AiMediaRequest  *request,
	GError         **error
);

gchar *
ai_meshy_image_data_uri(
	const gchar  *path,
	GError      **error
);

G_END_DECLS
