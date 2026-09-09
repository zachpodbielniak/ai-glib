/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Read native Grok and Codex history for display only. Never replay old
 * prompts/tools into the provider: its resumed session already owns that
 * context.
 */
#pragma once

#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "core/ai-json-util.h"

/* Read bounded local files; malformed logs must not leave a half-restored UI. */
static JsonParser *
ai_tui_history_json(const gchar *path)
{
	GStatBuf stat_buf;
	JsonParser *parser;

	if (g_stat(path, &stat_buf) != 0 || stat_buf.st_size > 64 * 1024 * 1024)
		return NULL;
	parser = json_parser_new();
	if (!json_parser_load_from_file(parser, path, NULL))
		g_clear_object(&parser);
	return parser;
}

/* The child's explicit environment takes precedence over the parent. */
static const gchar *
ai_tui_history_env(AiCliClient *client, const gchar *name)
{
	GHashTable *environment = ai_cli_client_get_environment(client);
	const gchar *value = environment != NULL ? g_hash_table_lookup(environment, name) : NULL;

	return value != NULL ? value : g_getenv(name);
}

/* /home and /var/home can name the same project on an immutable host. */
static gboolean
ai_tui_history_same_directory(const gchar *left, const gchar *right)
{
	g_autofree gchar *a = left != NULL ? realpath(left, NULL) : NULL;
	g_autofree gchar *b = right != NULL ? realpath(right, NULL) : NULL;

	return a != NULL && b != NULL && g_str_equal(a, b);
}

/* Select only this project's sessions, including Grok's hashed cwd buckets.
 * A supplied session ID wins; otherwise order by the native activity time. */
static gchar *
ai_tui_history_find(AiCliClient *client, gchar **session_id)
{
	const gchar *base = ai_tui_history_env(client, "GROK_HOME");
	const gchar *home = ai_tui_history_env(client, "HOME");
	const gchar *wanted = ai_cli_client_get_session_id(client);
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *cwd = g_get_current_dir();
	g_autofree gchar *root = NULL;
	g_autoptr(GDir) groups = NULL;
	g_autoptr(GDateTime) latest = NULL;
	gchar *selected = NULL;
	const gchar *group;

	if (base == NULL || *base == '\0')
		base = fallback = g_build_filename(home != NULL ? home : g_get_home_dir(), ".grok", NULL);
	root = g_build_filename(base, "sessions", NULL);
	groups = g_dir_open(root, 0, NULL);
	if (groups == NULL) return NULL;
	if (ai_cli_client_get_working_directory(client) != NULL)
	{
		g_free(cwd);
		cwd = g_canonicalize_filename(ai_cli_client_get_working_directory(client), NULL);
	}
	while ((group = g_dir_read_name(groups)) != NULL)
	{
		g_autofree gchar *decoded = g_uri_unescape_string(group, NULL);
		g_autofree gchar *bucket = g_build_filename(root, group, NULL);
		g_autofree gchar *marker = g_build_filename(bucket, ".cwd", NULL);
		g_autofree gchar *original = NULL;
		g_autoptr(GDir) sessions = NULL;
		const gchar *name;

		if (!ai_tui_history_same_directory(decoded, cwd))
		{
			if (!g_file_get_contents(marker, &original, NULL, NULL) ||
			    !ai_tui_history_same_directory(g_strchomp(original), cwd)) continue;
		}
		sessions = g_dir_open(bucket, 0, NULL);
		if (sessions == NULL) continue;
		while ((name = g_dir_read_name(sessions)) != NULL)
		{
			g_autofree gchar *directory = g_build_filename(bucket, name, NULL);
			g_autofree gchar *summary = g_build_filename(directory, "summary.json", NULL);
			g_autoptr(JsonParser) parser = NULL;
			g_autoptr(GDateTime) timestamp = NULL;
			JsonNode *node;
			JsonObject *object;
			JsonObject *info;
			const gchar *updated;

			if (wanted != NULL && *wanted != '\0' && !g_str_equal(wanted, name)) continue;
			parser = ai_tui_history_json(summary);
			if (parser == NULL) continue;
			node = json_parser_get_root(parser);
			if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
			object = json_node_get_object(node);
			info = ai_json_get_object(object, "info");
			if (!ai_tui_history_same_directory(ai_json_get_string(info, "cwd", NULL), cwd) ||
			    g_strcmp0(ai_json_get_string(info, "id", NULL), name) != 0 ||
			    g_strcmp0(ai_json_get_string(object, "session_kind", ""), "subagent") == 0)
				continue;
			updated = ai_json_get_string(object, "last_active_at",
				ai_json_get_string(object, "updated_at", NULL));
			if (updated == NULL) continue;
			timestamp = g_date_time_new_from_iso8601(updated, NULL);
			if (timestamp == NULL || (latest != NULL && g_date_time_compare(timestamp, latest) <= 0)) continue;
			g_clear_pointer(&latest, g_date_time_unref);
			latest = g_steal_pointer(&timestamp);
			g_free(selected);
			selected = g_build_filename(directory, "updates.jsonl", NULL);
			g_free(*session_id);
			*session_id = g_strdup(name);
		}
	}
	return selected;
}

/*
 * Grok's ACP history titles are prose ("Read `/path/file.c`"). The variant
 * is the actual tool. Map those onto the streaming-messages-json names so a
 * restored session and a live turn share the same summary wording.
 */
static const gchar *
ai_tui_history_tool_name(JsonObject *update)
{
	JsonObject *raw = ai_json_get_object(update, "rawInput");
	const gchar *variant = ai_json_get_string(raw, "variant", NULL);
	const gchar *title = ai_json_get_string(update, "title", NULL);
	const gchar *kind = ai_json_get_string(update, "kind", NULL);

	if (variant != NULL && variant[0] != '\0')
	{
		if (g_str_equal(variant, "ReadFile")) return "read_file";
		if (g_str_equal(variant, "SearchReplace")) return "search_replace";
		if (g_str_equal(variant, "ListDir")) return "list_dir";
		if (g_str_equal(variant, "Bash")) return "run_terminal_command";
		if (g_str_equal(variant, "Write")) return "write";
		if (g_str_equal(variant, "Grep")) return "grep";
		if (g_str_equal(variant, "TodoWrite")) return "todo_write";
		if (g_str_equal(variant, "TaskOutput")) return "get_command_or_subagent_output";
		if (g_str_equal(variant, "SearchTool")) return "search_tool";
		if (g_str_equal(variant, "WebSearch")) return "web_search";
		if (g_str_equal(variant, "WebFetch")) return "web_fetch";
		if (g_str_equal(variant, "UseTool"))
		{
			const gchar *tool_name = ai_json_get_string(raw, "tool_name", NULL);
			if (tool_name != NULL && tool_name[0] != '\0') return tool_name;
		}
		return variant;
	}
	/* A title with spaces or backticks is a sentence, not a tool id. */
	if (title != NULL && title[0] != '\0' &&
	    strchr(title, ' ') == NULL && strchr(title, '`') == NULL)
		return title;
	if (kind != NULL && kind[0] != '\0')
		return kind;
	return "tool";
}

