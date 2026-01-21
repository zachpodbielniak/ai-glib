/*
 * ai-text-content.c - Text content block
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-text-content.h"

/*
 * Private structure for AiTextContent.
 * Stores the text string.
 */
struct _AiTextContent
{
    AiContentBlock parent_instance;

    gchar *text;
};

G_DEFINE_TYPE(AiTextContent, ai_text_content, AI_TYPE_CONTENT_BLOCK)

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_TEXT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_text_content_finalize(GObject *object)
{
    AiTextContent *self = AI_TEXT_CONTENT(object);

    g_clear_pointer(&self->text, g_free);

    G_OBJECT_CLASS(ai_text_content_parent_class)->finalize(object);
}

static void
ai_text_content_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiTextContent *self = AI_TEXT_CONTENT(object);

    switch (prop_id)
    {
        case PROP_TEXT:
            g_value_set_string(value, self->text);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_text_content_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiTextContent *self = AI_TEXT_CONTENT(object);

    switch (prop_id)
    {
        case PROP_TEXT:
            ai_text_content_set_text(self, g_value_get_string(value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * Override get_content_type to return AI_CONTENT_TYPE_TEXT.
 */
static AiContentType
ai_text_content_get_content_type(AiContentBlock *block)
{
    (void)block;
    return AI_CONTENT_TYPE_TEXT;
}

/*
 * Serialize to JSON in the format:
 * { "type": "text", "text": "..." }
 */
static JsonNode *
ai_text_content_to_json(AiContentBlock *block)
{
    AiTextContent *self = AI_TEXT_CONTENT(block);
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "text");

    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, self->text != NULL ? self->text : "");

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static void
ai_text_content_class_init(AiTextContentClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiContentBlockClass *content_class = AI_CONTENT_BLOCK_CLASS(klass);

    object_class->finalize = ai_text_content_finalize;
    object_class->get_property = ai_text_content_get_property;
    object_class->set_property = ai_text_content_set_property;

    /* Override virtual methods */
    content_class->get_content_type = ai_text_content_get_content_type;
    content_class->to_json = ai_text_content_to_json;

    /**
     * AiTextContent:text:
     *
     * The text content.
     */
    properties[PROP_TEXT] =
        g_param_spec_string("text",
                            "Text",
                            "The text content",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_text_content_init(AiTextContent *self)
{
    self->text = NULL;
}

/**
 * ai_text_content_new:
 * @text: the text content
 *
 * Creates a new #AiTextContent with the given text.
 *
 * Returns: (transfer full): a new #AiTextContent
 */
AiTextContent *
ai_text_content_new(const gchar *text)
{
    g_autoptr(AiTextContent) self = g_object_new(AI_TYPE_TEXT_CONTENT,
                                                  "text", text,
                                                  NULL);

    return (AiTextContent *)g_steal_pointer(&self);
}

/**
 * ai_text_content_get_text:
 * @self: an #AiTextContent
 *
 * Gets the text content.
 *
 * Returns: (transfer none): the text content
 */
const gchar *
ai_text_content_get_text(AiTextContent *self)
{
    g_return_val_if_fail(AI_IS_TEXT_CONTENT(self), NULL);

    return self->text;
}

/**
 * ai_text_content_set_text:
 * @self: an #AiTextContent
 * @text: the text to set
 *
 * Sets the text content.
 */
void
ai_text_content_set_text(
    AiTextContent *self,
    const gchar   *text
){
    g_return_if_fail(AI_IS_TEXT_CONTENT(self));

    g_clear_pointer(&self->text, g_free);
    self->text = g_strdup(text);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TEXT]);
}
