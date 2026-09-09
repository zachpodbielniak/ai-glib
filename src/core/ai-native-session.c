/*
 * ai-native-session.c - Read a wrapped CLI's own session transcript
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Why this exists
 * ---------------
 * A CLI wrapper does most of its work in its own process.  ai-glib sees
 * the prompt it sent and the final text that came back; it does not see
 * the fourteen tool calls in between, and when a session was resumed
 * with `--continue` it did not see the backlog either.  The portable
 * #AiMessage history is therefore a strict subset of what the wrapped
 * program actually knows, and switching providers on that subset drops
 * the difference silently --- the new model answers confidently from a
 * conversation it only half received, which reads as a bad model rather
 * than as missing context.
 *
 * Every one of these programs already writes the full record to disk.
 * This file reads it back.
 *
 * The compaction boundary
 * -----------------------
 * A long session gets compacted: the harness replaces the transcript
 * prefix with a summary and continues.  Everything before that boundary
 * is, by the harness's own decision, superseded --- and it is also the
 * bulk of the bytes.  So the read starts at the *last* compaction
 * boundary and takes everything after it.  Each harness marks the
 * boundary differently, and each marker below was confirmed against real
 * transcripts rather than inferred:
 *
 *   claude-code / claude-tmux  `isCompactSummary: true` on a record, or
 *                              `type:"system", subtype:"compact_boundary"`
 *   codex                      `type:"compacted"`, whose payload carries
 *                              `replacement_history` --- the post-compaction
 *                              history in full, so it seeds the result
 *   grok-build                 `sessionUpdate: "compaction_checkpoint"`,
 *                              and `auto_compact_completed`
 *
 * A transcript with no boundary is not an error and not empty: it means
 * nothing has been compacted, so the whole file is the history since the
 * last compaction.  That fallback is the common case.
 *
 * These files are not ours
 * ------------------------
 * Same category as subprocess stdout and the harness layer's markdown: a
 * malformed record costs itself and nothing else.  A line that does not
 * parse, a record of an unknown shape, a member of the wrong type --- all
 * skipped with a g_debug.  One bad line must not hide four hundred good
 * ones, and must not abort a `G_DEBUG=fatal-warnings` run.  Every member
 * is read through core/ai-json-util.h for that reason.
 *
 * The hard failures are the ones where continuing would lie: a file that
 * cannot be read at all, and a file past the size bound.  Silently
 * importing the first 32 MiB of a 90 MiB transcript would hand the model
 * a conversation that stops mid-sentence with no indication it was cut.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "core/ai-native-session.h"
#include "core/ai-error.h"
#include "core/ai-json-util.h"
#include "core/ai-provider.h"
#include "model/ai-message.h"
#include "model/ai-text-content.h"

/*
 * A transcript is a local file this library did not write, so the bound
 * is about arithmetic and memory rather than about trust.  64 MiB is far
 * past any real session and still comfortably addressable.
 */
#define NATIVE_SESSION_MAX_BYTES ((gsize)(64 * 1024 * 1024))

/* Tool arguments and results are summarised, not reproduced.  A 300 KiB
 * file read pasted back into a system prompt is the whole context window
 * spent on something the new provider can simply read again. */
#define NATIVE_SESSION_MAX_BLOB (2048)

struct _AiNativeSession
{
	GObject parent_instance;

	gchar               *provider;
	gchar               *session_id;
	gchar               *path;
	AiNativeSessionKind  kind;
	GList               *messages;    /* AiMessage, owned, in order */
	/*
	 * call id -> tool name.  Every one of these formats names the tool
	 * on the *call* and identifies the result only by id, so a result
	 * rendered on its own would read "[Tool result: (unnamed)]" --- the
	 * id is meaningless to a model that never saw the call frame.
	 */
	GHashTable          *tool_names;
	gboolean             compacted;
	guint                dropped;
};

G_DEFINE_FINAL_TYPE(AiNativeSession, ai_native_session, G_TYPE_OBJECT)

static void
ai_native_session_finalize(GObject *object)
{
	AiNativeSession *self = AI_NATIVE_SESSION(object);

	g_clear_pointer(&self->provider, g_free);
	g_clear_pointer(&self->session_id, g_free);
	g_clear_pointer(&self->path, g_free);
	g_clear_pointer(&self->tool_names, g_hash_table_unref);
	g_list_free_full(g_steal_pointer(&self->messages), g_object_unref);

	G_OBJECT_CLASS(ai_native_session_parent_class)->finalize(object);
}

static void
ai_native_session_class_init(AiNativeSessionClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ai_native_session_finalize;
}

static void
ai_native_session_init(AiNativeSession *self)
{
	self->kind = AI_NATIVE_SESSION_UNSUPPORTED;
	self->tool_names = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                         g_free, g_free);
}

/* ================================================================
 * Building the message list
 * ================================================================ */

/*
 * append_message: add one role/text pair, coalescing into the previous
 * message when the role matches.
 *
 * Every one of these formats emits an assistant turn as many records ---
 * streamed chunks for grok, one record per content block for claude, a
 * reasoning item and a message item for codex.  Appending each as its own
 * #AiMessage would turn one answer into nine, and a history of nine
 * consecutive assistant turns is a shape no provider ever produces, which
 * some of them reject outright.
 */
static void
append_message(
	AiNativeSession *self,
	AiRole           role,
	const gchar     *text
){
	AiMessage *previous;

	if (text == NULL || text[0] == '\0')
	{
		return;
	}

	previous = self->messages != NULL
		? AI_MESSAGE(g_list_last(self->messages)->data)
		: NULL;

	/*
	 * A second text block on the same message, rather than a rebuilt
	 * message: ai_message_get_text() already concatenates every text
	 * block, so this is the coalescing, and it costs one allocation
	 * instead of re-flattening a message that may already be megabytes.
	 */
	if (previous != NULL && ai_message_get_role(previous) == role)
	{
		ai_message_add_text(previous, text);
		return;
	}

	previous = ai_message_new(role);
	ai_message_add_text(previous, text);
	self->messages = g_list_append(self->messages, previous);
}

