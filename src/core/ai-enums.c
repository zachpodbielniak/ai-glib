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
            { AI_PROVIDER_CLAUDE_TMUX, "AI_PROVIDER_CLAUDE_TMUX", "claude-tmux" },
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
        case AI_PROVIDER_CLAUDE_TMUX:
            return "claude-tmux";
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
    else if (g_ascii_strcasecmp(str, "claude-tmux") == 0 ||
             g_ascii_strcasecmp(str, "claude_tmux") == 0 ||
             g_ascii_strcasecmp(str, "claude-code-tmux") == 0 ||
             g_ascii_strcasecmp(str, "claude_code_tmux") == 0)
    {
        return AI_PROVIDER_CLAUDE_TMUX;
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
 * GType registration for AiEffortLevel.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_effort_level_get_type(void)
{
    static GType effort_level_type = 0;

    if (g_once_init_enter(&effort_level_type))
    {
        static const GEnumValue values[] = {
            { AI_EFFORT_LOW, "AI_EFFORT_LOW", "low" },
            { AI_EFFORT_MEDIUM, "AI_EFFORT_MEDIUM", "medium" },
            { AI_EFFORT_HIGH, "AI_EFFORT_HIGH", "high" },
            { AI_EFFORT_XHIGH, "AI_EFFORT_XHIGH", "xhigh" },
            { AI_EFFORT_MAX, "AI_EFFORT_MAX", "max" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiEffortLevel", values);
        g_once_init_leave(&effort_level_type, type);
    }

    return effort_level_type;
}

/**
 * ai_effort_level_to_string:
 * @level: an #AiEffortLevel
 *
 * Converts an #AiEffortLevel to its string representation.
 *
 * Returns: (transfer none): the string representation of the effort level
 */
const gchar *
ai_effort_level_to_string(AiEffortLevel level)
{
    switch (level)
    {
        case AI_EFFORT_LOW:
            return "low";
        case AI_EFFORT_MEDIUM:
            return "medium";
        case AI_EFFORT_HIGH:
            return "high";
        case AI_EFFORT_XHIGH:
            return "xhigh";
        case AI_EFFORT_MAX:
            return "max";
        default:
            return "medium";
    }
}

/**
 * ai_effort_level_from_string:
 * @str: an effort level string
 *
 * Converts a string to an #AiEffortLevel.
 * Accepts both effort names and variant aliases.
 *
 * Returns: the #AiEffortLevel, or %AI_EFFORT_MEDIUM if not recognized
 */
AiEffortLevel
ai_effort_level_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_EFFORT_MEDIUM;
    }

    if (g_ascii_strcasecmp(str, "low") == 0 ||
        g_ascii_strcasecmp(str, "min") == 0)
    {
        return AI_EFFORT_LOW;
    }
    else if (g_ascii_strcasecmp(str, "medium") == 0 ||
             g_ascii_strcasecmp(str, "med") == 0)
    {
        return AI_EFFORT_MEDIUM;
    }
    else if (g_ascii_strcasecmp(str, "high") == 0)
    {
        return AI_EFFORT_HIGH;
    }
    else if (g_ascii_strcasecmp(str, "xhigh") == 0 ||
             g_ascii_strcasecmp(str, "x-high") == 0 ||
             g_ascii_strcasecmp(str, "extra-high") == 0)
    {
        return AI_EFFORT_XHIGH;
    }
    else if (g_ascii_strcasecmp(str, "max") == 0)
    {
        return AI_EFFORT_MAX;
    }

    return AI_EFFORT_MEDIUM;
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
            { AI_IMAGE_QUALITY_LOW, "AI_IMAGE_QUALITY_LOW", "low" },
            { AI_IMAGE_QUALITY_MEDIUM, "AI_IMAGE_QUALITY_MEDIUM", "medium" },
            { AI_IMAGE_QUALITY_HIGH, "AI_IMAGE_QUALITY_HIGH", "high" },
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
        case AI_IMAGE_QUALITY_LOW:
            return "low";
        case AI_IMAGE_QUALITY_MEDIUM:
            return "medium";
        case AI_IMAGE_QUALITY_HIGH:
            return "high";
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
    else if (g_strcmp0(str, "low") == 0)
    {
        return AI_IMAGE_QUALITY_LOW;
    }
    else if (g_strcmp0(str, "medium") == 0)
    {
        return AI_IMAGE_QUALITY_MEDIUM;
    }
    else if (g_strcmp0(str, "high") == 0)
    {
        return AI_IMAGE_QUALITY_HIGH;
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

/*
 * GType registration for AiTriState.
 */
GType
ai_tri_state_get_type(void)
{
    static GType tri_state_type = 0;

    if (g_once_init_enter(&tri_state_type))
    {
        static const GEnumValue values[] = {
            { AI_TRI_UNSET, "AI_TRI_UNSET", "unset" },
            { AI_TRI_FALSE, "AI_TRI_FALSE", "false" },
            { AI_TRI_TRUE, "AI_TRI_TRUE", "true" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiTriState", values);
        g_once_init_leave(&tri_state_type, type);
    }

    return tri_state_type;
}

/*
 * GType registration for AiImageOperation.
 */
GType
ai_image_operation_get_type(void)
{
    static GType image_operation_type = 0;

    if (g_once_init_enter(&image_operation_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_OPERATION_GENERATE, "AI_IMAGE_OPERATION_GENERATE", "generate" },
            { AI_IMAGE_OPERATION_EDIT, "AI_IMAGE_OPERATION_EDIT", "edit" },
            { AI_IMAGE_OPERATION_VARIATION, "AI_IMAGE_OPERATION_VARIATION", "variation" },
            { AI_IMAGE_OPERATION_UPSCALE, "AI_IMAGE_OPERATION_UPSCALE", "upscale" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageOperation", values);
        g_once_init_leave(&image_operation_type, type);
    }

    return image_operation_type;
}

/**
 * ai_image_operation_to_string:
 * @operation: an #AiImageOperation
 *
 * Converts an #AiImageOperation to its string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_image_operation_to_string(AiImageOperation operation)
{
    switch (operation)
    {
        case AI_IMAGE_OPERATION_GENERATE:
            return "generate";
        case AI_IMAGE_OPERATION_EDIT:
            return "edit";
        case AI_IMAGE_OPERATION_VARIATION:
            return "variation";
        case AI_IMAGE_OPERATION_UPSCALE:
            return "upscale";
        default:
            return "generate";
    }
}

/**
 * ai_image_operation_from_string:
 * @str: an operation string
 *
 * Converts a string to an #AiImageOperation.
 *
 * Returns: the #AiImageOperation, or %AI_IMAGE_OPERATION_GENERATE if not
 *   recognized
 */
AiImageOperation
ai_image_operation_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_OPERATION_GENERATE;
    }

    if (g_strcmp0(str, "edit") == 0)
    {
        return AI_IMAGE_OPERATION_EDIT;
    }
    else if (g_strcmp0(str, "variation") == 0 ||
             g_strcmp0(str, "variations") == 0)
    {
        return AI_IMAGE_OPERATION_VARIATION;
    }
    else if (g_strcmp0(str, "upscale") == 0)
    {
        return AI_IMAGE_OPERATION_UPSCALE;
    }

    return AI_IMAGE_OPERATION_GENERATE;
}

/*
 * GType registration for AiImageResolution.
 */
GType
ai_image_resolution_get_type(void)
{
    static GType image_resolution_type = 0;

    if (g_once_init_enter(&image_resolution_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_RESOLUTION_AUTO, "AI_IMAGE_RESOLUTION_AUTO", "auto" },
            { AI_IMAGE_RESOLUTION_1K, "AI_IMAGE_RESOLUTION_1K", "1K" },
            { AI_IMAGE_RESOLUTION_2K, "AI_IMAGE_RESOLUTION_2K", "2K" },
            { AI_IMAGE_RESOLUTION_4K, "AI_IMAGE_RESOLUTION_4K", "4K" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageResolution", values);
        g_once_init_leave(&image_resolution_type, type);
    }

    return image_resolution_type;
}

/**
 * ai_image_resolution_to_string:
 * @resolution: an #AiImageResolution
 *
 * Converts an #AiImageResolution to the wire representation Gemini uses
 * (`1K`, `2K`, `4K`).
 *
 * Returns: (transfer none) (nullable): the string representation, or %NULL
 *   for %AI_IMAGE_RESOLUTION_AUTO
 */
const gchar *
ai_image_resolution_to_string(AiImageResolution resolution)
{
    switch (resolution)
    {
        case AI_IMAGE_RESOLUTION_AUTO:
            return NULL;
        case AI_IMAGE_RESOLUTION_1K:
            return "1K";
        case AI_IMAGE_RESOLUTION_2K:
            return "2K";
        case AI_IMAGE_RESOLUTION_4K:
            return "4K";
        default:
            return NULL;
    }
}

/**
 * ai_image_resolution_from_string:
 * @str: a resolution string
 *
 * Converts a string to an #AiImageResolution.  Accepts either case, so
 * both `2k` and `2K` are understood.
 *
 * Returns: the #AiImageResolution, or %AI_IMAGE_RESOLUTION_AUTO if not
 *   recognized
 */
AiImageResolution
ai_image_resolution_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_RESOLUTION_AUTO;
    }

    if (g_ascii_strcasecmp(str, "1k") == 0)
    {
        return AI_IMAGE_RESOLUTION_1K;
    }
    else if (g_ascii_strcasecmp(str, "2k") == 0)
    {
        return AI_IMAGE_RESOLUTION_2K;
    }
    else if (g_ascii_strcasecmp(str, "4k") == 0)
    {
        return AI_IMAGE_RESOLUTION_4K;
    }

    return AI_IMAGE_RESOLUTION_AUTO;
}

/*
 * GType registration for AiImageBackground.
 */
GType
ai_image_background_get_type(void)
{
    static GType image_background_type = 0;

    if (g_once_init_enter(&image_background_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_BACKGROUND_AUTO, "AI_IMAGE_BACKGROUND_AUTO", "auto" },
            { AI_IMAGE_BACKGROUND_TRANSPARENT, "AI_IMAGE_BACKGROUND_TRANSPARENT", "transparent" },
            { AI_IMAGE_BACKGROUND_OPAQUE, "AI_IMAGE_BACKGROUND_OPAQUE", "opaque" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageBackground", values);
        g_once_init_leave(&image_background_type, type);
    }

    return image_background_type;
}

/**
 * ai_image_background_to_string:
 * @background: an #AiImageBackground
 *
 * Converts an #AiImageBackground to its string representation.
 *
 * Returns: (transfer none) (nullable): the string representation, or %NULL
 *   for %AI_IMAGE_BACKGROUND_AUTO
 */
const gchar *
ai_image_background_to_string(AiImageBackground background)
{
    switch (background)
    {
        case AI_IMAGE_BACKGROUND_AUTO:
            return NULL;
        case AI_IMAGE_BACKGROUND_TRANSPARENT:
            return "transparent";
        case AI_IMAGE_BACKGROUND_OPAQUE:
            return "opaque";
        default:
            return NULL;
    }
}

/**
 * ai_image_background_from_string:
 * @str: a background string
 *
 * Converts a string to an #AiImageBackground.
 *
 * Returns: the #AiImageBackground, or %AI_IMAGE_BACKGROUND_AUTO if not
 *   recognized
 */
AiImageBackground
ai_image_background_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_BACKGROUND_AUTO;
    }

    if (g_strcmp0(str, "transparent") == 0)
    {
        return AI_IMAGE_BACKGROUND_TRANSPARENT;
    }
    else if (g_strcmp0(str, "opaque") == 0)
    {
        return AI_IMAGE_BACKGROUND_OPAQUE;
    }

    return AI_IMAGE_BACKGROUND_AUTO;
}

/*
 * GType registration for AiImageFormat.
 */
GType
ai_image_format_get_type(void)
{
    static GType image_format_type = 0;

    if (g_once_init_enter(&image_format_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_FORMAT_AUTO, "AI_IMAGE_FORMAT_AUTO", "auto" },
            { AI_IMAGE_FORMAT_PNG, "AI_IMAGE_FORMAT_PNG", "png" },
            { AI_IMAGE_FORMAT_JPEG, "AI_IMAGE_FORMAT_JPEG", "jpeg" },
            { AI_IMAGE_FORMAT_WEBP, "AI_IMAGE_FORMAT_WEBP", "webp" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageFormat", values);
        g_once_init_leave(&image_format_type, type);
    }

    return image_format_type;
}

/**
 * ai_image_format_to_string:
 * @format: an #AiImageFormat
 *
 * Converts an #AiImageFormat to its string representation.
 *
 * Returns: (transfer none) (nullable): the string representation, or %NULL
 *   for %AI_IMAGE_FORMAT_AUTO
 */
const gchar *
ai_image_format_to_string(AiImageFormat format)
{
    switch (format)
    {
        case AI_IMAGE_FORMAT_AUTO:
            return NULL;
        case AI_IMAGE_FORMAT_PNG:
            return "png";
        case AI_IMAGE_FORMAT_JPEG:
            return "jpeg";
        case AI_IMAGE_FORMAT_WEBP:
            return "webp";
        default:
            return NULL;
    }
}

/**
 * ai_image_format_from_string:
 * @str: a format string
 *
 * Converts a string to an #AiImageFormat.  `jpg` is accepted as a synonym
 * for `jpeg`.
 *
 * Returns: the #AiImageFormat, or %AI_IMAGE_FORMAT_AUTO if not recognized
 */
AiImageFormat
ai_image_format_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_FORMAT_AUTO;
    }

    if (g_ascii_strcasecmp(str, "png") == 0)
    {
        return AI_IMAGE_FORMAT_PNG;
    }
    else if (g_ascii_strcasecmp(str, "jpeg") == 0 ||
             g_ascii_strcasecmp(str, "jpg") == 0)
    {
        return AI_IMAGE_FORMAT_JPEG;
    }
    else if (g_ascii_strcasecmp(str, "webp") == 0)
    {
        return AI_IMAGE_FORMAT_WEBP;
    }

    return AI_IMAGE_FORMAT_AUTO;
}

/**
 * ai_image_format_to_mime_type:
 * @format: an #AiImageFormat
 *
 * Converts an #AiImageFormat to the corresponding MIME type.
 *
 * Providers that let you choose an output format do not always echo the
 * resulting MIME type back in the response, so callers need to be able to
 * derive it from what they asked for.
 *
 * Returns: (transfer none) (nullable): the MIME type, or %NULL for
 *   %AI_IMAGE_FORMAT_AUTO
 */
const gchar *
ai_image_format_to_mime_type(AiImageFormat format)
{
    switch (format)
    {
        case AI_IMAGE_FORMAT_AUTO:
            return NULL;
        case AI_IMAGE_FORMAT_PNG:
            return "image/png";
        case AI_IMAGE_FORMAT_JPEG:
            return "image/jpeg";
        case AI_IMAGE_FORMAT_WEBP:
            return "image/webp";
        default:
            return NULL;
    }
}

/*
 * GType registration for AiImageModeration.
 */
GType
ai_image_moderation_get_type(void)
{
    static GType image_moderation_type = 0;

    if (g_once_init_enter(&image_moderation_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_MODERATION_AUTO, "AI_IMAGE_MODERATION_AUTO", "auto" },
            { AI_IMAGE_MODERATION_LOW, "AI_IMAGE_MODERATION_LOW", "low" },
            { AI_IMAGE_MODERATION_NONE, "AI_IMAGE_MODERATION_NONE", "none" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageModeration", values);
        g_once_init_leave(&image_moderation_type, type);
    }

    return image_moderation_type;
}

/**
 * ai_image_moderation_to_string:
 * @moderation: an #AiImageModeration
 *
 * Converts an #AiImageModeration to its string representation.
 *
 * Returns: (transfer none) (nullable): the string representation, or %NULL
 *   for %AI_IMAGE_MODERATION_AUTO
 */
const gchar *
ai_image_moderation_to_string(AiImageModeration moderation)
{
    switch (moderation)
    {
        case AI_IMAGE_MODERATION_AUTO:
            return NULL;
        case AI_IMAGE_MODERATION_LOW:
            return "low";
        case AI_IMAGE_MODERATION_NONE:
            return "none";
        default:
            return NULL;
    }
}

/**
 * ai_image_moderation_from_string:
 * @str: a moderation string
 *
 * Converts a string to an #AiImageModeration.
 *
 * Returns: the #AiImageModeration, or %AI_IMAGE_MODERATION_AUTO if not
 *   recognized
 */
AiImageModeration
ai_image_moderation_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_MODERATION_AUTO;
    }

    if (g_strcmp0(str, "low") == 0)
    {
        return AI_IMAGE_MODERATION_LOW;
    }
    else if (g_strcmp0(str, "none") == 0 || g_strcmp0(str, "off") == 0)
    {
        return AI_IMAGE_MODERATION_NONE;
    }

    return AI_IMAGE_MODERATION_AUTO;
}

