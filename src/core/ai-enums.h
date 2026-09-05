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
 * @AI_PROVIDER_CLAUDE_CODE: Claude Code CLI wrapper (--print mode,
 *   billed against the Agent SDK credit pool from 2026-06-15)
 * @AI_PROVIDER_OPENCODE: OpenCode CLI wrapper
 * @AI_PROVIDER_CLAUDE_TMUX: Claude Code CLI driven via an ephemeral
 *   tmux session in interactive TUI mode (billed as normal
 *   subscription usage, bypassing the Agent SDK credit pool)
 * @AI_PROVIDER_GROK_BUILD: Grok Build CLI wrapper (the `grok` binary in
 *   headless mode)
 * @AI_PROVIDER_ANTIGRAVITY: Google Antigravity CLI wrapper (the `agy`
 *   binary in print / stream-json mode)
 * @AI_PROVIDER_CURSOR: Cursor Agent CLI wrapper (the `cursor-agent`
 *   binary in `--print` mode)
 * @AI_PROVIDER_CODEX_CLI: Codex CLI wrapper (codex exec)
 * @AI_PROVIDER_OPENAI_COMPATIBLE: User-configured OpenAI-compatible HTTP API
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
    AI_PROVIDER_OPENCODE,
    AI_PROVIDER_CLAUDE_TMUX,
    AI_PROVIDER_GROK_BUILD,
    AI_PROVIDER_ANTIGRAVITY,
    AI_PROVIDER_CURSOR,
    AI_PROVIDER_CODEX_CLI,
    AI_PROVIDER_OPENAI_COMPATIBLE
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

const gchar *
ai_role_to_string(AiRole role);

AiRole
ai_role_from_string(const gchar *str);

const gchar *
ai_stop_reason_to_string(AiStopReason reason);

AiStopReason
ai_stop_reason_from_string(const gchar *str);

const gchar *
ai_provider_type_to_string(AiProviderType provider);

AiProviderType
ai_provider_type_from_string(const gchar *str);

const gchar *
ai_content_type_to_string(AiContentType content_type);

AiContentType
ai_content_type_from_string(const gchar *str);

/**
 * AiEffortLevel:
 * @AI_EFFORT_LOW: Low effort / minimal reasoning
 * @AI_EFFORT_MEDIUM: Medium effort (default)
 * @AI_EFFORT_HIGH: High effort / extended reasoning
 * @AI_EFFORT_XHIGH: Extra-high effort / very deep reasoning (between
 *   high and max).  Supported by newer Claude Code models such as
 *   fable (claude-fable-5).
 * @AI_EFFORT_MAX: Maximum effort / deepest reasoning
 *
 * Enumeration of reasoning effort levels for AI providers.
 * Maps to --effort for Claude Code and Antigravity, --variant for
 * OpenCode, and --reasoning-effort for Grok Build (which has no "max" —
 * it is folded onto xhigh). Antigravity accepts only low|medium|high;
 * xhigh and max fold onto high.
 *
 * Ordered low → max; XHIGH sits between HIGH and MAX so the numeric
 * ordering matches the semantic ordering.
 */
typedef enum
{
    AI_EFFORT_LOW = 0,
    AI_EFFORT_MEDIUM,
    AI_EFFORT_HIGH,
    AI_EFFORT_XHIGH,
    AI_EFFORT_MAX
} AiEffortLevel;

GType ai_effort_level_get_type(void);
#define AI_TYPE_EFFORT_LEVEL (ai_effort_level_get_type())

const gchar *
ai_effort_level_to_string(AiEffortLevel level);

