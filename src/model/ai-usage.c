/*
 * ai-usage.c - Token usage information
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-usage.h"

/*
 * Private structure for AiUsage boxed type.
 * Stores the token counts from an API response.
 */
struct _AiUsage
{
    gint input_tokens;
    gint output_tokens;
    gint ref_count;
};

/*
 * ai_usage_get_type:
 *
 * Registers the AiUsage boxed type with the GLib type system.
 * Uses copy and free functions for memory management.
 */
G_DEFINE_BOXED_TYPE(AiUsage, ai_usage, ai_usage_copy, ai_usage_free)

/**
 * ai_usage_new:
 * @input_tokens: number of input tokens
 * @output_tokens: number of output tokens
 *
 * Creates a new #AiUsage instance with the specified token counts.
 *
 * Returns: (transfer full): a new #AiUsage
 */
AiUsage *
ai_usage_new(
    gint input_tokens,
    gint output_tokens
){
    AiUsage *self;

    self = g_slice_new0(AiUsage);
    self->input_tokens = input_tokens;
    self->output_tokens = output_tokens;
    self->ref_count = 1;

    return self;
}

/**
 * ai_usage_copy:
 * @self: an #AiUsage
 *
 * Creates a copy of an #AiUsage instance.
 *
 * Returns: (transfer full): a copy of @self
 */
AiUsage *
ai_usage_copy(const AiUsage *self)
{
    if (self == NULL)
    {
        return NULL;
    }

    return ai_usage_new(self->input_tokens, self->output_tokens);
}

/**
 * ai_usage_free:
 * @self: (nullable): an #AiUsage
 *
 * Frees an #AiUsage instance.
 * If @self is %NULL, this function does nothing.
 */
void
ai_usage_free(AiUsage *self)
{
    if (self == NULL)
    {
        return;
    }

    g_slice_free(AiUsage, self);
}

/**
 * ai_usage_get_input_tokens:
 * @self: an #AiUsage
 *
 * Gets the number of input tokens used in the request.
 *
 * Returns: the input token count
 */
gint
ai_usage_get_input_tokens(const AiUsage *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->input_tokens;
}

/**
 * ai_usage_get_output_tokens:
 * @self: an #AiUsage
 *
 * Gets the number of output tokens generated in the response.
 *
 * Returns: the output token count
 */
gint
ai_usage_get_output_tokens(const AiUsage *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->output_tokens;
}

/**
 * ai_usage_get_total_tokens:
 * @self: an #AiUsage
 *
 * Gets the total number of tokens (input + output).
 *
 * Returns: the total token count
 */
gint
ai_usage_get_total_tokens(const AiUsage *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->input_tokens + self->output_tokens;
}