/*
 * GType registration for AiImagePersonGeneration.
 */
GType
ai_image_person_generation_get_type(void)
{
    static GType person_generation_type = 0;

    if (g_once_init_enter(&person_generation_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_PERSON_GENERATION_DEFAULT, "AI_IMAGE_PERSON_GENERATION_DEFAULT", "default" },
            { AI_IMAGE_PERSON_GENERATION_DONT_ALLOW, "AI_IMAGE_PERSON_GENERATION_DONT_ALLOW", "dont_allow" },
            { AI_IMAGE_PERSON_GENERATION_ALLOW_ADULT, "AI_IMAGE_PERSON_GENERATION_ALLOW_ADULT", "allow_adult" },
            { AI_IMAGE_PERSON_GENERATION_ALLOW_ALL, "AI_IMAGE_PERSON_GENERATION_ALLOW_ALL", "allow_all" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImagePersonGeneration", values);
        g_once_init_leave(&person_generation_type, type);
    }

    return person_generation_type;
}

/**
 * ai_image_person_generation_to_string:
 * @person_generation: an #AiImagePersonGeneration
 *
 * Converts an #AiImagePersonGeneration to the wire representation Imagen
 * uses.
 *
 * Returns: (transfer none) (nullable): the string representation, or %NULL
 *   for %AI_IMAGE_PERSON_GENERATION_DEFAULT
 */
const gchar *
ai_image_person_generation_to_string(AiImagePersonGeneration person_generation)
{
    switch (person_generation)
    {
        case AI_IMAGE_PERSON_GENERATION_DEFAULT:
            return NULL;
        case AI_IMAGE_PERSON_GENERATION_DONT_ALLOW:
            return "dont_allow";
        case AI_IMAGE_PERSON_GENERATION_ALLOW_ADULT:
            return "allow_adult";
        case AI_IMAGE_PERSON_GENERATION_ALLOW_ALL:
            return "allow_all";
        default:
            return NULL;
    }
}

/**
 * ai_image_person_generation_from_string:
 * @str: a person-generation string
 *
 * Converts a string to an #AiImagePersonGeneration.  Hyphens are accepted
 * in place of underscores.
 *
 * Returns: the #AiImagePersonGeneration, or
 *   %AI_IMAGE_PERSON_GENERATION_DEFAULT if not recognized
 */
AiImagePersonGeneration
ai_image_person_generation_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_PERSON_GENERATION_DEFAULT;
    }

    if (g_strcmp0(str, "dont_allow") == 0 || g_strcmp0(str, "dont-allow") == 0)
    {
        return AI_IMAGE_PERSON_GENERATION_DONT_ALLOW;
    }
    else if (g_strcmp0(str, "allow_adult") == 0 ||
             g_strcmp0(str, "allow-adult") == 0)
    {
        return AI_IMAGE_PERSON_GENERATION_ALLOW_ADULT;
    }
    else if (g_strcmp0(str, "allow_all") == 0 ||
             g_strcmp0(str, "allow-all") == 0)
    {
        return AI_IMAGE_PERSON_GENERATION_ALLOW_ALL;
    }

    return AI_IMAGE_PERSON_GENERATION_DEFAULT;
}