AiEffortLevel
ai_effort_level_from_string(const gchar *str);

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
 * @AI_IMAGE_QUALITY_STANDARD: Standard quality (DALL-E vocabulary)
 * @AI_IMAGE_QUALITY_HD: High definition quality (DALL-E vocabulary)
 * @AI_IMAGE_QUALITY_LOW: Low quality (GPT Image vocabulary)
 * @AI_IMAGE_QUALITY_MEDIUM: Medium quality (GPT Image vocabulary)
 * @AI_IMAGE_QUALITY_HIGH: High quality (GPT Image vocabulary)
 *
 * Enumeration of image quality levels.
 *
 * OpenAI's two image families use disjoint vocabularies for the same
 * concept: DALL-E accepts `standard`/`hd` while GPT Image accepts
 * `low`/`medium`/`high`/`auto`, and each rejects the other's values.  Both
 * sets are therefore represented here, and each provider maps whichever it
 * is given onto the vocabulary its model actually accepts -- so a caller
 * asking for %AI_IMAGE_QUALITY_HD against a GPT Image model gets `high`
 * rather than an API error.
 */
typedef enum
{
    AI_IMAGE_QUALITY_AUTO = 0,
    AI_IMAGE_QUALITY_STANDARD,
    AI_IMAGE_QUALITY_HD,
    AI_IMAGE_QUALITY_LOW,
    AI_IMAGE_QUALITY_MEDIUM,
    AI_IMAGE_QUALITY_HIGH
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

const gchar *
ai_image_size_to_string(AiImageSize size);

AiImageSize
ai_image_size_from_string(const gchar *str);

const gchar *
ai_image_quality_to_string(AiImageQuality quality);

AiImageQuality
ai_image_quality_from_string(const gchar *str);

const gchar *
ai_image_style_to_string(AiImageStyle style);

AiImageStyle
ai_image_style_from_string(const gchar *str);

const gchar *
ai_image_response_format_to_string(AiImageResponseFormat format);

AiImageResponseFormat
ai_image_response_format_from_string(const gchar *str);

/**
 * AiTriState:
 * @AI_TRI_UNSET: Not specified -- omit the parameter entirely (default)
 * @AI_TRI_FALSE: Explicitly false
 * @AI_TRI_TRUE: Explicitly true
 *
 * A boolean that can also be "not specified".
 *
 * Image APIs reject parameters their model does not understand, so ai-glib
 * must be able to distinguish "the caller wants this off" from "the caller
 * never mentioned it".  A plain #gboolean cannot express that, and
 * defaulting to %FALSE would serialise an opt-out the caller never asked
 * for.
 */
typedef enum
{
    AI_TRI_UNSET = 0,
    AI_TRI_FALSE,
    AI_TRI_TRUE
} AiTriState;

GType ai_tri_state_get_type(void);
#define AI_TYPE_TRI_STATE (ai_tri_state_get_type())

/**
 * AiImageOperation:
 * @AI_IMAGE_OPERATION_GENERATE: Generate from a prompt alone (default)
 * @AI_IMAGE_OPERATION_EDIT: Edit or recompose the supplied reference images
 * @AI_IMAGE_OPERATION_VARIATION: Produce variations of a single reference
 * @AI_IMAGE_OPERATION_UPSCALE: Upscale a single reference
 *
 * What an image request is asking the provider to do.
 *
 * Providers route these to different endpoints -- OpenAI has
 * `/v1/images/generations`, `/v1/images/edits` and `/v1/images/variations`,
 * while Gemini expresses all of them through `:generateContent` -- so the
 * operation is part of the request rather than part of the API surface.
 */
typedef enum
{
    AI_IMAGE_OPERATION_GENERATE = 0,
    AI_IMAGE_OPERATION_EDIT,
    AI_IMAGE_OPERATION_VARIATION,
    AI_IMAGE_OPERATION_UPSCALE
} AiImageOperation;

GType ai_image_operation_get_type(void);
#define AI_TYPE_IMAGE_OPERATION (ai_image_operation_get_type())

const gchar *
ai_image_operation_to_string(AiImageOperation operation);

AiImageOperation
ai_image_operation_from_string(const gchar *str);

/**
 * AiImageResolution:
 * @AI_IMAGE_RESOLUTION_AUTO: Let the provider choose (default)
 * @AI_IMAGE_RESOLUTION_1K: Roughly 1024px on the long edge
 * @AI_IMAGE_RESOLUTION_2K: Roughly 2048px on the long edge
 * @AI_IMAGE_RESOLUTION_4K: Roughly 4096px on the long edge
 *
 * A resolution tier, independent of aspect ratio.
 *
 * Gemini's Nano Banana Pro and Imagen express output size as a tier plus an
 * aspect ratio rather than as explicit pixel dimensions, which is why this
 * is distinct from #AiImageSize.
 */
typedef enum
{
    AI_IMAGE_RESOLUTION_AUTO = 0,
    AI_IMAGE_RESOLUTION_1K,
    AI_IMAGE_RESOLUTION_2K,
    AI_IMAGE_RESOLUTION_4K
} AiImageResolution;

GType ai_image_resolution_get_type(void);
#define AI_TYPE_IMAGE_RESOLUTION (ai_image_resolution_get_type())

const gchar *
ai_image_resolution_to_string(AiImageResolution resolution);

AiImageResolution
ai_image_resolution_from_string(const gchar *str);

/**
 * AiImageBackground:
 * @AI_IMAGE_BACKGROUND_AUTO: Let the provider choose (default)
 * @AI_IMAGE_BACKGROUND_TRANSPARENT: Render with an alpha channel
 * @AI_IMAGE_BACKGROUND_OPAQUE: Render on an opaque background
 *
 * Background treatment for the generated image.
 *
 * Transparency requires an output format that has an alpha channel, so
 * pairing %AI_IMAGE_BACKGROUND_TRANSPARENT with a JPEG output format is a
 * request the provider cannot honour.
 */
typedef enum
{
    AI_IMAGE_BACKGROUND_AUTO = 0,
    AI_IMAGE_BACKGROUND_TRANSPARENT,
    AI_IMAGE_BACKGROUND_OPAQUE
} AiImageBackground;

GType ai_image_background_get_type(void);
#define AI_TYPE_IMAGE_BACKGROUND (ai_image_background_get_type())

const gchar *
ai_image_background_to_string(AiImageBackground background);

AiImageBackground
ai_image_background_from_string(const gchar *str);

/**
 * AiImageFormat:
 * @AI_IMAGE_FORMAT_AUTO: Let the provider choose (default)
 * @AI_IMAGE_FORMAT_PNG: PNG
 * @AI_IMAGE_FORMAT_JPEG: JPEG
 * @AI_IMAGE_FORMAT_WEBP: WebP
 *
 * Encoding of the returned image.
 */
typedef enum
{
    AI_IMAGE_FORMAT_AUTO = 0,
    AI_IMAGE_FORMAT_PNG,
    AI_IMAGE_FORMAT_JPEG,
    AI_IMAGE_FORMAT_WEBP
} AiImageFormat;

GType ai_image_format_get_type(void);
#define AI_TYPE_IMAGE_FORMAT (ai_image_format_get_type())

const gchar *
ai_image_format_to_string(AiImageFormat format);

AiImageFormat
ai_image_format_from_string(const gchar *str);

const gchar *
ai_image_format_to_mime_type(AiImageFormat format);

/**
 * AiImageModeration:
 * @AI_IMAGE_MODERATION_AUTO: Provider default filtering (default)
 * @AI_IMAGE_MODERATION_LOW: Relax filtering as far as the provider allows
 * @AI_IMAGE_MODERATION_NONE: Request no filtering
 *
 * How aggressively the provider should filter generated content.
 *
 * Providers differ in how far down they let this go; %AI_IMAGE_MODERATION_NONE
 * is a request, not a guarantee, and a provider with no such tier maps it
 * onto its most permissive setting.
 */
typedef enum
{
    AI_IMAGE_MODERATION_AUTO = 0,
    AI_IMAGE_MODERATION_LOW,
    AI_IMAGE_MODERATION_NONE
} AiImageModeration;

GType ai_image_moderation_get_type(void);
#define AI_TYPE_IMAGE_MODERATION (ai_image_moderation_get_type())

const gchar *
ai_image_moderation_to_string(AiImageModeration moderation);

AiImageModeration
ai_image_moderation_from_string(const gchar *str);

/**
 * AiImagePersonGeneration:
 * @AI_IMAGE_PERSON_GENERATION_DEFAULT: Provider default (default)
 * @AI_IMAGE_PERSON_GENERATION_DONT_ALLOW: Refuse to depict people
 * @AI_IMAGE_PERSON_GENERATION_ALLOW_ADULT: Allow adults only
 * @AI_IMAGE_PERSON_GENERATION_ALLOW_ALL: Allow people of any age
 *
 * Policy for depicting people, as exposed by Imagen.
 */
typedef enum
{
    AI_IMAGE_PERSON_GENERATION_DEFAULT = 0,
    AI_IMAGE_PERSON_GENERATION_DONT_ALLOW,
    AI_IMAGE_PERSON_GENERATION_ALLOW_ADULT,
    AI_IMAGE_PERSON_GENERATION_ALLOW_ALL
} AiImagePersonGeneration;

GType ai_image_person_generation_get_type(void);
#define AI_TYPE_IMAGE_PERSON_GENERATION (ai_image_person_generation_get_type())

const gchar *
ai_image_person_generation_to_string(AiImagePersonGeneration person_generation);

AiImagePersonGeneration
ai_image_person_generation_from_string(const gchar *str);

/**
 * AiImageFidelity:
 * @AI_IMAGE_FIDELITY_AUTO: Let the provider choose (default)
 * @AI_IMAGE_FIDELITY_LOW: Treat the reference loosely
 * @AI_IMAGE_FIDELITY_HIGH: Preserve the reference closely
 *
 * How closely an edit should preserve its input image.
 */
typedef enum
{
    AI_IMAGE_FIDELITY_AUTO = 0,
    AI_IMAGE_FIDELITY_LOW,
    AI_IMAGE_FIDELITY_HIGH
} AiImageFidelity;

GType ai_image_fidelity_get_type(void);
#define AI_TYPE_IMAGE_FIDELITY (ai_image_fidelity_get_type())

const gchar *
ai_image_fidelity_to_string(AiImageFidelity fidelity);

AiImageFidelity
ai_image_fidelity_from_string(const gchar *str);

/**
 * AiSearchFreshness:
 * @AI_SEARCH_FRESHNESS_ANY: No recency restriction (default)
 * @AI_SEARCH_FRESHNESS_DAY: Results from the past day
 * @AI_SEARCH_FRESHNESS_WEEK: Results from the past week
 * @AI_SEARCH_FRESHNESS_MONTH: Results from the past month
 * @AI_SEARCH_FRESHNESS_YEAR: Results from the past year
 *
 * Recency filter for web search results. Each #AiSearchProvider maps these
 * to its backend's own freshness parameter; backends that cannot express a
 * given window omit the filter rather than approximating it.
 */
typedef enum
{
    AI_SEARCH_FRESHNESS_ANY = 0,
    AI_SEARCH_FRESHNESS_DAY,
    AI_SEARCH_FRESHNESS_WEEK,
    AI_SEARCH_FRESHNESS_MONTH,
    AI_SEARCH_FRESHNESS_YEAR
} AiSearchFreshness;

GType ai_search_freshness_get_type(void);
#define AI_TYPE_SEARCH_FRESHNESS (ai_search_freshness_get_type())

const gchar *
ai_search_freshness_to_string(AiSearchFreshness freshness);

AiSearchFreshness
ai_search_freshness_from_string(const gchar *str);

/**
 * AiSearchSafeSearch:
 * @AI_SEARCH_SAFE_OFF: No content filtering
 * @AI_SEARCH_SAFE_MODERATE: Moderate filtering (default)
 * @AI_SEARCH_SAFE_STRICT: Strict filtering
 *
 * Safe-search level for web search results.
 */
typedef enum
{
    AI_SEARCH_SAFE_OFF = 0,
    AI_SEARCH_SAFE_MODERATE,
    AI_SEARCH_SAFE_STRICT
} AiSearchSafeSearch;

GType ai_search_safe_search_get_type(void);
#define AI_TYPE_SEARCH_SAFE_SEARCH (ai_search_safe_search_get_type())

const gchar *
ai_search_safe_search_to_string(AiSearchSafeSearch safe_search);

AiSearchSafeSearch
ai_search_safe_search_from_string(const gchar *str);

G_END_DECLS