/* Fold text fragments without inventing new turns at each streamed chunk. */
static void
ai_tui_history_text(AiTranscript *transcript, const gchar *kind, const gchar *text,
                   gchar **previous, GString *user_text, AiViewBlock **open)
{
	if (g_strcmp0(*previous, kind) != 0)
	{
		if (user_text->len > 0)
		{
			g_autoptr(AiViewBlock) turn = ai_view_turn_block_new(user_text->str);
			ai_transcript_append(transcript, turn);
			g_string_truncate(user_text, 0);
		}
		*open = NULL;
		g_free(*previous);
		*previous = g_strdup(kind);
	}
	if (g_str_equal(kind, "user_message_chunk"))
		g_string_append(user_text, text);
	else if (g_str_equal(kind, "agent_message_chunk") || g_str_equal(kind, "agent_thought_chunk"))
	{
		gboolean thinking = g_str_equal(kind, "agent_thought_chunk");
		if (*open == NULL)
		{
			g_autoptr(AiViewBlock) block = thinking ? ai_view_thinking_block_new() : ai_view_text_block_new();
			*open = block;
			ai_view_block_set_complete(block, TRUE);
			ai_transcript_append(transcript, block);
		}
		if (thinking) ai_view_thinking_block_append(AI_VIEW_THINKING_BLOCK(*open), text);
		else ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(*open), text);
	}
}

/* Restore ACP updates into a temporary transcript before publishing any blocks.
 * Unknown update types are metadata; invalid JSON is an actionable error. */
static AiTranscript *
ai_tui_history_read(const gchar *path, const gchar *session_id, GError **error)
{
	g_autofree gchar *contents = NULL;
	g_autofree gchar *previous = NULL;
	g_autoptr(GString) user_text = g_string_new(NULL);
	g_autoptr(AiTranscript) transcript = ai_transcript_new();
	g_autoptr(GHashTable) calls = g_hash_table_new(g_str_hash, g_str_equal);
	AiViewBlock *open = NULL;
	AiViewToolBlock *tool_block = NULL;
	GStatBuf stat_buf;
	gchar *line;

	if (g_stat(path, &stat_buf) == 0 && stat_buf.st_size > 64 * 1024 * 1024)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Native history exceeds the 64 MiB display limit");
		return NULL;
	}
	if (!g_file_get_contents(path, &contents, NULL, error)) return NULL;
	line = contents;
	while (*line != '\0')
	{
		gchar *end = strchr(line, '\n');
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root;
		JsonObject *params;
		JsonObject *update;
		const gchar *kind;
		const gchar *text;

		if (end != NULL) *end = '\0';
		if (*line != '\0')
		{
			if (!json_parser_load_from_data(parser, line, -1, error)) return NULL;
			root = json_parser_get_root(parser);
			params = JSON_NODE_HOLDS_OBJECT(root) ? ai_json_get_object(json_node_get_object(root), "params") : NULL;
			update = ai_json_get_object(params, "update");
			kind = ai_json_get_string(update, "sessionUpdate", "");
			if (g_strcmp0(ai_json_get_string(params, "sessionId", NULL), session_id) == 0)
			{
				text = ai_json_get_string(ai_json_get_object(update, "content"), "text", "");
				if (g_str_equal(kind, "user_message_chunk") ||
				    g_str_equal(kind, "agent_message_chunk") || g_str_equal(kind, "agent_thought_chunk"))
				{
					ai_tui_history_text(transcript, kind, text, &previous, user_text, &open);
					tool_block = NULL;
				}
				else if (g_str_equal(kind, "tool_call") || g_str_equal(kind, "tool_call_update"))
				{
					const gchar *id = ai_json_get_string(update, "toolCallId", NULL);
					AiToolCall *call = id != NULL ? g_hash_table_lookup(calls, id) : NULL;
					const gchar *status = ai_json_get_string(update, "status", "");

					ai_tui_history_text(transcript, "tool", "", &previous, user_text, &open);
					if (call == NULL && id != NULL)
					{
						g_autoptr(AiToolUse) use = ai_tool_use_new(id,
							ai_tui_history_tool_name(update), ai_json_get_node(update, "rawInput"));
						if (tool_block == NULL)
						{
							g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
							tool_block = AI_VIEW_TOOL_BLOCK(block);
							ai_view_block_set_complete(block, TRUE);
							ai_transcript_append(transcript, block);
						}
						call = ai_view_tool_block_add_call(tool_block, use);
						g_hash_table_insert(calls, (gpointer)ai_tool_call_get_id(call), call);
					}
					if (call != NULL && (g_str_equal(status, "completed") || g_str_equal(status, "failed")))
					{
						g_autoptr(GString) output = g_string_new(NULL);
						JsonArray *parts = ai_json_get_array(update, "content");
						guint i;
						for (i = 0; parts != NULL && i < json_array_get_length(parts); i++)
						{
							JsonNode *part = json_array_get_element(parts, i);
							if (JSON_NODE_HOLDS_OBJECT(part))
								g_string_append(output, ai_json_get_string(ai_json_get_object(json_node_get_object(part), "content"), "text", ""));
						}
						{
							g_autoptr(AiToolResult) result = ai_tool_result_new(id, output->str, g_str_equal(status, "failed"));
							ai_tool_call_finish(call, result);
						}
					}
				}
			}
		}
		if (end == NULL) break;
		line = end + 1;
	}
	ai_tui_history_text(transcript, "end", "", &previous, user_text, &open);
	return g_steal_pointer(&transcript);
}

/* First-line only: rollout files carry huge base_instructions in session_meta. */
static gchar *
ai_tui_history_first_line(const gchar *path)
{
	g_autoptr(GIOChannel) channel = g_io_channel_new_file(path, "r", NULL);
	gchar *line = NULL;
	gsize n = 0;

	if (channel == NULL) return NULL;
	g_io_channel_set_encoding(channel, NULL, NULL);
	if (g_io_channel_read_line(channel, &line, &n, NULL, NULL) != G_IO_STATUS_NORMAL)
		return NULL;
	if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
	return line;
}

/* Join text / Text parts, plus Codex reasoning summaries when those exist. */
static gchar *
ai_tui_history_item_text(JsonObject *item)
{
	JsonArray *content = ai_json_get_array(item, "content");
	JsonArray *summary = ai_json_get_array(item, "summary_text");
	const gchar *text = ai_json_get_string(item, "text", NULL);
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;

	if (text != NULL && text[0] != '\0') g_string_append(out, text);
	for (i = 0; content != NULL && i < json_array_get_length(content); i++)
	{
		JsonNode *node = json_array_get_element(content, i);
		JsonObject *part = ai_json_array_get_object(content, i);
		const gchar *chunk = ai_json_get_string(part, "text", NULL);

		if (chunk == NULL) chunk = ai_json_get_string(part, "thinking", NULL);
		if (chunk == NULL && node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
		    json_node_get_value_type(node) == G_TYPE_STRING)
			chunk = json_node_get_string(node);
		if (chunk != NULL) g_string_append(out, chunk);
	}
	for (i = 0; summary != NULL && i < json_array_get_length(summary); i++)
	{
		JsonNode *node = json_array_get_element(summary, i);
		if (node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
		    json_node_get_value_type(node) == G_TYPE_STRING)
			g_string_append(out, json_node_get_string(node));
		else if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
			g_string_append(out, ai_json_get_string(json_node_get_object(node), "text", ""));
	}
	return out->len > 0 ? g_string_free(g_steal_pointer(&out), FALSE) : NULL;
}