/*
 * GType registration for AiImageFidelity.
 */
GType
ai_image_fidelity_get_type(void)
{
    static GType image_fidelity_type = 0;

    if (g_once_init_enter(&image_fidelity_type))
    {
        static const GEnumValue values[] = {
            { AI_IMAGE_FIDELITY_AUTO, "AI_IMAGE_FIDELITY_AUTO", "auto" },
            { AI_IMAGE_FIDELITY_LOW, "AI_IMAGE_FIDELITY_LOW", "low" },
            { AI_IMAGE_FIDELITY_HIGH, "AI_IMAGE_FIDELITY_HIGH", "high" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiImageFidelity", values);
        g_once_init_leave(&image_fidelity_type, type);
    }

    return image_fidelity_type;
}

/**
 * ai_image_fidelity_to_string:
 * @fidelity: an #AiImageFidelity
 *
 * Converts an #AiImageFidelity to its string representation.
 *
 * Returns: (transfer none) (nullable): the string representation, or %NULL
 *   for %AI_IMAGE_FIDELITY_AUTO
 */
const gchar *
ai_image_fidelity_to_string(AiImageFidelity fidelity)
{
    switch (fidelity)
    {
        case AI_IMAGE_FIDELITY_AUTO:
            return NULL;
        case AI_IMAGE_FIDELITY_LOW:
            return "low";
        case AI_IMAGE_FIDELITY_HIGH:
            return "high";
        default:
            return NULL;
    }
}

/**
 * ai_image_fidelity_from_string:
 * @str: a fidelity string
 *
 * Converts a string to an #AiImageFidelity.
 *
 * Returns: the #AiImageFidelity, or %AI_IMAGE_FIDELITY_AUTO if not
 *   recognized
 */
AiImageFidelity
ai_image_fidelity_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_IMAGE_FIDELITY_AUTO;
    }

    if (g_strcmp0(str, "low") == 0)
    {
        return AI_IMAGE_FIDELITY_LOW;
    }
    else if (g_strcmp0(str, "high") == 0)
    {
        return AI_IMAGE_FIDELITY_HIGH;
    }

    return AI_IMAGE_FIDELITY_AUTO;
}

