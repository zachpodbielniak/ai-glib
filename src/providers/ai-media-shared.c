/*
 * ai-media-shared.c - Plumbing shared by the media-job providers (private)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.  See ai-media-shared.h for why it exists.
 */

#include "config.h"

#include <string.h>

#include "providers/ai-media-shared.h"
#include "core/ai-error.h"

/* ---------------------------------------------------------------------- */
/* Secrets                                                                */
/* ---------------------------------------------------------------------- */

/*
 * ai_media_shared_wipe_free:
 * @text: (nullable) (transfer full): a string that may hold a secret
 *
 * Overwrites @text before freeing it, so a freed token is not left lying
 * in the heap for the next allocation to hand out.
 */
void
ai_media_shared_wipe_free(gchar *text)
{
	if (text != NULL)
	{
		memset(text, 0, strlen(text));
		g_free(text);
	}
}

/*
 * ai_media_shared_redact:
 * @text: (nullable) (transfer full): a message
 * @secret: (nullable): the value to remove
 *
 * Returns @text with every occurrence of @secret replaced by
 * "[REDACTED]".  A secret shorter than four bytes is left alone: it cannot
 * be a real token, and redacting every "a" would make messages unreadable
 * without protecting anything.
 *
 * Returns: (transfer full) (nullable): the redacted message
 */
gchar *
ai_media_shared_redact(
	gchar       *text,
	const gchar *secret
){
	GString *out;
	const gchar *cursor;
	const gchar *hit;
	gsize secret_len;

	if (text == NULL || secret == NULL || strlen(secret) < 4 || strstr(text, secret) == NULL)
	{
		return text;
	}

	secret_len = strlen(secret);
	out = g_string_new(NULL);
	cursor = text;
	while ((hit = strstr(cursor, secret)) != NULL)
	{
		g_string_append_len(out, cursor, hit - cursor);
		g_string_append(out, "[REDACTED]");
		cursor = hit + secret_len;
	}
	g_string_append(out, cursor);
	ai_media_shared_wipe_free(text);

	return g_string_free(out, FALSE);
}

/*
 * ai_media_shared_header_value_is_valid:
 *
 * Something a header can carry: not empty, no line break (which would
 * start a new header).
 */
gboolean
ai_media_shared_header_value_is_valid(const gchar *value)
{
	return value != NULL && value[0] != '\0' && strpbrk(value, "\r\n") == NULL;
}

/*
 * ai_media_shared_id_is_valid:
 *
 * A service's task or object id, which is spliced into a URL path: ASCII
 * letters, digits, '-' and '_' only, so a '/', '?', '#' or '..' can never
 * move the request somewhere else.
 */
gboolean
ai_media_shared_id_is_valid(
	const gchar *id,
	gsize        max_length
){
	const gchar *p;

	if (id == NULL || id[0] == '\0' || strlen(id) > max_length)
	{
		return FALSE;
	}
	for (p = id; *p != '\0'; p++)
	{
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* URLs                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * ai_media_shared_parse_http_uri:
 *
 * Parses an http(s) URL with a host, refusing file:, data: and the rest.
 *
 * Returns: (transfer full) (nullable): the URI
 */
GUri *
ai_media_shared_parse_http_uri(const gchar *url)
{
	GUri *uri;
	const gchar *scheme;

	if (url == NULL)
	{
		return NULL;
	}
	uri = g_uri_parse(url, SOUP_HTTP_URI_FLAGS, NULL);
	if (uri == NULL)
	{
		return NULL;
	}
	scheme = g_uri_get_scheme(uri);
	if (g_uri_get_host(uri) == NULL ||
	    (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0))
	{
		g_uri_unref(uri);
		return NULL;
	}
	return uri;
}

/*
 * ai_media_shared_join_url:
 * @base_url: the configured base, with or without a trailing slash
 * @path: a path relative to it, without a leading slash
 * @query: (nullable): a query string, already escaped
 *
 * Returns: (transfer full): `<base>/<path>[?query]`
 */
gchar *
ai_media_shared_join_url(
	const gchar *base_url,
	const gchar *path,
	const gchar *query
){
	g_autofree gchar *base = g_strdup(base_url);
	gsize length = strlen(base);

	while (length > 0 && base[length - 1] == '/')
	{
		base[--length] = '\0';
	}
	return g_strdup_printf("%s/%s%s%s", base, path,
	                       (query != NULL) ? "?" : "",
	                       (query != NULL) ? query : "");
}

/* ---------------------------------------------------------------------- */
/* JSON                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * Flattens FastAPI's validation list, [{"loc": [...], "msg": "..."}], to
 * "field: message; field: message" -- the field name is the useful part
 * when a service rejects one parameter.
 */
static gchar *
flatten_detail_list(JsonArray *items)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < json_array_get_length(items); i++)
	{
		JsonNode *item = json_array_get_element(items, i);
		JsonObject *object;
		const gchar *msg;

		if (!JSON_NODE_HOLDS_OBJECT(item))
		{
			continue;
		}
		object = json_node_get_object(item);
		msg = json_object_get_string_member_with_default(object, "msg", NULL);
		if (msg == NULL)
		{
			continue;
		}
		if (out->len > 0)
		{
			g_string_append(out, "; ");
		}
		if (json_object_has_member(object, "loc") &&
		    JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "loc")))
		{
			JsonArray *loc = json_object_get_array_member(object, "loc");
			guint n = json_array_get_length(loc);

			if (n > 0)
			{
				JsonNode *last = json_array_get_element(loc, n - 1);

				if (JSON_NODE_HOLDS_VALUE(last) &&
				    json_node_get_value_type(last) == G_TYPE_STRING)
				{
					g_string_append_printf(out, "%s: ", json_node_get_string(last));
				}
			}
		}
		g_string_append(out, msg);
	}
	if (out->len > 0)
	{
		return g_string_free(out, FALSE);
	}
	g_string_free(out, TRUE);
	return NULL;
}

