/*
 * ai-gui-session.h - One conversation, plus everything the window shows about it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * An #AiConversation knows how to drive a provider.  It does not know
 * what a person called this conversation, when they last touched it, or
 * that three more prompts are waiting behind the one in flight.  That is
 * what this adds, and it is deliberately the only place the GUI keeps
 * per-session state: the sidebar, the chat pane and the preferences
 * dialog all read the same object, so two of them cannot disagree about
 * which model is answering.
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include <ai-glib.h>

#include "ai-gui-util.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_SESSION (ai_gui_session_get_type())

G_DECLARE_FINAL_TYPE(AiGuiSession, ai_gui_session, AI_GUI, SESSION, GObject)

AiGuiSession *
ai_gui_session_new(
	const AiGuiOptions  *options,
	const gchar         *provider_name,
	const gchar         *model,
	GError             **error
);

AiGuiSession *
ai_gui_session_new_from_json(
	JsonObject          *object,
	const AiGuiOptions  *options,
	GError             **error
);

JsonNode *
ai_gui_session_to_json(AiGuiSession *self);

const gchar *
ai_gui_session_get_id(AiGuiSession *self);

const gchar *
ai_gui_session_get_title(AiGuiSession *self);

void
ai_gui_session_set_title(
	AiGuiSession *self,
	const gchar  *title
);

const gchar *
ai_gui_session_get_provider_name(AiGuiSession *self);

const gchar *
ai_gui_session_get_model(AiGuiSession *self);

const gchar *
ai_gui_session_get_working_directory(AiGuiSession *self);

gint64
ai_gui_session_get_updated_at(AiGuiSession *self);

gboolean
ai_gui_session_get_pinned(AiGuiSession *self);

void
ai_gui_session_set_pinned(
	AiGuiSession *self,
	gboolean      pinned
);

gboolean
ai_gui_session_get_busy(AiGuiSession *self);

const gchar *
ai_gui_session_get_activity(AiGuiSession *self);

guint
ai_gui_session_get_queued(AiGuiSession *self);

AiConversation *
ai_gui_session_get_conversation(AiGuiSession *self);

AiTranscript *
ai_gui_session_get_transcript(AiGuiSession *self);

AiCompletionContext *
ai_gui_session_get_completion(AiGuiSession *self);

AiCommandSet *
ai_gui_session_get_commands(AiGuiSession *self);

GObject *
ai_gui_session_get_provider(AiGuiSession *self);

gboolean
ai_gui_session_switch_provider(
	AiGuiSession  *self,
	const gchar   *provider_name,
	const gchar   *model,
	GError       **error
);

void
ai_gui_session_set_working_directory(
	AiGuiSession *self,
	const gchar  *path
);

gboolean
ai_gui_session_send(
	AiGuiSession  *self,
	const gchar   *text,
	GList         *images,
	GError       **error
);

void
ai_gui_session_cancel(AiGuiSession *self);

void
ai_gui_session_clear(AiGuiSession *self);

void
ai_gui_session_clear_queue(AiGuiSession *self);

gboolean
ai_gui_session_get_approve_all(AiGuiSession *self);

void
ai_gui_session_set_approve_all(
	AiGuiSession *self,
	gboolean      approve_all
);

AiGuiOptions *
ai_gui_session_get_options(AiGuiSession *self);

gchar *
ai_gui_session_export(
	AiGuiSession   *self,
	AiExportFormat  format
);

G_END_DECLS