/*
 * GType registration for AiSearchFreshness.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_search_freshness_get_type(void)
{
    static GType freshness_type = 0;

    if (g_once_init_enter(&freshness_type))
    {
        static const GEnumValue values[] = {
            { AI_SEARCH_FRESHNESS_ANY, "AI_SEARCH_FRESHNESS_ANY", "any" },
            { AI_SEARCH_FRESHNESS_DAY, "AI_SEARCH_FRESHNESS_DAY", "day" },
            { AI_SEARCH_FRESHNESS_WEEK, "AI_SEARCH_FRESHNESS_WEEK", "week" },
            { AI_SEARCH_FRESHNESS_MONTH, "AI_SEARCH_FRESHNESS_MONTH", "month" },
            { AI_SEARCH_FRESHNESS_YEAR, "AI_SEARCH_FRESHNESS_YEAR", "year" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiSearchFreshness", values);
        g_once_init_leave(&freshness_type, type);
    }

    return freshness_type;
}

/**
 * ai_search_freshness_to_string:
 * @freshness: an #AiSearchFreshness
 *
 * Converts an #AiSearchFreshness to its canonical string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_search_freshness_to_string(AiSearchFreshness freshness)
{
    switch (freshness)
    {
        case AI_SEARCH_FRESHNESS_ANY:
            return "any";
        case AI_SEARCH_FRESHNESS_DAY:
            return "day";
        case AI_SEARCH_FRESHNESS_WEEK:
            return "week";
        case AI_SEARCH_FRESHNESS_MONTH:
            return "month";
        case AI_SEARCH_FRESHNESS_YEAR:
            return "year";
        default:
            return "any";
    }
}

/**
 * ai_search_freshness_from_string:
 * @str: (nullable): a freshness string
 *
 * Converts a string to an #AiSearchFreshness. Accepts the canonical names
 * plus common aliases (d/w/m/y, "all"/"none").
 *
 * Returns: the #AiSearchFreshness, or %AI_SEARCH_FRESHNESS_ANY if unrecognized
 */
