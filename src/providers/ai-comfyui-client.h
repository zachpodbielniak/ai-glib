/*
 * ai-comfyui-client.h - A self-hosted ComfyUI server as a media job service
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
 * AI_COMFYUI_DEFAULT_BASE_URL:
 *
 * Where ComfyUI listens when started with no arguments.
 */
#define AI_COMFYUI_DEFAULT_BASE_URL "http://127.0.0.1:8188"

/**
 * AI_COMFYUI_PLACEHOLDER_PROMPT:
 *
 * Replaced by the request's prompt in a workflow's string inputs.
 */
#define AI_COMFYUI_PLACEHOLDER_PROMPT "{{prompt}}"

/**
 * AI_COMFYUI_PLACEHOLDER_NEGATIVE_PROMPT:
 *
 * Replaced by the request's `negative_prompt` parameter, or nothing.
 */
#define AI_COMFYUI_PLACEHOLDER_NEGATIVE_PROMPT "{{negative_prompt}}"

/**
 * AI_COMFYUI_PLACEHOLDER_SEED:
 *
 * Replaced by the request's `seed` parameter, or a random seed; a number
 * when it is the whole input.
 */
#define AI_COMFYUI_PLACEHOLDER_SEED "{{seed}}"

/**
 * AI_COMFYUI_PLACEHOLDER_IMAGE:
 *
 * Replaced by the name of an image uploaded to the server's input folder.
 */
#define AI_COMFYUI_PLACEHOLDER_IMAGE "{{image}}"

#define AI_TYPE_COMFYUI_CLIENT (ai_comfyui_client_get_type())

G_DECLARE_FINAL_TYPE(AiComfyuiClient, ai_comfyui_client, AI, COMFYUI_CLIENT, GObject)

AiComfyuiClient *
ai_comfyui_client_new(
	const gchar *base_url
);

const gchar *
ai_comfyui_client_get_base_url(
	AiComfyuiClient *self
);

void
ai_comfyui_client_set_base_url(
	AiComfyuiClient *self,
	const gchar     *base_url
);

guint
ai_comfyui_client_get_timeout(
	AiComfyuiClient *self
);

void
ai_comfyui_client_set_timeout(
	AiComfyuiClient *self,
	guint            seconds
);

void
ai_comfyui_client_set_token(
	AiComfyuiClient *self,
	const gchar     *token
);

gboolean
ai_comfyui_client_has_token(
	AiComfyuiClient *self
);

gboolean
ai_comfyui_client_set_workflow(
	AiComfyuiClient  *self,
	const gchar      *workflow_json,
	GError          **error
);

const gchar *
ai_comfyui_client_get_workflow(
	AiComfyuiClient *self
);

void
ai_comfyui_client_upload_async(
	AiComfyuiClient     *self,
	GBytes              *data,
	const gchar         *filename,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gchar *
ai_comfyui_client_upload_finish(
	AiComfyuiClient  *self,
	GAsyncResult     *result,
	GError          **error
);

G_END_DECLS