/*
 * truncate_blob: bound one tool argument or result.
 *
 * Returns a newly allocated string that is either @text or its first
 * NATIVE_SESSION_MAX_BLOB bytes with a marker.  The cut is made on a
 * UTF-8 character boundary: the result is spliced into a system prompt,
 * and a prompt ending in half a character is rejected by some providers
 * and mangled by the rest.
 */
static gchar *
truncate_blob(const gchar *text)
{
	const gchar *end;

	if (text == NULL)
	{
		return NULL;
	}

	if (strlen(text) <= NATIVE_SESSION_MAX_BLOB)
	{
		return g_strdup(text);
	}

	/*
	 * g_utf8_find_prev_char() needs a valid string to walk back through.
	 * A transcript can contain anything, so fall back to a byte cut with
	 * the invalid tail scrubbed rather than trusting the walk.
	 */
	if (!g_utf8_validate(text, NATIVE_SESSION_MAX_BLOB, &end))
	{
		g_autofree gchar *raw = g_strndup(text, (gsize)(end - text));
		return g_strdup_printf("%s... [truncated]", raw);
	}

	{
		g_autofree gchar *raw = g_strndup(text, NATIVE_SESSION_MAX_BLOB);
		return g_strdup_printf("%s... [truncated]", raw);
	}
}

/* Render a tool call the way docs/provider-switching.org already
 * describes the CLI projection, so a reader sees one vocabulary. */
static void
append_tool_call(
	AiNativeSession *self,
	const gchar     *name,
	const gchar     *id,
	const gchar     *arguments
){
	g_autofree gchar *bounded = truncate_blob(arguments);
	g_autofree gchar *line = NULL;

	if (name == NULL || name[0] == '\0')
	{
		name = "(unnamed)";
	}

	if (id != NULL && id[0] != '\0')
	{
		g_hash_table_insert(self->tool_names, g_strdup(id),
		                    g_strdup(name));
	}

	line = g_strdup_printf("[Tool call: %s; id=%s; arguments=%s]",
	                       name,
	                       id != NULL ? id : "",
	                       bounded != NULL ? bounded : "{}");
	append_message(self, AI_ROLE_ASSISTANT, line);
}

static void
append_tool_result(
	AiNativeSession *self,
	const gchar     *name,
	const gchar     *id,
	const gchar     *result,
	gboolean         is_error
){
	g_autofree gchar *bounded = truncate_blob(result);
	g_autofree gchar *line = NULL;

	if (name == NULL && id != NULL)
	{
		name = g_hash_table_lookup(self->tool_names, id);
	}

	line = g_strdup_printf("[Tool result: %s; id=%s; error=%s]\n%s\n"
	                       "[End tool result]",
	                       name != NULL ? name : "(unnamed)",
	                       id != NULL ? id : "",
	                       is_error ? "true" : "false",
	                       bounded != NULL ? bounded : "");
	append_message(self, AI_ROLE_USER, line);
}

/*
 * node_to_text: flatten any JSON node to something a model can read.
 *
 * Tool arguments and results are typed differently by every harness and
 * sometimes by every tool --- a string here, an object there, an array of
 * content blocks in a third place.  Rather than a branch per shape, a
 * string node yields its own text (so the common case is not wrapped in
 * quotes) and anything else is serialised.
 */
static gchar *
node_to_text(JsonNode *node)
{
	g_autoptr(JsonGenerator) generator = NULL;

	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
	{
		return NULL;
	}

	if (JSON_NODE_HOLDS_VALUE(node)
	    && json_node_get_value_type(node) == G_TYPE_STRING)
	{
		return g_strdup(json_node_get_string(node));
	}

	generator = json_generator_new();
	json_generator_set_root(generator, node);

	return json_generator_to_data(generator, NULL);
}

/* ================================================================
 * The line splitter
 * ================================================================ */

/*
 * for_each_record: parse @contents as JSONL and hand each object to @func.
 *
 * Two passes run over the same file --- one to find the last compaction
 * boundary, one to collect the records after it --- so this exists rather
 * than a split into a GStrv, which would double the peak memory of a
 * transcript already allowed to reach 64 MiB.
 *
 * A line that is empty, that does not parse, or whose root is not an
 * object is skipped.  A partially written last line is the normal state
 * of a transcript belonging to a program that is still running.
 */
typedef gboolean (*RecordFunc)(JsonObject *record, guint index, gpointer data);

static void
for_each_record(
	const gchar *contents,
	RecordFunc   func,
	gpointer     data
){
	const gchar *line = contents;
	guint        index = 0;

	while (*line != '\0')
	{
		const gchar *end = strchr(line, '\n');
		gsize        length = end != NULL
			? (gsize)(end - line)
			: strlen(line);
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *record;

		if (length > 0)
		{
			if (json_parser_load_from_data(parser, line, (gssize)length,
			                               NULL)
			    && (record = ai_json_root_object(parser)) != NULL)
			{
				if (!func(record, index, data))
				{
					return;
				}
			}

			index++;
		}

		if (end == NULL)
		{
			return;
		}

		line = end + 1;
	}
}

/* ================================================================
 * Per-harness readers
 * ================================================================ */

typedef struct
{
	AiNativeSession *session;
	guint            boundary;    /* first record index to keep */
	gboolean         found;
	/*
	 * codex: the history the compaction marker carries.  Deep-copied,
	 * because the JsonNode it comes from belongs to the per-line
	 * JsonParser inside for_each_record(), which is gone by the time the
	 * scan pass returns.  Borrowing it here was a use-after-free that
	 * surfaced as a Json critical on a valid document.
	 */
	JsonNode        *replacement;
} ReadState;

