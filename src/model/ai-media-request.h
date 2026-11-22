/*
 * ai-media-request.h - Parameters for an asynchronous media generation job
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

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define AI_TYPE_MEDIA_REQUEST (ai_media_request_get_type())

G_DECLARE_FINAL_TYPE(AiMediaRequest, ai_media_request, AI, MEDIA_REQUEST, GObject)

AiMediaRequest *
ai_media_request_new(
	const gchar *model,
	const gchar *prompt
);

const gchar *
ai_media_request_get_model(
	AiMediaRequest *self
);

void
ai_media_request_set_model(
	AiMediaRequest *self,
	const gchar    *model
);

const gchar *
ai_media_request_get_prompt(
	AiMediaRequest *self
);

void
ai_media_request_set_prompt(
	AiMediaRequest *self,
	const gchar    *prompt
);

void
ai_media_request_set_image_url(
	AiMediaRequest *self,
	const gchar    *url
);

void
ai_media_request_set_param_string(
	AiMediaRequest *self,
	const gchar    *name,
	const gchar    *value
);

void
ai_media_request_set_param_int(
	AiMediaRequest *self,
	const gchar    *name,
	gint64          value
);

void
ai_media_request_set_param_double(
	AiMediaRequest *self,
	const gchar    *name,
	gdouble         value
);

void
ai_media_request_set_param_boolean(
	AiMediaRequest *self,
	const gchar    *name,
	gboolean        value
);

void
ai_media_request_set_param_json(
	AiMediaRequest *self,
	const gchar    *name,
	JsonNode       *value
);

gboolean
ai_media_request_has_param(
	AiMediaRequest *self,
	const gchar    *name
);

void
ai_media_request_remove_param(
	AiMediaRequest *self,
	const gchar    *name
);

gchar *
ai_media_request_to_json(
	AiMediaRequest *self
);

const gchar *
ai_media_request_get_idempotency_key(
	AiMediaRequest *self
);

void
ai_media_request_set_idempotency_key(
	AiMediaRequest *self,
	const gchar    *key
);

const gchar *
ai_media_request_ensure_idempotency_key(
	AiMediaRequest *self
);

const gchar *
ai_media_request_get_webhook_url(
	AiMediaRequest *self
);

void
ai_media_request_set_webhook_url(
	AiMediaRequest *self,
	const gchar    *url
);

guint
ai_media_request_get_expires_after(
	AiMediaRequest *self
);

void
ai_media_request_set_expires_after(
	AiMediaRequest *self,
	guint           seconds
);

G_END_DECLS
