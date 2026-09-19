/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#ifndef AI_GLIB_COMPILATION
#error "Private header"
#endif
#include "voice/ai-voice-session.h"
#include "core/ai-event.h"
typedef struct _AiVoiceWorker AiVoiceWorker;
typedef enum {
	AI_VOICE_MAIL_EVENT,
	AI_VOICE_MAIL_DONE,
	AI_VOICE_MAIL_AGENT
} AiVoiceMailKind;
typedef void (*AiVoiceMailFunc)(GObject *owner, AiVoiceMailKind kind, guint64 generation,
								AiEvent *event, const gchar *text, const GError *error);
AiVoiceWorker *
ai_voice_worker_new(AiConversation *conversation, GObject *owner, GMainContext *context,
					AiVoiceMailFunc callback);
void
ai_voice_worker_send(AiVoiceWorker *worker, const gchar *text, GCancellable *cancel,
					 guint64 generation);
void
ai_voice_worker_spoken(AiVoiceWorker *worker, const gchar *text, guint64 generation);
void
ai_voice_worker_stop(AiVoiceWorker *worker);
