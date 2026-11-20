/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <glib-object.h>
G_BEGIN_DECLS
#define AI_TYPE_VOICE_ROOM (ai_voice_room_get_type())
G_DECLARE_DERIVABLE_TYPE(AiVoiceRoom, ai_voice_room, AI, VOICE_ROOM, GObject)
/**
 * AiVoiceRoomClass:
 * @parent_class: the parent class
 * @format_line: one line of talk as the model sees it; "[Name]: text" by
 *   default
 * @format_turn: the text of a turn: who is present (when announced), what
 *   was heard since the last turn, then the line that made the turn
 *
 * What a voice session remembers of a room: what was said that was not a
 * turn, and who has been talking. Subclass to change how either reaches
 * the model.
 */
struct _AiVoiceRoomClass {
	GObjectClass parent_class;
	gchar *(*format_line)(AiVoiceRoom *self, const gchar *speaker, const gchar *text);
	gchar *(*format_turn)(AiVoiceRoom *self, const gchar *const *present,
						  const gchar *const *heard, const gchar *line);
	gpointer _reserved[8];
};
AiVoiceRoom *
ai_voice_room_new(void);
void
ai_voice_room_hear(AiVoiceRoom *self, const gchar *speaker, const gchar *text);
void
ai_voice_room_note_speaker(AiVoiceRoom *self, const gchar *speaker);
void
ai_voice_room_rename_speaker(AiVoiceRoom *self, const gchar *from, const gchar *to);
gchar **
ai_voice_room_get_lines(AiVoiceRoom *self);
guint
ai_voice_room_get_n_lines(AiVoiceRoom *self);
gchar **
ai_voice_room_get_present(AiVoiceRoom *self);
guint64
ai_voice_room_get_revision(AiVoiceRoom *self);
gchar *
ai_voice_room_take_turn(AiVoiceRoom *self, const gchar *speaker, const gchar *text);
void
ai_voice_room_clear(AiVoiceRoom *self);
G_END_DECLS
