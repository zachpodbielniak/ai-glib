/*
 * ai-media-shared.h - Plumbing shared by the media-job providers (private)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.  It is INTERNAL: compiled into the library,
 * not installed, not in the umbrella header.
 *
 * AiMeshyClient and AiPixellabClient both hold a bearer token, both talk
 * to a JSON API that queues work, and both hand back result files that are
 * either a storage URL or bytes the API sent inline.  The rules those
 * share -- never echo the token, never follow a URL that is not http(s),
 * read the service's error envelope rather than guessing, turn a dollar
 * amount into micro-USD without a rounding error -- live here once.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-media-shared.h is private to ai-glib and cannot be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* What a non-JSON error body is quoted to, at most. */
#define AI_MEDIA_SHARED_ERROR_EXCERPT (400)

void
ai_media_shared_wipe_free(
	gchar *text
);

gchar *
ai_media_shared_redact(
	gchar       *text,
	const gchar *secret
);

gboolean
ai_media_shared_header_value_is_valid(
	const gchar *value
);

gboolean
ai_media_shared_id_is_valid(
	const gchar *id,
	gsize        max_length
);

GUri *
ai_media_shared_parse_http_uri(
	const gchar *url
);

gchar *
ai_media_shared_join_url(
	const gchar *base_url,
	const gchar *path,
	const gchar *query
);

gchar *
ai_media_shared_error_detail(
	GBytes *body
);

JsonObject *
ai_media_shared_parse_object(
	GBytes  *body,
	gchar  **why
);

JsonObject *
ai_media_shared_object_from_text(
	const gchar *json
);

gboolean
ai_media_shared_parse_usd(
	const gchar *text,
	gint64      *micro_usd
);

gboolean
ai_media_shared_usd_node_to_micro(
	JsonNode *node,
	gint64   *micro_usd
);

const gchar *
ai_media_shared_sniff_image(
	gconstpointer data,
	gsize         length
);

gchar *
ai_media_shared_data_url(
	const gchar   *mime_type,
	gconstpointer  data,
	gsize          length
);

gchar *
ai_media_shared_base64_to_data_url(
	const gchar *payload,
	const gchar *format
);

GBytes *
ai_media_shared_decode_data_url(
	const gchar  *url,
	GError      **error
);

gchar *
ai_media_shared_iso8601_from_unix_ms(
	gint64 unix_ms
);

G_END_DECLS
