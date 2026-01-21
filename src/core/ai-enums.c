/*
 * ai-enums.c - Enumeration implementations for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-enums.h"

/*
 * GType registration for AiProviderType.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_provider_type_get_type(void)
{
    static GType provider_type = 0;

    if (g_once_init_enter(&provider_type))
    {
        static const GEnumValue values[] = {
            { AI_PROVIDER_CLAUDE, "AI_PROVIDER_CLAUDE", "claude" },
            { AI_PROVIDER_OPENAI, "AI_PROVIDER_OPENAI", "openai" },
            { AI_PROVIDER_GEMINI, "AI_PROVIDER_GEMINI", "gemini" },
            { AI_PROVIDER_GROK, "AI_PROVIDER_GROK", "grok" },
            { AI_PROVIDER_OLLAMA, "AI_PROVIDER_OLLAMA", "ollama" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiProviderType", values);
        g_once_init_leave(&provider_type, type);
    }

    return provider_type;
}

/*
 * GType registration for AiRole.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_role_get_type(void)
{
    static GType role_type = 0;

    if (g_once_init_enter(&role_type))
    {
        static const GEnumValue values[] = {
            { AI_ROLE_USER, "AI_ROLE_USER", "user" },
            { AI_ROLE_ASSISTANT, "AI_ROLE_ASSISTANT", "assistant" },
            { AI_ROLE_SYSTEM, "AI_ROLE_SYSTEM", "system" },
            { AI_ROLE_TOOL, "AI_ROLE_TOOL", "tool" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiRole", values);
        g_once_init_leave(&role_type, type);
    }

    return role_type;
}

/*
 * GType registration for AiStopReason.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_stop_reason_get_type(void)
{
    static GType stop_reason_type = 0;

    if (g_once_init_enter(&stop_reason_type))
    {
        static const GEnumValue values[] = {
            { AI_STOP_REASON_NONE, "AI_STOP_REASON_NONE", "none" },
            { AI_STOP_REASON_END_TURN, "AI_STOP_REASON_END_TURN", "end_turn" },
            { AI_STOP_REASON_STOP_SEQUENCE, "AI_STOP_REASON_STOP_SEQUENCE", "stop_sequence" },
            { AI_STOP_REASON_MAX_TOKENS, "AI_STOP_REASON_MAX_TOKENS", "max_tokens" },
            { AI_STOP_REASON_TOOL_USE, "AI_STOP_REASON_TOOL_USE", "tool_use" },
            { AI_STOP_REASON_CONTENT_FILTER, "AI_STOP_REASON_CONTENT_FILTER", "content_filter" },
            { AI_STOP_REASON_ERROR, "AI_STOP_REASON_ERROR", "error" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiStopReason", values);
        g_once_init_leave(&stop_reason_type, type);
    }

    return stop_reason_type;
}

/*
 * GType registration for AiContentType.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_content_type_get_type(void)
{
    static GType content_type = 0;

    if (g_once_init_enter(&content_type))
    {
        static const GEnumValue values[] = {
            { AI_CONTENT_TYPE_TEXT, "AI_CONTENT_TYPE_TEXT", "text" },
            { AI_CONTENT_TYPE_TOOL_USE, "AI_CONTENT_TYPE_TOOL_USE", "tool_use" },
            { AI_CONTENT_TYPE_TOOL_RESULT, "AI_CONTENT_TYPE_TOOL_RESULT", "tool_result" },
            { AI_CONTENT_TYPE_IMAGE, "AI_CONTENT_TYPE_IMAGE", "image" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiContentType", values);
        g_once_init_leave(&content_type, type);
    }

    return content_type;
}

/**
 * ai_role_to_string:
 * @role: an #AiRole
 *
 * Converts an #AiRole to its string representation for API serialization.
 *
 * Returns: (transfer none): the string representation of the role
 */
const gchar *
ai_role_to_string(AiRole role)
{
    switch (role)
    {
        case AI_ROLE_USER:
            return "user";
        case AI_ROLE_ASSISTANT:
            return "assistant";
        case AI_ROLE_SYSTEM:
            return "system";
        case AI_ROLE_TOOL:
            return "tool";
        default:
            return "user";
    }
}

/**
 * ai_role_from_string:
 * @str: a role string
 *
 * Converts a string to an #AiRole for API deserialization.
 *
 * Returns: the #AiRole, or %AI_ROLE_USER if the string is not recognized
 */
AiRole
ai_role_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_ROLE_USER;
    }

    if (g_strcmp0(str, "user") == 0)
    {
        return AI_ROLE_USER;
    }
    else if (g_strcmp0(str, "assistant") == 0)
    {
        return AI_ROLE_ASSISTANT;
    }
    else if (g_strcmp0(str, "system") == 0)
    {
        return AI_ROLE_SYSTEM;
    }
    else if (g_strcmp0(str, "tool") == 0)
    {
        return AI_ROLE_TOOL;
    }

    return AI_ROLE_USER;
}

/**
 * ai_stop_reason_to_string:
 * @reason: an #AiStopReason
 *
 * Converts an #AiStopReason to its string representation.
 *
 * Returns: (transfer none): the string representation of the stop reason
 */
