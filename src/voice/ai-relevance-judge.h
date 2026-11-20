/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
#include "core/ai-provider.h"
G_BEGIN_DECLS
#define AI_TYPE_RELEVANCE_JUDGE (ai_relevance_judge_get_type())
G_DECLARE_INTERFACE(AiRelevanceJudge, ai_relevance_judge, AI, RELEVANCE_JUDGE, GObject)
/**
 * AiRelevanceJudgeInterface:
 * @parent_iface: the parent interface
 * @judge_async: start scoring room talk the assistant was not addressed in
 * @judge_finish: the score, 0 to 10, and a one-line reason
 *
 * Decides how much the assistant should speak up, unasked, about what was
 * said in a room. #AiProviderRelevanceJudge asks a model; an application
 * can supply its own to #AiVoiceSession:relevance-judge.
 */
struct _AiRelevanceJudgeInterface {
	GTypeInterface parent_iface;
	void (*judge_async)(AiRelevanceJudge *self, const gchar *assistant,
						const gchar *const *lines, GCancellable *cancellable,
						GAsyncReadyCallback callback, gpointer user_data);
	gint (*judge_finish)(AiRelevanceJudge *self, GAsyncResult *result, gchar **reason,
						 GError **error);
	gpointer _reserved[8];
};
void
ai_relevance_judge_judge_async(AiRelevanceJudge *self, const gchar *assistant,
							   const gchar *const *lines, GCancellable *cancellable,
							   GAsyncReadyCallback callback, gpointer user_data);
gint
ai_relevance_judge_judge_finish(AiRelevanceJudge *self, GAsyncResult *result, gchar **reason,
								GError **error);
guint
ai_relevance_judge_parse_reply(const gchar *text, gchar **reason);

#define AI_TYPE_PROVIDER_RELEVANCE_JUDGE (ai_provider_relevance_judge_get_type())
G_DECLARE_DERIVABLE_TYPE(AiProviderRelevanceJudge, ai_provider_relevance_judge, AI,
						 PROVIDER_RELEVANCE_JUDGE, GObject)
/**
 * AiProviderRelevanceJudgeClass:
 * @parent_class: the parent class
 * @build_system_prompt: the instructions for the model, given the
 *   assistant's name
 * @build_message: the user message, given the lines of talk
 * @parse: read a score (0 to 10) and reason from the model's reply;
 *   ai_relevance_judge_parse_reply() by default
 * @failed: class handler of #AiProviderRelevanceJudge::failed
 *
 * Subclass to change what the model is asked or how its answer is read.
 */
struct _AiProviderRelevanceJudgeClass {
	GObjectClass parent_class;
	gchar *(*build_system_prompt)(AiProviderRelevanceJudge *self, const gchar *assistant);
	gchar *(*build_message)(AiProviderRelevanceJudge *self, const gchar *const *lines);
	guint (*parse)(AiProviderRelevanceJudge *self, const gchar *text, gchar **reason);
	void (*failed)(AiProviderRelevanceJudge *self, const GError *error);
	gpointer _reserved[8];
};
AiProviderRelevanceJudge *
ai_provider_relevance_judge_new(AiProvider *provider);
G_END_DECLS