/*
 * ai_media_shared_error_detail:
 * @body: (nullable): an error response body
 *
 * The service's own explanation of an error.  Understands the two
 * envelopes the media providers use: FastAPI's {"detail": "..."} or
 * {"detail": [...]} (PixelLab, Higgsfield) and {"message": "..."} (Meshy).
 * Anything else is quoted as a bounded, valid-UTF-8 excerpt.
 *
 * Returns: (transfer full) (nullable): the explanation, or %NULL for an
 *   empty body
 */
gchar *
ai_media_shared_error_detail(GBytes *body)
{
	gconstpointer data;
	gsize length = 0;
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;

	if (body == NULL)
	{
		return NULL;
	}
	data = g_bytes_get_data(body, &length);
	if (length == 0)
	{
		return NULL;
	}

	parser = json_parser_new();
	if (json_parser_load_from_data(parser, data, (gssize)length, NULL) &&
	    (root = json_parser_get_root(parser)) != NULL &&
	    JSON_NODE_HOLDS_OBJECT(root))
	{
		JsonObject *object = json_node_get_object(root);
		static const gchar * const members[] = { "detail", "message", "error", NULL };
		guint i;

		for (i = 0; members[i] != NULL; i++)
		{
			JsonNode *detail = json_object_get_member(object, members[i]);

			if (detail == NULL || JSON_NODE_HOLDS_NULL(detail))
			{
				continue;
			}
			if (JSON_NODE_HOLDS_VALUE(detail) &&
			    json_node_get_value_type(detail) == G_TYPE_STRING)
			{
				return g_strdup(json_node_get_string(detail));
			}
			if (JSON_NODE_HOLDS_ARRAY(detail))
			{
				gchar *flat = flatten_detail_list(json_node_get_array(detail));

				if (flat != NULL)
				{
					return flat;
				}
			}
			if (JSON_NODE_HOLDS_OBJECT(detail) &&
			    json_object_has_member(json_node_get_object(detail), "message"))
			{
				const gchar *message = json_object_get_string_member_with_default(
					json_node_get_object(detail), "message", NULL);

				if (message != NULL)
				{
					return g_strdup(message);
				}
			}
			return json_to_string(detail, FALSE);
		}
	}

	/* Not an envelope we know: a bounded, valid-UTF-8 excerpt beats nothing. */
	{
		g_autofree gchar *raw = g_strndup(data, MIN(length, (gsize)AI_MEDIA_SHARED_ERROR_EXCERPT));
		gchar *valid = g_utf8_make_valid(raw, -1);

		return g_strstrip(valid);
	}
}

/*
 * ai_media_shared_parse_object:
 * @body: (nullable): a 2xx body
 * @why: (out) (transfer full) (nullable): why it was refused
 *
 * Parses a body that must be a JSON object.
 *
 * Returns: (transfer full) (nullable): the object
 */