const gchar *
ai_stop_reason_to_string(AiStopReason reason)
{
    switch (reason)
    {
        case AI_STOP_REASON_NONE:
            return "none";
        case AI_STOP_REASON_END_TURN:
            return "end_turn";
        case AI_STOP_REASON_STOP_SEQUENCE:
            return "stop_sequence";
        case AI_STOP_REASON_MAX_TOKENS:
            return "max_tokens";
        case AI_STOP_REASON_TOOL_USE:
            return "tool_use";
        case AI_STOP_REASON_CONTENT_FILTER:
            return "content_filter";
        case AI_STOP_REASON_ERROR:
            return "error";
        default:
            return "none";
    }
}

/**
 * ai_stop_reason_from_string:
 * @str: a stop reason string
 *
 * Converts a string to an #AiStopReason for API deserialization.
 * Handles both underscore and hyphen variants.
 *
 * Returns: the #AiStopReason, or %AI_STOP_REASON_NONE if not recognized
 */
AiStopReason
ai_stop_reason_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_STOP_REASON_NONE;
    }

    if (g_strcmp0(str, "end_turn") == 0 || g_strcmp0(str, "stop") == 0)
    {
        return AI_STOP_REASON_END_TURN;
    }
    else if (g_strcmp0(str, "stop_sequence") == 0)
    {
        return AI_STOP_REASON_STOP_SEQUENCE;
    }
    else if (g_strcmp0(str, "max_tokens") == 0 || g_strcmp0(str, "length") == 0)
    {
        return AI_STOP_REASON_MAX_TOKENS;
    }
    else if (g_strcmp0(str, "tool_use") == 0 || g_strcmp0(str, "tool_calls") == 0)
    {
        return AI_STOP_REASON_TOOL_USE;
    }
    else if (g_strcmp0(str, "content_filter") == 0)
    {
        return AI_STOP_REASON_CONTENT_FILTER;
    }
    else if (g_strcmp0(str, "error") == 0)
    {
        return AI_STOP_REASON_ERROR;
    }

    return AI_STOP_REASON_NONE;
}

/**
 * ai_provider_type_to_string:
 * @provider: an #AiProviderType
 *
 * Converts an #AiProviderType to its string representation.
 *
 * Returns: (transfer none): the string representation of the provider
 */
const gchar *
ai_provider_type_to_string(AiProviderType provider)
{
    switch (provider)
    {
        case AI_PROVIDER_CLAUDE:
            return "claude";
        case AI_PROVIDER_OPENAI:
            return "openai";
        case AI_PROVIDER_GEMINI:
            return "gemini";
        case AI_PROVIDER_GROK:
            return "grok";
        case AI_PROVIDER_OLLAMA:
            return "ollama";
        default:
            return "unknown";
    }
}

/**
 * ai_provider_type_from_string:
 * @str: a provider string
 *
 * Converts a string to an #AiProviderType.
 *
 * Returns: the #AiProviderType, or %AI_PROVIDER_CLAUDE if not recognized
 */
AiProviderType
ai_provider_type_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_PROVIDER_CLAUDE;
    }

    if (g_ascii_strcasecmp(str, "claude") == 0 ||
        g_ascii_strcasecmp(str, "anthropic") == 0)
    {
        return AI_PROVIDER_CLAUDE;
    }
    else if (g_ascii_strcasecmp(str, "openai") == 0 ||
             g_ascii_strcasecmp(str, "gpt") == 0)
    {
        return AI_PROVIDER_OPENAI;
    }
    else if (g_ascii_strcasecmp(str, "gemini") == 0 ||
             g_ascii_strcasecmp(str, "google") == 0)
    {
        return AI_PROVIDER_GEMINI;
    }
    else if (g_ascii_strcasecmp(str, "grok") == 0 ||
             g_ascii_strcasecmp(str, "xai") == 0)
    {
        return AI_PROVIDER_GROK;
    }
    else if (g_ascii_strcasecmp(str, "ollama") == 0)
    {
        return AI_PROVIDER_OLLAMA;
    }

    return AI_PROVIDER_CLAUDE;
}

/**
 * ai_content_type_to_string:
 * @content_type: an #AiContentType
 *
 * Converts an #AiContentType to its string representation.
 *
 * Returns: (transfer none): the string representation of the content type
 */
const gchar *
ai_content_type_to_string(AiContentType content_type)
{
    switch (content_type)
    {
        case AI_CONTENT_TYPE_TEXT:
            return "text";
        case AI_CONTENT_TYPE_TOOL_USE:
            return "tool_use";
        case AI_CONTENT_TYPE_TOOL_RESULT:
            return "tool_result";
        case AI_CONTENT_TYPE_IMAGE:
            return "image";
        default:
            return "text";
    }
}

/**
 * ai_content_type_from_string:
 * @str: a content type string
 *
 * Converts a string to an #AiContentType.
 *
 * Returns: the #AiContentType, or %AI_CONTENT_TYPE_TEXT if not recognized
 */
AiContentType
ai_content_type_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_CONTENT_TYPE_TEXT;
    }

    if (g_strcmp0(str, "text") == 0)
    {
        return AI_CONTENT_TYPE_TEXT;
    }
    else if (g_strcmp0(str, "tool_use") == 0)
    {
        return AI_CONTENT_TYPE_TOOL_USE;
    }
    else if (g_strcmp0(str, "tool_result") == 0)
    {
        return AI_CONTENT_TYPE_TOOL_RESULT;
    }
    else if (g_strcmp0(str, "image") == 0 || g_strcmp0(str, "image_url") == 0)
    {
        return AI_CONTENT_TYPE_IMAGE;
    }

    return AI_CONTENT_TYPE_TEXT;
}