/* `command` is a string on the exec JSONL path and an argv array in rollouts. */
static gchar *
ai_tui_codex_command(JsonObject *item)
{
	const gchar *command = ai_json_get_string(item, "command", NULL);
	JsonArray *argv = ai_json_get_array(item, "command");
	guint i;

	if (command != NULL && command[0] != '\0') return g_strdup(command);
	if (argv == NULL) return NULL;
	for (i = json_array_get_length(argv); i > 0; i--)
	{
		JsonNode *node = json_array_get_element(argv, i - 1);
		const gchar *word;

		if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
		    json_node_get_value_type(node) != G_TYPE_STRING) continue;
		word = json_node_get_string(node);
		if (word != NULL && word[0] != '\0') return g_strdup(word);
	}
	return NULL;
}

/* Project a rollout FileChange onto the live file_change input the summariser knows. */
static JsonNode *
ai_tui_codex_file_input(JsonObject *item)
{
	JsonObject *input = json_object_new();
	JsonNode *changes = ai_json_get_node(item, "changes");
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonArray *out = json_array_new();
	const gchar *first_path = NULL;

	if (changes != NULL && JSON_NODE_HOLDS_ARRAY(changes))
	{
		json_array_unref(out);
		json_object_set_array_member(input, "changes", json_array_ref(json_node_get_array(changes)));
		if (json_array_get_length(json_node_get_array(changes)) == 1)
			json_object_set_string_member(input, "path",
				ai_json_get_string(ai_json_array_get_object(json_node_get_array(changes), 0), "path", NULL));
		out = NULL;
	}
	else if (changes != NULL && JSON_NODE_HOLDS_OBJECT(changes))
	{
		GList *members = json_object_get_members(json_node_get_object(changes));
		GList *iter;

		for (iter = members; iter != NULL; iter = iter->next)
		{
			const gchar *path = iter->data;
			JsonObject *entry = json_object_new();
			JsonObject *value = ai_json_get_object(json_node_get_object(changes), path);

			json_object_set_string_member(entry, "path", path);
			json_object_set_string_member(entry, "kind", ai_json_get_string(value, "type", "update"));
			json_array_add_object_element(out, entry);
			if (first_path == NULL) first_path = path;
		}
		g_list_free(members);
		if (first_path != NULL && json_array_get_length(out) == 1)
			json_object_set_string_member(input, "path", first_path);
		json_object_set_array_member(input, "changes", out);
		out = NULL;
	}
	if (out != NULL) json_array_unref(out);
	json_node_take_object(node, input);
	return node;
}

static JsonNode *
ai_tui_codex_command_input(const gchar *command)
{
	JsonObject *input = json_object_new();
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);

	json_object_set_string_member(input, "command", command);
	json_node_take_object(node, input);
	return node;
}

static void
ai_tui_codex_history_scan(const gchar *directory, const gchar *cwd, const gchar *wanted,
                          gchar **selected, gchar **session_id, gint64 *best_mtime)
{
	g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
	const gchar *name;

	if (dir == NULL) return;
	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = g_build_filename(directory, name, NULL);
		g_autofree gchar *line = NULL;
		g_autoptr(JsonParser) parser = NULL;
		GStatBuf stat_buf;
		JsonNode *root;
		JsonObject *payload;
		const gchar *id;
		gint64 mtime;

		if (g_file_test(path, G_FILE_TEST_IS_SYMLINK)) continue;
		if (g_file_test(path, G_FILE_TEST_IS_DIR))
		{
			ai_tui_codex_history_scan(path, cwd, wanted, selected, session_id, best_mtime);
			continue;
		}
		if (!g_str_has_prefix(name, "rollout-") || !g_str_has_suffix(name, ".jsonl")) continue;
		if (g_stat(path, &stat_buf) != 0 || stat_buf.st_size > 64 * 1024 * 1024) continue;
		line = ai_tui_history_first_line(path);
		if (line == NULL || *line == '\0') continue;
		parser = json_parser_new();
		if (!json_parser_load_from_data(parser, line, -1, NULL)) continue;
		root = json_parser_get_root(parser);
		if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) continue;
		if (g_strcmp0(ai_json_get_string(json_node_get_object(root), "type", ""), "session_meta") != 0) continue;
		payload = ai_json_get_object(json_node_get_object(root), "payload");
		id = ai_json_get_string(payload, "session_id", ai_json_get_string(payload, "id", NULL));
		if (id == NULL || *id == '\0') continue;
		if (wanted != NULL && *wanted != '\0' && !g_str_equal(wanted, id)) continue;
		if (!ai_tui_history_same_directory(ai_json_get_string(payload, "cwd", NULL), cwd)) continue;
		mtime = (gint64)stat_buf.st_mtime;
		if (*selected != NULL && mtime < *best_mtime) continue;
		if (*selected != NULL && mtime == *best_mtime && g_strcmp0(id, *session_id) <= 0) continue;
		*best_mtime = mtime;
		g_free(*selected);
		*selected = g_strdup(path);
		g_free(*session_id);
		*session_id = g_strdup(id);
	}
}

/* Select only this project's Codex rollouts. A supplied session ID wins. */
static gchar *
ai_tui_codex_history_find(AiCliClient *client, gchar **session_id)
{
	const gchar *base = ai_tui_history_env(client, "CODEX_HOME");
	const gchar *home = ai_tui_history_env(client, "HOME");
	const gchar *wanted = ai_cli_client_get_session_id(client);
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *cwd = g_get_current_dir();
	g_autofree gchar *root = NULL;
	gchar *selected = NULL;
	gint64 best_mtime = G_MININT64;

	if (base == NULL || *base == '\0')
		base = fallback = g_build_filename(home != NULL ? home : g_get_home_dir(), ".codex", NULL);
	if (ai_cli_client_get_working_directory(client) != NULL)
	{
		g_free(cwd);
		cwd = g_canonicalize_filename(ai_cli_client_get_working_directory(client), NULL);
	}
	root = g_build_filename(base, "sessions", NULL);
	ai_tui_codex_history_scan(root, cwd, wanted, &selected, session_id, &best_mtime);
	return selected;
}