/* -------- claude-code and claude-tmux -------- */

static gboolean
claude_is_boundary(JsonObject *record)
{
	const gchar *type;

	if (ai_json_get_boolean(record, "isCompactSummary", FALSE))
	{
		return TRUE;
	}

	type = ai_json_get_string(record, "type", "");

	return g_str_equal(type, "system")
		&& g_strcmp0(ai_json_get_string(record, "subtype", ""),
		             "compact_boundary") == 0;
}

static gboolean
claude_scan(JsonObject *record, guint index, gpointer data)
{
	ReadState *state = data;

	if (claude_is_boundary(record))
	{
		state->boundary = index;
		state->found = TRUE;
	}

	return TRUE;
}

/*
 * claude_content: fold one message's content array into the session.
 *
 * `message.content` is an array of blocks in every record but the
 * type:"system" ones, where it is a bare string.  Both shapes appear in
 * real transcripts, so both are handled rather than one being treated as
 * corruption.
 */
static void
claude_content(
	AiNativeSession *self,
	JsonObject      *message,
	AiRole           role
){
	JsonArray   *blocks;
	const gchar *plain;
	guint        i;

	plain = ai_json_get_string(message, "content", NULL);

	if (plain != NULL)
	{
		append_message(self, role, plain);
		return;
	}

	blocks = ai_json_get_array(message, "content");

	if (blocks == NULL)
	{
		return;
	}

	for (i = 0; i < json_array_get_length(blocks); i++)
	{
		JsonObject  *block = ai_json_array_get_object(blocks, i);
		const gchar *type;

		if (block == NULL)
		{
			continue;
		}

		type = ai_json_get_string(block, "type", "");

		if (g_str_equal(type, "text"))
		{
			append_message(self, role,
			               ai_json_get_string(block, "text", NULL));
		}
		else if (g_str_equal(type, "thinking"))
		{
			/*
			 * Reasoning is deliberately dropped.  It is the largest
			 * part of a modern transcript and the least useful to a
			 * different model, which did not produce it and cannot
			 * continue it --- several providers reject another model's
			 * thinking blocks outright.
			 */
			continue;
		}
		else if (g_str_equal(type, "tool_use"))
		{
			g_autofree gchar *arguments =
				node_to_text(ai_json_get_node(block, "input"));

			append_tool_call(self,
			                 ai_json_get_string(block, "name", NULL),
			                 ai_json_get_string(block, "id", NULL),
			                 arguments);
		}
		else if (g_str_equal(type, "tool_result"))
		{
			g_autofree gchar *text =
				node_to_text(ai_json_get_node(block, "content"));

			append_tool_result(self, NULL,
			                   ai_json_get_string(block, "tool_use_id",
			                                      NULL),
			                   text,
			                   ai_json_get_boolean(block, "is_error",
			                                       FALSE));
		}
		else if (g_str_equal(type, "image"))
		{
			/* Binary content cannot survive a text projection; say so
			 * rather than dropping it without trace. */
			append_message(self, role, "[image omitted]");
		}
	}
}

static gboolean
claude_collect(JsonObject *record, guint index, gpointer data)
{
	ReadState   *state = data;
	JsonObject  *message;
	const gchar *type;
	AiRole       role;

	if (state->found && index <= state->boundary)
	{
		/* The summary record itself is kept: it *is* the compacted
		 * history, and dropping it would discard the very thing the
		 * boundary was created to preserve. */
		if (index < state->boundary)
		{
			state->session->dropped++;
			return TRUE;
		}
	}

	/*
	 * A sidechain is a subagent's own transcript, interleaved into the
	 * parent file.  It is not part of this conversation, and folding it
	 * in makes the main thread read as though it argued with itself.
	 */
	if (ai_json_get_boolean(record, "isSidechain", FALSE))
	{
		return TRUE;
	}

	type = ai_json_get_string(record, "type", "");

	if (g_str_equal(type, "user"))
	{
		role = AI_ROLE_USER;
	}
	else if (g_str_equal(type, "assistant"))
	{
		role = AI_ROLE_ASSISTANT;
	}
	else
	{
		/* Every other type is harness bookkeeping --- titles, latches,
		 * queue operations, file snapshots, cost state. */
		return TRUE;
	}

	message = ai_json_get_object(record, "message");

	if (message == NULL)
	{
		return TRUE;
	}

	claude_content(state->session, message, role);

	return TRUE;
}

/* -------- codex -------- */

static gboolean
codex_scan(JsonObject *record, guint index, gpointer data)
{
	ReadState *state = data;

	if (g_strcmp0(ai_json_get_string(record, "type", ""), "compacted") == 0)
	{
		JsonNode *history = ai_json_get_node(
			ai_json_get_object(record, "payload"), "replacement_history");

		state->boundary = index;
		state->found = TRUE;
		g_clear_pointer(&state->replacement, json_node_unref);

		if (history != NULL)
		{
			state->replacement = json_node_copy(history);
		}
	}

	return TRUE;
}

/*
 * codex_item: fold one `response_item` payload into the session.
 *
 * Shared by the live records and by the `replacement_history` array the
 * compaction marker carries, because they hold the same item shape.  A
 * second implementation for the replacement path is exactly the drift
 * this codebase has already paid for once.
 */
