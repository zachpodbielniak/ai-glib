/*
 * ai-usage.h - Token usage information
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

#define AI_TYPE_USAGE (ai_usage_get_type())

/**
 * AiUsage:
 *
 * A boxed type containing token usage information from an API response.
 * This is typically included in responses to help track API usage.
 */
typedef struct _AiUsage AiUsage;

/**
 * ai_usage_get_type:
 *
 * Gets the #GType for #AiUsage.
 *
 * Returns: the #GType for #AiUsage
 */
GType
ai_usage_get_type(void);

AiUsage *
ai_usage_new(
    gint input_tokens,
    gint output_tokens
);

AiUsage *
ai_usage_copy(const AiUsage *self);

void
ai_usage_free(AiUsage *self);

gint
ai_usage_get_input_tokens(const AiUsage *self);

gint
ai_usage_get_output_tokens(const AiUsage *self);

gint
ai_usage_get_total_tokens(const AiUsage *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiUsage, ai_usage_free)

G_END_DECLS
