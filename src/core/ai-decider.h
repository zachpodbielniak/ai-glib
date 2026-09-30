/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
#include "model/ai-decision.h"
G_BEGIN_DECLS
#define AI_TYPE_DECIDER (ai_decider_get_type())
G_DECLARE_INTERFACE(AiDecider, ai_decider, AI, DECIDER, GObject)
struct _AiDeciderInterface
{
	GTypeInterface parent_iface;
	void (*decide_async)(AiDecider *self, AiDecisionRequest *request, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
	AiDecisionResponse *(*decide_finish)(AiDecider *self, GAsyncResult *result, GError **error);
	gpointer _reserved[8];
};
void
ai_decider_decide_async(
	AiDecider *self,
	AiDecisionRequest *request,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data
);
AiDecisionResponse *
ai_decider_decide_finish(
	AiDecider *self,
	GAsyncResult *result,
	GError **error
);
AiDecisionResponse *
ai_decider_decide(
	AiDecider *self,
	AiDecisionRequest *request,
	GCancellable *cancellable,
	GError **error
);
G_END_DECLS
