/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Read native Grok and Codex history for display only. Never replay old
 * prompts/tools into the provider: its resumed session already owns that
 * context.
 */
#pragma once

#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
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
		JsonObject *part = ai_json_array_get_object(content, i);
		const gchar *chunk = ai_json_get_string(part, "text", NULL);
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

	if (!AI_IS_GROK_BUILD_CLIENT(provider) && !AI_IS_CODEX_CLI_CLIENT(provider)) return;
	client = AI_CLI_CLIENT(provider);
	g_object_get(provider, "continue-session", &continuing, NULL);
	if (!ai_cli_client_get_session_persistence(client) ||
	    (!continuing && ai_cli_client_get_session_id(client) == NULL)) return;
	path = AI_IS_GROK_BUILD_CLIENT(provider) ? ai_tui_history_find(client, &id) :
		ai_tui_codex_history_find(client, &id);
	if (path == NULL) return;
	history = AI_IS_GROK_BUILD_CLIENT(provider) ? ai_tui_history_read(path, id, &error) :
		ai_tui_codex_history_read(path, &error);
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