JsonObject *
ai_media_shared_parse_object(
	GBytes  *body,
	gchar  **why
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) parse_error = NULL;
	gconstpointer data;
	gsize length = 0;
	JsonNode *root;

	data = (body != NULL) ? g_bytes_get_data(body, &length) : NULL;
	if (length == 0)
	{
		*why = g_strdup("an empty body");
		return NULL;
	}
	if (!json_parser_load_from_data(parser, data, (gssize)length, &parse_error))
	{
		*why = g_strdup_printf("a body that is not JSON: %s", parse_error->message);
		return NULL;
	}
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
	{
		*why = g_strdup("a body that is not a JSON object");
		return NULL;
	}

	*why = NULL;
	return json_object_ref(json_node_get_object(root));
}

/*
 * ai_media_shared_object_from_text:
 * @json: JSON text that should be an object
 *
 * Returns: (transfer full) (nullable): the object, or %NULL if @json is
 *   not one
 */
JsonObject *
ai_media_shared_object_from_text(const gchar *json)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL))
	{
		return NULL;
	}
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
	{
		return NULL;
	}
	return json_object_ref(json_node_get_object(root));
}

/* ---------------------------------------------------------------------- */
/* Money                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * ai_media_shared_parse_usd:
 *
 * Converts a decimal dollar amount ("0.094") to micro-USD without going
 * through a double, so 0.1 is 100000 and not 99999.  Digits past the sixth
 * decimal round half up.  Negative and malformed amounts are refused.
 */
gboolean
ai_media_shared_parse_usd(
	const gchar *text,
	gint64      *micro_usd
){
	const gchar *p = text;
	gint64 whole = 0;
	gint64 fraction = 0;
	guint fraction_digits = 0;
	gboolean any_digit = FALSE;

	if (text == NULL)
	{
		return FALSE;
	}
	while (g_ascii_isspace(*p))
	{
		p++;
	}
	if (*p == '+')
	{
		p++;
	}
	for (; g_ascii_isdigit(*p); p++)
	{
		any_digit = TRUE;
		if (whole > (G_MAXINT64 / 1000000 - 9) / 10)
		{
			return FALSE;
		}
		whole = whole * 10 + (*p - '0');
	}
	if (*p == '.')
	{
		p++;
		for (; g_ascii_isdigit(*p); p++)
		{
			any_digit = TRUE;
			if (fraction_digits < 6)
			{
				fraction = fraction * 10 + (*p - '0');
				fraction_digits++;
			}
			else if (fraction_digits == 6)
			{
				/* The seventh digit decides the rounding; the rest are noise. */
				if (*p >= '5')
				{
					fraction++;
				}
				fraction_digits++;
			}
		}
	}
	while (g_ascii_isspace(*p))
	{
		p++;
	}
	if (!any_digit || *p != '\0')
	{
		return FALSE;
	}

	while (fraction_digits < 6)
	{
		fraction *= 10;
		fraction_digits++;
	}
	*micro_usd = whole * 1000000 + fraction;
	return TRUE;
}

/*
 * ai_media_shared_usd_node_to_micro:
 *
 * A dollar amount sent as a JSON number or a decimal string.  A number is
 * printed with nine decimals and parsed as text, so 0.01 becomes 10000
 * rather than whatever the nearest double truncates to.
 */
gboolean
ai_media_shared_usd_node_to_micro(
	JsonNode *node,
	gint64   *micro_usd
){
	GType type;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
	{
		return FALSE;
	}
	type = json_node_get_value_type(node);
	if (type == G_TYPE_STRING)
	{
		return ai_media_shared_parse_usd(json_node_get_string(node), micro_usd);
	}
	if (type == G_TYPE_DOUBLE || type == G_TYPE_INT64)
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

		g_ascii_formatd(buffer, sizeof buffer, "%.9f", json_node_get_double(node));
		return ai_media_shared_parse_usd(buffer, micro_usd);
	}
	return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Images and data URLs                                                   */
/* ---------------------------------------------------------------------- */

/*
 * ai_media_shared_sniff_image:
 *
 * What an image is, from its own first bytes rather than a filename: PNG,
 * JPEG or WebP, the three formats both services accept.
 *
 * Returns: (transfer none) (nullable): its MIME type, or %NULL
 */
