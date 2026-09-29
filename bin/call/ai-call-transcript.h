/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_CALL_TRANSCRIPT (ai_call_transcript_get_type())
G_DECLARE_FINAL_TYPE(AiCallTranscript, ai_call_transcript, AI, CALL_TRANSCRIPT, GObject)
/* $XDG_STATE_HOME/ai-glib/calls; transfer full. */
gchar *
ai_call_transcript_default_dir(void);
/* Creates DIR (0700) and a new JSON Lines file (0600) whose first line is the
 * call header. Utterances are appended as the call goes, so a crash keeps what
 * was said; the header's end and duration stay null until close. */
AiCallTranscript *
ai_call_transcript_open(const gchar *dir, const gchar *room, GDateTime *start,
						GError **error);
gboolean
ai_call_transcript_append(AiCallTranscript *self, GDateTime *at, const gchar *speaker,
						  const gchar *text, GError **error);
/* Rewrites the header with END and the duration, atomically. Idempotent. */
gboolean
ai_call_transcript_close(AiCallTranscript *self, GDateTime *end, GError **error);
const gchar *
ai_call_transcript_get_path(AiCallTranscript *self);
/* Runs COMMAND (a command line parsed like a shell's, but not run by one) with
 * PATH appended as its last argument, without waiting for it. */
gboolean
ai_call_transcript_run_hook(const gchar *command, const gchar *path, GError **error);
G_END_DECLS