/* Fold completed rollout items. Unknown item types are metadata, not errors. */
static AiTranscript *
ai_tui_codex_history_read(const gchar *path, GError **error)
{
	g_autofree gchar *contents = NULL;
	g_autofree gchar *previous = NULL;
	g_autoptr(GString) user_text = g_string_new(NULL);
	g_autoptr(AiTranscript) transcript = ai_transcript_new();
	g_autoptr(GHashTable) calls = g_hash_table_new(g_str_hash, g_str_equal);
	AiViewBlock *open = NULL;
	AiViewToolBlock *tool_block = NULL;
	GStatBuf stat_buf;
	gchar *line;

	if (g_stat(path, &stat_buf) == 0 && stat_buf.st_size > 64 * 1024 * 1024)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Native history exceeds the 64 MiB display limit");
		return NULL;
	}
	if (!g_file_get_contents(path, &contents, NULL, error)) return NULL;
	line = contents;
	while (*line != '\0')
	{
		gchar *end = strchr(line, '\n');
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root;
		JsonObject *object;
		JsonObject *payload;
		JsonObject *item;
		const gchar *kind;

		if (end != NULL) *end = '\0';
		if (*line != '\0')
		{
			if (!json_parser_load_from_data(parser, line, -1, error)) return NULL;
			root = json_parser_get_root(parser);
			object = root != NULL && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
			payload = ai_json_get_object(object, "payload");
			if (g_strcmp0(ai_json_get_string(object, "type", ""), "event_msg") == 0 &&
			    g_str_equal(ai_json_get_string(payload, "type", ""), "item_completed"))
			{
				item = ai_json_get_object(payload, "item");
				kind = ai_json_get_string(item, "type", "");
				if (g_str_equal(kind, "UserMessage") || g_str_equal(kind, "AgentMessage") ||
				    g_str_equal(kind, "Reasoning"))
				{
					g_autofree gchar *text = ai_tui_history_item_text(item);
					const gchar *chunk = g_str_equal(kind, "UserMessage") ? "user_message_chunk" :
						g_str_equal(kind, "Reasoning") ? "agent_thought_chunk" : "agent_message_chunk";
					if (text != NULL)
					{
						ai_tui_history_text(transcript, chunk, text, &previous, user_text, &open);
						tool_block = NULL;
					}
				}
				else if (g_str_equal(kind, "CommandExecution") || g_str_equal(kind, "FileChange"))
				{
					const gchar *id = ai_json_get_string(item, "id", NULL);
					AiToolCall *call = id != NULL ? g_hash_table_lookup(calls, id) : NULL;
					const gchar *status = ai_json_get_string(item, "status", "");
					gboolean failed = g_str_equal(status, "failed") ||
						ai_json_get_int(item, "exit_code", 0) != 0;

					ai_tui_history_text(transcript, "tool", "", &previous, user_text, &open);
					if (call == NULL && id != NULL)
					{
						g_autoptr(JsonNode) input = NULL;
						g_autoptr(AiToolUse) use = NULL;
						g_autofree gchar *command = NULL;

						if (g_str_equal(kind, "FileChange"))
							input = ai_tui_codex_file_input(item);
						else
						{
							command = ai_tui_codex_command(item);
							input = ai_tui_codex_command_input(command != NULL ? command : "");
						}
						use = ai_tool_use_new(id, g_str_equal(kind, "FileChange") ? "file_change" : "command_execution", input);
						if (tool_block == NULL)
						{
							g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
							tool_block = AI_VIEW_TOOL_BLOCK(block);
							ai_view_block_set_complete(block, TRUE);
							ai_transcript_append(transcript, block);
						}
						call = ai_view_tool_block_add_call(tool_block, use);
						g_hash_table_insert(calls, (gpointer)ai_tool_call_get_id(call), call);
					}
					if (call != NULL)
					{
						const gchar *output = ai_json_get_string(item, "aggregated_output",
							ai_json_get_string(item, "stdout", ""));
						g_autoptr(AiToolResult) result = ai_tool_result_new(id, output, failed);
						ai_tool_call_finish(call, result);
					}
				}
			}
		}
		if (end == NULL) break;
		line = end + 1;
	}
	ai_tui_history_text(transcript, "end", "", &previous, user_text, &open);
	return g_steal_pointer(&transcript);
}

static gchar *
ai_tui_history_cwd(AiCliClient *client)
{
	g_autofree gchar *cwd = g_get_current_dir();

	if (ai_cli_client_get_working_directory(client) != NULL)
	{
		g_free(cwd);
		cwd = g_canonicalize_filename(ai_cli_client_get_working_directory(client), NULL);
	}
	return g_steal_pointer(&cwd);
}

/* Claude keeps the leading slash as a dash; Cursor strips leading slashes. */
static gchar *
ai_tui_history_dash_path(const gchar *path, gboolean keep_leading)
{
	GString *out;
	const gchar *p;

	if (path == NULL) return NULL;
	p = path;
	if (!keep_leading)
		while (*p == '/') p++;
	out = g_string_new(NULL);
	for (; *p != '\0'; p++)
		g_string_append_c(out, *p == '/' ? '-' : *p);
	return g_string_free(out, FALSE);
}

static gchar *
ai_tui_history_between(const gchar *text, const gchar *open_tag, const gchar *close_tag)
{
	const gchar *start;
	const gchar *end;

	if (text == NULL) return NULL;
	start = strstr(text, open_tag);
	if (start == NULL) return g_strdup(text);
	start += strlen(open_tag);
	end = strstr(start, close_tag);
	if (end == NULL) return g_strstrip(g_strdup(start));
	return g_strstrip(g_strndup(start, (gsize)(end - start)));
}

static void
ai_tui_history_add_tool(AiTranscript *transcript, gchar **previous, GString *user_text,
                        AiViewBlock **open, AiViewToolBlock **tool_block, GHashTable *calls,
                        const gchar *id, const gchar *name, JsonNode *input,
                        const gchar *output, gboolean failed)
{
	AiToolCall *call = id != NULL ? g_hash_table_lookup(calls, id) : NULL;

	ai_tui_history_text(transcript, "tool", "", previous, user_text, open);
	if (call == NULL && id != NULL && name != NULL)
	{
		g_autoptr(AiToolUse) use = ai_tool_use_new(id, name, input);
		if (*tool_block == NULL)
		{
			g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
			*tool_block = AI_VIEW_TOOL_BLOCK(block);
			ai_view_block_set_complete(block, TRUE);
			ai_transcript_append(transcript, block);
		}
		call = ai_view_tool_block_add_call(*tool_block, use);
		g_hash_table_insert(calls, (gpointer)ai_tool_call_get_id(call), call);
	}
	if (call != NULL)
	{
		g_autoptr(AiToolResult) result = ai_tool_result_new(id, output != NULL ? output : "", failed);
		ai_tool_call_finish(call, result);
	}
}