const gchar *
ai_media_shared_sniff_image(
	gconstpointer data,
	gsize         length
){
	const guchar *bytes = data;

	if (length >= 8 && memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0)
	{
		return "image/png";
	}
	if (length >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
	{
		return "image/jpeg";
	}
	if (length >= 12 && memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WEBP", 4) == 0)
	{
		return "image/webp";
	}
	return NULL;
}

/*
 * ai_media_shared_data_url:
 *
 * Returns: (transfer full): `data:<mime>;base64,<payload>`
 */
gchar *
ai_media_shared_data_url(
	const gchar   *mime_type,
	gconstpointer  data,
	gsize          length
){
	g_autofree gchar *payload = g_base64_encode(data, length);

	return g_strdup_printf("data:%s;base64,%s", mime_type, payload);
}

/* Whether @text is nothing but the base64 alphabet and its padding. */
static gboolean
is_base64_text(const gchar *text)
{
	const gchar *p;

	if (text == NULL || text[0] == '\0')
	{
		return FALSE;
	}
	for (p = text; *p != '\0'; p++)
	{
		if (!g_ascii_isalnum(*p) && *p != '+' && *p != '/' && *p != '=')
		{
			return FALSE;
		}
	}
	return TRUE;
}

/*
 * ai_media_shared_base64_to_data_url:
 * @payload: base64 image data as a service sent it, with or without a
 *   `data:image/png;base64,` prefix of its own
 * @format: (nullable): the service's format word ("png"), default png
 *
 * PixelLab sends an image as {"base64": ...}, and the base64 is sometimes
 * a bare payload and sometimes already a data URL; both are normalised to
 * one data URL here.  A payload with anything but the base64 alphabet in
 * it is refused rather than passed on.
 *
 * Returns: (transfer full) (nullable): the data URL
 */
gchar *
ai_media_shared_base64_to_data_url(
	const gchar *payload,
	const gchar *format
){
	g_autofree gchar *mime = NULL;
	const gchar *comma;

	if (payload == NULL)
	{
		return NULL;
	}
	if (g_str_has_prefix(payload, "data:"))
	{
		comma = strchr(payload, ',');
		if (comma == NULL)
		{
			return NULL;
		}
		/* Keep the MIME type the payload named, if it named one. */
		mime = g_strndup(payload + 5, comma - (payload + 5));
		if (!g_str_has_suffix(mime, ";base64"))
		{
			return NULL;
		}
		mime[strlen(mime) - strlen(";base64")] = '\0';
		payload = comma + 1;
	}
	if (!is_base64_text(payload))
	{
		return NULL;
	}
	if (mime == NULL || mime[0] == '\0')
	{
		g_free(mime);
		if (format == NULL || format[0] == '\0' || g_ascii_strcasecmp(format, "png") == 0)
		{
			mime = g_strdup("image/png");
		}
		else if (g_ascii_strcasecmp(format, "jpg") == 0 || g_ascii_strcasecmp(format, "jpeg") == 0)
		{
			mime = g_strdup("image/jpeg");
		}
		else if (g_ascii_strcasecmp(format, "webp") == 0)
		{
			mime = g_strdup("image/webp");
		}
		else
		{
			mime = g_strdup("application/octet-stream");
		}
	}
	return g_strdup_printf("data:%s;base64,%s", mime, payload);
}

/*
 * ai_media_shared_decode_data_url:
 * @url: a `data:<mime>;base64,<payload>` URL
 * @error: return location for a #GError
 *
 * Decodes a base64 data URL.  Only base64 is accepted: a percent-encoded
 * data URL never comes from either service, so one here is a mistake.
 *
 * Returns: (transfer full) (nullable): the bytes
 */
GBytes *
ai_media_shared_decode_data_url(
	const gchar  *url,
	GError      **error
){
	const gchar *comma;
	g_autofree gchar *header = NULL;
	guchar *decoded;
	gsize length = 0;

	if (url == NULL || !g_str_has_prefix(url, "data:") || (comma = strchr(url, ',')) == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Not a data: URL");
		return NULL;
	}
	header = g_strndup(url, comma - url);
	if (!g_str_has_suffix(header, ";base64") || !is_base64_text(comma + 1))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Only base64 data: URLs can be decoded");
		return NULL;
	}

	decoded = g_base64_decode(comma + 1, &length);
	if (length == 0)
	{
		g_free(decoded);
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
		                    "The data: URL holds no data");
		return NULL;
	}
	return g_bytes_new_take(decoded, length);
}

/*
 * ai_media_shared_iso8601_from_unix_ms:
 *
 * Meshy stamps everything in Unix milliseconds; AiMediaJob carries an
 * expiry as ISO 8601 text, so the two providers agree on one spelling.
 *
 * Returns: (transfer full) (nullable): the time, or %NULL if out of range
 */
gchar *
ai_media_shared_iso8601_from_unix_ms(gint64 unix_ms)
{
	g_autoptr(GDateTime) when = NULL;

	if (unix_ms <= 0)
	{
		return NULL;
	}
	when = g_date_time_new_from_unix_utc(unix_ms / 1000);
	if (when == NULL)
	{
		return NULL;
	}
	return g_date_time_format_iso8601(when);
}
