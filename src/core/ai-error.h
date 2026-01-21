/*
 * ai-error.h - Error domain and codes for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * AI_ERROR:
 *
 * Error domain for ai-glib errors. Errors in this domain will be from
 * the #AiError enumeration.
 *
 * See #GError for information on error domains.
 */
#define AI_ERROR (ai_error_quark())

/**
 * AiError:
 * @AI_ERROR_INVALID_API_KEY: The API key is invalid or missing.
 * @AI_ERROR_RATE_LIMITED: The request was rate limited by the provider.
 * @AI_ERROR_NETWORK_ERROR: A network error occurred during the request.
 * @AI_ERROR_TIMEOUT: The request timed out.
 * @AI_ERROR_INVALID_REQUEST: The request was malformed or invalid.
 * @AI_ERROR_INVALID_RESPONSE: The response from the provider was invalid.
 * @AI_ERROR_MODEL_NOT_FOUND: The requested model was not found.
 * @AI_ERROR_CONTEXT_LENGTH_EXCEEDED: The context length limit was exceeded.
 * @AI_ERROR_CONTENT_FILTERED: The content was filtered by the provider.
 * @AI_ERROR_SERVER_ERROR: The provider returned a server error.
 * @AI_ERROR_SERVICE_UNAVAILABLE: The provider service is unavailable.
 * @AI_ERROR_PERMISSION_DENIED: Permission denied for the requested operation.
 * @AI_ERROR_INSUFFICIENT_QUOTA: Insufficient quota or credits.
 * @AI_ERROR_CANCELLED: The operation was cancelled.
 * @AI_ERROR_NOT_SUPPORTED: The operation is not supported by the provider.
 * @AI_ERROR_CONFIGURATION_ERROR: Configuration error.
 * @AI_ERROR_SERIALIZATION_ERROR: Error serializing or deserializing data.
 * @AI_ERROR_STREAMING_ERROR: Error during streaming response.
 * @AI_ERROR_TOOL_ERROR: Error related to tool use.
 * @AI_ERROR_UNKNOWN: An unknown error occurred.
 *
 * Error codes for ai-glib operations.
 */
typedef enum
{
    AI_ERROR_INVALID_API_KEY = 1,
    AI_ERROR_RATE_LIMITED,
    AI_ERROR_NETWORK_ERROR,
    AI_ERROR_TIMEOUT,
    AI_ERROR_INVALID_REQUEST,
    AI_ERROR_INVALID_RESPONSE,
    AI_ERROR_MODEL_NOT_FOUND,
    AI_ERROR_CONTEXT_LENGTH_EXCEEDED,
    AI_ERROR_CONTENT_FILTERED,
    AI_ERROR_SERVER_ERROR,
    AI_ERROR_SERVICE_UNAVAILABLE,
    AI_ERROR_PERMISSION_DENIED,
    AI_ERROR_INSUFFICIENT_QUOTA,
    AI_ERROR_CANCELLED,
    AI_ERROR_NOT_SUPPORTED,
    AI_ERROR_CONFIGURATION_ERROR,
    AI_ERROR_SERIALIZATION_ERROR,
    AI_ERROR_STREAMING_ERROR,
    AI_ERROR_TOOL_ERROR,
    AI_ERROR_UNKNOWN
} AiError;

/**
 * ai_error_quark:
 *
 * Gets the ai-glib error quark.
 *
 * Returns: the error quark for ai-glib
 */
GQuark
ai_error_quark(void);

/**
 * ai_error_get_type:
 *
 * Gets the #GType for #AiError.
 *
 * Returns: the #GType for #AiError
 */
GType
ai_error_get_type(void);

#define AI_TYPE_ERROR (ai_error_get_type())

G_END_DECLS