static gchar *
ai_tui_history_newest_jsonl(const gchar *directory, const gchar *wanted, const gchar *suffix)
{
	g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
	const gchar *name;
	g_autofree gchar *selected = NULL;
	gint64 best = G_MININT64;

	if (dir == NULL) return NULL;
	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = g_build_filename(directory, name, NULL);
		GStatBuf stat_buf;
		g_autofree gchar *id = NULL;

		if (!g_str_has_suffix(name, suffix != NULL ? suffix : ".jsonl")) continue;
		if (g_file_test(path, G_FILE_TEST_IS_DIR) || g_file_test(path, G_FILE_TEST_IS_SYMLINK)) continue;
		id = g_strndup(name, strlen(name) - strlen(suffix != NULL ? suffix : ".jsonl"));
		if (wanted != NULL && *wanted != '\0' && !g_str_equal(wanted, id)) continue;
		if (g_stat(path, &stat_buf) != 0 || stat_buf.st_size > 64 * 1024 * 1024) continue;
		if (selected != NULL && (gint64)stat_buf.st_mtime < best) continue;
		best = (gint64)stat_buf.st_mtime;
		g_free(selected);
		selected = g_steal_pointer(&path);
	}
	return g_steal_pointer(&selected);
}

static gchar *
ai_tui_claude_history_find(GObject *provider, AiCliClient *client, gchar **session_id)
{
	const gchar *home = ai_tui_history_env(client, "HOME");
	const gchar *config = ai_tui_history_env(client, "CLAUDE_CONFIG_DIR");
	g_autofree gchar *cwd = ai_tui_history_cwd(client);
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *encoded = ai_tui_history_dash_path(cwd, TRUE);
	g_autofree gchar *resolved = cwd != NULL ? realpath(cwd, NULL) : NULL;
	g_autofree gchar *encoded_real = ai_tui_history_dash_path(resolved, TRUE);
	g_autofree gchar *project_dir = NULL;
	const gchar *wanted = ai_cli_client_get_session_id(client);
	g_autofree gchar *selected = NULL;
	gchar *roots[2];
	guint i;

	if (AI_IS_CLAUDE_TMUX_CLIENT(provider))
		g_object_get(provider, "claude-project-dir", &project_dir, NULL);
	if (project_dir == NULL || *project_dir == '\0')
	{
		g_free(project_dir);
		project_dir = g_build_filename(config != NULL && *config != '\0' ? config :
			(fallback = g_build_filename(home != NULL ? home : g_get_home_dir(), ".claude", NULL)),
			"projects", NULL);
	}
	roots[0] = encoded;
	roots[1] = encoded_real;
	for (i = 0; i < G_N_ELEMENTS(roots); i++)
	{
		g_autofree gchar *directory = NULL;
		if (roots[i] == NULL || *roots[i] == '\0') continue;
		if (i > 0 && g_strcmp0(roots[0], roots[i]) == 0) continue;
		directory = g_build_filename(project_dir, roots[i], NULL);
		g_free(selected);
		selected = ai_tui_history_newest_jsonl(directory, wanted, ".jsonl");
		if (selected != NULL) break;
	}
	if (selected != NULL)
	{
		g_autofree gchar *base = g_path_get_basename(selected);
		g_free(*session_id);
		*session_id = g_strndup(base, strlen(base) - strlen(".jsonl"));
	}
	return g_steal_pointer(&selected);
}

static AiTranscript *
ai_tui_claude_history_read(const gchar *path, GError **error)
{
	g_autofree gchar *contents = NULL;
	g_autofree gchar *previous = NULL;
	g_autoptr(GString) user_text = g_string_new(NULL);
	g_autoptr(AiTranscript) transcript = ai_transcript_new();
	g_autoptr(GHashTable) calls = g_hash_table_new(g_str_hash, g_str_equal);
	AiViewBlock *open = NULL;
	AiViewToolBlock *tool_block = NULL;
	GStatBuf stat_buf;
	gchar *line;

	if (g_stat(path, &stat_buf) == 0 && stat_buf.st_size > 64 * 1024 * 1024)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Native history exceeds the 64 MiB display limit");
		return NULL;
	}
	if (!g_file_get_contents(path, &contents, NULL, error)) return NULL;
	line = contents;
	while (*line != '\0')
	{
		gchar *end = strchr(line, '\n');
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root;
		JsonObject *object;
		JsonObject *message;
		JsonArray *content;
		const gchar *type;
		guint i;

		if (end != NULL) *end = '\0';
		if (*line != '\0')
		{
			if (!json_parser_load_from_data(parser, line, -1, NULL))
			{
				if (end == NULL) break;
				line = end + 1;
				continue;
			}
			root = json_parser_get_root(parser);
			object = root != NULL && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
			type = ai_json_get_string(object, "type", "");
			message = ai_json_get_object(object, "message");
			content = ai_json_get_array(message, "content");
			if (g_str_equal(type, "user"))
			{
				gboolean saw_text = FALSE;
				g_autofree gchar *text = NULL;

				if (content == NULL)
					text = g_strdup(ai_json_get_string(message, "content", NULL));
				for (i = 0; content != NULL && i < json_array_get_length(content); i++)
				{
					JsonObject *part = ai_json_array_get_object(content, i);
					const gchar *kind = ai_json_get_string(part, "type", "");
					if (g_str_equal(kind, "tool_result"))
					{
						const gchar *id = ai_json_get_string(part, "tool_use_id", NULL);
						const gchar *output = ai_json_get_string(part, "content", NULL);
						AiToolCall *call = id != NULL ? g_hash_table_lookup(calls, id) : NULL;
						if (call != NULL)
						{
							g_autoptr(AiToolResult) result = ai_tool_result_new(id,
								output != NULL ? output : "", ai_json_get_boolean(part, "is_error", FALSE));
							ai_tool_call_finish(call, result);
						}
					}
					else if (g_str_equal(kind, "text") || kind[0] == '\0')
						saw_text = TRUE;
				}
				if (text == NULL && saw_text) text = ai_tui_history_item_text(message);
				if (text != NULL && *text != '\0')
				{
					ai_tui_history_text(transcript, "user_message_chunk", text, &previous, user_text, &open);
					tool_block = NULL;
				}
			}
			else if (g_str_equal(type, "assistant"))
			{
				for (i = 0; content != NULL && i < json_array_get_length(content); i++)
				{
					JsonObject *part = ai_json_array_get_object(content, i);
					const gchar *kind = ai_json_get_string(part, "type", "");
					if (g_str_equal(kind, "text"))
					{
						const gchar *text = ai_json_get_string(part, "text", NULL);
						if (text != NULL)
						{
							ai_tui_history_text(transcript, "agent_message_chunk", text, &previous, user_text, &open);
							tool_block = NULL;
						}
					}
					else if (g_str_equal(kind, "thinking"))
					{
						const gchar *text = ai_json_get_string(part, "thinking", NULL);
						if (text != NULL)
						{
							ai_tui_history_text(transcript, "agent_thought_chunk", text, &previous, user_text, &open);
							tool_block = NULL;
						}
					}
					else if (g_str_equal(kind, "tool_use"))
						ai_tui_history_add_tool(transcript, &previous, user_text, &open, &tool_block, calls,
							ai_json_get_string(part, "id", NULL), ai_json_get_string(part, "name", "tool"),
							ai_json_get_node(part, "input"), NULL, FALSE);
				}
			}
		}
		if (end == NULL) break;
		line = end + 1;
	}
	ai_tui_history_text(transcript, "end", "", &previous, user_text, &open);
	return g_steal_pointer(&transcript);
}