static void
codex_item(AiNativeSession *self, JsonObject *item)
{
	const gchar *type;
	const gchar *role;
	JsonArray   *content;
	guint        i;

	if (item == NULL)
	{
		return;
	}

	type = ai_json_get_string(item, "type", "");

	if (g_str_equal(type, "function_call")
	    || g_str_equal(type, "custom_tool_call"))
	{
		g_autofree gchar *arguments =
			node_to_text(ai_json_get_node(item, "arguments"));

		if (arguments == NULL)
		{
			arguments = node_to_text(ai_json_get_node(item, "input"));
		}

		append_tool_call(self, ai_json_get_string(item, "name", NULL),
		                 ai_json_get_string(item, "call_id", NULL),
		                 arguments);
		return;
	}

	if (g_str_equal(type, "function_call_output")
	    || g_str_equal(type, "custom_tool_call_output"))
	{
		g_autofree gchar *text =
			node_to_text(ai_json_get_node(item, "output"));

		append_tool_result(self, NULL,
		                   ai_json_get_string(item, "call_id", NULL),
		                   text, FALSE);
		return;
	}

	if (!g_str_equal(type, "message") && !g_str_equal(type, "agent_message"))
	{
		/* `reasoning` lands here and is dropped for the same reason
		 * claude's thinking blocks are. */
		return;
	}

	role = ai_json_get_string(item, "role", "assistant");

	/*
	 * A `developer` item is codex's own system scaffolding --- skills
	 * instructions, multi-agent role text, sandbox policy.  It describes
	 * how codex is configured, not what the conversation was about, and
	 * carrying it to another provider would tell that provider it has
	 * tools and rules it does not have.
	 */
	if (g_str_equal(role, "developer") || g_str_equal(role, "system"))
	{
		return;
	}

	content = ai_json_get_array(item, "content");

	if (content == NULL)
	{
		append_message(self,
		               g_str_equal(role, "user")
		                       ? AI_ROLE_USER : AI_ROLE_ASSISTANT,
		               ai_json_get_string(item, "text", NULL));
		return;
	}

	for (i = 0; i < json_array_get_length(content); i++)
	{
		JsonObject *block = ai_json_array_get_object(content, i);

		if (block == NULL)
		{
			continue;
		}

		append_message(self,
		               g_str_equal(role, "user")
		                       ? AI_ROLE_USER : AI_ROLE_ASSISTANT,
		               ai_json_get_string(block, "text", NULL));
	}
}

static gboolean
codex_collect(JsonObject *record, guint index, gpointer data)
{
	ReadState *state = data;

	if (state->found && index <= state->boundary)
	{
		if (index < state->boundary)
		{
			state->session->dropped++;
		}

		return TRUE;
	}

	if (g_strcmp0(ai_json_get_string(record, "type", ""), "response_item")
	    != 0)
	{
		return TRUE;
	}

	codex_item(state->session, ai_json_get_object(record, "payload"));

	return TRUE;
}

/* -------- grok-build -------- */

/*
 * Grok writes ACP session/update frames.  Text arrives as chunks that
 * must be folded --- append_message() already does that by role, so a
 * chunk is simply appended and coalesces with its neighbours.
 */
static gboolean
grok_scan(JsonObject *record, guint index, gpointer data)
{
	ReadState   *state = data;
	JsonObject  *update;
	const gchar *kind;

	update = ai_json_get_object(ai_json_get_object(record, "params"),
	                            "update");
	kind = ai_json_get_string(update, "sessionUpdate", "");

	if (g_str_equal(kind, "compaction_checkpoint")
	    || g_str_equal(kind, "auto_compact_completed"))
	{
		state->boundary = index;
		state->found = TRUE;
	}

	return TRUE;
}

/* The text of an ACP content node, which is `{"type":"text","text":...}`
 * when it is text at all. */
static const gchar *
grok_content_text(JsonObject *update, const gchar *member)
{
	return ai_json_get_string(ai_json_get_object(update, member), "text",
	                          NULL);
}

static gboolean
grok_collect(JsonObject *record, guint index, gpointer data)
{
	ReadState   *state = data;
	JsonObject  *update;
	const gchar *kind;

	if (state->found && index <= state->boundary)
	{
		if (index < state->boundary)
		{
			state->session->dropped++;
		}

		/*
		 * The checkpoint frame carries no summary text of its own ---
		 * the summary lives in a separate checkpoint file that the CLI
		 * reloads.  A `session_recap` after the boundary is what a
		 * reader actually sees, and it is collected normally below.
		 */
		return TRUE;
	}

	update = ai_json_get_object(ai_json_get_object(record, "params"),
	                            "update");

	if (update == NULL)
	{
		return TRUE;
	}

	kind = ai_json_get_string(update, "sessionUpdate", "");

	if (g_str_equal(kind, "user_message_chunk"))
	{
		append_message(state->session, AI_ROLE_USER,
		               grok_content_text(update, "content"));
	}
	else if (g_str_equal(kind, "agent_message_chunk"))
	{
		append_message(state->session, AI_ROLE_ASSISTANT,
		               grok_content_text(update, "content"));
	}
	else if (g_str_equal(kind, "session_recap"))
	{
		g_autofree gchar *line = NULL;
		const gchar      *summary =
			ai_json_get_string(update, "summary", NULL);

		if (summary != NULL)
		{
			line = g_strdup_printf("[Session recap: %s]", summary);
			append_message(state->session, AI_ROLE_ASSISTANT, line);
		}
	}
	else if (g_str_equal(kind, "tool_call"))
	{
		g_autofree gchar *arguments =
			node_to_text(ai_json_get_node(update, "rawInput"));

		append_tool_call(state->session,
		                 ai_json_get_string(update, "title",
		                     ai_json_get_string(update, "kind", NULL)),
		                 ai_json_get_string(update, "toolCallId", NULL),
		                 arguments);
	}

	/*
	 * `tool_call_update` is skipped: it is the progress and completion
	 * stream for a call already recorded, at two frames per call, and it
	 * repeats the whole payload each time.  Folding those in doubles the
	 * import for nothing a different model can act on.
	 */

	return TRUE;
}

/* ================================================================
 * The reader table --- this is the registration
 * ================================================================ */

typedef struct
{
	const gchar         *provider;   /* the canonical name `ai -p` takes */
	const gchar         *type_name;  /* G_OBJECT_TYPE_NAME of the client */
	AiNativeSessionKind  kind;
	RecordFunc           scan;
	RecordFunc           collect;
} NativeReader;

