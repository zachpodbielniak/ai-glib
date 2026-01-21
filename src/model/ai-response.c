/*
 * ai-response.c - API response
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-response.h"
#include "model/ai-text-content.h"

/*
 * Private structure for AiResponse.
 */
struct _AiResponse
{
    GObject parent_instance;

    gchar        *id;
    gchar        *model;
    AiStopReason  stop_reason;
    AiUsage      *usage;
    GList        *content_blocks; /* List of AiContentBlock */
};

G_DEFINE_TYPE(AiResponse, ai_response, G_TYPE_OBJECT)

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_ID,
    PROP_MODEL,
    PROP_STOP_REASON,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_response_finalize(GObject *object)
{
    AiResponse *self = AI_RESPONSE(object);

    g_clear_pointer(&self->id, g_free);
    g_clear_pointer(&self->model, g_free);
    g_clear_pointer(&self->usage, ai_usage_free);
    g_list_free_full(self->content_blocks, g_object_unref);

    G_OBJECT_CLASS(ai_response_parent_class)->finalize(object);
}

static void
ai_response_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiResponse *self = AI_RESPONSE(object);

    switch (prop_id)
    {
        case PROP_ID:
            g_value_set_string(value, self->id);
            break;
        case PROP_MODEL:
            g_value_set_string(value, self->model);
            break;
        case PROP_STOP_REASON:
            g_value_set_enum(value, self->stop_reason);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_response_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiResponse *self = AI_RESPONSE(object);

    switch (prop_id)
    {
        case PROP_ID:
            g_clear_pointer(&self->id, g_free);
            self->id = g_value_dup_string(value);
            break;
        case PROP_MODEL:
            g_clear_pointer(&self->model, g_free);
            self->model = g_value_dup_string(value);
            break;
        case PROP_STOP_REASON:
            self->stop_reason = g_value_get_enum(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_response_class_init(AiResponseClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_response_finalize;
    object_class->get_property = ai_response_get_property;
    object_class->set_property = ai_response_set_property;

    /**
     * AiResponse:id:
     *
     * The response ID.
     */
    properties[PROP_ID] =
        g_param_spec_string("id",
                            "ID",
                            "The response ID",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiResponse:model:
     *
     * The model used for this response.
     */
    properties[PROP_MODEL] =
        g_param_spec_string("model",
                            "Model",
                            "The model used for this response",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiResponse:stop-reason:
     *
     * The reason generation stopped.
     */
    properties[PROP_STOP_REASON] =
        g_param_spec_enum("stop-reason",
                          "Stop Reason",
                          "The reason generation stopped",
                          AI_TYPE_STOP_REASON,
                          AI_STOP_REASON_NONE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_response_init(AiResponse *self)
{
    self->id = NULL;
    self->model = NULL;
    self->stop_reason = AI_STOP_REASON_NONE;
    self->usage = NULL;
    self->content_blocks = NULL;
}

/**
 * ai_response_new:
 * @id: (nullable): the response ID
 * @model: (nullable): the model used
 *
 * Creates a new #AiResponse.
 *
 * Returns: (transfer full): a new #AiResponse
 */
AiResponse *
ai_response_new(
    const gchar *id,
    const gchar *model
){
    g_autoptr(AiResponse) self = g_object_new(AI_TYPE_RESPONSE,
                                               "id", id,
                                               "model", model,
                                               NULL);

    return (AiResponse *)g_steal_pointer(&self);
}

/**
 * ai_response_get_id:
 * @self: an #AiResponse
 *
 * Gets the response ID.
 *
 * Returns: (transfer none) (nullable): the response ID
 */
const gchar *
ai_response_get_id(AiResponse *self)
{
    g_return_val_if_fail(AI_IS_RESPONSE(self), NULL);

    return self->id;
}

/**
 * ai_response_get_model:
 * @self: an #AiResponse
 *
 * Gets the model used for this response.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_response_get_model(AiResponse *self)
{
    g_return_val_if_fail(AI_IS_RESPONSE(self), NULL);

    return self->model;
}

/**
 * ai_response_get_stop_reason:
 * @self: an #AiResponse
 *
 * Gets the reason generation stopped.
 *
 * Returns: the #AiStopReason
 */
AiStopReason
ai_response_get_stop_reason(AiResponse *self)
{
    g_return_val_if_fail(AI_IS_RESPONSE(self), AI_STOP_REASON_NONE);

    return self->stop_reason;
}

/**
 * ai_response_set_stop_reason:
 * @self: an #AiResponse
 * @reason: the stop reason
 *
 * Sets the stop reason.
 */
void
ai_response_set_stop_reason(
    AiResponse   *self,
    AiStopReason  reason
){
    g_return_if_fail(AI_IS_RESPONSE(self));

    self->stop_reason = reason;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STOP_REASON]);
}

/**
 * ai_response_get_usage:
 * @self: an #AiResponse
 *
 * Gets the token usage information.
 *
 * Returns: (transfer none) (nullable): the #AiUsage
 */
AiUsage *
ai_response_get_usage(AiResponse *self)
{
    g_return_val_if_fail(AI_IS_RESPONSE(self), NULL);

    return self->usage;
}

/**
 * ai_response_set_usage:
 * @self: an #AiResponse
 * @usage: (nullable): the usage information
 *
 * Sets the usage information.
 */
void
ai_response_set_usage(
    AiResponse *self,
    AiUsage    *usage
){
    g_return_if_fail(AI_IS_RESPONSE(self));

    g_clear_pointer(&self->usage, ai_usage_free);
    if (usage != NULL)
    {
        self->usage = ai_usage_copy(usage);
    }
}

/**
 * ai_response_get_content_blocks:
 * @self: an #AiResponse
 *
 * Gets the list of content blocks in this response.
 *
 * Returns: (transfer none) (element-type AiContentBlock): the content blocks
 */
GList *
ai_response_get_content_blocks(AiResponse *self)
{
    g_return_val_if_fail(AI_IS_RESPONSE(self), NULL);

    return self->content_blocks;
}

/**
 * ai_response_add_content_block:
 * @self: an #AiResponse
 * @block: (transfer full): the content block to add
 *
 * Adds a content block to the response.
 * The response takes ownership of the block.
 */
void
ai_response_add_content_block(
    AiResponse     *self,
    AiContentBlock *block
){
    g_return_if_fail(AI_IS_RESPONSE(self));
    g_return_if_fail(AI_IS_CONTENT_BLOCK(block));

    self->content_blocks = g_list_append(self->content_blocks, block);
}

/**
 * ai_response_get_text:
 * @self: an #AiResponse
 *
 * Gets the concatenated text content of the response.
 * This combines all text content blocks into a single string.
 *
 * Returns: (transfer full) (nullable): the text content, free with g_free()
 */
gchar *
ai_response_get_text(AiResponse *self)
{
    g_autoptr(GString) result = NULL;
    GList *l;

    g_return_val_if_fail(AI_IS_RESPONSE(self), NULL);

    result = g_string_new(NULL);

    for (l = self->content_blocks; l != NULL; l = l->next)
    {
        AiContentBlock *block = l->data;

        if (AI_IS_TEXT_CONTENT(block))
        {
            const gchar *text = ai_text_content_get_text(AI_TEXT_CONTENT(block));
            if (text != NULL)
            {
                if (result->len > 0)
                {
                    g_string_append_c(result, '\n');
                }
                g_string_append(result, text);
            }
        }
    }

    if (result->len == 0)
    {
        return NULL;
    }

    return g_string_free(g_steal_pointer(&result), FALSE);
}

/**
 * ai_response_has_tool_use:
 * @self: an #AiResponse
 *
 * Checks if the response contains any tool use requests.
 *
 * Returns: %TRUE if there are tool use blocks
 */
gboolean
ai_response_has_tool_use(AiResponse *self)
{
    GList *l;

    g_return_val_if_fail(AI_IS_RESPONSE(self), FALSE);

    for (l = self->content_blocks; l != NULL; l = l->next)
    {
        if (AI_IS_TOOL_USE(l->data))
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * ai_response_get_tool_uses:
 * @self: an #AiResponse
 *
 * Gets the list of tool use requests in this response.
 *
 * Returns: (transfer container) (element-type AiToolUse): tool use blocks
 */
GList *
ai_response_get_tool_uses(AiResponse *self)
{
    GList *result = NULL;
    GList *l;

    g_return_val_if_fail(AI_IS_RESPONSE(self), NULL);

    for (l = self->content_blocks; l != NULL; l = l->next)
    {
        if (AI_IS_TOOL_USE(l->data))
        {
            result = g_list_append(result, l->data);
        }
    }

    return result;
}