static gchar *
ai_tui_cursor_history_find(AiCliClient *client, gchar **session_id)
{
	const gchar *home = ai_tui_history_env(client, "HOME");
	g_autofree gchar *cwd = ai_tui_history_cwd(client);
	g_autofree gchar *encoded = ai_tui_history_dash_path(cwd, FALSE);
	g_autofree gchar *resolved = cwd != NULL ? realpath(cwd, NULL) : NULL;
	g_autofree gchar *encoded_real = ai_tui_history_dash_path(resolved, FALSE);
	g_autofree gchar *root = g_build_filename(home != NULL ? home : g_get_home_dir(), ".cursor", "projects", NULL);
	const gchar *wanted = ai_cli_client_get_session_id(client);
	g_autofree gchar *selected = NULL;
	gchar *names[2];
	guint i;

	names[0] = encoded;
	names[1] = encoded_real;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autofree gchar *transcripts = NULL;
		g_autoptr(GDir) dir = NULL;
		const gchar *name;
		gint64 best = G_MININT64;

		if (names[i] == NULL || *names[i] == '\0') continue;
		if (i > 0 && g_strcmp0(names[0], names[i]) == 0) continue;
		transcripts = g_build_filename(root, names[i], "agent-transcripts", NULL);
		dir = g_dir_open(transcripts, 0, NULL);
		if (dir == NULL) continue;
		while ((name = g_dir_read_name(dir)) != NULL)
		{
			g_autofree gchar *path = g_build_filename(transcripts, name, name, NULL);
			GStatBuf stat_buf;
			g_autofree gchar *with_ext = g_strconcat(path, ".jsonl", NULL);

			if (wanted != NULL && *wanted != '\0' && !g_str_equal(wanted, name)) continue;
			if (!g_file_test(with_ext, G_FILE_TEST_IS_REGULAR)) continue;
			if (g_stat(with_ext, &stat_buf) != 0 || stat_buf.st_size > 64 * 1024 * 1024) continue;
			if (selected != NULL && (gint64)stat_buf.st_mtime < best) continue;
			best = (gint64)stat_buf.st_mtime;
			g_free(selected);
			selected = g_steal_pointer(&with_ext);
			g_free(*session_id);
			*session_id = g_strdup(name);
		}
	}
	return g_steal_pointer(&selected);
}

static AiTranscript *
ai_tui_cursor_history_read(const gchar *path, GError **error)
{
	g_autofree gchar *contents = NULL;
	g_autofree gchar *previous = NULL;
	g_autoptr(GString) user_text = g_string_new(NULL);
	g_autoptr(AiTranscript) transcript = ai_transcript_new();
	g_autoptr(GHashTable) calls = g_hash_table_new(g_str_hash, g_str_equal);
	AiViewBlock *open = NULL;
	AiViewToolBlock *tool_block = NULL;
	GStatBuf stat_buf;
	gchar *line;

	if (g_stat(path, &stat_buf) == 0 && stat_buf.st_size > 64 * 1024 * 1024)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Native history exceeds the 64 MiB display limit");
		return NULL;
	}
	if (!g_file_get_contents(path, &contents, NULL, error)) return NULL;
	line = contents;
	while (*line != '\0')
	{
		gchar *end = strchr(line, '\n');
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root;
		JsonObject *object;
		JsonObject *message;
		JsonArray *content;
		const gchar *role;
		guint i;

		if (end != NULL) *end = '\0';
		if (*line != '\0')
		{
			if (!json_parser_load_from_data(parser, line, -1, error)) return NULL;
			root = json_parser_get_root(parser);
			object = root != NULL && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
			role = ai_json_get_string(object, "role", "");
			message = ai_json_get_object(object, "message");
			content = ai_json_get_array(message, "content");
			if (g_str_equal(role, "user"))
			{
				g_autofree gchar *raw = ai_tui_history_item_text(message);
				g_autofree gchar *text = ai_tui_history_between(raw, "<user_query>", "</user_query>");
				if (text != NULL && *text != '\0')
				{
					ai_tui_history_text(transcript, "user_message_chunk", text, &previous, user_text, &open);
					tool_block = NULL;
				}
			}
			else if (g_str_equal(role, "assistant"))
			{
				for (i = 0; content != NULL && i < json_array_get_length(content); i++)
				{
					JsonObject *part = ai_json_array_get_object(content, i);
					const gchar *kind = ai_json_get_string(part, "type", "");
					if (g_str_equal(kind, "text"))
					{
						const gchar *text = ai_json_get_string(part, "text", NULL);
						if (text != NULL)
						{
							ai_tui_history_text(transcript, "agent_message_chunk", text, &previous, user_text, &open);
							tool_block = NULL;
						}
					}
					else if (g_str_equal(kind, "tool_use") || g_str_equal(kind, "tool_call"))
						ai_tui_history_add_tool(transcript, &previous, user_text, &open, &tool_block, calls,
							ai_json_get_string(part, "id", ai_json_get_string(part, "toolCallId", NULL)),
							ai_json_get_string(part, "name", "tool"),
							ai_json_get_node(part, "input"), NULL, FALSE);
				}
			}
		}
		if (end == NULL) break;
		line = end + 1;
	}
	ai_tui_history_text(transcript, "end", "", &previous, user_text, &open);
	return g_steal_pointer(&transcript);
}

static gchar *
ai_tui_agy_history_find(AiCliClient *client, gchar **session_id)
{
	const gchar *home = ai_tui_history_env(client, "HOME");
	g_autofree gchar *cwd = ai_tui_history_cwd(client);
	g_autofree gchar *root = g_build_filename(home != NULL ? home : g_get_home_dir(),
		".gemini", "antigravity-cli", NULL);
	g_autofree gchar *index_path = g_build_filename(root, "cache", "last_conversations.json", NULL);
	g_autoptr(JsonParser) parser = ai_tui_history_json(index_path);
	const gchar *wanted = ai_cli_client_get_session_id(client);
	JsonObject *index;
	GList *members;
	GList *iter;
	g_autofree gchar *selected = NULL;
	gint64 best = G_MININT64;

	if (parser == NULL) return NULL;
	{
		JsonNode *root_node = json_parser_get_root(parser);
		index = root_node != NULL && JSON_NODE_HOLDS_OBJECT(root_node) ? json_node_get_object(root_node) : NULL;
	}
	members = index != NULL ? json_object_get_members(index) : NULL;
	for (iter = members; iter != NULL; iter = iter->next)
	{
		const gchar *workspace = iter->data;
		const gchar *id = ai_json_get_string(index, workspace, NULL);
		g_autofree gchar *path = NULL;
		GStatBuf stat_buf;

		if (id == NULL || *id == '\0') continue;
		if (wanted != NULL && *wanted != '\0' && !g_str_equal(wanted, id)) continue;
		if (!ai_tui_history_same_directory(workspace, cwd)) continue;
		path = g_build_filename(root, "brain", id, ".system_generated", "logs", "transcript.jsonl", NULL);
		if (g_stat(path, &stat_buf) != 0 || stat_buf.st_size > 64 * 1024 * 1024) continue;
		if (selected != NULL && (gint64)stat_buf.st_mtime < best) continue;
		best = (gint64)stat_buf.st_mtime;
		g_free(selected);
		selected = g_steal_pointer(&path);
		g_free(*session_id);
		*session_id = g_strdup(id);
	}
	g_list_free(members);
	return g_steal_pointer(&selected);
}

