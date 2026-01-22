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
            { AI_PROVIDER_CLAUDE_CODE, "AI_PROVIDER_CLAUDE_CODE", "claude-code" },
            { AI_PROVIDER_OPENCODE, "AI_PROVIDER_OPENCODE", "opencode" },
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
        case AI_PROVIDER_CLAUDE_CODE:
            return "claude-code";
        case AI_PROVIDER_OPENCODE:
            return "opencode";
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
    else if (g_ascii_strcasecmp(str, "claude-code") == 0 ||
             g_ascii_strcasecmp(str, "claude_code") == 0)
    {
        return AI_PROVIDER_CLAUDE_CODE;
    }
    else if (g_ascii_strcasecmp(str, "opencode") == 0)
    {
        return AI_PROVIDER_OPENCODE;
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

/*
 * GType registration for AiImageSize.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_image_size_get_type(void)
{
    static GType image_size_type = 0;

    if (g_once_init_enter(&image_size_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_SIZE_AUTO, "AI_IMAGE_SIZE_AUTO", "auto" },
            { AI_IMAGE_SIZE_256, "AI_IMAGE_SIZE_256", "256x256" },
            { AI_IMAGE_SIZE_512, "AI_IMAGE_SIZE_512", "512x512" },
            { AI_IMAGE_SIZE_1024, "AI_IMAGE_SIZE_1024", "1024x1024" },
            { AI_IMAGE_SIZE_1024_1792, "AI_IMAGE_SIZE_1024_1792", "1024x1792" },
            { AI_IMAGE_SIZE_1792_1024, "AI_IMAGE_SIZE_1792_1024", "1792x1024" },
            { AI_IMAGE_SIZE_CUSTOM, "AI_IMAGE_SIZE_CUSTOM", "custom" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageSize", values);
        g_once_init_leave(&image_size_type, type);
    }

    return image_size_type;
}

/*
 * GType registration for AiImageQuality.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_image_quality_get_type(void)
{
    static GType image_quality_type = 0;

    if (g_once_init_enter(&image_quality_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_QUALITY_AUTO, "AI_IMAGE_QUALITY_AUTO", "auto" },
            { AI_IMAGE_QUALITY_STANDARD, "AI_IMAGE_QUALITY_STANDARD", "standard" },
            { AI_IMAGE_QUALITY_HD, "AI_IMAGE_QUALITY_HD", "hd" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageQuality", values);
        g_once_init_leave(&image_quality_type, type);
    }

    return image_quality_type;
}

/*
 * GType registration for AiImageStyle.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_image_style_get_type(void)
{
    static GType image_style_type = 0;

    if (g_once_init_enter(&image_style_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_STYLE_AUTO, "AI_IMAGE_STYLE_AUTO", "auto" },
            { AI_IMAGE_STYLE_VIVID, "AI_IMAGE_STYLE_VIVID", "vivid" },
            { AI_IMAGE_STYLE_NATURAL, "AI_IMAGE_STYLE_NATURAL", "natural" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageStyle", values);
        g_once_init_leave(&image_style_type, type);
    }

    return image_style_type;
}

/*
 * GType registration for AiImageResponseFormat.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_image_response_format_get_type(void)
{
    static GType image_response_format_type = 0;

    if (g_once_init_enter(&image_response_format_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_RESPONSE_URL, "AI_IMAGE_RESPONSE_URL", "url" },
            { AI_IMAGE_RESPONSE_BASE64, "AI_IMAGE_RESPONSE_BASE64", "b64_json" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageResponseFormat", values);
        g_once_init_leave(&image_response_format_type, type);
    }

    return image_response_format_type;
}

/**
 * ai_image_size_to_string:
 * @size: an #AiImageSize
 *
 * Converts an #AiImageSize to its string representation for API serialization.
 *
 * Returns: (transfer none): the string representation (e.g., "1024x1024")
 */
const gchar *
ai_image_size_to_string(AiImageSize size)
{
    switch (size)
    {
        case AI_IMAGE_SIZE_AUTO:
            return NULL;
        case AI_IMAGE_SIZE_256:
            return "256x256";
        case AI_IMAGE_SIZE_512:
            return "512x512";
        case AI_IMAGE_SIZE_1024:
            return "1024x1024";
        case AI_IMAGE_SIZE_1024_1792:
            return "1024x1792";
        case AI_IMAGE_SIZE_1792_1024:
            return "1792x1024";
        case AI_IMAGE_SIZE_CUSTOM:
            return NULL;
        default:
            return NULL;
    }
}

/**
 * ai_image_size_from_string:
 * @str: a size string (e.g., "1024x1024")
 *
 * Converts a string to an #AiImageSize.
 *
 * Returns: the #AiImageSize, or %AI_IMAGE_SIZE_AUTO if not recognized
 */
AiImageSize
ai_image_size_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_SIZE_AUTO;
    }

    if (g_strcmp0(str, "256x256") == 0)
    {
        return AI_IMAGE_SIZE_256;
    }
    else if (g_strcmp0(str, "512x512") == 0)
    {
        return AI_IMAGE_SIZE_512;
    }
    else if (g_strcmp0(str, "1024x1024") == 0)
    {
        return AI_IMAGE_SIZE_1024;
    }
    else if (g_strcmp0(str, "1024x1792") == 0)
    {
        return AI_IMAGE_SIZE_1024_1792;
    }
    else if (g_strcmp0(str, "1792x1024") == 0)
    {
        return AI_IMAGE_SIZE_1792_1024;
    }

    return AI_IMAGE_SIZE_AUTO;
}

