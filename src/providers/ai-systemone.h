/*
 * ai-systemone.h - Shared POST /v1/systemone transport (private)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One wire, three policies. TypeSafe, Ollama and Laya all answer a state
 * and a set of named questions at /v1/systemone. What they refuse — a
 * checkpoint name, a noul criterion, a box, a 64 KiB body — is the policy
 * of that server, applied here before a byte is sent. The HTTP operation
 * itself is not copied per client.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-systemone.h is private to ai-glib and cannot be included directly."
#endif

#include <gio/gio.h>
#include "model/ai-decision.h"

G_BEGIN_DECLS

typedef enum
{
	AI_SYSTEMONE_LAYA,
	AI_SYSTEMONE_TYPESAFE,
	AI_SYSTEMONE_OLLAMA
} AiSystemOneKind;

/*
 * Borrowed for the duration of ai_systemone_decide_async(), which copies
 * what it keeps before returning. model and api_key may be NULL.
 * timeout_ms is the whole operation, not a per-retry budget: this path
 * does not retry. A classification that is late is already the wrong
 * answer to wait a second time for.
 */
typedef struct
{
	AiSystemOneKind kind;
	const gchar *base_url;
	const gchar *model;
	const gchar *api_key;
	guint timeout_ms;
	gboolean long_context;
} AiSystemOneTarget;

void
ai_systemone_decide_async(
	GObject *source,
	const AiSystemOneTarget *target,
	AiDecisionRequest *request,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data,
	gpointer source_tag
);

AiDecisionResponse *
ai_systemone_decide_finish(
	GObject *source,
	GAsyncResult *result,
	gpointer source_tag,
	GError **error
);

G_END_DECLS
