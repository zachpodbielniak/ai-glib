/*
 * ai-native-session.h - Read a wrapped CLI's own session transcript
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "core/ai-cli-client.h"

G_BEGIN_DECLS

/**
 * AiNativeSessionKind:
 * @AI_NATIVE_SESSION_UNSUPPORTED: no reader is registered for the provider
 * @AI_NATIVE_SESSION_JSONL: a line-delimited JSON transcript on disk
 *
 * How a provider's private session store is held.
 *
 * Only stores this library can read without a new dependency are
 * represented. `opencode`, `cursor` and `antigravity` keep their history
 * in SQLite databases and are deliberately %AI_NATIVE_SESSION_UNSUPPORTED
 * rather than half-read: a partial import is worse than none, because the
 * gap is invisible to the model that receives it.
 */
typedef enum
{
	AI_NATIVE_SESSION_UNSUPPORTED = 0,
	AI_NATIVE_SESSION_JSONL
} AiNativeSessionKind;

#define AI_TYPE_NATIVE_SESSION (ai_native_session_get_type())
G_DECLARE_FINAL_TYPE(AiNativeSession, ai_native_session, AI, NATIVE_SESSION,
                     GObject)

const gchar *
ai_native_session_get_provider(AiNativeSession *self);

const gchar *
ai_native_session_get_session_id(AiNativeSession *self);

const gchar *
ai_native_session_get_path(AiNativeSession *self);

AiNativeSessionKind
ai_native_session_get_kind(AiNativeSession *self);

GList *
ai_native_session_get_messages(AiNativeSession *self);

gboolean
ai_native_session_get_compacted(AiNativeSession *self);

guint
ai_native_session_get_dropped(AiNativeSession *self);

gchar *
ai_native_session_to_context_text(
	AiNativeSession *self,
	gsize            max_bytes
);

gchar *
ai_native_session_to_context_text_full(
	AiNativeSession *self,
	GList           *exclude,
	gsize            max_bytes
);

AiNativeSessionKind
ai_native_session_kind_for_provider(const gchar *provider);

AiNativeSession *
ai_native_session_read_file(
	const gchar  *provider,
	const gchar  *path,
	const gchar  *session_id,
	GError      **error
);

AiNativeSession *
ai_cli_client_read_native_session(
	AiCliClient  *client,
	GError      **error
);

G_END_DECLS
