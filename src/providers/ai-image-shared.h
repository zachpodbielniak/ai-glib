/*
 * ai-image-shared.h - Shared image-request plumbing for providers (private)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib. It is INTERNAL: it is compiled into the
 * library but not installed and not part of the public umbrella header.
 *
 * Everything here exists so that adding an image provider is a model table
 * plus an endpoint, rather than another copy of the same JSON building,
 * multipart assembly, response parsing, status mapping and retry logic.
 * The OpenAI-shaped helpers are used by OpenAI and Grok today and work
 * unchanged for any OpenAI-compatible service.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-image-shared.h is private to ai-glib and cannot be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "core/ai-image-capabilities.h"
#include "model/ai-image-request.h"
#include "model/ai-image-response.h"

G_BEGIN_DECLS

/*
 * ai_image_shared_status_to_error:
 * @status: an HTTP status code
 * @body: (nullable): the response body, for the error message
 * @body_len: length of @body
 * @error: return location for a GError
 *
 * Maps an HTTP status onto the AI_ERROR domain, quoting a bounded excerpt
 * of @body so the provider's own explanation survives into the message.
 * This mapping was duplicated at five call sites before being collected
 * here.
 */
void
ai_image_shared_status_to_error (
    guint         status,
    const gchar  *body,
    gsize         body_len,
    GError      **error
);

/*
 * ai_image_shared_send_async:
 * @session: the SoupSession to use
 * @msg: (transfer none): the message describing the request
 * @body: (nullable): the request body, needed to rebuild @msg on a retry
 * @max_retries: how many times to retry a transient failure; 0 disables
 * @cancellable: (nullable): a GCancellable
 * @callback: called on completion
 * @user_data: user data for @callback
 *
 * Sends the request, retrying transient failures (429 honouring
 * Retry-After, 5xx and network errors) with exponential backoff, and
 * mapping a final non-2xx status onto AI_ERROR.
 *
 * @msg is used as a template rather than sent directly: a message whose
 * body stream has been consumed cannot be sent again, so each attempt gets
 * a freshly-built copy. That is why @body must be passed separately --
 * there is no way to read it back off a SoupMessage.
 *
 * Unlike ai_search_http_get_json(), which sleeps, this waits on a timeout
 * source attached to the thread-default context: the image paths are async
 * and must not block the caller's loop while backing off, and a
 * synchronous caller is driving a nested loop on a private context that a
 * global-default timer would never reach.
 */
void
ai_image_shared_send_async (
    SoupSession         *session,
    SoupMessage         *msg,
    GBytes              *body,
    guint                max_retries,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

/*
 * ai_image_shared_send_finish:
 * @result: the GAsyncResult
 * @error: return location for a GError
 *
 * Finishes an ai_image_shared_send_async() call.
 *
 * Returns: (transfer full) (nullable): the response body, or NULL on error
 */
GBytes *
ai_image_shared_send_finish (
    GAsyncResult  *result,
    GError       **error
);

/*
 * ai_image_shared_apply_extras:
 * @builder: a JsonBuilder positioned inside the request object
 * @request: the image request
 *
 * Splices the request's provider-specific extras into the object being
 * built. Called last so an extra overrides any modelled parameter that
 * serialises to the same member.
 */
void
ai_image_shared_apply_extras (
    JsonBuilder    *builder,
    AiImageRequest *request
);

/*
 * ai_image_shared_build_openai_json:
 * @request: the image request
 * @model: the resolved model id
 * @info: (nullable): the model's capability descriptor
 *
 * Builds an OpenAI-compatible image-generation request body, emitting only
 * the parameters @info says the model accepts.
 *
 * Returns: (transfer full): the request body root node
 */
JsonNode *
ai_image_shared_build_openai_json (
    AiImageRequest         *request,
    const gchar            *model,
    const AiImageModelInfo *info
);

/*
 * ai_image_shared_build_openai_multipart:
 * @request: the image request
 * @model: the resolved model id
 * @info: (nullable): the model's capability descriptor
 * @error: return location for a GError
 *
 * Builds the multipart/form-data body for the OpenAI edits and variations
 * endpoints, which take their input images as file parts rather than as
 * base64 in JSON.
 *
 * Returns: (transfer full) (nullable): the multipart body, or NULL on error
 */
SoupMultipart *
ai_image_shared_build_openai_multipart (
    AiImageRequest          *request,
    const gchar             *model,
    const AiImageModelInfo  *info,
    GError                 **error
);

/*
 * ai_image_shared_parse_openai_response:
 * @root: the parsed response body
 * @model: (nullable): the model that produced it
 * @error: return location for a GError
 *
 * Parses an OpenAI-compatible image response: a `data` array whose members
 * carry `url` or `b64_json`, plus an optional `revised_prompt`.
 *
 * Returns: (transfer full) (nullable): the response, or NULL on error
 */
AiImageResponse *
ai_image_shared_parse_openai_response (
    JsonNode     *root,
    const gchar  *model,
    GError      **error
);

/*
 * ai_image_shared_build_gemini_parts:
 * @builder: a JsonBuilder positioned where the parts array should go
 * @request: the image request
 *
 * Emits the Gemini `parts` array: the prompt text followed by one
 * `inline_data` part per reference image.
 *
 * Reference roles are folded into the text part rather than sent as
 * structured data, because the wire format has no field for them and the
 * prompt is the only channel the model actually reads.
 */
void
ai_image_shared_build_gemini_parts (
    JsonBuilder    *builder,
    AiImageRequest *request
);

/*
 * ai_image_shared_prompt_with_roles:
 * @request: the image request
 *
 * Builds the effective prompt text: the caller's prompt, plus a short
 * preamble naming each labelled reference image in order when any of them
 * carry a role.
 *
 * Returns: (transfer full): the prompt to send
 */
gchar *
ai_image_shared_prompt_with_roles (AiImageRequest *request);

G_END_DECLS