AiSearchFreshness
ai_search_freshness_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_SEARCH_FRESHNESS_ANY;
    }

    if (g_ascii_strcasecmp(str, "day") == 0 ||
        g_ascii_strcasecmp(str, "d") == 0)
    {
        return AI_SEARCH_FRESHNESS_DAY;
    }
    else if (g_ascii_strcasecmp(str, "week") == 0 ||
             g_ascii_strcasecmp(str, "w") == 0)
    {
        return AI_SEARCH_FRESHNESS_WEEK;
    }
    else if (g_ascii_strcasecmp(str, "month") == 0 ||
             g_ascii_strcasecmp(str, "m") == 0)
    {
        return AI_SEARCH_FRESHNESS_MONTH;
    }
    else if (g_ascii_strcasecmp(str, "year") == 0 ||
             g_ascii_strcasecmp(str, "y") == 0)
    {
        return AI_SEARCH_FRESHNESS_YEAR;
    }

    return AI_SEARCH_FRESHNESS_ANY;
}

/*
 * GType registration for AiSearchSafeSearch.
 * Registers the enumeration values with the GLib type system for introspection.
 */
GType
ai_search_safe_search_get_type(void)
{
    static GType safe_search_type = 0;

    if (g_once_init_enter(&safe_search_type))
    {
        static const GEnumValue values[] = {
            { AI_SEARCH_SAFE_OFF, "AI_SEARCH_SAFE_OFF", "off" },
            { AI_SEARCH_SAFE_MODERATE, "AI_SEARCH_SAFE_MODERATE", "moderate" },
            { AI_SEARCH_SAFE_STRICT, "AI_SEARCH_SAFE_STRICT", "strict" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static("AiSearchSafeSearch", values);
        g_once_init_leave(&safe_search_type, type);
    }

    return safe_search_type;
}

/**
 * ai_search_safe_search_to_string:
 * @safe_search: an #AiSearchSafeSearch
 *
 * Converts an #AiSearchSafeSearch to its canonical string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_search_safe_search_to_string(AiSearchSafeSearch safe_search)
{
    switch (safe_search)
    {
        case AI_SEARCH_SAFE_OFF:
            return "off";
        case AI_SEARCH_SAFE_MODERATE:
            return "moderate";
        case AI_SEARCH_SAFE_STRICT:
            return "strict";
        default:
            return "moderate";
    }
}

/**
 * ai_search_safe_search_from_string:
 * @str: (nullable): a safe-search string
 *
 * Converts a string to an #AiSearchSafeSearch. Accepts the canonical names
 * plus common aliases ("none"/"high").
 *
 * Returns: the #AiSearchSafeSearch, or %AI_SEARCH_SAFE_MODERATE if unrecognized
 */
AiSearchSafeSearch
ai_search_safe_search_from_string(const gchar *str)
{
    if (str == NULL)
    {
        return AI_SEARCH_SAFE_MODERATE;
    }

    if (g_ascii_strcasecmp(str, "off") == 0 ||
        g_ascii_strcasecmp(str, "none") == 0)
    {
        return AI_SEARCH_SAFE_OFF;
    }
    else if (g_ascii_strcasecmp(str, "moderate") == 0 ||
             g_ascii_strcasecmp(str, "medium") == 0)
    {
        return AI_SEARCH_SAFE_MODERATE;
    }
    else if (g_ascii_strcasecmp(str, "strict") == 0 ||
             g_ascii_strcasecmp(str, "high") == 0)
    {
        return AI_SEARCH_SAFE_STRICT;
    }

    return AI_SEARCH_SAFE_MODERATE;
}