static AiTranscript *
ai_tui_agy_history_read(const gchar *path, GError **error)
{
	g_autofree gchar *contents = NULL;
	g_autofree gchar *previous = NULL;
	g_autoptr(GString) user_text = g_string_new(NULL);
	g_autoptr(AiTranscript) transcript = ai_transcript_new();
	g_autoptr(GHashTable) calls = g_hash_table_new(g_str_hash, g_str_equal);
	AiViewBlock *open = NULL;
	AiViewToolBlock *tool_block = NULL;
	GStatBuf stat_buf;
	gchar *line;
	guint tool_n = 0;

	if (g_stat(path, &stat_buf) == 0 && stat_buf.st_size > 64 * 1024 * 1024)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Native history exceeds the 64 MiB display limit");
		return NULL;
	}
	if (!g_file_get_contents(path, &contents, NULL, error)) return NULL;
	line = contents;
	while (*line != '\0')
	{
		gchar *end = strchr(line, '\n');
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root;
		JsonObject *object;
		JsonArray *tools;
		const gchar *type;
		guint i;

		if (end != NULL) *end = '\0';
		if (*line != '\0')
		{
			if (!json_parser_load_from_data(parser, line, -1, error)) return NULL;
			root = json_parser_get_root(parser);
			object = root != NULL && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
			type = ai_json_get_string(object, "type", "");
			if (g_str_equal(type, "USER_INPUT"))
			{
				g_autofree gchar *text = ai_tui_history_between(
					ai_json_get_string(object, "content", NULL), "<USER_REQUEST>", "</USER_REQUEST>");
				if (text != NULL && *text != '\0')
				{
					ai_tui_history_text(transcript, "user_message_chunk", text, &previous, user_text, &open);
					tool_block = NULL;
				}
			}
			else if (g_str_equal(type, "PLANNER_RESPONSE") || g_str_equal(type, "MODEL_RESPONSE"))
			{
				const gchar *text = ai_json_get_string(object, "content", NULL);
				if (text != NULL && *text != '\0')
				{
					ai_tui_history_text(transcript, "agent_message_chunk", text, &previous, user_text, &open);
					tool_block = NULL;
				}
				tools = ai_json_get_array(object, "tool_calls");
				for (i = 0; tools != NULL && i < json_array_get_length(tools); i++)
				{
					JsonObject *call = ai_json_array_get_object(tools, i);
					g_autofree gchar *id = g_strdup_printf("agy-tool-%u", ++tool_n);
					ai_tui_history_add_tool(transcript, &previous, user_text, &open, &tool_block, calls,
						id, ai_json_get_string(call, "name", "tool"),
						ai_json_get_node(call, "args"), NULL, FALSE);
				}
			}
		}
		if (end == NULL) break;
		line = end + 1;
	}
	ai_tui_history_text(transcript, "end", "", &previous, user_text, &open);
	return g_steal_pointer(&transcript);
}

static gchar *
ai_tui_opencode_db_path(AiCliClient *client)
{
	const gchar *data = ai_tui_history_env(client, "XDG_DATA_HOME");
	const gchar *home = ai_tui_history_env(client, "HOME");
	g_autofree gchar *fallback = NULL;

	if (data == NULL || *data == '\0')
		data = fallback = g_build_filename(home != NULL ? home : g_get_home_dir(), ".local", "share", NULL);
	return g_build_filename(data, "opencode", "opencode.db", NULL);
}