/*
 * Teaching this about another harness is one struct literal plus its two
 * callbacks, the same pattern as AiToolStyle and AiImageModelInfo.
 *
 * `opencode`, `cursor` and `antigravity` are absent on purpose.  All
 * three keep history in SQLite (`opencode.db`, `chats/<id>/store.db`,
 * `conversations/<id>.db`), which would mean a new hard dependency to
 * read and a schema this library does not control to track.  They report
 * AI_NATIVE_SESSION_UNSUPPORTED, and a switch away from one of them falls
 * back to the portable history it always used.
 */
static const NativeReader readers[] = {
	{ "claude-code", "AiClaudeCodeClient",
	  AI_NATIVE_SESSION_JSONL, claude_scan, claude_collect },
	{ "claude-tmux", "AiClaudeTmuxClient",
	  AI_NATIVE_SESSION_JSONL, claude_scan, claude_collect },
	{ "codex-cli",   "AiCodexCliClient",
	  AI_NATIVE_SESSION_JSONL, codex_scan,  codex_collect  },
	{ "grok-build",  "AiGrokBuildClient",
	  AI_NATIVE_SESSION_JSONL, grok_scan,   grok_collect   }
};

/*
 * Matched on the canonical provider name --- the one `ai -p` and
 * `/provider` accept --- or on the client's C type name.
 *
 * Not on ai_provider_get_name(), which returns a *display* name ("Claude
 * Code", "Grok Build"): a table keyed on those would silently stop
 * matching the day somebody improved the capitalisation, and the symptom
 * would be a provider switch that quietly carried no context.  The type
 * name is what the client actually is, and reaching it needs no provider
 * header here --- core/ including providers/ is the wrong way round.
 */
static const NativeReader *
reader_for(const gchar *provider)
{
	gsize i;

	if (provider == NULL)
	{
		return NULL;
	}

	for (i = 0; i < G_N_ELEMENTS(readers); i++)
	{
		if (g_str_equal(readers[i].provider, provider)
		    || g_str_equal(readers[i].type_name, provider))
		{
			return &readers[i];
		}
	}

	/* `codex` is what a user types; `codex-cli` is the canonical name. */
	if (g_str_equal(provider, "codex"))
	{
		return reader_for("codex-cli");
	}

	return NULL;
}

/**
 * ai_native_session_kind_for_provider:
 * @provider: (nullable): a provider name as ai_provider_get_name() gives it
 *
 * Whether this library can read @provider's private session store.
 *
 * Callers use this to decide whether to offer native import at all,
 * without paying for a directory scan that will find nothing.
 *
 * Returns: the store's kind, or %AI_NATIVE_SESSION_UNSUPPORTED
 */
AiNativeSessionKind
ai_native_session_kind_for_provider(const gchar *provider)
{
	const NativeReader *reader = reader_for(provider);

	return reader != NULL ? reader->kind : AI_NATIVE_SESSION_UNSUPPORTED;
}

/* ================================================================
 * Reading a file
 * ================================================================ */

/**
 * ai_native_session_read_file:
 * @provider: the provider name the transcript belongs to
 * @path: the transcript file
 * @session_id: (nullable): the native session id, recorded for the caller
 * @error: return location for a #GError
 *
 * Reads @path as @provider's native transcript, keeping everything from
 * the last compaction boundary onward.
 *
 * Separate from ai_cli_client_read_native_session() so that a caller with
 * a path already in hand --- and every test --- does not have to
 * reproduce a harness's directory layout to exercise the parser.
 *
 * Returns: (transfer full) (nullable): the session, or %NULL on error
 */
AiNativeSession *
ai_native_session_read_file(
	const gchar  *provider,
	const gchar  *path,
	const gchar  *session_id,
	GError      **error
){
	const NativeReader   *reader;
	g_autoptr(AiNativeSession) self = NULL;
	g_autofree gchar     *contents = NULL;
	gsize                 length = 0;
	GStatBuf              info;
	ReadState             state;

	g_return_val_if_fail(provider != NULL, NULL);
	g_return_val_if_fail(path != NULL, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	reader = reader_for(provider);

	if (reader == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		            "%s keeps no readable session transcript", provider);
		return NULL;
	}

	/*
	 * Checked before the read rather than after: g_file_get_contents()
	 * on a runaway transcript would allocate the whole thing first and
	 * only then be told it was too big.
	 */
	if (g_stat(path, &info) == 0
	    && (gsize)info.st_size > NATIVE_SESSION_MAX_BYTES)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "%s transcript is larger than the %u MiB import limit",
		            provider,
		            (guint)(NATIVE_SESSION_MAX_BYTES / (1024 * 1024)));
		return NULL;
	}

	if (!g_file_get_contents(path, &contents, &length, error))
	{
		return NULL;
	}

	/*
	 * A NUL byte would truncate every subsequent strchr() walk without
	 * saying so, producing a short import that looks like a short
	 * session.  Invalid UTF-8 is the same hard failure the harness layer
	 * makes it, because everything downstream assumes otherwise.
	 */
	if (memchr(contents, '\0', length) != NULL
	    || !g_utf8_validate(contents, (gssize)length, NULL))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
		            "%s transcript is not valid UTF-8 text", provider);
		return NULL;
	}

	self = g_object_new(AI_TYPE_NATIVE_SESSION, NULL);
	self->provider = g_strdup(provider);
	self->path = g_strdup(path);
	self->session_id = g_strdup(session_id);
	self->kind = reader->kind;

	state.session = self;
	state.boundary = 0;
	state.found = FALSE;
	state.replacement = NULL;

	for_each_record(contents, reader->scan, &state);

	self->compacted = state.found;

	/*
	 * codex hands the post-compaction history to the marker itself, so
	 * that array is the history and the records after it are the
	 * continuation.  Seeding from it before the collect pass is what
	 * makes the two join up in order.
	 */
	if (state.replacement != NULL
	    && JSON_NODE_HOLDS_ARRAY(state.replacement))
	{
		JsonArray *items = json_node_get_array(state.replacement);
		guint      i;

		for (i = 0; i < json_array_get_length(items); i++)
		{
			codex_item(self, ai_json_array_get_object(items, i));
		}
	}

	for_each_record(contents, reader->collect, &state);

	g_clear_pointer(&state.replacement, json_node_unref);

	return (AiNativeSession *)g_steal_pointer(&self);
}

