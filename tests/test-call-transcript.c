/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../bin/call/ai-call-transcript.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

/* Every line of the file, each parsed as a JSON object. */
static GPtrArray *
read_lines(const gchar *path)
{
	g_autofree gchar *contents = NULL;
	g_auto(GStrv) lines = NULL;
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_unref);
	guint i;
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_true(g_str_has_suffix(contents, "\n"));
	lines = g_strsplit(contents, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		if (*lines[i] == '\0')
			continue;
		g_assert_true(json_parser_load_from_data(parser, lines[i], -1, NULL));
		g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
		g_ptr_array_add(out, json_node_copy(json_parser_get_root(parser)));
	}
	return out;
}
static JsonObject *
line(GPtrArray *lines, guint i)
{
	return json_node_get_object(g_ptr_array_index(lines, i));
}
static void
written_as_the_call_goes(void)
{
	g_autofree gchar *base = g_dir_make_tmp("call-transcript-XXXXXX", NULL);
	g_autofree gchar *dir = g_build_filename(base, "nested", "calls", NULL);
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 9, 29, 16, 0, 0);
	g_autoptr(GDateTime) at = g_date_time_add_seconds(start, 3);
	g_autoptr(GDateTime) later = g_date_time_add_seconds(start, 9);
	g_autoptr(GDateTime) end = g_date_time_add_seconds(start, 42.5);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCallTranscript) transcript =
		ai_call_transcript_open(dir, "!room:example.org", start, &error);
	g_autoptr(GPtrArray) lines = NULL;
	GStatBuf st;
	const gchar *path;

	g_assert_no_error(error);
	g_assert_nonnull(transcript);
	path = ai_call_transcript_get_path(transcript);
	g_assert_true(g_str_has_prefix(path, dir));
	g_assert_true(g_str_has_suffix(path, ".jsonl"));
	/* The room id carries a server name and punctuation; it is not the file name. */
	g_assert_null(strstr(path, "example.org"));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(g_stat(dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);

	/* Header on disk before anyone speaks; end and duration unknown yet. */
	lines = read_lines(path);
	g_assert_cmpuint(lines->len, ==, 1);
	g_assert_cmpstr(json_object_get_string_member(line(lines, 0), "type"), ==, "call");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 0), "room"), ==,
					"!room:example.org");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 0), "start"), ==,
					"2026-09-29T16:00:00Z");
	g_assert_true(json_object_get_null_member(line(lines, 0), "end"));
	g_assert_true(json_object_get_null_member(line(lines, 0), "duration_ms"));
	g_clear_pointer(&lines, g_ptr_array_unref);

	/* Each utterance is on disk as soon as it is appended: a crash keeps it. */
	g_assert_true(ai_call_transcript_append(transcript, at, "Caller",
											"Hello \"Sam\",\nwhat's on — today? \360\237\230\210",
											&error));
	g_assert_no_error(error);
	lines = read_lines(path);
	g_assert_cmpuint(lines->len, ==, 2);
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "type"), ==,
					"utterance");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "at"), ==,
					"2026-09-29T16:00:03Z");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "speaker"), ==,
					"Caller");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "role"), ==, "caller");
	/* "complete" belongs to the assistant's lines; a caller's final is final. */
	g_assert_false(json_object_has_member(line(lines, 1), "complete"));
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "text"), ==,
					"Hello \"Sam\",\nwhat's on — today? \360\237\230\210");
	g_clear_pointer(&lines, g_ptr_array_unref);

	g_assert_true(ai_call_transcript_append(transcript, later, "Second", "Bye.", &error));
	g_assert_true(ai_call_transcript_append_spoken(transcript, later, "@assistant:example.org",
												   "Goodbye then. See you", FALSE, &error));
	g_assert_true(ai_call_transcript_close(transcript, end, &error));
	g_assert_no_error(error);
	/* Closing twice changes nothing and is not an error. */
	g_assert_true(ai_call_transcript_close(transcript, end, &error));
	lines = read_lines(path);
	g_assert_cmpuint(lines->len, ==, 4);
	g_assert_cmpstr(json_object_get_string_member(line(lines, 3), "role"), ==, "assistant");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 3), "speaker"), ==,
					"@assistant:example.org");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 3), "text"), ==,
					"Goodbye then. See you");
	g_assert_false(json_object_get_boolean_member(line(lines, 3), "complete"));
	g_assert_cmpstr(json_object_get_string_member(line(lines, 0), "end"), ==,
					"2026-09-29T16:00:42.500Z");
	g_assert_cmpint(json_object_get_int_member(line(lines, 0), "duration_ms"), ==, 42500);
	g_assert_cmpstr(json_object_get_string_member(line(lines, 2), "speaker"), ==,
					"Second");
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	/* Nothing may be appended to a closed transcript. */
	g_assert_false(ai_call_transcript_append(transcript, end, "Late", "no", &error));
	g_assert_nonnull(error);
}
static void
tool_lines(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("call-transcript-XXXXXX", NULL);
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 9, 29, 16, 0, 0);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCallTranscript) transcript =
		ai_call_transcript_open(dir, "!room:example.org", start, &error);
	g_autoptr(GPtrArray) lines = NULL;
	GString *big = g_string_new(NULL);
	JsonObject *arguments;
	const gchar *result;
	guint i;
	g_assert_nonnull(transcript);
	g_assert_true(ai_call_transcript_append_tool(transcript, start, "read",
												 "{\"path\":\"/tmp/notes.txt\",\"limit\":5}",
												 "line one\nline two", FALSE, &error));
	/* A result is capped, on a character boundary, and says so. */
	for (i = 0; i < 6000; i++)
		g_string_append(big, "\303\251");
	g_assert_true(ai_call_transcript_append_tool(transcript, start, "bash", "not json {",
												 big->str, TRUE, &error));
	g_assert_no_error(error);
	lines = read_lines(ai_call_transcript_get_path(transcript));
	g_assert_cmpuint(lines->len, ==, 3);
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "type"), ==, "tool");
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "name"), ==, "read");
	/* Arguments stay structured, not a string of JSON inside JSON. */
	arguments = json_object_get_object_member(line(lines, 1), "arguments");
	g_assert_nonnull(arguments);
	g_assert_cmpstr(json_object_get_string_member(arguments, "path"), ==, "/tmp/notes.txt");
	g_assert_cmpint(json_object_get_int_member(arguments, "limit"), ==, 5);
	g_assert_cmpstr(json_object_get_string_member(line(lines, 1), "result"), ==,
					"line one\nline two");
	g_assert_false(json_object_get_boolean_member(line(lines, 1), "is_error"));
	g_assert_false(json_object_has_member(line(lines, 1), "result_truncated"));
	/* Arguments that are not JSON are kept verbatim as a string. */
	g_assert_cmpstr(json_object_get_string_member(line(lines, 2), "arguments"), ==,
					"not json {");
	g_assert_true(json_object_get_boolean_member(line(lines, 2), "is_error"));
	g_assert_true(json_object_get_boolean_member(line(lines, 2), "result_truncated"));
	result = json_object_get_string_member(line(lines, 2), "result");
	g_assert_true(g_utf8_validate(result, -1, NULL));
	g_assert_cmpint(g_utf8_strlen(result, -1), ==, 4000);
	g_string_free(big, TRUE);
}
static void
one_file_per_call(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("call-transcript-XXXXXX", NULL);
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 9, 29, 16, 0, 0);
	g_autoptr(AiCallTranscript) first =
		ai_call_transcript_open(dir, "!room:example.org", start, NULL);
	g_autoptr(AiCallTranscript) second =
		ai_call_transcript_open(dir, "!room:example.org", start, NULL);
	g_assert_nonnull(first);
	g_assert_nonnull(second);
	g_assert_cmpstr(ai_call_transcript_get_path(first), !=,
					ai_call_transcript_get_path(second));
}
static void
default_dir(void)
{
	g_autofree gchar *dir = ai_call_transcript_default_dir();
	g_autofree gchar *expected =
		g_build_filename(g_get_user_state_dir(), "ai-glib", "calls", NULL);
	g_assert_cmpstr(dir, ==, expected);
}
static gboolean
timeout(gpointer data)
{
	*(gboolean *)data = TRUE;
	return G_SOURCE_REMOVE;
}
static void
hook(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("call-transcript-hook-XXXXXX", NULL);
	g_autofree gchar *script = g_build_filename(dir, "hook with space.sh", NULL);
	g_autofree gchar *out = g_build_filename(dir, "hook-ran", NULL);
	g_autofree gchar *command = NULL, *seen = NULL, *expected = NULL;
	g_autofree gchar *transcript = g_build_filename(dir, "a call's transcript.jsonl", NULL);
	g_autoptr(GError) error = NULL;
	gboolean expired = FALSE;
	guint source;
	g_assert_true(g_file_set_contents(
		script,
		"#!/bin/sh\nprintf '%s|%s|%s' \"$1\" \"$2\" \"$#\" > \"$(dirname \"$0\")/hook-ran.tmp\" && "
		"mv \"$(dirname \"$0\")/hook-ran.tmp\" \"$(dirname \"$0\")/hook-ran\"\n",
		-1, NULL));
	g_assert_cmpint(g_chmod(script, 0755), ==, 0);
	command = g_strdup_printf("'%s' --from-test", script);
	g_assert_true(ai_call_transcript_run_hook(command, transcript, &error));
	g_assert_no_error(error);
	source = g_timeout_add(5000, timeout, &expired);
	while (!expired && !g_file_test(out, G_FILE_TEST_EXISTS))
		g_main_context_iteration(NULL, TRUE);
	if (!expired)
		g_source_remove(source);
	/* The hook renames its output into place, so it exists only complete. */
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_true(g_file_get_contents(out, &seen, NULL, NULL));
	/* The configured argument first, the transcript path last, unsplit. */
	expected = g_strdup_printf("--from-test|%s|2", transcript);
	g_assert_cmpstr(seen, ==, expected);
	g_assert_false(ai_call_transcript_run_hook("'unbalanced", transcript, &error));
	g_assert_nonnull(error);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/voice/call-transcript/written-as-the-call-goes",
					written_as_the_call_goes);
	g_test_add_func("/voice/call-transcript/one-file-per-call", one_file_per_call);
	g_test_add_func("/voice/call-transcript/tool-lines", tool_lines);
	g_test_add_func("/voice/call-transcript/default-dir", default_dir);
	g_test_add_func("/voice/call-transcript/hook", hook);
	return g_test_run();
}