/**
 * ai_image_quality_to_string:
 * @quality: an #AiImageQuality
 *
 * Converts an #AiImageQuality to its string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_image_quality_to_string(AiImageQuality quality)
{
    switch (quality)
    {
        case AI_IMAGE_QUALITY_AUTO:
            return NULL;
        case AI_IMAGE_QUALITY_STANDARD:
            return "standard";
        case AI_IMAGE_QUALITY_HD:
            return "hd";
        default:
            return NULL;
    }
}

/**
 * ai_image_quality_from_string:
 * @str: a quality string
 *
 * Converts a string to an #AiImageQuality.
 *
 * Returns: the #AiImageQuality, or %AI_IMAGE_QUALITY_AUTO if not recognized
 */
AiImageQuality
ai_image_quality_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_QUALITY_AUTO;
    }

    if (g_strcmp0(str, "standard") == 0)
    {
        return AI_IMAGE_QUALITY_STANDARD;
    }
    else if (g_strcmp0(str, "hd") == 0)
    {
        return AI_IMAGE_QUALITY_HD;
    }

    return AI_IMAGE_QUALITY_AUTO;
}

/**
 * ai_image_style_to_string:
 * @style: an #AiImageStyle
 *
 * Converts an #AiImageStyle to its string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_image_style_to_string(AiImageStyle style)
{
    switch (style)
    {
        case AI_IMAGE_STYLE_AUTO:
            return NULL;
        case AI_IMAGE_STYLE_VIVID:
            return "vivid";
        case AI_IMAGE_STYLE_NATURAL:
            return "natural";
        default:
            return NULL;
    }
}

/**
 * ai_image_style_from_string:
 * @str: a style string
 *
 * Converts a string to an #AiImageStyle.
 *
 * Returns: the #AiImageStyle, or %AI_IMAGE_STYLE_AUTO if not recognized
 */
AiImageStyle
ai_image_style_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_STYLE_AUTO;
    }

    if (g_strcmp0(str, "vivid") == 0)
    {
        return AI_IMAGE_STYLE_VIVID;
    }
    else if (g_strcmp0(str, "natural") == 0)
    {
        return AI_IMAGE_STYLE_NATURAL;
    }

    return AI_IMAGE_STYLE_AUTO;
}

/**
 * ai_image_response_format_to_string:
 * @format: an #AiImageResponseFormat
 *
 * Converts an #AiImageResponseFormat to its string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_image_response_format_to_string(AiImageResponseFormat format)
{
    switch (format)
    {
        case AI_IMAGE_RESPONSE_URL:
            return "url";
        case AI_IMAGE_RESPONSE_BASE64:
            return "b64_json";
        default:
            return "url";
    }
}

/**
 * ai_image_response_format_from_string:
 * @str: a format string
 *
 * Converts a string to an #AiImageResponseFormat.
 *
 * Returns: the #AiImageResponseFormat, or %AI_IMAGE_RESPONSE_URL if not recognized
 */
AiImageResponseFormat
ai_image_response_format_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_RESPONSE_URL;
    }

    if (g_strcmp0(str, "url") == 0)
    {
        return AI_IMAGE_RESPONSE_URL;
    }
    else if (g_strcmp0(str, "b64_json") == 0 || g_strcmp0(str, "base64") == 0)
    {
        return AI_IMAGE_RESPONSE_BASE64;
    }

    return AI_IMAGE_RESPONSE_URL;
}