/* ================================================================
 * Locating a client's transcript
 * ================================================================ */

/* The child's explicit environment wins over this process's, because
 * that is the environment the CLI actually ran with. */
static const gchar *
client_env(AiCliClient *client, const gchar *name)
{
	GHashTable  *environment = ai_cli_client_get_environment(client);
	const gchar *value = environment != NULL
		? g_hash_table_lookup(environment, name)
		: NULL;

	return value != NULL ? value : g_getenv(name);
}

static gchar *
client_home(AiCliClient *client)
{
	const gchar *home = client_env(client, "HOME");

	return g_strdup(home != NULL && home[0] != '\0'
	                ? home : g_get_home_dir());
}

static gchar *
client_cwd(AiCliClient *client)
{
	const gchar *directory = ai_cli_client_get_working_directory(client);

	if (directory != NULL && directory[0] != '\0')
	{
		return g_canonicalize_filename(directory, NULL);
	}

	return g_get_current_dir();
}

/*
 * same_directory: whether two paths name the same place.
 *
 * String equality is not enough on this project's own target platform:
 * /home/zach and /var/home/zach are the same directory through a
 * symlink, and a session recorded under one spelling would be invisible
 * to a lookup using the other.
 */
static gboolean
same_directory(const gchar *left, const gchar *right)
{
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;

	if (left == NULL || right == NULL)
	{
		return FALSE;
	}

	if (g_str_equal(left, right))
	{
		return TRUE;
	}

	a = realpath(left, NULL);
	b = realpath(right, NULL);

	return a != NULL && b != NULL && g_str_equal(a, b);
}

/* claude encodes the project directory into a single path component by
 * replacing every separator and dot with a dash. */
static gchar *
claude_encode_directory(const gchar *directory)
{
	GString *encoded = g_string_new(NULL);
	gsize    i;

	for (i = 0; directory[i] != '\0'; i++)
	{
		gchar c = directory[i];

		g_string_append_c(encoded, (c == '/' || c == '.' || c == '_')
		                  ? '-' : c);
	}

	return g_string_free(encoded, FALSE);
}

/*
 * newest_in_directory: the most recently modified entry matching @suffix.
 *
 * Used when no session id is pinned.  Modification time rather than name
 * order, because none of these harnesses names a transcript in a way that
 * sorts chronologically, and the newest one is what `--continue` would
 * have resumed.
 */
static gchar *
newest_in_directory(
	const gchar  *directory,
	const gchar  *suffix,
	gchar       **out_id
){
	g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
	g_autofree gchar *best = NULL;
	g_autofree gchar *best_id = NULL;
	gint64            best_time = 0;
	const gchar      *name;

	if (dir == NULL)
	{
		return NULL;
	}

	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = NULL;
		GStatBuf          info;

		if (!g_str_has_suffix(name, suffix))
		{
			continue;
		}

		path = g_build_filename(directory, name, NULL);

		if (g_stat(path, &info) != 0 || !S_ISREG(info.st_mode))
		{
			continue;
		}

		if ((gint64)info.st_mtime <= best_time)
		{
			continue;
		}

		best_time = (gint64)info.st_mtime;
		g_free(best);
		best = g_steal_pointer(&path);
		g_free(best_id);
		best_id = g_strndup(name, strlen(name) - strlen(suffix));
	}

	if (best != NULL && out_id != NULL)
	{
		*out_id = g_steal_pointer(&best_id);
	}

	return g_steal_pointer(&best);
}

static gchar *
locate_claude(AiCliClient *client, gchar **out_id)
{
	g_autofree gchar *home = client_home(client);
	g_autofree gchar *cwd = client_cwd(client);
	g_autofree gchar *encoded = claude_encode_directory(cwd);
	g_autofree gchar *directory = NULL;
	const gchar      *wanted = ai_cli_client_get_session_id(client);
	const gchar      *base = client_env(client, "CLAUDE_CONFIG_DIR");

	if (base != NULL && base[0] != '\0')
	{
		directory = g_build_filename(base, "projects", encoded, NULL);
	}
	else
	{
		directory = g_build_filename(home, ".claude", "projects", encoded,
		                             NULL);
	}

	if (wanted != NULL && wanted[0] != '\0')
	{
		g_autofree gchar *name = g_strconcat(wanted, ".jsonl", NULL);
		gchar            *path = g_build_filename(directory, name, NULL);

		*out_id = g_strdup(wanted);

		return path;
	}

	return newest_in_directory(directory, ".jsonl", out_id);
}

/*
 * locate_codex: codex files a rollout under sessions/YYYY/MM/DD, so the
 * id is not enough to build a path and the tree has to be walked.  The
 * id is the last field of the filename; the cwd lives inside the file's
 * session_meta rather than in the path, so a pinned id is matched by name
 * and an unpinned one falls back to the newest rollout overall.
 */