static gchar *
ai_tui_opencode_history_find(AiCliClient *client, gchar **session_id)
{
	g_autofree gchar *path = ai_tui_opencode_db_path(client);
	g_autofree gchar *cwd = ai_tui_history_cwd(client);
	const gchar *wanted = ai_cli_client_get_session_id(client);
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	gint64 best = G_MININT64;
	g_autofree gchar *selected_id = NULL;

	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
	{
		if (db != NULL) sqlite3_close(db);
		return NULL;
	}
	if (sqlite3_prepare_v2(db, "SELECT id, directory, time_updated, parent_id FROM session", -1, &stmt, NULL) != SQLITE_OK &&
	    sqlite3_prepare_v2(db, "SELECT id, directory, time_updated FROM session", -1, &stmt, NULL) != SQLITE_OK)
	{
		sqlite3_close(db);
		return NULL;
	}
	while (sqlite3_step(stmt) == SQLITE_ROW)
	{
		const gchar *id = (const gchar *)sqlite3_column_text(stmt, 0);
		const gchar *directory = (const gchar *)sqlite3_column_text(stmt, 1);
		gint64 updated = sqlite3_column_int64(stmt, 2);
		const gchar *parent = sqlite3_column_count(stmt) > 3 ? (const gchar *)sqlite3_column_text(stmt, 3) : NULL;

		if (id == NULL || directory == NULL) continue;
		if (parent != NULL && *parent != '\0') continue;
		if (wanted != NULL && *wanted != '\0' && !g_str_equal(wanted, id)) continue;
		if (!ai_tui_history_same_directory(directory, cwd)) continue;
		if (selected_id != NULL && updated < best) continue;
		best = updated;
		g_free(selected_id);
		selected_id = g_strdup(id);
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	if (selected_id == NULL) return NULL;
	g_free(*session_id);
	*session_id = g_steal_pointer(&selected_id);
	return g_steal_pointer(&path);
}

static AiTranscript *
ai_tui_opencode_history_read(const gchar *path, const gchar *session_id, GError **error)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	g_autofree gchar *previous = NULL;
	g_autoptr(GString) user_text = g_string_new(NULL);
	g_autoptr(AiTranscript) transcript = ai_transcript_new();
	g_autoptr(GHashTable) calls = g_hash_table_new(g_str_hash, g_str_equal);
	AiViewBlock *open = NULL;
	AiViewToolBlock *tool_block = NULL;

	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
	{
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not open OpenCode history: %s",
			db != NULL ? sqlite3_errmsg(db) : path);
		if (db != NULL) sqlite3_close(db);
		return NULL;
	}
	if (sqlite3_prepare_v2(db,
		"SELECT m.data, p.data FROM part p JOIN message m ON m.id = p.message_id "
		"WHERE p.session_id = ? ORDER BY p.time_created, p.id", -1, &stmt, NULL) != SQLITE_OK)
	{
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read OpenCode history: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW)
	{
		const gchar *message_json = (const gchar *)sqlite3_column_text(stmt, 0);
		const gchar *part_json = (const gchar *)sqlite3_column_text(stmt, 1);
		g_autoptr(JsonParser) message_parser = json_parser_new();
		g_autoptr(JsonParser) part_parser = json_parser_new();
		JsonObject *message;
		JsonObject *part;
		JsonObject *state;
		const gchar *role;
		const gchar *kind;

		if (message_json == NULL || part_json == NULL) continue;
		if (!json_parser_load_from_data(message_parser, message_json, -1, error) ||
		    !json_parser_load_from_data(part_parser, part_json, -1, error))
		{
			sqlite3_finalize(stmt);
			sqlite3_close(db);
			return NULL;
		}
		message = json_parser_get_root(message_parser) != NULL &&
			JSON_NODE_HOLDS_OBJECT(json_parser_get_root(message_parser)) ?
			json_node_get_object(json_parser_get_root(message_parser)) : NULL;
		part = json_parser_get_root(part_parser) != NULL &&
			JSON_NODE_HOLDS_OBJECT(json_parser_get_root(part_parser)) ?
			json_node_get_object(json_parser_get_root(part_parser)) : NULL;
		role = ai_json_get_string(message, "role", "");
		kind = ai_json_get_string(part, "type", "");
		if (g_str_equal(kind, "text"))
		{
			const gchar *text = ai_json_get_string(part, "text", NULL);
			const gchar *chunk = g_str_equal(role, "user") ? "user_message_chunk" : "agent_message_chunk";
			if (text != NULL)
			{
				ai_tui_history_text(transcript, chunk, text, &previous, user_text, &open);
				tool_block = NULL;
			}
		}
		else if (g_str_equal(kind, "reasoning"))
		{
			const gchar *text = ai_json_get_string(part, "text", NULL);
			if (text != NULL)
			{
				ai_tui_history_text(transcript, "agent_thought_chunk", text, &previous, user_text, &open);
				tool_block = NULL;
			}
		}
		else if (g_str_equal(kind, "tool"))
		{
			state = ai_json_get_object(part, "state");
			ai_tui_history_add_tool(transcript, &previous, user_text, &open, &tool_block, calls,
				ai_json_get_string(part, "callID", ai_json_get_string(part, "id", NULL)),
				ai_json_get_string(part, "tool", "tool"),
				ai_json_get_node(state, "input"),
				ai_json_get_string(state, "output", NULL),
				g_strcmp0(ai_json_get_string(state, "status", ""), "error") == 0 ||
				g_strcmp0(ai_json_get_string(state, "status", ""), "failed") == 0);
		}
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	ai_tui_history_text(transcript, "end", "", &previous, user_text, &open);
	return g_steal_pointer(&transcript);
}

static void
ai_tui_history_publish(AiTranscript *target, AiTranscript *history, GPtrArray *input_history)
{
	guint i;

	for (i = 0; i < ai_transcript_get_n_blocks(history); i++)
	{
		AiViewBlock *block = ai_transcript_get_block(history, i);
		ai_transcript_append(target, block);
		if (AI_IS_VIEW_TURN_BLOCK(block))
			g_ptr_array_add(input_history, g_strdup(ai_view_turn_block_get_text(AI_VIEW_TURN_BLOCK(block))));
	}
}

/* Startup-only display restoration. Pin the exact session before any new turn,
 * and keep historical messages out of AiConversation's outgoing prompt list. */
static void
ai_tui_history_restore(AiConversation *conversation, GPtrArray *input_history)
{
	GObject *provider = ai_conversation_get_provider(conversation);
	AiCliClient *client;
	gboolean continuing = FALSE;
	g_autofree gchar *id = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiTranscript) history = NULL;
	AiTranscript *target = ai_conversation_get_transcript(conversation);

	if (!AI_IS_CLI_CLIENT(provider) && !AI_IS_CLAUDE_TMUX_CLIENT(provider)) return;
	if (!AI_IS_GROK_BUILD_CLIENT(provider) && !AI_IS_CODEX_CLI_CLIENT(provider) &&
	    !AI_IS_CLAUDE_CODE_CLIENT(provider) && !AI_IS_CLAUDE_TMUX_CLIENT(provider) &&
	    !AI_IS_CURSOR_CLIENT(provider) && !AI_IS_OPENCODE_CLIENT(provider) &&
	    !AI_IS_ANTIGRAVITY_CLIENT(provider)) return;
	client = AI_CLI_CLIENT(provider);
	g_object_get(provider, "continue-session", &continuing, NULL);
	if (!ai_cli_client_get_session_persistence(client) ||
	    (!continuing && ai_cli_client_get_session_id(client) == NULL)) return;
	if (AI_IS_GROK_BUILD_CLIENT(provider))
		path = ai_tui_history_find(client, &id);
	else if (AI_IS_CODEX_CLI_CLIENT(provider))
		path = ai_tui_codex_history_find(client, &id);
	else if (AI_IS_CLAUDE_CODE_CLIENT(provider) || AI_IS_CLAUDE_TMUX_CLIENT(provider))
		path = ai_tui_claude_history_find(provider, client, &id);
	else if (AI_IS_CURSOR_CLIENT(provider))
		path = ai_tui_cursor_history_find(client, &id);
	else if (AI_IS_OPENCODE_CLIENT(provider))
		path = ai_tui_opencode_history_find(client, &id);
	else
		path = ai_tui_agy_history_find(client, &id);
	if (path == NULL) return;
	if (AI_IS_GROK_BUILD_CLIENT(provider))
		history = ai_tui_history_read(path, id, &error);
	else if (AI_IS_CODEX_CLI_CLIENT(provider))
		history = ai_tui_codex_history_read(path, &error);
	else if (AI_IS_CLAUDE_CODE_CLIENT(provider) || AI_IS_CLAUDE_TMUX_CLIENT(provider))
		history = ai_tui_claude_history_read(path, &error);
	else if (AI_IS_CURSOR_CLIENT(provider))
		history = ai_tui_cursor_history_read(path, &error);
	else if (AI_IS_OPENCODE_CLIENT(provider))
		history = ai_tui_opencode_history_read(path, id, &error);
	else
		history = ai_tui_agy_history_read(path, &error);
	if (history == NULL)
	{
		g_autofree gchar *message = g_strdup_printf("Could not load native session history: %s", error->message);
		g_autoptr(AiViewBlock) block = ai_view_status_block_new(AI_VIEW_STATUS_ERROR, message);
		ai_transcript_append(target, block);
		return;
	}
	ai_cli_client_set_session_id(client, id);
	ai_tui_history_publish(target, history, input_history);
}
