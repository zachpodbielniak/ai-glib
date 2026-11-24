/*
 * ai-pixellab-client.h - PixelLab pixel-art generation API (v2)
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
 * AI_PIXELLAB_DEFAULT_BASE_URL:
 *
 * The v2 API's base URL, as its OpenAPI document names it.
 */
#define AI_PIXELLAB_DEFAULT_BASE_URL "https://api.pixellab.ai/v2"

/* Operations answered in the response itself. */
#define AI_PIXELLAB_OPERATION_IMAGE "image"
#define AI_PIXELLAB_OPERATION_BITFORGE "bitforge"
#define AI_PIXELLAB_OPERATION_ROTATE "rotate"
#define AI_PIXELLAB_OPERATION_INPAINT "inpaint"
#define AI_PIXELLAB_OPERATION_ANIMATE "animate"

/* Operations run as background jobs. */
#define AI_PIXELLAB_OPERATION_CHARACTER_4 "character-4"
#define AI_PIXELLAB_OPERATION_CHARACTER_8 "character-8"
#define AI_PIXELLAB_OPERATION_CHARACTER_ANIMATE "character-animate"
#define AI_PIXELLAB_OPERATION_TILESET_TOPDOWN "tileset-topdown"
#define AI_PIXELLAB_OPERATION_TILESET_SIDESCROLLER "tileset-sidescroller"
#define AI_PIXELLAB_OPERATION_ISOMETRIC_TILE "isometric-tile"
#define AI_PIXELLAB_OPERATION_MAP_OBJECT "map-object"

/**
 * AI_PIXELLAB_MODEL_PIXEN:
 *
 * The model that sends an `image` request to create-image-pixen instead
 * of create-image-pixflux.
 */
#define AI_PIXELLAB_MODEL_PIXEN "pixen"

#define AI_TYPE_PIXELLAB_CLIENT (ai_pixellab_client_get_type())

G_DECLARE_FINAL_TYPE(AiPixellabClient, ai_pixellab_client, AI, PIXELLAB_CLIENT, GObject)

AiPixellabClient *
ai_pixellab_client_new(void);

AiPixellabClient *
ai_pixellab_client_new_with_api_key(
	const gchar *api_key
);

void
ai_pixellab_client_set_api_key(
	AiPixellabClient *self,
	const gchar      *api_key
);

gboolean
ai_pixellab_client_has_api_key(
	AiPixellabClient *self
);

const gchar *
ai_pixellab_client_get_base_url(
	AiPixellabClient *self
);

void
ai_pixellab_client_set_base_url(
	AiPixellabClient *self,
	const gchar      *base_url
);

guint
ai_pixellab_client_get_timeout(
	AiPixellabClient *self
);

void
ai_pixellab_client_set_timeout(
	AiPixellabClient *self,
	guint             seconds
);

void
ai_pixellab_client_balance_async(
	AiPixellabClient    *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gboolean
ai_pixellab_client_balance_finish(
	AiPixellabClient  *self,
	GAsyncResult      *result,
	gint64            *credits_micro_usd,
	gdouble           *generations,
	gdouble           *generations_total,
	gchar            **subscription_status,
	GError           **error
);

JsonNode *
ai_pixellab_image_node_new(
	GBytes  *data,
	GError **error
);

G_END_DECLS