static gchar *
locate_codex_walk(
	const gchar  *directory,
	const gchar  *wanted,
	gint64       *best_time,
	gchar       **out_id,
	gchar       **best
){
	g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
	const gchar    *name;

	if (dir == NULL)
	{
		return *best;
	}

	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = g_build_filename(directory, name, NULL);
		GStatBuf          info;

		if (g_stat(path, &info) != 0)
		{
			continue;
		}

		if (S_ISDIR(info.st_mode))
		{
			locate_codex_walk(path, wanted, best_time, out_id, best);
			continue;
		}

		if (!g_str_has_suffix(name, ".jsonl"))
		{
			continue;
		}

		if (wanted != NULL && wanted[0] != '\0')
		{
			g_autofree gchar *needle = g_strconcat(wanted, ".jsonl", NULL);

			if (!g_str_has_suffix(name, needle))
			{
				continue;
			}
		}

		if ((gint64)info.st_mtime <= *best_time)
		{
			continue;
		}

		*best_time = (gint64)info.st_mtime;
		g_free(*best);
		*best = g_steal_pointer(&path);
		g_free(*out_id);
		*out_id = wanted != NULL && wanted[0] != '\0'
			? g_strdup(wanted)
			: g_strndup(name, strlen(name) - strlen(".jsonl"));
	}

	return *best;
}

static gchar *
locate_codex(AiCliClient *client, gchar **out_id)
{
	g_autofree gchar *home = client_home(client);
	g_autofree gchar *root = NULL;
	const gchar      *base = client_env(client, "CODEX_HOME");
	const gchar      *wanted = ai_cli_client_get_session_id(client);
	gchar            *best = NULL;
	gint64            best_time = 0;

	if (base != NULL && base[0] != '\0')
	{
		root = g_build_filename(base, "sessions", NULL);
	}
	else
	{
		root = g_build_filename(home, ".codex", "sessions", NULL);
	}

	locate_codex_walk(root, wanted, &best_time, out_id, &best);

	return best;
}

/*
 * locate_grok: sessions are bucketed by percent-encoded cwd, with a
 * `.cwd` marker file inside the bucket for the cases where the encoding
 * does not round-trip.  Both are checked, because a bucket recorded
 * under /home and a client running in /var/home is the normal state on
 * this project's own platform.
 */
static gchar *
locate_grok(AiCliClient *client, gchar **out_id)
{
	g_autofree gchar *home = client_home(client);
	g_autofree gchar *cwd = client_cwd(client);
	g_autofree gchar *root = NULL;
	const gchar      *base = client_env(client, "GROK_HOME");
	const gchar      *wanted = ai_cli_client_get_session_id(client);
	g_autoptr(GDir)   buckets = NULL;
	g_autofree gchar *best = NULL;
	gint64            best_time = 0;
	const gchar      *bucket_name;

	if (base != NULL && base[0] != '\0')
	{
		root = g_build_filename(base, "sessions", NULL);
	}
	else
	{
		root = g_build_filename(home, ".grok", "sessions", NULL);
	}

	buckets = g_dir_open(root, 0, NULL);

	if (buckets == NULL)
	{
		return NULL;
	}

	while ((bucket_name = g_dir_read_name(buckets)) != NULL)
	{
		g_autofree gchar *decoded = g_uri_unescape_string(bucket_name, NULL);
		g_autofree gchar *bucket = g_build_filename(root, bucket_name, NULL);
		g_autofree gchar *marker = g_build_filename(bucket, ".cwd", NULL);
		g_autofree gchar *recorded = NULL;
		g_autoptr(GDir)   sessions = NULL;
		const gchar      *name;

		if (!same_directory(decoded, cwd))
		{
			if (!g_file_get_contents(marker, &recorded, NULL, NULL)
			    || !same_directory(g_strchomp(recorded), cwd))
			{
				continue;
			}
		}

		sessions = g_dir_open(bucket, 0, NULL);

		if (sessions == NULL)
		{
			continue;
		}

		while ((name = g_dir_read_name(sessions)) != NULL)
		{
			g_autofree gchar *path = NULL;
			GStatBuf          info;

			if (wanted != NULL && wanted[0] != '\0'
			    && !g_str_equal(wanted, name))
			{
				continue;
			}

			path = g_build_filename(bucket, name, "updates.jsonl", NULL);

			if (g_stat(path, &info) != 0 || !S_ISREG(info.st_mode))
			{
				continue;
			}

			if ((gint64)info.st_mtime <= best_time)
			{
				continue;
			}

			best_time = (gint64)info.st_mtime;
			g_free(best);
			best = g_steal_pointer(&path);
			g_free(*out_id);
			*out_id = g_strdup(name);
		}
	}

	return g_steal_pointer(&best);
}

/**
 * ai_cli_client_read_native_session:
 * @client: an #AiCliClient
 * @error: return location for a #GError
 *
 * Finds and reads @client's own session transcript.
 *
 * The transcript is located from the client's working directory,
 * `session-id` and environment --- the same three things the wrapped
 * program itself used to choose the file --- so a client configured to
 * run somewhere reads the history it made there.  With no `session-id`
 * set, the most recently modified transcript for that directory is used,
 * which is the one `--continue` would have resumed.
 *
 * Fails with %AI_ERROR_NOT_SUPPORTED for a provider whose store this
 * library cannot read, and with %AI_ERROR_CLI_NOT_FOUND when there is no
 * transcript yet --- a first turn that has not run is not an error
 * condition callers should log loudly.
 *
 * Returns: (transfer full) (nullable): the session, or %NULL on error
 */
AiNativeSession *
ai_cli_client_read_native_session(
	AiCliClient  *client,
	GError      **error
){
	const NativeReader *reader;
	g_autofree gchar   *path = NULL;
	g_autofree gchar   *id = NULL;

	g_return_val_if_fail(AI_IS_CLI_CLIENT(client), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	reader = reader_for(G_OBJECT_TYPE_NAME(client));

	if (reader == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		            "%s keeps no readable session transcript",
		            ai_provider_get_name(AI_PROVIDER(client)));
		return NULL;
	}

	if (g_str_has_prefix(reader->provider, "claude"))
	{
		path = locate_claude(client, &id);
	}
	else if (g_str_equal(reader->provider, "codex-cli"))
	{
		path = locate_codex(client, &id);
	}
	else
	{
		path = locate_grok(client, &id);
	}

	if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND,
		            "no %s session transcript for this directory",
		            reader->provider);
		return NULL;
	}

	return ai_native_session_read_file(reader->provider, path, id, error);
}

