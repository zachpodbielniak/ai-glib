/*
 * ai-loop-runner.h - Drives one session's loops and goals
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

#include "harness/ai-loop.h"

G_BEGIN_DECLS

#define AI_TYPE_LOOP_RUNNER (ai_loop_runner_get_type())

G_DECLARE_FINAL_TYPE(AiLoopRunner, ai_loop_runner, AI, LOOP_RUNNER, GObject)

AiLoopRunner *
ai_loop_runner_new(void);

AiLoopSchedule *
ai_loop_runner_get_schedule(AiLoopRunner *self);

gboolean
ai_loop_runner_open(
	AiLoopRunner *self,
	const gchar  *directory,
	const gchar  *owner,
	gint64        now_us,
	GError      **error
);

void
ai_loop_runner_close(AiLoopRunner *self);

const gchar *
ai_loop_runner_get_owner(AiLoopRunner *self);

const gchar *
ai_loop_runner_get_path(AiLoopRunner *self);

const gchar *
ai_loop_runner_get_active_id(AiLoopRunner *self);

const gchar *
ai_loop_runner_get_working_directory(AiLoopRunner *self);

void
ai_loop_runner_set_working_directory(AiLoopRunner *self, const gchar *directory);

void
ai_loop_runner_start(AiLoopRunner *self);

void
ai_loop_runner_stop(AiLoopRunner *self);

gboolean
ai_loop_runner_tick(AiLoopRunner *self, gint64 now_us);

void
ai_loop_runner_turn_finished(
	AiLoopRunner *self,
	const gchar  *assistant_text,
	const gchar  *turn_error,
	gint64        now_us
);

void
ai_loop_runner_turn_cancelled(AiLoopRunner *self, gint64 now_us);

gchar *
ai_loop_runner_command(
	AiLoopRunner *self,
	const gchar  *name,
	const gchar  *arguments,
	gint64        now_us,
	GError      **error
);

gboolean
ai_loop_runner_has_pending_goal(AiLoopRunner *self);

gboolean
ai_loop_runner_save(AiLoopRunner *self, GError **error);

void
ai_loop_runner_clear(AiLoopRunner *self);

G_END_DECLS
