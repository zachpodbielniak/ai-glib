/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-call-transcript.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

struct _AiCallTranscript {
	GObject parent;
	gchar *path, *room;
	GDateTime *start;
	gint fd;
	gboolean closed;
};
G_DEFINE_TYPE(AiCallTranscript, ai_call_transcript, G_TYPE_OBJECT)

static void
finalize(GObject *object)
{
	AiCallTranscript *self = AI_CALL_TRANSCRIPT(object);
	if (self->fd >= 0)
		g_close(self->fd, NULL);
	g_free(self->path);
	g_free(self->room);
	g_clear_pointer(&self->start, g_date_time_unref);
	G_OBJECT_CLASS(ai_call_transcript_parent_class)->finalize(object);
}
static void
ai_call_transcript_class_init(AiCallTranscriptClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = finalize;
}
static void
ai_call_transcript_init(AiCallTranscript *self)
{
	self->fd = -1;
}
/* UTC, milliseconds only when there are any: 2026-09-29T16:00:42.500Z. */
static gchar *
timestamp(GDateTime *when)
{
	g_autoptr(GDateTime) utc = g_date_time_to_utc(when);
	g_autofree gchar *seconds = g_date_time_format(utc, "%Y-%m-%dT%H:%M:%S");
	gint ms = g_date_time_get_microsecond(utc) / 1000;
	return ms != 0 ? g_strdup_printf("%s.%03dZ", seconds, ms)
				   : g_strdup_printf("%sZ", seconds);
}
static gchar *
header(AiCallTranscript *self, GDateTime *end)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *start = timestamp(self->start);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "call");
	json_builder_set_member_name(builder, "room");
	json_builder_add_string_value(builder, self->room);
	json_builder_set_member_name(builder, "start");
	json_builder_add_string_value(builder, start);
	json_builder_set_member_name(builder, "end");
	if (end != NULL) {
		g_autofree gchar *text = timestamp(end);
		json_builder_add_string_value(builder, text);
	} else
		json_builder_add_null_value(builder);
	json_builder_set_member_name(builder, "duration_ms");
	if (end != NULL)
		json_builder_add_int_value(builder,
								   MAX(0, g_date_time_difference(end, self->start) / 1000));
	else
		json_builder_add_null_value(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	return json_to_string(root, FALSE);
}
/* One line, then to the disk: what was said survives this process dying. */
static gboolean
write_line(AiCallTranscript *self, const gchar *json, GError **error)
{
	g_autofree gchar *line = g_strconcat(json, "\n", NULL);
	gsize length = strlen(line), done = 0;
	while (done < length) {
		gssize n = write(self->fd, line + done, length - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			gint saved = errno;
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
						"Cannot write transcript %s: %s", self->path, g_strerror(saved));
			return FALSE;
		}
		done += n;
	}
	fdatasync(self->fd);
	return TRUE;
}
gchar *
ai_call_transcript_default_dir(void)
{
	return g_build_filename(g_get_user_state_dir(), "ai-glib", "calls", NULL);
}
AiCallTranscript *
ai_call_transcript_open(const gchar *dir, const gchar *room, GDateTime *start,
						GError **error)
{
	g_autoptr(AiCallTranscript) self = NULL;
	g_autoptr(GDateTime) utc = NULL;
	g_autofree gchar *stamp = NULL, *digest = NULL, *line = NULL;
	guint attempt;
	g_return_val_if_fail(dir != NULL && *dir != '\0', NULL);
	g_return_val_if_fail(room != NULL && start != NULL, NULL);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		gint saved = errno;
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
					"Cannot create transcript directory %s: %s", dir, g_strerror(saved));
		return NULL;
	}
	self = g_object_new(AI_TYPE_CALL_TRANSCRIPT, NULL);
	self->room = g_strdup(room);
	self->start = g_date_time_ref(start);
	utc = g_date_time_to_utc(start);
	stamp = g_date_time_format(utc, "%Y%m%dT%H%M%SZ");
	/* The room id carries a server name and punctuation; name the file by a
	 * digest of it instead, plus a random suffix so two calls never collide. */
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, room, -1);
	for (attempt = 0; self->fd < 0 && attempt < 8; attempt++) {
		g_autofree gchar *name = g_strdup_printf("%s-%.12s-%06x.jsonl", stamp, digest,
												 g_random_int_range(0, 0x1000000));
		g_free(self->path);
		self->path = g_build_filename(dir, name, NULL);
		self->fd = g_open(self->path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC,
						  0600);
		if (self->fd < 0 && errno != EEXIST) {
			gint saved = errno;
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
						"Cannot create transcript %s: %s", self->path, g_strerror(saved));
			return NULL;
		}
	}
	if (self->fd < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
					"Cannot create a unique transcript in %s", dir);
		return NULL;
	}
	line = header(self, NULL);
	if (!write_line(self, line, error))
		return NULL;
	return g_steal_pointer(&self);
}
/* COMPLETE < 0 omits the member: a caller's final transcript is final. */
static gboolean
append_line(AiCallTranscript *self, GDateTime *at, const gchar *role,
			const gchar *speaker, const gchar *text, gint complete, GError **error)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *when = NULL, *line = NULL;
	if (self->closed) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Transcript %s is closed",
					self->path);
		return FALSE;
	}
	builder = json_builder_new();
	when = timestamp(at);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "utterance");
	json_builder_set_member_name(builder, "at");
	json_builder_add_string_value(builder, when);
	json_builder_set_member_name(builder, "role");
	json_builder_add_string_value(builder, role);
	json_builder_set_member_name(builder, "speaker");
	json_builder_add_string_value(builder, speaker);
	json_builder_set_member_name(builder, "text");
	json_builder_add_string_value(builder, text);
	if (complete >= 0) {
		json_builder_set_member_name(builder, "complete");
		json_builder_add_boolean_value(builder, complete != 0);
	}
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	line = json_to_string(root, FALSE);
	return write_line(self, line, error);
}
gboolean
ai_call_transcript_append(AiCallTranscript *self, GDateTime *at, const gchar *speaker,
						  const gchar *text, GError **error)
{
	g_return_val_if_fail(AI_IS_CALL_TRANSCRIPT(self), FALSE);
	g_return_val_if_fail(at != NULL && speaker != NULL && text != NULL, FALSE);
	return append_line(self, at, "caller", speaker, text, -1, error);
}
gboolean
ai_call_transcript_append_spoken(AiCallTranscript *self, GDateTime *at,
								 const gchar *speaker, const gchar *text, gboolean complete,
								 GError **error)
{
	g_return_val_if_fail(AI_IS_CALL_TRANSCRIPT(self), FALSE);
	g_return_val_if_fail(at != NULL && speaker != NULL && text != NULL, FALSE);
	return append_line(self, at, "assistant", speaker, text, complete ? 1 : 0, error);
}
gboolean
ai_call_transcript_close(AiCallTranscript *self, GDateTime *end, GError **error)
{
	g_autofree gchar *contents = NULL, *first = NULL, *rewritten = NULL;
	const gchar *rest;
	gsize length;
	g_return_val_if_fail(AI_IS_CALL_TRANSCRIPT(self), FALSE);
	g_return_val_if_fail(end != NULL, FALSE);
	if (self->closed)
		return TRUE;
	self->closed = TRUE;
	g_close(self->fd, NULL);
	self->fd = -1;
	if (!g_file_get_contents(self->path, &contents, &length, error))
		return FALSE;
	rest = memchr(contents, '\n', length);
	rest = rest != NULL ? rest + 1 : contents + length;
	first = header(self, end);
	rewritten = g_strconcat(first, "\n", rest, NULL);
	/* Atomic: a reader sees the old header or the new one, never half. */
	return g_file_set_contents_full(self->path, rewritten, -1,
									G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}
const gchar *
ai_call_transcript_get_path(AiCallTranscript *self)
{
	g_return_val_if_fail(AI_IS_CALL_TRANSCRIPT(self), NULL);
	return self->path;
}
static void
reaped(GPid pid, gint status, gpointer data)
{
	g_autoptr(GError) error = NULL;
	if (!g_spawn_check_wait_status(status, &error))
		g_log("ai-call", G_LOG_LEVEL_INFO, "Transcript hook failed: %s", error->message);
	g_spawn_close_pid(pid);
}
gboolean
ai_call_transcript_run_hook(const gchar *command, const gchar *path, GError **error)
{
	g_auto(GStrv) parsed = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	GPid pid;
	gint argc, i;
	g_return_val_if_fail(command != NULL && path != NULL, FALSE);
	if (!g_shell_parse_argv(command, &argc, &parsed, error))
		return FALSE;
	argv = g_ptr_array_new();
	for (i = 0; i < argc; i++)
		g_ptr_array_add(argv, parsed[i]);
	g_ptr_array_add(argv, (gpointer)path);
	g_ptr_array_add(argv, NULL);
	if (!g_spawn_async(NULL, (gchar **)argv->pdata, NULL,
					   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid,
					   error))
		return FALSE;
	g_child_watch_add(pid, reaped, NULL);
	return TRUE;
}