/* ================================================================
 * Accessors and projection
 * ================================================================ */

/**
 * ai_native_session_get_provider:
 * @self: an #AiNativeSession
 *
 * Returns: (transfer none): the provider the transcript came from
 */
const gchar *
ai_native_session_get_provider(AiNativeSession *self)
{
	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self), NULL);

	return self->provider;
}

/**
 * ai_native_session_get_session_id:
 * @self: an #AiNativeSession
 *
 * Returns: (transfer none) (nullable): the native session id, if known
 */
const gchar *
ai_native_session_get_session_id(AiNativeSession *self)
{
	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self), NULL);

	return self->session_id;
}

/**
 * ai_native_session_get_path:
 * @self: an #AiNativeSession
 *
 * Returns: (transfer none): the file the history was read from
 */
const gchar *
ai_native_session_get_path(AiNativeSession *self)
{
	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self), NULL);

	return self->path;
}

/**
 * ai_native_session_get_kind:
 * @self: an #AiNativeSession
 *
 * Returns: how the store was held
 */
AiNativeSessionKind
ai_native_session_get_kind(AiNativeSession *self)
{
	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self),
	                     AI_NATIVE_SESSION_UNSUPPORTED);

	return self->kind;
}

/**
 * ai_native_session_get_messages:
 * @self: an #AiNativeSession
 *
 * The history since the last compaction, oldest first.
 *
 * Returns: (transfer none) (element-type AiMessage): the messages
 */
GList *
ai_native_session_get_messages(AiNativeSession *self)
{
	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self), NULL);

	return self->messages;
}

/**
 * ai_native_session_get_compacted:
 * @self: an #AiNativeSession
 *
 * Whether a compaction boundary was found and used as the start.
 *
 * %FALSE means the whole transcript was taken, which is what a session
 * that has never been compacted should yield --- not an error, and not
 * an empty result.
 *
 * Returns: %TRUE if the history starts at a compaction boundary
 */
gboolean
ai_native_session_get_compacted(AiNativeSession *self)
{
	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self), FALSE);

	return self->compacted;
}

/**
 * ai_native_session_get_dropped:
 * @self: an #AiNativeSession
 *
 * Returns: how many records before the compaction boundary were skipped
 */
guint
ai_native_session_get_dropped(AiNativeSession *self)
{
	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self), 0);

	return self->dropped;
}

/**
 * ai_native_session_to_context_text:
 * @self: an #AiNativeSession
 * @max_bytes: a byte ceiling for the result, or 0 for no limit
 *
 * Renders the history as labelled text for a system prompt.
 *
 * When the rendering exceeds @max_bytes the *oldest* messages are
 * dropped, not the newest, and a marker says so.  Recency is what a
 * continuing conversation needs; a digest trimmed from the other end
 * would preserve the opening pleasantries and lose the thing just being
 * worked on.  The cut is always at a message boundary, so the result
 * never ends mid-exchange.
 *
 * Returns: (transfer full) (nullable): the digest, or %NULL if empty
 */
gchar *
ai_native_session_to_context_text(
	AiNativeSession *self,
	gsize            max_bytes
){
	g_autoptr(GPtrArray) rendered = NULL;
	g_autoptr(GString)   out = NULL;
	GList               *iter;
	gsize                total = 0;
	guint                first = 0;
	guint                i;

	g_return_val_if_fail(AI_IS_NATIVE_SESSION(self), NULL);

	if (self->messages == NULL)
	{
		return NULL;
	}

	rendered = g_ptr_array_new_with_free_func(g_free);

	for (iter = self->messages; iter != NULL; iter = iter->next)
	{
		AiMessage        *message = AI_MESSAGE(iter->data);
		g_autofree gchar *text = ai_message_get_text(message);
		const gchar      *label;

		if (text == NULL || text[0] == '\0')
		{
			continue;
		}

		switch (ai_message_get_role(message))
		{
			case AI_ROLE_USER:
				label = "User";
				break;
			case AI_ROLE_ASSISTANT:
				label = "Assistant";
				break;
			default:
				label = "System";
				break;
		}

		g_ptr_array_add(rendered, g_strdup_printf("%s: %s", label, text));
	}

	if (rendered->len == 0)
	{
		return NULL;
	}

	/*
	 * Size the tail first, then render it.  Building the whole string and
	 * cutting it afterwards would mean allocating the very thing the
	 * limit exists to avoid.
	 */
	if (max_bytes > 0)
	{
		for (i = rendered->len; i > 0; i--)
		{
			gsize size = strlen(g_ptr_array_index(rendered, i - 1)) + 2;

			if (total + size > max_bytes && i < rendered->len)
			{
				first = i;
				break;
			}

			total += size;
		}
	}

	out = g_string_new(NULL);
	g_string_append_printf(out,
		"The following is the conversation so far, carried over from the "
		"%s CLI session%s%s. It is a text projection of that session's own "
		"transcript: tool calls and their results are labelled rather than "
		"replayed as native tool events. Continue this conversation.\n\n",
		self->provider,
		self->session_id != NULL ? " " : "",
		self->session_id != NULL ? self->session_id : "");

	if (self->compacted)
	{
		g_string_append(out,
			"[Earlier turns were compacted away by that CLI and are "
			"unavailable.]\n\n");
	}

	if (first > 0)
	{
		g_string_append_printf(out,
			"[%u earlier message%s omitted to fit the context limit.]\n\n",
			first, first == 1 ? "" : "s");
	}

	for (i = first; i < rendered->len; i++)
	{
		g_string_append(out, g_ptr_array_index(rendered, i));
		g_string_append(out, "\n\n");
	}

	return g_strdup(out->str);
}
