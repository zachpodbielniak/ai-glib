/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include <string.h>
#include "view/ai-prompt-queue.h"
#include "core/ai-error.h"

typedef struct {
	gchar *text;
	GList *images;
	gboolean separate;
} Prompt;

struct _AiPromptQueue {
	GObject parent_instance;
	GQueue pending;
	guint limit;
	gboolean coalesce;
};
G_DEFINE_TYPE(AiPromptQueue, ai_prompt_queue, G_TYPE_OBJECT)
enum { PROP_0, PROP_LENGTH, PROP_LIMIT, PROP_COALESCE, N_PROPS };
static GParamSpec *properties[N_PROPS];

static void
prompt_free(gpointer data)
{
	Prompt *prompt = data;
	g_free(prompt->text);
	g_list_free_full(prompt->images, g_object_unref);
	g_free(prompt);
}

static void
queue_finalize(GObject *object)
{
	g_queue_clear_full(&AI_PROMPT_QUEUE(object)->pending, prompt_free);
	G_OBJECT_CLASS(ai_prompt_queue_parent_class)->finalize(object);
}

static void
queue_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	AiPromptQueue *self = AI_PROMPT_QUEUE(object);
	switch (id) {
	case PROP_LENGTH: g_value_set_uint(value, self->pending.length); break;
	case PROP_LIMIT: g_value_set_uint(value, self->limit); break;
	case PROP_COALESCE: g_value_set_boolean(value, self->coalesce); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
queue_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	AiPromptQueue *self = AI_PROMPT_QUEUE(object);
	switch (id) {
	case PROP_LIMIT: self->limit = g_value_get_uint(value); break;
	case PROP_COALESCE: self->coalesce = g_value_get_boolean(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
ai_prompt_queue_class_init(AiPromptQueueClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = queue_finalize;
	object_class->get_property = queue_get_property;
	object_class->set_property = queue_set_property;
	properties[PROP_LENGTH] = g_param_spec_uint("length", "Length",
		"Number of pending submissions", 0, G_MAXUINT, 0,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_LIMIT] = g_param_spec_uint("max-length", "Maximum length",
		"Maximum pending submissions; lowering does not discard existing entries",
		1, 4096, 32, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_COALESCE] = g_param_spec_boolean("coalesce", "Coalesce",
		"Combine adjacent text-only prompts on pop", TRUE,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_prompt_queue_init(AiPromptQueue *self)
{
	g_queue_init(&self->pending);
	self->limit = 32;
	self->coalesce = TRUE;
}

/**
 * ai_prompt_queue_new:
 *
 * Creates a bounded FIFO. Use from one main context. This object stores
 * submissions; the host decides when a turn has finished and pops a batch.
 *
 * Returns: (transfer full): a new queue
 */
AiPromptQueue *
ai_prompt_queue_new(void)
{
	g_autoptr(AiPromptQueue) self = g_object_new(AI_TYPE_PROMPT_QUEUE, NULL);
	return g_steal_pointer(&self);
}

/**
 * ai_prompt_queue_get_length:
 * @self: a queue
 * Returns: number of pending submissions, before coalescing
 */
guint
ai_prompt_queue_get_length(AiPromptQueue *self)
{
	g_return_val_if_fail(AI_IS_PROMPT_QUEUE(self), 0);
	return self->pending.length;
}

/**
 * ai_prompt_queue_push:
 * @self: a queue
 * @text: UTF-8 prompt text; may be empty with attachments
 * @images: (nullable) (element-type AiImageContent) (transfer none): attachments
 * @separate: keep this submission separate, for example a slash command
 * @error: return location for an error
 *
 * Copies text and references attachments. Rejection leaves the queue intact.
 * Attachments always form a separate batch, preserving their association.
 * Text is bounded to 1 MiB per submission.
 *
 * Returns: whether the submission was accepted
 */
gboolean
ai_prompt_queue_push(AiPromptQueue *self, const gchar *text, GList *images,
                     gboolean separate, GError **error)
{
	Prompt *prompt;
	GList *l;
	g_autofree gchar *trimmed = NULL;
	g_return_val_if_fail(AI_IS_PROMPT_QUEUE(self), FALSE);
	if (text == NULL || !g_utf8_validate(text, -1, NULL) || strlen(text) > 1024 * 1024) {
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Invalid or oversized queued prompt");
		return FALSE;
	}
	trimmed = g_strdup(text);
	if (*g_strstrip(trimmed) == '\0' && images == NULL) {
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Nothing to queue");
		return FALSE;
	}
	for (l = images; l != NULL; l = l->next) {
		if (!AI_IS_IMAGE_CONTENT(l->data)) {
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Invalid queued attachment");
			return FALSE;
		}
	}
	if (self->pending.length >= self->limit) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Send queue is full");
		return FALSE;
	}
	prompt = g_new0(Prompt, 1);
	prompt->text = g_strdup(text);
	for (l = images; l != NULL; l = l->next)
		prompt->images = g_list_append(prompt->images, g_object_ref(l->data));
	prompt->separate = separate || images != NULL;
	g_queue_push_tail(&self->pending, prompt);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LENGTH]);
	return TRUE;
}

/**
 * ai_prompt_queue_peek:
 * @self: a queue
 * @images: (out) (optional) (element-type AiImageContent) (transfer none): attachments
 *
 * Inspects the oldest submission without removing or coalescing it. Use this
 * to validate attachments before taking a batch. Borrowed values remain valid
 * until the queue is popped, cleared or destroyed; do not modify the list.
 *
 * Returns: (transfer none) (nullable): oldest text, or NULL if empty
 */
const gchar *
ai_prompt_queue_peek(AiPromptQueue *self, GList **images)
{
	Prompt *prompt;
	g_return_val_if_fail(AI_IS_PROMPT_QUEUE(self), NULL);
	prompt = g_queue_peek_head(&self->pending);
	if (images != NULL) *images = prompt != NULL ? prompt->images : NULL;
	return prompt != NULL ? prompt->text : NULL;
}

/**
 * ai_prompt_queue_pop:
 * @self: a queue
 * @images: (out) (optional) (element-type AiImageContent) (transfer full): batch attachments
 *
 * Takes the oldest batch. With coalescing enabled, adjacent text-only entries
 * are joined with two newlines, up to 1 MiB. A separate entry is a barrier:
 * commands and attachments are never merged or reordered across it.
 *
 * Returns: (transfer full) (nullable): batch text, or NULL if empty
 */
gchar *
ai_prompt_queue_pop(AiPromptQueue *self, GList **images)
{
	Prompt *prompt;
	g_autoptr(GString) text = NULL;
	gboolean merge;
	g_return_val_if_fail(AI_IS_PROMPT_QUEUE(self), NULL);
	if (images != NULL) *images = NULL;
	prompt = g_queue_pop_head(&self->pending);
	if (prompt == NULL) return NULL;
	text = g_string_new(prompt->text);
	merge = self->coalesce && !prompt->separate;
	if (images != NULL) *images = g_steal_pointer(&prompt->images);
	prompt_free(prompt);
	while (merge && (prompt = g_queue_peek_head(&self->pending)) != NULL &&
	       !prompt->separate && text->len + strlen(prompt->text) + 2 <= 1024 * 1024) {
		g_string_append(text, "\n\n");
		g_string_append(text, prompt->text);
		prompt_free(g_queue_pop_head(&self->pending));
	}
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LENGTH]);
	return g_string_free(g_steal_pointer(&text), FALSE);
}

/**
 * ai_prompt_queue_clear:
 * @self: a queue
 *
 * Discards all pending submissions, without affecting any in-flight turn.
 */
void
ai_prompt_queue_clear(AiPromptQueue *self)
{
	g_return_if_fail(AI_IS_PROMPT_QUEUE(self));
	if (g_queue_is_empty(&self->pending)) return;
	g_queue_clear_full(&self->pending, prompt_free);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LENGTH]);
}
