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
 * @AI_PROVIDER_CLAUDE: Anthropic Claude (HTTP API)
 * @AI_PROVIDER_OPENAI: OpenAI GPT (HTTP API)
 * @AI_PROVIDER_GEMINI: Google Gemini (HTTP API)
 * @AI_PROVIDER_GROK: xAI Grok (HTTP API)
 * @AI_PROVIDER_OLLAMA: Ollama (local HTTP API)
 * @AI_PROVIDER_CLAUDE_CODE: Claude Code CLI wrapper
 * @AI_PROVIDER_OPENCODE: OpenCode CLI wrapper
 *
 * Enumeration of supported AI providers.
 */
typedef enum
{
    AI_PROVIDER_CLAUDE = 0,
    AI_PROVIDER_OPENAI,
    AI_PROVIDER_GEMINI,
    AI_PROVIDER_GROK,
    AI_PROVIDER_OLLAMA,
    AI_PROVIDER_CLAUDE_CODE,
    AI_PROVIDER_OPENCODE
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

/**
 * AiImageSize:
 * @AI_IMAGE_SIZE_AUTO: Let the provider choose (default)
 * @AI_IMAGE_SIZE_256: 256x256 pixels
 * @AI_IMAGE_SIZE_512: 512x512 pixels
 * @AI_IMAGE_SIZE_1024: 1024x1024 pixels
 * @AI_IMAGE_SIZE_1024_1792: 1024x1792 pixels (portrait)
 * @AI_IMAGE_SIZE_1792_1024: 1792x1024 pixels (landscape)
 * @AI_IMAGE_SIZE_CUSTOM: Custom size specified by string
 *
 * Enumeration of supported image sizes for generation.
 */
typedef enum
{
    AI_IMAGE_SIZE_AUTO = 0,
    AI_IMAGE_SIZE_256,
    AI_IMAGE_SIZE_512,
    AI_IMAGE_SIZE_1024,
    AI_IMAGE_SIZE_1024_1792,
    AI_IMAGE_SIZE_1792_1024,
    AI_IMAGE_SIZE_CUSTOM
} AiImageSize;

GType ai_image_size_get_type(void);
#define AI_TYPE_IMAGE_SIZE (ai_image_size_get_type())

/**
 * AiImageQuality:
 * @AI_IMAGE_QUALITY_AUTO: Let the provider choose (default)
 * @AI_IMAGE_QUALITY_STANDARD: Standard quality
 * @AI_IMAGE_QUALITY_HD: High definition quality
 *
 * Enumeration of image quality levels.
 */
typedef enum
{
    AI_IMAGE_QUALITY_AUTO = 0,
    AI_IMAGE_QUALITY_STANDARD,
    AI_IMAGE_QUALITY_HD
} AiImageQuality;

GType ai_image_quality_get_type(void);
#define AI_TYPE_IMAGE_QUALITY (ai_image_quality_get_type())

/**
 * AiImageStyle:
 * @AI_IMAGE_STYLE_AUTO: Let the provider choose (default)
 * @AI_IMAGE_STYLE_VIVID: Vivid, dramatic style
 * @AI_IMAGE_STYLE_NATURAL: Natural, realistic style
 *
 * Enumeration of image generation styles.
 */
typedef enum
{
    AI_IMAGE_STYLE_AUTO = 0,
    AI_IMAGE_STYLE_VIVID,
    AI_IMAGE_STYLE_NATURAL
} AiImageStyle;

GType ai_image_style_get_type(void);
#define AI_TYPE_IMAGE_STYLE (ai_image_style_get_type())

/**
 * AiImageResponseFormat:
 * @AI_IMAGE_RESPONSE_URL: Return URL to the generated image
 * @AI_IMAGE_RESPONSE_BASE64: Return base64-encoded image data
 *
 * Enumeration of response formats for generated images.
 */
typedef enum
{
    AI_IMAGE_RESPONSE_URL = 0,
    AI_IMAGE_RESPONSE_BASE64
} AiImageResponseFormat;

GType ai_image_response_format_get_type(void);
#define AI_TYPE_IMAGE_RESPONSE_FORMAT (ai_image_response_format_get_type())

/**
 * ai_image_size_to_string:
 * @size: an #AiImageSize
 *
 * Converts an #AiImageSize to its string representation for API serialization.
 *
 * Returns: (transfer none): the string representation (e.g., "1024x1024")
 */
const gchar *
ai_image_size_to_string(AiImageSize size);

/**
 * ai_image_size_from_string:
 * @str: a size string (e.g., "1024x1024")
 *
 * Converts a string to an #AiImageSize.
 *
 * Returns: the #AiImageSize, or %AI_IMAGE_SIZE_AUTO if not recognized
 */
AiImageSize
ai_image_size_from_string(const gchar *str);

/**
 * ai_image_quality_to_string:
 * @quality: an #AiImageQuality
 *
 * Converts an #AiImageQuality to its string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_image_quality_to_string(AiImageQuality quality);

/**
 * ai_image_quality_from_string:
 * @str: a quality string
 *
 * Converts a string to an #AiImageQuality.
 *
 * Returns: the #AiImageQuality, or %AI_IMAGE_QUALITY_AUTO if not recognized
 */
AiImageQuality
ai_image_quality_from_string(const gchar *str);

/**
 * ai_image_style_to_string:
 * @style: an #AiImageStyle
 *
 * Converts an #AiImageStyle to its string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_image_style_to_string(AiImageStyle style);

/**
 * ai_image_style_from_string:
 * @str: a style string
 *
 * Converts a string to an #AiImageStyle.
 *
 * Returns: the #AiImageStyle, or %AI_IMAGE_STYLE_AUTO if not recognized
 */
AiImageStyle
ai_image_style_from_string(const gchar *str);

/**
 * ai_image_response_format_to_string:
 * @format: an #AiImageResponseFormat
 *
 * Converts an #AiImageResponseFormat to its string representation.
 *
 * Returns: (transfer none): the string representation
 */
const gchar *
ai_image_response_format_to_string(AiImageResponseFormat format);

/**
 * ai_image_response_format_from_string:
 * @str: a format string
 *
 * Converts a string to an #AiImageResponseFormat.
 *
 * Returns: the #AiImageResponseFormat, or %AI_IMAGE_RESPONSE_URL if not recognized
 */
AiImageResponseFormat
ai_image_response_format_from_string(const gchar *str);

G_END_DECLS
