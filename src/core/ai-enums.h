/*
 * ai-enums.h - Enumerations for ai-glib
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
 * AiProviderType:
 * @AI_PROVIDER_CLAUDE: Anthropic Claude
 * @AI_PROVIDER_OPENAI: OpenAI GPT
 * @AI_PROVIDER_GEMINI: Google Gemini
 * @AI_PROVIDER_GROK: xAI Grok
 * @AI_PROVIDER_OLLAMA: Ollama (local)
 *
 * Enumeration of supported AI providers.
 */
typedef enum
{
    AI_PROVIDER_CLAUDE = 0,
    AI_PROVIDER_OPENAI,
    AI_PROVIDER_GEMINI,
    AI_PROVIDER_GROK,
    AI_PROVIDER_OLLAMA
} AiProviderType;

GType ai_provider_type_get_type(void);
#define AI_TYPE_PROVIDER_TYPE (ai_provider_type_get_type())

/**
 * AiRole:
 * @AI_ROLE_USER: User message
 * @AI_ROLE_ASSISTANT: Assistant message
 * @AI_ROLE_SYSTEM: System message (used for system prompts)
 * @AI_ROLE_TOOL: Tool result message
 *
 * Enumeration of message roles in a conversation.
 */
typedef enum
{
    AI_ROLE_USER = 0,
    AI_ROLE_ASSISTANT,
    AI_ROLE_SYSTEM,
    AI_ROLE_TOOL
} AiRole;

GType ai_role_get_type(void);
#define AI_TYPE_ROLE (ai_role_get_type())

/**
 * AiStopReason:
 * @AI_STOP_REASON_NONE: No stop reason (still generating)
 * @AI_STOP_REASON_END_TURN: Natural end of turn
 * @AI_STOP_REASON_STOP_SEQUENCE: Hit a stop sequence
 * @AI_STOP_REASON_MAX_TOKENS: Hit max tokens limit
 * @AI_STOP_REASON_TOOL_USE: Stopped to use a tool
 * @AI_STOP_REASON_CONTENT_FILTER: Content was filtered
 * @AI_STOP_REASON_ERROR: An error occurred
 *
 * Enumeration of reasons why generation stopped.
 */
typedef enum
{
    AI_STOP_REASON_NONE = 0,
    AI_STOP_REASON_END_TURN,
    AI_STOP_REASON_STOP_SEQUENCE,
    AI_STOP_REASON_MAX_TOKENS,
    AI_STOP_REASON_TOOL_USE,
    AI_STOP_REASON_CONTENT_FILTER,
    AI_STOP_REASON_ERROR
} AiStopReason;

GType ai_stop_reason_get_type(void);
#define AI_TYPE_STOP_REASON (ai_stop_reason_get_type())

/**
 * AiContentType:
 * @AI_CONTENT_TYPE_TEXT: Text content
 * @AI_CONTENT_TYPE_TOOL_USE: Tool use request
 * @AI_CONTENT_TYPE_TOOL_RESULT: Tool result
 * @AI_CONTENT_TYPE_IMAGE: Image content
 *
 * Enumeration of content block types.
 */
typedef enum
{
    AI_CONTENT_TYPE_TEXT = 0,
    AI_CONTENT_TYPE_TOOL_USE,
    AI_CONTENT_TYPE_TOOL_RESULT,
    AI_CONTENT_TYPE_IMAGE
} AiContentType;

GType ai_content_type_get_type(void);
#define AI_TYPE_CONTENT_TYPE (ai_content_type_get_type())

/*
 * Conversion functions for enumerations.
 * These provide string <-> enum conversions for serialization.
 */

/**
 * ai_role_to_string:
 * @role: an #AiRole
 *
 * Converts an #AiRole to its string representation.
 *
 * Returns: (transfer none): the string representation of the role
 */
const gchar *
ai_role_to_string(AiRole role);

/**
 * ai_role_from_string:
 * @str: a role string
 *
 * Converts a string to an #AiRole.
 *
 * Returns: the #AiRole, or %AI_ROLE_USER if the string is not recognized
 */
AiRole
ai_role_from_string(const gchar *str);

/**
 * ai_stop_reason_to_string:
 * @reason: an #AiStopReason
 *
 * Converts an #AiStopReason to its string representation.
 *
 * Returns: (transfer none): the string representation of the stop reason
 */
const gchar *
ai_stop_reason_to_string(AiStopReason reason);

/**
 * ai_stop_reason_from_string:
 * @str: a stop reason string
 *
 * Converts a string to an #AiStopReason.
 *
 * Returns: the #AiStopReason, or %AI_STOP_REASON_NONE if not recognized
 */
AiStopReason
ai_stop_reason_from_string(const gchar *str);

/**
 * ai_provider_type_to_string:
 * @provider: an #AiProviderType
 *
 * Converts an #AiProviderType to its string representation.
 *
 * Returns: (transfer none): the string representation of the provider
 */
const gchar *
ai_provider_type_to_string(AiProviderType provider);

/**
 * ai_provider_type_from_string:
 * @str: a provider string
 *
 * Converts a string to an #AiProviderType.
 *
 * Returns: the #AiProviderType, or %AI_PROVIDER_CLAUDE if not recognized
 */
AiProviderType
ai_provider_type_from_string(const gchar *str);

/**
 * ai_content_type_to_string:
 * @content_type: an #AiContentType
 *
 * Converts an #AiContentType to its string representation.
 *
 * Returns: (transfer none): the string representation of the content type
 */
const gchar *
ai_content_type_to_string(AiContentType content_type);

/**
 * ai_content_type_from_string:
 * @str: a content type string
 *
 * Converts a string to an #AiContentType.
 *
 * Returns: the #AiContentType, or %AI_CONTENT_TYPE_TEXT if not recognized
 */
AiContentType
ai_content_type_from_string(const gchar *str);

G_END_DECLS
