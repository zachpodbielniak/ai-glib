/*
 * ai-claude-tmux-client.c - Claude Code CLI client driven via tmux
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See header for design rationale.  In short: the `claude` TUI insists
 * its stdin be a pty (it enters raw mode on startup); tmux gives us
 * that for free without us having to forkpty() and manage the pty
 * lifecycle ourselves.  The send-keys / @<file> trick is how we
 * deliver the prompt into the TUI without paying for the Agent SDK
 * non-interactive billing path.
 */

#include "config.h"

#include "providers/ai-claude-tmux-client.h"
#include "providers/ai-claude-tmux-client-internal.h"
#include "providers/ai-claude-launch.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#include "core/ai-error.h"
#include "core/ai-provider.h"
#include "core/ai-subprocess-util.h"
#include "model/ai-message.h"
#include "model/ai-text-content.h"
#include "model/ai-response.h"
#include "model/ai-usage.h"

/* ------------------------------------------------------------------ */
/* Class definition                                                    */
/* ------------------------------------------------------------------ */

struct _AiClaudeTmuxClient
{
    AiCliClient parent_instance;

    gchar    *tmux_path;            /* override for tmux binary */
    gchar    *claude_project_dir;   /* override for ~/.claude/projects */
    gint      turn_timeout_ms;      /* default 10 min */
    gint      startup_timeout_ms;   /* default 30 sec */
    gboolean  skip_permissions;     /* --dangerously-skip-permissions */
    gboolean  keep_artifacts;       /* leave prompt/sentinel on disk */
    gboolean  debug_preserve_tmux;   /* keep tmux session + artifacts alive */
    gint      prompt_resend_interval_ms; /* wait-for-user-entry window before
                                          * re-pressing Enter (default 2 sec) */
    gint      max_prompt_send_attempts;  /* max Enter keystrokes to land the
                                          * prompt before failing (default 5) */
    gboolean  dismiss_resume_prompt;     /* press Enter once on resume to
                                          * clear claude's resume-mode
                                          * picker (default TRUE) */
    gboolean  prompt_send_exponential_backoff; /* double the per-attempt
                                                * wait each retry instead
                                                * of waiting a fixed
                                                * prompt_resend_interval_ms
                                                * every time (default TRUE) */
    gint      command_timeout_ms;   /* deadline for each tmux plumbing
                                     * command (new-session, send-keys,
                                     * kill-session, ...); a wedged tmux
                                     * server otherwise blocks the turn
                                     * worker forever (default 30 sec,
                                     * 0 disables) */
    gint      turn_active;          /* atomic flag: 1 while this client
                                     * owns a live tmux session (a turn
                                     * is in flight), 0 otherwise — gates
                                     * orphaned-session reaping */
    gdouble   total_cost;           /* last parsed cost in USD */
};

enum
{
    PROP_0,
    PROP_TMUX_PATH,
    PROP_CLAUDE_PROJECT_DIR,
    PROP_TURN_TIMEOUT_MS,
    PROP_STARTUP_TIMEOUT_MS,
    PROP_SKIP_PERMISSIONS,
    PROP_KEEP_ARTIFACTS,
    PROP_DEBUG_PRESERVE_TMUX,
    PROP_PROMPT_RESEND_INTERVAL_MS,
    PROP_MAX_PROMPT_SEND_ATTEMPTS,
    PROP_DISMISS_RESUME_PROMPT,
    PROP_PROMPT_SEND_EXPONENTIAL_BACKOFF,
    PROP_COMMAND_TIMEOUT_MS,
    PROP_TOTAL_COST,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void ai_claude_tmux_client_provider_init(AiProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiClaudeTmuxClient, ai_claude_tmux_client,
                        AI_TYPE_CLI_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_claude_tmux_client_provider_init))

/* ------------------------------------------------------------------ */
/* Pure helpers                                                        */
/* ------------------------------------------------------------------ */

gchar *
ai_claude_tmux_client_encode_cwd(const gchar *cwd)
{
    GString *out;
    const gchar *p;

    g_return_val_if_fail(cwd != NULL, NULL);

    /* Claude's per-project subdir name is the absolute path with
     * every '/' replaced by '-'.  We don't normalize the input (no
     * realpath, no trailing-slash trimming) because we have no way
     * to know what claude itself does — and if our encoding diverges
     * from claude's we'll just fail to find the JSONL.  Best to
     * mirror exactly: byte-for-byte except the slash substitution. */
    out = g_string_sized_new(strlen(cwd) + 1);
    for (p = cwd; *p != '\0'; p++)
    {
        g_string_append_c(out, (*p == '/') ? '-' : *p);
    }
    return g_string_free(out, FALSE);
}

gchar *
ai_claude_tmux_client_compute_jsonl_path(
    const gchar *project_dir,
    const gchar *cwd,
    const gchar *session_id
){
    g_autofree gchar *encoded = NULL;
    g_autofree gchar *default_root = NULL;
    g_autofree gchar *filename = NULL;
    const gchar *root;

    g_return_val_if_fail(cwd != NULL, NULL);
    g_return_val_if_fail(session_id != NULL, NULL);

    if (project_dir != NULL && project_dir[0] != '\0')
    {
        root = project_dir;
    }
    else
    {
        default_root = g_build_filename(g_get_home_dir(),
                                        ".claude", "projects", NULL);
        root = default_root;
    }

    encoded = ai_claude_tmux_client_encode_cwd(cwd);
    filename = g_strconcat(session_id, ".jsonl", NULL);

    return g_build_filename(root, encoded, filename, NULL);
}

/*
 * Extract text from an assistant message's content array.
 * Concatenates every "type": "text" block's text field; ignores
 * anything else (tool_use, tool_result, thinking, redacted_thinking,
 * future block types).
 */
static gchar *
extract_text_from_content(JsonArray *content_arr)
{
    GString *out;
    guint i, n;

    if (content_arr == NULL)
    {
        return NULL;
    }

    out = g_string_new("");
    n = json_array_get_length(content_arr);
    for (i = 0; i < n; i++)
    {
        JsonNode *node;
        JsonObject *block;
        const gchar *btype;

        node = json_array_get_element(content_arr, i);
        if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        {
            continue;
        }

        block = json_node_get_object(node);
        btype = json_object_get_string_member_with_default(block, "type", "");
        if (g_strcmp0(btype, "text") == 0)
        {
            const gchar *text;
            text = json_object_get_string_member_with_default(block, "text", "");
            if (text != NULL && text[0] != '\0')
            {
                g_string_append(out, text);
            }
        }
    }

    if (out->len == 0)
    {
        /* No text blocks at all — free everything and return NULL.
         * Caller treats NULL as "this assistant entry produced no
         * user-visible text" (e.g. it was pure tool_use). */
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

/*
 * ai_claude_tmux_client_jsonl_has_accepted_prompt:
 *
 * Returns TRUE if @jsonl_slice (the transcript bytes written after the
 * pre-prompt watermark) shows that claude ACCEPTED a freshly-submitted
 * prompt — i.e. the submit Enter registered.  Acceptance has two
 * distinct shapes, and recognising both is the whole point of this
 * function:
 *
 *   - a `type:"user"` entry that is NOT a compaction summary.  This
 *     is the "claude was idle, started the turn immediately" case.
 *     The compaction-summary exclusion matters: when claude finishes
 *     an auto-compaction it logs its summary as a `type:"user"` entry
 *     with `isCompactSummary:true` — that is claude talking to
 *     itself, not our prompt landing, and counting it is a false
 *     positive.
 *
 *   - a `type:"queue-operation"` entry with `operation:"enqueue"`.
 *     claude's TUI writes this when a message is submitted while it
 *     is BUSY — running a turn, or (the case that bit us) auto-
 *     compacting a large resumed session.  The prompt is queued, not
 *     lost; it runs the moment claude is free.  The original code
 *     only looked for `type:"user"` and so mistook this for a
 *     swallowed keystroke: it re-sent Enter five times, gave up, and
 *     killed claude mid-compaction with our message still queued.
 *
 * Its ABSENCE — after we've pressed Enter and given the TUI a beat —
 * means the keystroke really was swallowed (tmux send-keys racing the
 * TUI's input-box render) and the pasted prompt is still sitting
 * un-submitted.  That is the cue to press Enter again.
 *
 * Parsing is per-line and tolerant: a partial / corrupt line (claude
 * writes the transcript incrementally and we tail-read it) simply
 * fails to parse and is skipped — the next poll re-reads once more
 * bytes land.  A bare "did the file grow?" check would be fooled by
 * exactly such a partial write, or by an unrelated metadata entry.
 *
 * Exposed (non-static) primarily so the acceptance logic can be unit
 * tested without spawning a real claude.
 */
gboolean
ai_claude_tmux_client_jsonl_has_accepted_prompt(const gchar *jsonl_slice)
{
    g_auto(GStrv) lines = NULL;
    guint i;

    g_return_val_if_fail(jsonl_slice != NULL, FALSE);

    lines = g_strsplit(jsonl_slice, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        g_autoptr(JsonParser) parser = NULL;
        JsonNode *root;
        JsonObject *obj;
        const gchar *type;

        if (lines[i][0] == '\0')
        {
            continue;
        }
        parser = json_parser_new();
        if (!json_parser_load_from_data(parser, lines[i], -1, NULL))
        {
            /* Partial / corrupt line — keep walking; the next poll
             * iteration will re-read once more bytes land. */
            continue;
        }
        root = json_parser_get_root(parser);
        if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        {
            continue;
        }
        obj = json_node_get_object(root);
        type = json_object_get_string_member_with_default(obj, "type", "");

        if (g_strcmp0(type, "user") == 0)
        {
            /* A real user prompt counts; a compaction summary (also
             * logged as type:"user") does not. */
            if (!json_object_get_boolean_member_with_default(
                    obj, "isCompactSummary", FALSE))
            {
                return TRUE;
            }
        }
        else if (g_strcmp0(type, "queue-operation") == 0)
        {
            /* Submitted while claude was busy — queued, not lost. */
            const gchar *op;
            op = json_object_get_string_member_with_default(
                obj, "operation", "");
            if (g_strcmp0(op, "enqueue") == 0)
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/*
 * File-I/O wrapper around ai_claude_tmux_client_jsonl_has_accepted_prompt:
 * reads @jsonl_path and hands the bytes after @from_offset (the
 * pre-prompt watermark) to the pure checker.  A file that doesn't
 * exist yet, or hasn't grown past the watermark, trivially shows no
 * accepted prompt.
 */
static gboolean
slice_has_accepted_prompt(
    const gchar *jsonl_path,
    goffset      from_offset
){
    g_autofree gchar *content = NULL;
    gsize content_len = 0;

    if (!g_file_get_contents(jsonl_path, &content, &content_len, NULL))
    {
        return FALSE;
    }
    if ((goffset)content_len <= from_offset)
    {
        return FALSE;
    }
    return ai_claude_tmux_client_jsonl_has_accepted_prompt(
        content + from_offset);
}

/*
 * Returns TRUE if the slice of @jsonl_path starting at byte offset
 * @from_offset contains a top-level `"type":"assistant"` entry whose
 * inner `message.stop_reason` is a TERMINAL stop reason (anything
 * other than `tool_use`).
 *
 * We can't rely on either of the simpler signals after the Stop hook
 * fires:
 *   - "did the file grow?"   — claude flushes the user-message line
 *     ahead of its own response in some cases, so the file can grow
 *     while still containing only the previous turn's last assistant
 *     entry.  Parsing in that window returns a stale echo.
 *   - "is there any new assistant entry?" — during a tool_use chain
 *     intermediate entries are written with stop_reason:"tool_use"
 *     before the terminal one lands.  We want the terminal entry.
 *
 * claude only fires the Stop hook on terminal stop_reasons, so by the
 * time we get here we WILL eventually see a terminal entry; we just
 * need to wait out the JSONL flush.  Anything not yet flushed is
 * "future" — keep polling.
 */
static gboolean
slice_has_terminal_assistant_entry(
    const gchar *jsonl_path,
    goffset      from_offset
){
    g_autofree gchar *content = NULL;
    gsize content_len = 0;
    g_auto(GStrv) lines = NULL;
    guint i;

    if (!g_file_get_contents(jsonl_path, &content, &content_len, NULL))
    {
        return FALSE;
    }
    if ((goffset)content_len <= from_offset)
    {
        return FALSE;
    }

    lines = g_strsplit(content + from_offset, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        g_autoptr(JsonParser) parser = NULL;
        JsonNode *root;
        JsonObject *obj;
        JsonObject *msg;
        const gchar *type;
        const gchar *stop_reason;

        if (lines[i][0] == '\0')
        {
            continue;
        }
        parser = json_parser_new();
        if (!json_parser_load_from_data(parser, lines[i], -1, NULL))
        {
            /* Partial / corrupt line — keep walking; the next poll
             * iteration will re-read once more bytes land. */
            continue;
        }
        root = json_parser_get_root(parser);
        if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        {
            continue;
        }
        obj = json_node_get_object(root);
        type = json_object_get_string_member_with_default(obj, "type", "");
        if (g_strcmp0(type, "assistant") != 0)
        {
            continue;
        }
        if (!json_object_has_member(obj, "message"))
        {
            continue;
        }
        msg = json_object_get_object_member(obj, "message");
        if (msg == NULL)
        {
            continue;
        }
        stop_reason = json_object_get_string_member_with_default(
            msg, "stop_reason", "");
        /* end_turn, stop_sequence, max_tokens are terminal.  tool_use
         * is intermediate — keep waiting for the post-tool follow-up
         * to land. */
        if (stop_reason != NULL && stop_reason[0] != '\0' &&
            g_strcmp0(stop_reason, "tool_use") != 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

AiResponse *
ai_claude_tmux_client_parse_jsonl(
    const gchar  *content,
    const gchar  *model,
    gdouble      *total_cost_out,
    GError      **error
){
    g_auto(GStrv) lines = NULL;
    guint i;
    /* State accumulated from the last assistant entry encountered. */
    g_autofree gchar *last_text = NULL;
    g_autofree gchar *last_session_id = NULL;
    g_autofree gchar *last_model = NULL;
    g_autofree gchar *last_stop_reason = NULL;
    gint    last_input_tokens = 0;
    gint    last_output_tokens = 0;
    gdouble last_cost = 0.0;
    gboolean found_assistant = FALSE;
    g_autoptr(AiResponse) response = NULL;

    g_return_val_if_fail(content != NULL, NULL);

    if (total_cost_out != NULL)
    {
        *total_cost_out = 0.0;
    }

    /* JSONL is line-oriented.  We deliberately don't bail on a single
     * malformed line — claude writes the file incrementally and a
     * read that races a write can produce a half line.  We do bail if
     * there's no assistant entry by EOF. */
    lines = g_strsplit(content, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        g_autoptr(JsonParser) parser = NULL;
        JsonNode *root;
        JsonObject *obj;
        JsonObject *msg;
        const gchar *type;
        const gchar *role;

        if (lines[i][0] == '\0')
        {
            continue;
        }

        parser = json_parser_new();
        if (!json_parser_load_from_data(parser, lines[i], -1, NULL))
        {
            /* Partial / corrupt line — skip. */
            continue;
        }

        root = json_parser_get_root(parser);
        if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        {
            continue;
        }

        obj = json_node_get_object(root);

        type = json_object_get_string_member_with_default(obj, "type", "");
        if (g_strcmp0(type, "assistant") != 0)
        {
            continue;
        }

        /* Top-level may carry sessionId. */
        if (json_object_has_member(obj, "sessionId"))
        {
            const gchar *sid;
            sid = json_object_get_string_member_with_default(obj, "sessionId", "");
            if (sid != NULL && sid[0] != '\0')
            {
                g_free(last_session_id);
                last_session_id = g_strdup(sid);
            }
        }

        if (!json_object_has_member(obj, "message"))
        {
            continue;
        }
        msg = json_object_get_object_member(obj, "message");
        if (msg == NULL)
        {
            continue;
        }

        /* Some transcript entries use role to disambiguate; assistant
         * type already implies role=assistant but we double-check
         * defensively for forward compatibility. */
        role = json_object_get_string_member_with_default(msg, "role", "assistant");
        if (g_strcmp0(role, "assistant") != 0)
        {
            continue;
        }

        /* This IS an assistant entry — record everything; later
         * entries will overwrite, which is what we want (last one
         * wins; that's the response we hand back). */
        found_assistant = TRUE;

        g_free(last_text);
        last_text = NULL;
        if (json_object_has_member(msg, "content"))
        {
            JsonNode *cnode = json_object_get_member(msg, "content");
            if (cnode != NULL && JSON_NODE_HOLDS_ARRAY(cnode))
            {
                last_text = extract_text_from_content(
                    json_node_get_array(cnode));
            }
        }

        if (json_object_has_member(msg, "model"))
        {
            const gchar *m;
            m = json_object_get_string_member_with_default(msg, "model", "");
            if (m != NULL && m[0] != '\0')
            {
                g_free(last_model);
                last_model = g_strdup(m);
            }
        }

        if (json_object_has_member(msg, "stop_reason"))
        {
            const gchar *sr;
            sr = json_object_get_string_member_with_default(msg, "stop_reason", "");
            g_free(last_stop_reason);
            last_stop_reason = g_strdup(sr != NULL ? sr : "");
        }

        if (json_object_has_member(msg, "usage"))
        {
            JsonObject *usage_obj;
            usage_obj = json_object_get_object_member(msg, "usage");
            if (usage_obj != NULL)
            {
                last_input_tokens = json_object_get_int_member_with_default(
                    usage_obj, "input_tokens", 0);
                last_output_tokens = json_object_get_int_member_with_default(
                    usage_obj, "output_tokens", 0);
            }
        }

        if (json_object_has_member(obj, "total_cost_usd"))
        {
            last_cost = json_object_get_double_member(obj, "total_cost_usd");
        }
        else if (json_object_has_member(msg, "total_cost_usd"))
        {
            last_cost = json_object_get_double_member(msg, "total_cost_usd");
        }
    }

    if (!found_assistant)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                    "Transcript contained no assistant entry");
        return NULL;
    }

    response = ai_response_new(
        last_session_id != NULL ? last_session_id : "",
        model != NULL ? model :
            (last_model != NULL ? last_model : ""));

    /* Map claude's stop_reason string to our enum.  end_turn is the
     * normal "done talking" case; anything else (tool_use,
     * stop_sequence, max_tokens) we currently flatten to END_TURN
     * since by the time the Stop hook fires, claude is genuinely
     * done with the turn from the user's perspective. */
    ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);

    if (last_text != NULL && last_text[0] != '\0')
    {
        g_autoptr(AiTextContent) tc = ai_text_content_new(last_text);
        ai_response_add_content_block(response,
                                      (AiContentBlock *)g_steal_pointer(&tc));
    }

    if (last_input_tokens > 0 || last_output_tokens > 0)
    {
        g_autoptr(AiUsage) usage = ai_usage_new(last_input_tokens,
                                                last_output_tokens);
        ai_response_set_usage(response, usage);
    }

    if (total_cost_out != NULL)
    {
        *total_cost_out = last_cost;
    }

    return g_steal_pointer(&response);
}

/* ------------------------------------------------------------------ */
/* Session lifecycle helpers                                           */
/* ------------------------------------------------------------------ */

/*
 * Compute the per-user runtime directory for our scratch files.
 * Returns an allocated string; caller frees with g_free.  Always
 * creates the directory (0700) if it doesn't exist.
 */
static gchar *
get_runtime_dir(GError **error)
{
    const gchar *runtime;
    g_autofree gchar *dir = NULL;

    runtime = g_get_user_runtime_dir();
    if (runtime == NULL || runtime[0] == '\0')
    {
        /* Fall back to /tmp/$USER, which g_get_tmp_dir+username
         * mimics what g_get_user_runtime_dir does when XDG_RUNTIME_DIR
         * is unset on a non-systemd host. */
        runtime = g_get_tmp_dir();
    }

    dir = g_build_filename(runtime, "ai-glib-tmux", NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                    "Cannot create runtime dir '%s'", dir);
        return NULL;
    }
    return g_steal_pointer(&dir);
}

/*
 * Atomically write @content to @path (mode 0600).  g_file_set_contents
 * is itself atomic (writes to a temp file then renames) — we rely on
 * that — but we additionally chmod the result so the prompt isn't
 * world-readable.
 */
static gboolean
write_prompt_file_atomic(
    const gchar  *path,
    const gchar  *content,
    GError      **error
){
    if (!g_file_set_contents(path, content, -1, error))
    {
        return FALSE;
    }
    /* chmod after rename — there is a tiny window where the file is
     * mode 0666 & umask, but it's inside our 0700 runtime dir so
     * cross-user reads are still blocked. */
    if (g_chmod(path, 0600) != 0)
    {
        /* Best-effort — log and continue. */
        g_debug("Could not chmod prompt file '%s': %s",
                path, g_strerror(errno));
    }
    return TRUE;
}

/*
 * Helper: emit one hook block { "matcher": "", "hooks": [{ type, command }] }.
 */
static void
emit_hook_entry(JsonBuilder *builder, const gchar *shell_cmd)
{
    json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "matcher");
        json_builder_add_string_value(builder, "");
        json_builder_set_member_name(builder, "hooks");
        json_builder_begin_array(builder);
            json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "type");
                json_builder_add_string_value(builder, "command");
                json_builder_set_member_name(builder, "command");
                json_builder_add_string_value(builder, shell_cmd);
            json_builder_end_object(builder);
        json_builder_end_array(builder);
    json_builder_end_object(builder);
}

/*
 * Build the JSON blob we pass to claude via --settings.  Configures
 * two hooks:
 *   SessionStart — touches ready_path when claude is initialised and
 *                  the TUI input box is live.  Used to gate
 *                  prompt-delivery so we don't fire send-keys into
 *                  a still-loading TUI.
 *   Stop         — touches done_path when claude finishes a turn.
 */
static gchar *
build_settings_json(const gchar *ready_path, const gchar *done_path)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) gen = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *ready_cmd = NULL;
    g_autofree gchar *done_cmd = NULL;

    /* `: > 'path'` is a portable touch — `:` is the no-op builtin and
     * the redirect creates an empty file.  Paths are single-quoted;
     * we use UUID-named files so embedded quotes are not possible. */
    ready_cmd = g_strdup_printf(": > '%s'", ready_path);
    done_cmd  = g_strdup_printf(": > '%s'", done_path);

    builder = json_builder_new();
    json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "hooks");
        json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "SessionStart");
            json_builder_begin_array(builder);
                emit_hook_entry(builder, ready_cmd);
            json_builder_end_array(builder);
            json_builder_set_member_name(builder, "Stop");
            json_builder_begin_array(builder);
                emit_hook_entry(builder, done_cmd);
            json_builder_end_array(builder);
        json_builder_end_object(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    gen = json_generator_new();
    json_generator_set_root(gen, root);
    return json_generator_to_data(gen, NULL);
}

/*
 * Block until a path exists or the timeout expires.  Uses a single
 * GMainContext / GMainLoop with a GFileMonitor on the parent dir and
 * a g_timeout source for the deadline.  Returns TRUE if the file
 * appeared, FALSE on timeout.
 */
typedef struct
{
    GMainLoop *loop;
    gboolean   appeared;
    gchar     *target_basename;
} WaitForFileCtx;

static void
on_dir_changed(
    GFileMonitor      *monitor,
    GFile             *file,
    GFile             *other,
    GFileMonitorEvent  event,
    gpointer           user_data
){
    WaitForFileCtx *ctx = user_data;
    g_autofree gchar *bn = NULL;

    (void)monitor;
    (void)other;

    if (event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    {
        return;
    }
    bn = g_file_get_basename(file);
    if (g_strcmp0(bn, ctx->target_basename) == 0)
    {
        ctx->appeared = TRUE;
        g_main_loop_quit(ctx->loop);
    }
}

static gboolean
on_wait_timeout(gpointer user_data)
{
    WaitForFileCtx *ctx = user_data;
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
}

static gboolean
wait_for_file(
    const gchar   *path,
    gint           timeout_ms,
    GCancellable  *cancellable,
    GError       **error
){
    g_autoptr(GFile) target = NULL;
    g_autoptr(GFile) parent = NULL;
    g_autoptr(GFileMonitor) monitor = NULL;
    g_autoptr(GMainContext) ctx_main = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    WaitForFileCtx ctx = { 0 };
    guint timeout_id;
    gulong sig_id = 0;
    gulong cancel_id = 0;

    /* Fast path: already exists. */
    if (g_file_test(path, G_FILE_TEST_EXISTS))
    {
        return TRUE;
    }

    target = g_file_new_for_path(path);
    parent = g_file_get_parent(target);
    if (parent == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                    "Path '%s' has no parent directory", path);
        return FALSE;
    }

    ctx_main = g_main_context_new();
    g_main_context_push_thread_default(ctx_main);
    loop = g_main_loop_new(ctx_main, FALSE);

    monitor = g_file_monitor_directory(parent, G_FILE_MONITOR_NONE,
                                       cancellable, error);
    if (monitor == NULL)
    {
        g_main_context_pop_thread_default(ctx_main);
        return FALSE;
    }

    ctx.loop = loop;
    ctx.appeared = FALSE;
    ctx.target_basename = g_file_get_basename(target);

    sig_id = g_signal_connect(monitor, "changed",
                              G_CALLBACK(on_dir_changed), &ctx);

    /* Re-check existence AFTER hooking the monitor — closes a TOCTOU
     * window where the file appears between the test above and the
     * monitor being live. */
    if (g_file_test(path, G_FILE_TEST_EXISTS))
    {
        ctx.appeared = TRUE;
        goto out;
    }

    timeout_id = g_timeout_add(timeout_ms, on_wait_timeout, &ctx);

    if (cancellable != NULL)
    {
        cancel_id = g_cancellable_connect(cancellable,
            G_CALLBACK(g_main_loop_quit), loop, NULL);
    }

    /*
     * If the cancellable was ALREADY triggered before we connected, the
     * immediate g_main_loop_quit fired against a loop that is not yet
     * running and was simply lost — g_main_loop_run() would then set
     * is_running = TRUE and block for the full timeout, ignoring the
     * cancellation.  Skip the run entirely in that case so a !kill that
     * lands between two waits still takes effect at once.
     */
    if (!(cancellable != NULL && g_cancellable_is_cancelled(cancellable)))
    {
        g_main_loop_run(loop);
    }

    if (cancel_id != 0)
    {
        g_cancellable_disconnect(cancellable, cancel_id);
    }
    g_source_remove(timeout_id);

out:
    g_signal_handler_disconnect(monitor, sig_id);
    g_main_context_pop_thread_default(ctx_main);
    g_free(ctx.target_basename);

    if (!ctx.appeared)
    {
        if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CANCELLED,
                        "Cancelled while waiting for '%s'", path);
        }
        else
        {
            g_set_error(error, AI_ERROR, AI_ERROR_TIMEOUT,
                        "Timed out waiting for '%s' (%d ms)",
                        path, timeout_ms);
        }
        return FALSE;
    }
    return TRUE;
}

/*
 * Wait for the Stop-hook @sentinel_path to appear, bounding the wait by
 * INACTIVITY rather than a hard wall-clock deadline.
 *
 * Why this exists: a single long-running agentic turn (claude making
 * dozens of tool calls) can legitimately run far longer than any fixed
 * turn budget.  The old code waited for the sentinel with a hard
 * @idle_timeout_ms cap, so a turn that was actively working — but not
 * yet finished — got killed mid-flight at the deadline and its response
 * was lost.  This is the "ran for 10-20 minutes then no reply" bug.
 *
 * Claude Code appends to the JSONL transcript incrementally as the turn
 * progresses (each assistant step, tool_use, and tool_result is flushed
 * as it happens).  So transcript growth is a reliable "still working"
 * signal: every time @activity_path grows we reset the idle clock.  The
 * wait only fails once the transcript has been COMPLETELY silent — no
 * growth and no sentinel — for @idle_timeout_ms, which means claude is
 * genuinely wedged, not merely slow.  A user who wants to abort a
 * still-active turn can !stop / !kill it (handled separately).
 */
gboolean
ai_claude_tmux_client_wait_for_sentinel_or_idle(
    const gchar  *sentinel_path,
    const gchar  *activity_path,
    gint          idle_timeout_ms,
    GCancellable *cancellable,
    GError      **error
){
    const gint poll_ms = 200;
    gint       idle_ms = 0;
    goffset    last_size = -1;
    GStatBuf   st;

    /* Seed the activity watermark with the file's current size so that
     * "growth" is measured from now, not from an empty file. */
    if (g_stat(activity_path, &st) == 0)
        last_size = (goffset)st.st_size;

    for (;;)
    {
        /* Turn finished: the Stop hook touched the sentinel. */
        if (g_file_test(sentinel_path, G_FILE_TEST_EXISTS))
            return TRUE;

        /* Honour !stop / !kill immediately. */
        if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CANCELLED,
                        "Cancelled while waiting for the turn to finish");
            return FALSE;
        }

        /* Any transcript growth means claude is still working — reset
         * the idle clock. */
        if (g_stat(activity_path, &st) == 0)
        {
            goffset size = (goffset)st.st_size;
            if (size != last_size)
            {
                last_size = size;
                idle_ms = 0;
            }
        }

        if (idle_ms >= idle_timeout_ms)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_TIMEOUT,
                        "No transcript activity and no Stop hook for "
                        "%d ms — the claude turn appears wedged",
                        idle_timeout_ms);
            return FALSE;
        }

        g_usleep((gulong)poll_ms * 1000);
        idle_ms += poll_ms;
    }
}

/*
 * Run a one-shot child process synchronously, capturing exit status.
 * Returns TRUE on exit-zero, FALSE otherwise.  When @capture_stderr
 * is non-NULL, stderr is captured (used for diagnostics on failure).
 *
 * Every call is bounded: @timeout_ms caps the wall-clock wait (0
 * disables) and @cancellable aborts it early — in both cases the
 * child is killed.  tmux plumbing commands complete in milliseconds
 * when healthy; an unbounded wait here once pinned a libreclaw
 * session worker forever behind a wedged tmux server.
 */
static gboolean
run_command_sync(
    const gchar * const  *argv,
    gchar               **capture_stderr,
    gint                  timeout_ms,
    GCancellable         *cancellable,
    GError              **error
){
    g_autoptr(GSubprocess) sub = NULL;
    g_autofree gchar *stderr_owned = NULL;
    gchar **stderr_dest;
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                           | G_SUBPROCESS_FLAGS_STDERR_PIPE;

    /*
     * Always capture stderr so we can include it in the GError on
     * non-zero exit.  When the caller didn't ask for it, store it in
     * a local that goes out of scope here.
     */
    stderr_dest = capture_stderr != NULL ? capture_stderr : &stderr_owned;

    sub = g_subprocess_newv(argv, flags, error);
    if (sub == NULL)
    {
        return FALSE;
    }
    if (!ai_subprocess_communicate_utf8_bounded(sub, NULL, timeout_ms,
                                                cancellable, NULL,
                                                stderr_dest, error))
    {
        g_prefix_error(error, "Command '%s': ", argv[0]);
        return FALSE;
    }
    if (!g_subprocess_get_successful(sub))
    {
        const gchar *stderr_str = *stderr_dest != NULL ? *stderr_dest : "";
        gint exit_status = g_subprocess_get_exit_status(sub);
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                    "Command '%s' exited %d: %s",
                    argv[0], exit_status,
                    stderr_str[0] != '\0' ? stderr_str : "(no stderr)");
        return FALSE;
    }
    return TRUE;
}

/*
 * ---------- direct cancel -> tmux teardown ----------
 *
 * A user !stop / !kill cancels the per-turn GCancellable.  Relying on
 * the worker thread to *notice* the cancellation and reach its cleanup
 * path is not enough: the claude TUI can wedge the worker (a hung
 * send-keys, a stuck resume/compaction) so it never gets there, and the
 * tmux session then survives — pinning turn_active and forcing a
 * libreclaw restart.  Worse, tmux_session_name is stable across
 * restarts, so the orphan outlives the restart too.
 *
 * So we ALSO connect this handler directly to the cancellable.  It runs
 * the instant the turn is cancelled — in whichever thread calls
 * g_cancellable_cancel() — and kills the tmux session outright, without
 * waiting on the worker thread.  The worker's own cleanup_and_fail path
 * still runs kill-session afterwards; the second kill is a harmless
 * no-op.
 *
 * Lifetime: the ctx lives on chat_sync_real's stack.  The handler is
 * connected only after the session is spawned and is DISCONNECTED with
 * g_cancellable_disconnect() before chat_sync_real returns —
 * g_cancellable_disconnect() blocks until any in-flight handler has
 * finished, so the borrowed strings can never be touched after the
 * stack frame is gone.
 */
typedef struct
{
    const gchar *tmux_bin;            /* borrowed; valid for the turn */
    const gchar *tmux_session_name;   /* borrowed; valid for the turn */
    gint         command_timeout_ms;  /* bound for the kill-session cmd */
} TurnCancelKillCtx;

static void
on_turn_cancelled_kill_session(
    GCancellable *cancellable,
    gpointer      user_data
){
    const TurnCancelKillCtx *ctx = user_data;
    const gchar *kill_argv[] = {
        ctx->tmux_bin, "kill-session", "-t", ctx->tmux_session_name, NULL
    };

    (void)cancellable;

    g_warning("claude-tmux: turn cancelled (!stop/!kill) — tearing down "
              "tmux session '%s' directly", ctx->tmux_session_name);

    /* Best-effort: the session may already be gone. */
    run_command_sync(kill_argv, NULL, ctx->command_timeout_ms, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Property machinery                                                  */
/* ------------------------------------------------------------------ */

static void
ai_claude_tmux_client_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiClaudeTmuxClient *self = AI_CLAUDE_TMUX_CLIENT(object);

    switch (prop_id)
    {
        case PROP_TMUX_PATH:
            g_value_set_string(value, self->tmux_path);
            break;
        case PROP_CLAUDE_PROJECT_DIR:
            g_value_set_string(value, self->claude_project_dir);
            break;
        case PROP_TURN_TIMEOUT_MS:
            g_value_set_int(value, self->turn_timeout_ms);
            break;
        case PROP_STARTUP_TIMEOUT_MS:
            g_value_set_int(value, self->startup_timeout_ms);
            break;
        case PROP_SKIP_PERMISSIONS:
            g_value_set_boolean(value, self->skip_permissions);
            break;
        case PROP_KEEP_ARTIFACTS:
            g_value_set_boolean(value, self->keep_artifacts);
            break;
        case PROP_DEBUG_PRESERVE_TMUX:
            g_value_set_boolean(value, self->debug_preserve_tmux);
            break;
        case PROP_PROMPT_RESEND_INTERVAL_MS:
            g_value_set_int(value, self->prompt_resend_interval_ms);
            break;
        case PROP_MAX_PROMPT_SEND_ATTEMPTS:
            g_value_set_int(value, self->max_prompt_send_attempts);
            break;
        case PROP_DISMISS_RESUME_PROMPT:
            g_value_set_boolean(value, self->dismiss_resume_prompt);
            break;
        case PROP_PROMPT_SEND_EXPONENTIAL_BACKOFF:
            g_value_set_boolean(value, self->prompt_send_exponential_backoff);
            break;
        case PROP_COMMAND_TIMEOUT_MS:
            g_value_set_int(value, self->command_timeout_ms);
            break;
        case PROP_TOTAL_COST:
            g_value_set_double(value, self->total_cost);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ai_claude_tmux_client_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiClaudeTmuxClient *self = AI_CLAUDE_TMUX_CLIENT(object);

    switch (prop_id)
    {
        case PROP_TMUX_PATH:
            g_free(self->tmux_path);
            self->tmux_path = g_value_dup_string(value);
            break;
        case PROP_CLAUDE_PROJECT_DIR:
            g_free(self->claude_project_dir);
            self->claude_project_dir = g_value_dup_string(value);
            break;
        case PROP_TURN_TIMEOUT_MS:
            self->turn_timeout_ms = g_value_get_int(value);
            break;
        case PROP_STARTUP_TIMEOUT_MS:
            self->startup_timeout_ms = g_value_get_int(value);
            break;
        case PROP_SKIP_PERMISSIONS:
            self->skip_permissions = g_value_get_boolean(value);
            break;
        case PROP_KEEP_ARTIFACTS:
            self->keep_artifacts = g_value_get_boolean(value);
            break;
        case PROP_DEBUG_PRESERVE_TMUX:
            self->debug_preserve_tmux = g_value_get_boolean(value);
            break;
        case PROP_PROMPT_RESEND_INTERVAL_MS:
            self->prompt_resend_interval_ms = g_value_get_int(value);
            break;
        case PROP_MAX_PROMPT_SEND_ATTEMPTS:
            self->max_prompt_send_attempts = g_value_get_int(value);
            break;
        case PROP_DISMISS_RESUME_PROMPT:
            self->dismiss_resume_prompt = g_value_get_boolean(value);
            break;
        case PROP_PROMPT_SEND_EXPONENTIAL_BACKOFF:
            self->prompt_send_exponential_backoff = g_value_get_boolean(value);
            break;
        case PROP_COMMAND_TIMEOUT_MS:
            self->command_timeout_ms = g_value_get_int(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ai_claude_tmux_client_finalize(GObject *object)
{
    AiClaudeTmuxClient *self = AI_CLAUDE_TMUX_CLIENT(object);

    g_free(self->tmux_path);
    g_free(self->claude_project_dir);

    G_OBJECT_CLASS(ai_claude_tmux_client_parent_class)->finalize(object);
}

static AiResponse *
ai_claude_tmux_client_chat_sync_real(
    AiClaudeTmuxClient *self,
    GList              *messages,
    GCancellable       *cancellable,
    GError            **error);

static AiResponse *
ai_claude_tmux_client_chat_sync_vfunc(
    AiCliClient   *client,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
){
    return ai_claude_tmux_client_chat_sync_real(
        AI_CLAUDE_TMUX_CLIENT(client), messages, cancellable, error);
}

/*
 * The "CLI executable" for this client is claude itself — tmux is
 * the wrapper and is resolved separately via the tmux-path property.
 * Mirrors AiClaudeCodeClient: honour CLAUDE_CODE_PATH, else search PATH.
 */
static gchar *
ai_claude_tmux_client_get_executable_path(AiCliClient *client)
{
    const gchar *env_path;

    (void)client;

    env_path = g_getenv("CLAUDE_CODE_PATH");
    if (env_path != NULL && env_path[0] != '\0')
    {
        return g_strdup(env_path);
    }

    return g_strdup("claude");
}

static void
ai_claude_tmux_client_class_init(AiClaudeTmuxClientClass *klass)
{
    GObjectClass     *object_class = G_OBJECT_CLASS(klass);
    AiCliClientClass *cli_class    = AI_CLI_CLIENT_CLASS(klass);

    object_class->finalize     = ai_claude_tmux_client_finalize;
    object_class->get_property = ai_claude_tmux_client_get_property;
    object_class->set_property = ai_claude_tmux_client_set_property;

    cli_class->chat_sync           = ai_claude_tmux_client_chat_sync_vfunc;
    cli_class->get_executable_path = ai_claude_tmux_client_get_executable_path;

    properties[PROP_TMUX_PATH] = g_param_spec_string(
        "tmux-path", "Tmux Path",
        "Path to tmux binary (NULL to search PATH)",
        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_CLAUDE_PROJECT_DIR] = g_param_spec_string(
        "claude-project-dir", "Claude Project Dir",
        "Root directory for claude session transcripts "
        "(NULL = $HOME/.claude/projects)",
        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_TURN_TIMEOUT_MS] = g_param_spec_int(
        "turn-timeout-ms", "Turn Timeout (ms)",
        "Max time to wait for the Stop hook to fire",
        1, G_MAXINT, 600000,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_STARTUP_TIMEOUT_MS] = g_param_spec_int(
        "startup-timeout-ms", "Startup Timeout (ms)",
        "Max time to wait for claude to create its JSONL transcript",
        1, G_MAXINT, 15000,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_SKIP_PERMISSIONS] = g_param_spec_boolean(
        "skip-permissions", "Skip Permissions",
        "Pass --dangerously-skip-permissions to claude",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_KEEP_ARTIFACTS] = g_param_spec_boolean(
        "keep-artifacts", "Keep Artifacts",
        "Leave prompt/sentinel files on disk after the turn",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_DEBUG_PRESERVE_TMUX] = g_param_spec_boolean(
        "debug-preserve-tmux", "Debug: Preserve Tmux",
        "Skip the tmux kill-session and artifact cleanup so the "
        "session can be inspected post-mortem.  Implies keep-artifacts.",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_PROMPT_RESEND_INTERVAL_MS] = g_param_spec_int(
        "prompt-resend-interval-ms", "Prompt Resend Interval (ms)",
        "How long to wait for a user entry to appear in the transcript "
        "after pressing Enter before deciding the keystroke was "
        "swallowed and re-sending it",
        1, G_MAXINT, 2000,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_MAX_PROMPT_SEND_ATTEMPTS] = g_param_spec_int(
        "max-prompt-send-attempts", "Max Prompt Send Attempts",
        "Maximum number of Enter keystrokes to deliver while trying to "
        "get the claude TUI to accept the pasted prompt, before failing",
        1, G_MAXINT, 5,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_DISMISS_RESUME_PROMPT] = g_param_spec_boolean(
        "dismiss-resume-prompt", "Dismiss Resume Prompt",
        "When resuming an existing session, type \"2\" before "
        "delivering the prompt to pick \"resume as-is\" on claude's "
        "interactive resume-mode picker (whose default, \"resume with "
        "a summary\", would trigger a multi-minute compaction), then "
        "wait prompt-resend-interval-ms for the TUI to settle",
        TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_PROMPT_SEND_EXPONENTIAL_BACKOFF] = g_param_spec_boolean(
        "prompt-send-exponential-backoff",
        "Prompt-Send Exponential Backoff",
        "When TRUE (the default), each unconfirmed submit-Enter retry "
        "doubles the wait before the next attempt: attempt N waits "
        "(prompt-resend-interval-ms << (N-1)) milliseconds for proof "
        "of acceptance before re-pressing Enter.  With the default "
        "interval (2000 ms) and max attempts (5), the total retry "
        "budget grows from a flat 10 s to ~62 s (2+4+8+16+32 s), "
        "wide enough to ride out claude auto-compacting a large "
        "resumed transcript.  When FALSE, every attempt waits the "
        "static prompt-resend-interval-ms — the pre-0.20.4 behaviour",
        TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_COMMAND_TIMEOUT_MS] = g_param_spec_int(
        "command-timeout-ms", "Command Timeout (ms)",
        "Deadline for each tmux plumbing command (new-session, "
        "send-keys, kill-session, ...).  A wedged tmux server would "
        "otherwise block the turn worker thread forever.  On expiry "
        "the tmux command is killed and the turn fails with "
        "AI_ERROR_TIMEOUT.  0 disables the deadline",
        0, G_MAXINT, 30000,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_TOTAL_COST] = g_param_spec_double(
        "total-cost", "Total Cost",
        "Cost in USD reported by the last response (0.0 if absent)",
        0.0, G_MAXDOUBLE, 0.0,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_claude_tmux_client_init(AiClaudeTmuxClient *self)
{
    self->tmux_path = NULL;
    self->claude_project_dir = NULL;
    self->turn_timeout_ms = 600000;     /* 10 min */
    self->startup_timeout_ms = 15000;   /* 15 sec — TUI ready delay (resume needs more) */
    self->skip_permissions = FALSE;
    self->keep_artifacts = FALSE;
    self->debug_preserve_tmux = FALSE;
    self->prompt_resend_interval_ms = 2000;  /* 2 sec */
    self->max_prompt_send_attempts = 5;
    self->dismiss_resume_prompt = TRUE;
    self->prompt_send_exponential_backoff = TRUE;
    self->command_timeout_ms = 30000;   /* 30 sec */
    self->turn_active = 0;
    self->total_cost = 0.0;

    ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_CLAUDE_TMUX_DEFAULT_MODEL);
}

/* ------------------------------------------------------------------ */
/* The actual chat path                                                */
/* ------------------------------------------------------------------ */

/*
 * Build the prompt text we'll write to the temp file.  Same shape as
 * the other claude clients: concatenate user messages with double-
 * newline separators, then append the standard "always include a
 * plain text response" instruction.
 */
static gchar *
build_prompt_text(GList *messages)
{
    GString *out;
    GList   *l;

    out = g_string_new("");
    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        g_autofree gchar *text = ai_message_get_text(msg);
        AiRole role = ai_message_get_role(msg);

        if (text == NULL || text[0] == '\0')
        {
            continue;
        }
        if (out->len > 0)
        {
            g_string_append(out, "\n\n");
        }
        if (role == AI_ROLE_USER)
        {
            g_string_append(out, text);
        }
        else if (role == AI_ROLE_ASSISTANT)
        {
            g_string_append_printf(out,
                "Previous assistant response: %s", text);
        }
    }

    g_string_append(out,
        "\n\nIMPORTANT: Always include a plain text response. "
        "Tool use is fine, but you MUST provide a text summary of "
        "your work when finished. Never end your turn on tool calls alone.");

    return g_string_free(out, FALSE);
}

/*
 * Resolve the cwd that claude will report in its transcript.  This is
 * the working_directory property if set, else the process cwd.
 *
 * The result is canonicalized via realpath() so it matches what
 * claude itself derives from getcwd() inside the subprocess.  On
 * systems where /home is a symlink to /var/home (Silverblue / atomic
 * Fedora), an unresolved "/home/foo" would encode to
 * "-home-foo" while claude writes its transcript under
 * "-var-home-foo".  Canonicalizing both sides closes the gap.
 */
static gchar *
resolve_session_cwd(AiClaudeTmuxClient *self)
{
    g_autofree gchar *raw = NULL;
    gchar *canonical;
    const gchar *wd;

    wd = ai_cli_client_get_working_directory(AI_CLI_CLIENT(self));
    if (wd != NULL && wd[0] != '\0')
    {
        raw = g_strdup(wd);
    }
    else
    {
        raw = g_get_current_dir();
    }

    canonical = realpath(raw, NULL);
    if (canonical != NULL)
    {
        /* realpath() uses malloc; hand glib a g_malloc copy. */
        gchar *out = g_strdup(canonical);
        free(canonical);
        return out;
    }

    /* realpath failed (path doesn't exist yet, etc.) — use the raw
     * value rather than NULL. */
    return g_steal_pointer(&raw);
}

/*
 * Assemble the argv for `tmux new-session -d -s NAME -c CWD -- CMD ARGS`.
 *
 * Non-static so unit tests (and the `ai` CLI --dry-run) can assert the
 * exact command line, including the Ollama transport rewrite. See
 * ai-claude-tmux-client-internal.h.
 */
GPtrArray *
ai_claude_tmux_client_build_session_argv(
    const gchar *tmux_bin,
    const gchar *session_name,
    const gchar *cwd,
    const gchar *claude_exec_path,
    gboolean     resuming_existing_session,
    const gchar *session_id,
    const gchar *settings_path,
    const gchar *model,
    const gchar *effort,
    gboolean     skip_permissions
){
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *program = NULL;

    g_ptr_array_add(argv, g_strdup(tmux_bin));
    g_ptr_array_add(argv, g_strdup("new-session"));
    g_ptr_array_add(argv, g_strdup("-d"));
    g_ptr_array_add(argv, g_strdup("-s"));
    g_ptr_array_add(argv, g_strdup(session_name));
    /*
     * Anchor the session's start-directory to the resolved workspace cwd.
     * Without this tmux (and therefore claude) inherit libreclaw's process
     * cwd, and claude writes its transcript under a "wrong" project
     * subdirectory in ~/.claude/projects — our jsonl_path lookup misses it.
     */
    g_ptr_array_add(argv, g_strdup("-c"));
    g_ptr_array_add(argv, g_strdup(cwd));
    g_ptr_array_add(argv, g_strdup("--"));

    /*
     * Program token after the tmux `--`. In Ollama mode this is the
     * launcher binary and emit_tokens wraps it as
     * `ollama launch claude --model <suffix> --`; otherwise it is the
     * resolved claude path. The claude args appended below ride after the
     * Ollama wrapper's own `--`.
     */
    program = ai_claude_launch_model_is_ollama(model)
                  ? ai_claude_launch_executable_name(model)
                  : g_strdup(claude_exec_path);
    ai_claude_launch_emit_tokens(argv, model, program);

    if (resuming_existing_session)
    {
        g_ptr_array_add(argv, g_strdup("--resume"));
    }
    else
    {
        g_ptr_array_add(argv, g_strdup("--session-id"));
    }
    g_ptr_array_add(argv, g_strdup(session_id));
    g_ptr_array_add(argv, g_strdup("--settings"));
    g_ptr_array_add(argv, g_strdup(settings_path));

    /*
     * Model — omitted in Ollama mode (carried by `ollama launch --model`),
     * and omitted when empty/unset, as before.
     */
    if (ai_claude_launch_should_emit_claude_model(model) &&
        model != NULL && model[0] != '\0')
    {
        g_ptr_array_add(argv, g_strdup("--model"));
        g_ptr_array_add(argv, g_strdup(model));
    }
    if (effort != NULL && effort[0] != '\0')
    {
        g_ptr_array_add(argv, g_strdup("--effort"));
        g_ptr_array_add(argv, g_strdup(effort));
    }
    if (skip_permissions)
    {
        g_ptr_array_add(argv, g_strdup("--dangerously-skip-permissions"));
    }
    g_ptr_array_add(argv, NULL);

    return argv;
}

/*
 * Synchronous chat — the actual workhorse.
 */
static AiResponse *
ai_claude_tmux_client_chat_sync_real(
    AiClaudeTmuxClient *self,
    GList              *messages,
    GCancellable       *cancellable,
    GError            **error
){
    g_autofree gchar *runtime_dir = NULL;
    g_autofree gchar *session_id = NULL;
    g_autofree gchar *prompt_path = NULL;
    g_autofree gchar *ready_path = NULL;
    g_autofree gchar *sentinel_path = NULL;
    g_autofree gchar *settings_path = NULL;
    g_autofree gchar *tmux_session_name = NULL;
    g_autofree gchar *cwd = NULL;
    g_autofree gchar *jsonl_path = NULL;
    g_autofree gchar *settings_json = NULL;
    g_autofree gchar *prompt_text = NULL;
    g_autofree gchar *jsonl_contents = NULL;
    g_autofree gchar *claude_exec_path = NULL;
    g_autoptr(AiResponse) response = NULL;
    const gchar *tmux_bin;
    const gchar *configured_session_id;
    gboolean resuming_existing_session = FALSE;
    goffset jsonl_size_before = 0;
    gdouble cost = 0.0;
    TurnCancelKillCtx kill_ctx = { NULL, NULL, 0 };
    gulong kill_cancel_id = 0;

    /*
     * Already cancelled before we even started?  A !stop / !kill can
     * land in the window between the GTask being queued and this worker
     * thread picking it up.  Bail before spawning tmux/claude — there is
     * nothing to tear down yet, and starting a session only to kill it
     * milliseconds later is pure waste (and another orphan risk).
     */
    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CANCELLED,
                    "Turn cancelled before the claude tmux session "
                    "was started");
        return NULL;
    }

    /* ---------- preflight ---------- */
    runtime_dir = get_runtime_dir(error);
    if (runtime_dir == NULL)
    {
        return NULL;
    }

    configured_session_id = ai_cli_client_get_session_id(AI_CLI_CLIENT(self));
    if (configured_session_id != NULL && configured_session_id[0] != '\0')
    {
        session_id = g_strdup(configured_session_id);
    }
    else
    {
        session_id = g_uuid_string_random();
    }

    cwd = resolve_session_cwd(self);
    jsonl_path = ai_claude_tmux_client_compute_jsonl_path(
        self->claude_project_dir, cwd, session_id);

    /*
     * If a transcript already exists for this session_id, claude
     * must be told to RESUME it rather than create a new session
     * with the same UUID.  --session-id <existing-uuid> conflicts
     * with the prior transcript and causes claude to exit on
     * startup, which then closes the tmux session.
     */
    resuming_existing_session =
        g_file_test(jsonl_path, G_FILE_TEST_EXISTS);

    {
        g_autofree gchar *base = g_strconcat("prompt-", session_id, ".md", NULL);
        prompt_path = g_build_filename(runtime_dir, base, NULL);
    }
    {
        g_autofree gchar *base = g_strconcat("ready-", session_id, NULL);
        ready_path = g_build_filename(runtime_dir, base, NULL);
    }
    {
        g_autofree gchar *base = g_strconcat("done-", session_id, NULL);
        sentinel_path = g_build_filename(runtime_dir, base, NULL);
    }
    {
        g_autofree gchar *base = g_strconcat("settings-", session_id, ".json", NULL);
        settings_path = g_build_filename(runtime_dir, base, NULL);
    }

    tmux_session_name = g_strconcat("claudetmux-", session_id, NULL);

    /*
     * Defensive: a previous run killed mid-flow may have left stale
     * sentinels.  wait_for_file() would then return success before
     * claude has actually fired the hook.  g_unlink on a missing
     * path is a no-op.
     */
    g_unlink(ready_path);
    g_unlink(sentinel_path);

    /* Resolve tmux path. */
    if (self->tmux_path != NULL && self->tmux_path[0] != '\0')
    {
        tmux_bin = self->tmux_path;
    }
    else
    {
        tmux_bin = "tmux";
    }

    /* Resolve claude path — reuse the parent's logic. */
    claude_exec_path = ai_cli_client_resolve_executable(
        AI_CLI_CLIENT(self), error);
    if (claude_exec_path == NULL)
    {
        return NULL;
    }

    /* ---------- write prompt file ---------- */
    prompt_text = build_prompt_text(messages);
    if (!write_prompt_file_atomic(prompt_path, prompt_text, error))
    {
        return NULL;
    }

    /*
     * ---------- write settings file ----------
     * --settings accepts either inline JSON or a file path.  Use a
     * file so the JSON braces/quotes are not at risk of being chewed
     * up by any shell/argv quirks in the tmux invocation chain.
     */
    settings_json = build_settings_json(ready_path, sentinel_path);
    if (!g_file_set_contents(settings_path, settings_json, -1, error))
    {
        g_prefix_error(error, "Failed to write settings file '%s': ",
                       settings_path);
        if (!self->keep_artifacts)
        {
            g_unlink(prompt_path);
        }
        return NULL;
    }

    /*
     * ---------- reap an orphaned tmux session ----------
     * tmux_session_name is derived from session_id, which is stable
     * across turns AND across libreclaw restarts.  If a previous
     * libreclaw process was killed hard — its restart SIGUSR1 never
     * caught, no clean shutdown — it can leave this tmux session
     * alive with nobody managing it.  `tmux new-session` would then
     * fail with "duplicate session".
     *
     * If this client has no turn in flight (turn_active == 0) and a
     * session with our name nonetheless exists, it can only be such
     * an orphan: kill it before creating the fresh one.  The
     * turn_active guard is what stops a genuinely concurrent turn on
     * the same client from reaping its own live session.
     */
    if (g_atomic_int_get(&self->turn_active) == 0)
    {
        const gchar *has_argv[] = {
            tmux_bin, "has-session", "-t", tmux_session_name, NULL
        };
        /* has-session exits 0 iff the session exists; any non-zero
         * (no such session, no server running) means nothing to reap. */
        if (run_command_sync(has_argv, NULL, self->command_timeout_ms,
                             cancellable, NULL))
        {
            const gchar *kill_argv[] = {
                tmux_bin, "kill-session", "-t", tmux_session_name, NULL
            };
            g_warning("claude-tmux: reaping orphaned tmux session '%s' "
                      "(no turn in flight — likely a prior libreclaw "
                      "process that exited without cleaning up) before "
                      "spawning a fresh one", tmux_session_name);
            /* Best-effort: if it vanished between the check and now,
             * kill-session just fails harmlessly. */
            run_command_sync(kill_argv, NULL, self->command_timeout_ms,
                             NULL, NULL);
        }
    }

    /*
     * Claim session ownership BEFORE spawning: from here until the
     * cleanup paths clear it, a concurrent call (or re-entry) on this
     * client sees turn_active == 1 and leaves our session alone.
     */
    g_atomic_int_set(&self->turn_active, 1);

    /* ---------- assemble argv for `tmux new-session -d -s NAME -- CMD ARGS` ---------- */
    {
        g_autoptr(GPtrArray) argv = NULL;
        gboolean ok;

        argv = ai_claude_tmux_client_build_session_argv(
            tmux_bin,
            tmux_session_name,
            cwd,
            claude_exec_path,
            resuming_existing_session,
            session_id,
            settings_path,
            ai_cli_client_get_model(AI_CLI_CLIENT(self)),
            ai_cli_client_get_effort_level(AI_CLI_CLIENT(self)),
            self->skip_permissions);

        ok = run_command_sync(
            (const gchar * const *)argv->pdata, NULL,
            self->command_timeout_ms, cancellable, error);
        if (!ok)
        {
            /* Never got a session — release the ownership claim. */
            g_atomic_int_set(&self->turn_active, 0);
            g_prefix_error(error, "Failed to start tmux session: ");
            if (!self->keep_artifacts)
            {
                g_unlink(prompt_path);
            }
            return NULL;
        }
    }

    /*
     * The tmux session now exists.  Arm the direct cancel -> teardown
     * hook so a !stop / !kill kills it immediately, even if the worker
     * thread later wedges interacting with a hung claude TUI.  Every
     * exit path below — the success tail and cleanup_and_fail — calls
     * g_cancellable_disconnect() before returning, so the borrowed
     * pointers in kill_ctx outlive the handler.
     *
     * If the turn was cancelled before we got here, g_cancellable_connect
     * invokes the handler synchronously right now (killing the session we
     * just made) and the wait_for_file calls below return AI_ERROR_CANCELLED.
     */
    if (cancellable != NULL)
    {
        kill_ctx.tmux_bin = tmux_bin;
        kill_ctx.tmux_session_name = tmux_session_name;
        kill_ctx.command_timeout_ms = self->command_timeout_ms;
        kill_cancel_id = g_cancellable_connect(
            cancellable,
            G_CALLBACK(on_turn_cancelled_kill_session),
            &kill_ctx, NULL);
    }

    /*
     * ---------- wait for claude TUI to be ready ----------
     * Block on the SessionStart hook firing.  Claude fires this once
     * its TUI is initialised and the input box is live, so any
     * subsequent send-keys / paste-buffer will actually land in the
     * input box rather than being swallowed by a still-loading TUI.
     */
    if (!wait_for_file(ready_path, self->startup_timeout_ms,
                       cancellable, error))
    {
        g_prefix_error(error,
                       "claude SessionStart hook never fired (TUI didn't "
                       "become ready, ready_path='%s'): ",
                       ready_path);
        goto cleanup_and_fail;
    }

    /*
     * ---------- dismiss the resume-mode picker ----------
     * When claude is launched with --resume its TUI can stop on an
     * interactive picker asking how to resume the conversation:
     *   1. resume with a summary   2. resume as-is   3. clear
     * We always kill the tmux session at the end of a turn (claude
     * never exits cleanly on its own), so from claude's point of view
     * every resumed session was interrupted mid-flight — which is
     * what makes this picker appear.  There is no CLI flag or
     * settings.json key to suppress it (checked against claude 2.1.x),
     * so the only lever we have is the keyboard.
     *
     * Left alone, the picker swallows the bracketed paste below as raw
     * keystrokes and the prompt never reaches the input box.
     *
     * We type "2" — the picker's "resume as-is" option.  Plain Enter
     * would take the picker's DEFAULT, which is "resume with a
     * summary": that kicks off a re-summarisation (a compaction) that
     * on a large session runs for one to two MINUTES, during which
     * claude is busy and our prompt just sits queued.  "Resume as-is"
     * skips that.  Typing the digit also doesn't need a confirming
     * Enter — it activates the option directly — and it targets a
     * static, idle picker, so the single keystroke lands reliably
     * (unlike the post-paste submit-Enter, which races the TUI's
     * input-box re-render and needs the verified retry loop below).
     * We then wait prompt_resend_interval_ms for claude to tear the
     * picker down and settle into the input box before pasting.
     *
     * (claude may still auto-compact a huge resumed context on its
     * own — but the submit loop below now recognises a queued prompt
     * and lets the Stop-hook wait ride the compaction out, rather
     * than giving up.  Here we just avoid triggering an avoidable
     * one.)
     *
     * Gated on resuming_existing_session: a fresh --session-id run
     * never shows the picker.  Worst case, dismiss_resume_prompt is on
     * but no picker is actually up — the "2" lands as a literal
     * character in the empty input box.  We follow it with a single
     * BSpace keystroke so that leaked character is rubbed out before
     * the paste lands, leaving a clean input box either way:
     *   - picker WAS up   → "2" selected option 2, picker dismissed,
     *                       BSpace hits the now-empty input box and is
     *                       a no-op
     *   - picker was NOT  → "2" landed in input box, BSpace clears it
     */
    if (resuming_existing_session && self->dismiss_resume_prompt)
    {
        const gchar *dismiss_argv[] = {
            tmux_bin, "send-keys", "-l", "-t", tmux_session_name,
            "2", NULL
        };
        const gchar *clean_argv[] = {
            tmux_bin, "send-keys", "-t", tmux_session_name,
            "BSpace", NULL
        };
        if (!run_command_sync(dismiss_argv, NULL,
                              self->command_timeout_ms, cancellable,
                              error))
        {
            g_prefix_error(error,
                           "tmux send-keys (resume-picker dismiss) "
                           "failed: ");
            goto cleanup_and_fail;
        }
        g_usleep((gulong)self->prompt_resend_interval_ms * 1000);
        if (!run_command_sync(clean_argv, NULL,
                              self->command_timeout_ms, cancellable,
                              error))
        {
            g_prefix_error(error,
                           "tmux send-keys (post-dismiss backspace) "
                           "failed: ");
            goto cleanup_and_fail;
        }
    }

    /*
     * Snapshot the transcript size BEFORE sending the prompt.  This
     * watermark is what slice_has_accepted_prompt() and
     * slice_has_terminal_assistant_entry() slice from, so it must sit
     * after everything that is not our turn.  In particular it is
     * taken AFTER the resume-picker dismissal above: the picker
     * selection — and any resume-time writes claude makes — therefore
     * stay below the watermark and cannot be mistaken for our
     * prompt's user entry.
     *
     * When the Stop hook fires after the turn, we verify the file
     * actually grew past this point — if it didn't, the hook fired
     * without a real turn (e.g. resume-time idle fire) and the last
     * assistant entry is stale.  Returning that as the "response"
     * would echo our previous reply back to the user.
     */
    {
        GStatBuf st;
        if (g_stat(jsonl_path, &st) == 0)
        {
            jsonl_size_before = (goffset)st.st_size;
        }
        else
        {
            jsonl_size_before = 0;
        }
    }

    /*
     * ---------- deliver the prompt ----------
     * Avoid claude TUI's @<file> syntax — send-keys doesn't trigger
     * its file-reference expansion reliably.  Instead, load the
     * prompt text into a tmux paste buffer and paste it: the TUI
     * receives this as a real paste event (bracketed paste), which
     * handles multi-line text without each newline being interpreted
     * as Enter.
     */
    {
        g_autofree gchar *buffer_name = g_strconcat("clawd-", session_id, NULL);
        const gchar *load_argv[] = {
            tmux_bin, "load-buffer", "-b", buffer_name, prompt_path, NULL
        };
        if (!run_command_sync(load_argv, NULL,
                              self->command_timeout_ms, cancellable,
                              error))
        {
            g_prefix_error(error, "tmux load-buffer failed: ");
            goto cleanup_and_fail;
        }
        {
            const gchar *paste_argv[] = {
                tmux_bin, "paste-buffer", "-b", buffer_name,
                "-t", tmux_session_name, "-d", NULL  /* -d = delete buffer after */
            };
            if (!run_command_sync(paste_argv, NULL,
                                  self->command_timeout_ms,
                                  cancellable, error))
            {
                g_prefix_error(error, "tmux paste-buffer failed: ");
                goto cleanup_and_fail;
            }
        }
    }
    /*
     * Give claude TUI a beat to finish ingesting the bracketed-paste
     * event before we deliver the submit keystroke.  Without this,
     * an immediate Enter can be swallowed while the input box is
     * still applying the paste and updating its draft state, and the
     * message ends up sitting in the input box un-submitted.
     */
    g_usleep(500 * 1000);   /* 500 ms */

    /*
     * ---------- deliver the submit keystroke, verified ----------
     * tmux send-keys is occasionally unreliable at landing the Enter
     * against the claude TUI: the keypress can be swallowed while the
     * input box is mid-render, leaving the pasted prompt sitting
     * un-submitted in the draft box.  A swallowed Enter means claude
     * never receives the prompt at all, so we'd otherwise burn the
     * entire turn_timeout_ms (minutes) waiting on a Stop hook that is
     * never going to fire.
     *
     * So treat the Enter as unconfirmed until proven otherwise: press
     * it, then watch the transcript for proof that claude ACCEPTED
     * the submission.  Acceptance has two shapes — see
     * ai_claude_tmux_client_jsonl_has_accepted_prompt():
     *
     *   - a real `type:"user"` entry: claude was idle and started the
     *     turn immediately; or
     *   - a `type:"queue-operation"` / `operation:"enqueue"` entry:
     *     claude was BUSY (running a turn, or auto-compacting a large
     *     resumed session) and queued the prompt.  It is NOT lost; it
     *     runs as soon as claude is free, and the long turn_timeout_ms
     *     on the Stop-hook wait below covers the wait.  The original
     *     bug here was treating this case as a swallowed keystroke:
     *     we re-sent Enter five times, gave up, and killed claude
     *     mid-compaction with our message still in its queue.
     *
     * If neither shape shows up within prompt_resend_interval_ms the
     * Enter genuinely did not register — press it again, up to
     * max_prompt_send_attempts times before giving up.
     *
     * We re-send only Enter, never the prompt text: the bracketed
     * paste already deposited the prompt in the draft box, and
     * re-pasting would duplicate it.  A spurious resend (the prompt
     * actually did land, the entry was just slow to flush) is
     * harmless — it hits an empty draft box, which the TUI ignores.
     */
    {
        const gchar *enter_argv[] = {
            tmux_bin, "send-keys", "-t", tmux_session_name,
            "Enter", NULL
        };
        const gint poll_ms = 100;
        gboolean prompt_accepted = FALSE;
        gint attempt;
        gint total_waited_ms = 0;

        for (attempt = 1;
             attempt <= self->max_prompt_send_attempts && !prompt_accepted;
             attempt++)
        {
            gint this_wait_ms;
            gint waited = 0;

            /*
             * Per-attempt wait: exponential when the backoff knob is on
             * (the default — attempt N waits base << (N-1)), or a flat
             * base every attempt when it's off.  Exponential is the
             * pre-flight remedy for large resumed transcripts: claude's
             * auto-compaction of a 2+ MB JSONL can keep the TUI busy
             * for tens of seconds, well past the flat 10 s budget the
             * old loop allowed.
             */
            if (self->prompt_send_exponential_backoff)
            {
                /*
                 * Cap the shift count to keep the wait inside gint and
                 * keep the *total* budget tractable.  We multiply by
                 * 1 << (attempt-1); with a 2000 ms base, shift 14 is
                 * already 32 768 × 2 s ≈ 18 hours, so 20 is plenty of
                 * head-room without risking overflow.
                 */
                gint shift = attempt - 1;
                if (shift > 20) shift = 20;
                if (G_MAXINT / (1 << shift) < self->prompt_resend_interval_ms)
                    this_wait_ms = G_MAXINT;
                else
                    this_wait_ms = self->prompt_resend_interval_ms
                                   << shift;
            }
            else
            {
                this_wait_ms = self->prompt_resend_interval_ms;
            }

            if (attempt > 1)
            {
                g_warning("claude-tmux: prompt not accepted after Enter "
                          "(attempt %d/%d) — submit keystroke was "
                          "swallowed by the TUI, re-sending Enter and "
                          "waiting %d ms for proof of acceptance%s",
                          attempt - 1, self->max_prompt_send_attempts,
                          this_wait_ms,
                          self->prompt_send_exponential_backoff
                              ? " (exponential backoff)"
                              : "");
            }

            if (!run_command_sync(enter_argv, NULL,
                                  self->command_timeout_ms,
                                  cancellable, error))
            {
                g_prefix_error(error,
                               "tmux send-keys (Enter) failed: ");
                goto cleanup_and_fail;
            }

            while (waited < this_wait_ms)
            {
                /* Honour !stop / !kill promptly: the direct hook has
                 * already killed the tmux session, so bail out instead
                 * of polling a transcript that will never advance. */
                if (cancellable != NULL &&
                    g_cancellable_is_cancelled(cancellable))
                {
                    g_set_error(error, AI_ERROR, AI_ERROR_CANCELLED,
                                "Turn cancelled while waiting for the "
                                "prompt to be accepted by the claude TUI");
                    goto cleanup_and_fail;
                }
                if (slice_has_accepted_prompt(jsonl_path, jsonl_size_before))
                {
                    prompt_accepted = TRUE;
                    break;
                }
                g_usleep(poll_ms * 1000);
                waited += poll_ms;
            }
            total_waited_ms += waited;
        }

        if (!prompt_accepted)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                        "Prompt was pasted into the claude TUI but "
                        "neither a user entry nor a queued submission "
                        "ever appeared in transcript '%s' after %d "
                        "Enter keystroke(s) over ~%d ms — the submit "
                        "keystroke never registered (pre-prompt size "
                        "%" G_GOFFSET_FORMAT ")",
                        jsonl_path, self->max_prompt_send_attempts,
                        total_waited_ms,
                        jsonl_size_before);
            goto cleanup_and_fail;
        }
    }

    /*
     * ---------- wait for Stop hook sentinel ----------
     * The Stop hook fires when claude finishes its turn.  By the time
     * the sentinel appears, the JSONL transcript has been fully written
     * for this turn.
     *
     * turn_timeout_ms is applied as an INACTIVITY budget, not a hard
     * wall-clock cap: as long as the transcript keeps growing (claude is
     * working through a long multi-tool turn) we keep waiting.  Only a
     * fully silent transcript for the whole budget counts as a wedged
     * turn.  See wait_for_sentinel_or_idle() for the rationale — this is
     * the fix for long turns being killed mid-flight with no reply.
     */
    if (!ai_claude_tmux_client_wait_for_sentinel_or_idle(
             sentinel_path, jsonl_path, self->turn_timeout_ms,
             cancellable, error))
    {
        g_prefix_error(error,
                       "Stop hook sentinel '%s' never appeared: ",
                       sentinel_path);
        goto cleanup_and_fail;
    }

    /*
     * Freshness check: poll the JSONL until a NEW terminal assistant
     * entry has actually been flushed past the pre-prompt watermark.
     *
     * Naive "did the file grow?" is insufficient: claude flushes the
     * user-prompt + attachment lines AHEAD of the response line, and
     * the Stop hook can fire — and the sentinel touch can complete —
     * before the response line itself hits disk.  In that window the
     * file is larger than `jsonl_size_before` but the LAST
     * `type:"assistant"` entry visible to the parser is still the
     * previous turn's response.  Returning that as "the answer" echoes
     * a stale message back to the caller (this was the actual bug —
     * a wave got back the prior "startup complete" message).
     *
     * Instead, walk the slice after the watermark each time the file
     * grows and break only once we see an assistant entry whose
     * `message.stop_reason` is terminal (anything other than
     * "tool_use").  claude only fires the Stop hook on terminal stop
     * reasons, so this will always converge — we're just waiting on
     * the disk flush.
     */
    {
        GStatBuf st;
        goffset size_after = 0;
        goffset last_checked_size = jsonl_size_before;
        const gint poll_ms = 100;
        const gint max_wait_ms = 10000;
        gint waited = 0;
        gboolean found_terminal = FALSE;

        while (waited < max_wait_ms)
        {
            /* Honour !stop / !kill promptly here too — see the prompt
             * acceptance loop above. */
            if (cancellable != NULL &&
                g_cancellable_is_cancelled(cancellable))
            {
                g_set_error(error, AI_ERROR, AI_ERROR_CANCELLED,
                            "Turn cancelled while waiting for the terminal "
                            "assistant entry to flush to the transcript");
                goto cleanup_and_fail;
            }
            if (g_stat(jsonl_path, &st) == 0)
            {
                size_after = (goffset)st.st_size;
                if (size_after > last_checked_size)
                {
                    /* Avoid re-parsing the whole file every 100 ms
                     * when nothing new has landed since the last
                     * attempt. */
                    last_checked_size = size_after;
                    if (slice_has_terminal_assistant_entry(
                            jsonl_path, jsonl_size_before))
                    {
                        found_terminal = TRUE;
                        break;
                    }
                }
            }
            g_usleep(poll_ms * 1000);
            waited += poll_ms;
        }

        if (!found_terminal)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                        "Stop hook fired but no new terminal "
                        "assistant entry appeared in transcript "
                        "'%s' within %d ms (pre-prompt size "
                        "%" G_GOFFSET_FORMAT ", final size "
                        "%" G_GOFFSET_FORMAT ") — claude wrote the "
                        "user prompt but never flushed its response, "
                        "or the prompt was never delivered",
                        jsonl_path, max_wait_ms,
                        jsonl_size_before, size_after);
            goto cleanup_and_fail;
        }
    }

    /* ---------- read and parse the JSONL ---------- */
    if (!g_file_get_contents(jsonl_path, &jsonl_contents, NULL, error))
    {
        g_prefix_error(error, "Cannot read transcript '%s': ",
                       jsonl_path);
        goto cleanup_and_fail;
    }

    response = ai_claude_tmux_client_parse_jsonl(
        jsonl_contents,
        ai_cli_client_get_model(AI_CLI_CLIENT(self)),
        &cost,
        error);
    if (response == NULL)
    {
        goto cleanup_and_fail;
    }
    self->total_cost = cost;

    /* ---------- record session id for continuity ---------- */
    if (ai_cli_client_get_session_persistence(AI_CLI_CLIENT(self)))
    {
        ai_cli_client_set_session_id(AI_CLI_CLIENT(self), session_id);
    }

    /* ---------- cleanup (success path) ---------- */
    if (!self->debug_preserve_tmux)
    {
        const gchar *argv[] = {
            tmux_bin, "kill-session", "-t", tmux_session_name, NULL
        };
        /* Best-effort: ignore errors — the session may have already
         * exited on its own. */
        run_command_sync(argv, NULL, self->command_timeout_ms,
                         NULL, NULL);
    }
    else
    {
        g_info("debug_preserve_tmux: leaving tmux session '%s' alive "
               "(attach with: tmux attach -t %s)",
               tmux_session_name, tmux_session_name);
    }

    if (!self->keep_artifacts && !self->debug_preserve_tmux)
    {
        g_unlink(prompt_path);
        g_unlink(ready_path);
        g_unlink(sentinel_path);
        g_unlink(settings_path);
    }

    /* Drop the cancel hook before the stack frame (and the strings it
     * borrows) goes away.  Blocks until any in-flight handler finishes. */
    if (cancellable != NULL && kill_cancel_id != 0)
    {
        g_cancellable_disconnect(cancellable, kill_cancel_id);
        kill_cancel_id = 0;
    }

    g_atomic_int_set(&self->turn_active, 0);
    return g_steal_pointer(&response);

cleanup_and_fail:
    if (!self->debug_preserve_tmux)
    {
        const gchar *argv[] = {
            tmux_bin, "kill-session", "-t", tmux_session_name, NULL
        };
        run_command_sync(argv, NULL, self->command_timeout_ms,
                         NULL, NULL);
    }
    else
    {
        g_info("debug_preserve_tmux: leaving tmux session '%s' alive "
               "after failure (attach with: tmux attach -t %s)",
               tmux_session_name, tmux_session_name);
    }
    if (!self->keep_artifacts && !self->debug_preserve_tmux)
    {
        g_unlink(prompt_path);
        g_unlink(ready_path);
        g_unlink(sentinel_path);
        g_unlink(settings_path);
    }

    /* Same teardown of the cancel hook as the success path — must run
     * before tmux_session_name (borrowed by kill_ctx) is freed. */
    if (cancellable != NULL && kill_cancel_id != 0)
    {
        g_cancellable_disconnect(cancellable, kill_cancel_id);
        kill_cancel_id = 0;
    }

    g_atomic_int_set(&self->turn_active, 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* AiProvider interface                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    AiClaudeTmuxClient *client;
    GList              *messages;       /* owned (deep copy refs) */
    GCancellable       *cancellable;    /* owned */
} TmuxChatTaskData;

static void
tmux_chat_task_data_free(gpointer p)
{
    TmuxChatTaskData *d = p;
    g_clear_object(&d->client);
    g_list_free_full(d->messages, g_object_unref);
    g_clear_object(&d->cancellable);
    g_slice_free(TmuxChatTaskData, d);
}

static void
tmux_chat_thread_func(
    GTask        *task,
    gpointer      source,
    gpointer      data,
    GCancellable *cancellable
){
    TmuxChatTaskData *td = data;
    g_autoptr(GError) error = NULL;
    AiResponse *resp;

    (void)source;

    resp = ai_claude_tmux_client_chat_sync_real(
        td->client, td->messages, cancellable, &error);
    if (resp == NULL)
    {
        g_task_return_error(task, g_steal_pointer(&error));
    }
    else
    {
        g_task_return_pointer(task, resp, g_object_unref);
    }
}

static void
ai_claude_tmux_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiClaudeTmuxClient *self = AI_CLAUDE_TMUX_CLIENT(provider);
    GTask *task;
    TmuxChatTaskData *td;
    GList *copy = NULL;
    GList *l;

    (void)system_prompt;
    (void)max_tokens;
    (void)tools;

    task = g_task_new(self, cancellable, callback, user_data);

    for (l = messages; l != NULL; l = l->next)
    {
        copy = g_list_prepend(copy, g_object_ref(l->data));
    }
    copy = g_list_reverse(copy);

    td = g_slice_new0(TmuxChatTaskData);
    td->client = g_object_ref(self);
    td->messages = copy;
    td->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;

    g_task_set_task_data(task, td, tmux_chat_task_data_free);
    g_task_run_in_thread(task, tmux_chat_thread_func);
    g_object_unref(task);
}

static AiResponse *
ai_claude_tmux_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static AiProviderType
ai_claude_tmux_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    /* Distinct enum value: the billing model and delivery mechanism
     * differ enough that callers may want to route by it (e.g. fall
     * back to CLAUDE_CODE if tmux isn't available, or prefer
     * CLAUDE_TMUX for high-volume autonomous loops). */
    return AI_PROVIDER_CLAUDE_TMUX;
}

static const gchar *
ai_claude_tmux_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "Claude (TUI via tmux)";
}

static const gchar *
ai_claude_tmux_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_CLAUDE_TMUX_DEFAULT_MODEL;
}

static void
ai_claude_tmux_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    /* Static list of model aliases, then the pinned Claude 5 IDs */
    task = g_task_new(provider, NULL, callback, user_data);
    models = g_list_append(models, g_strdup(AI_CLAUDE_TMUX_MODEL_FABLE));
    models = g_list_append(models, g_strdup(AI_CLAUDE_TMUX_MODEL_OPUS));
    models = g_list_append(models, g_strdup(AI_CLAUDE_TMUX_MODEL_SONNET));
    models = g_list_append(models, g_strdup(AI_CLAUDE_TMUX_MODEL_HAIKU));
    models = g_list_append(models, g_strdup(AI_CLAUDE_TMUX_MODEL_FABLE_5));
    models = g_list_append(models, g_strdup(AI_CLAUDE_TMUX_MODEL_OPUS_5));
    models = g_list_append(models, g_strdup(AI_CLAUDE_TMUX_MODEL_SONNET_5));
    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
}

static GList *
ai_claude_tmux_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_claude_tmux_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type  = ai_claude_tmux_client_get_provider_type;
    iface->get_name           = ai_claude_tmux_client_get_name;
    iface->get_default_model  = ai_claude_tmux_client_get_default_model;
    iface->chat_async         = ai_claude_tmux_client_chat_async;
    iface->chat_finish        = ai_claude_tmux_client_chat_finish;
    iface->list_models_async  = ai_claude_tmux_client_list_models_async;
    iface->list_models_finish = ai_claude_tmux_client_list_models_finish;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

AiClaudeTmuxClient *
ai_claude_tmux_client_new(void)
{
    return g_object_new(AI_TYPE_CLAUDE_TMUX_CLIENT, NULL);
}

AiClaudeTmuxClient *
ai_claude_tmux_client_new_with_config(AiConfig *config)
{
    return g_object_new(AI_TYPE_CLAUDE_TMUX_CLIENT,
                        "config", config,
                        NULL);
}

gdouble
ai_claude_tmux_client_get_total_cost(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), 0.0);
    return self->total_cost;
}

gboolean
ai_claude_tmux_client_get_skip_permissions(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), FALSE);
    return self->skip_permissions;
}

void
ai_claude_tmux_client_set_skip_permissions(
    AiClaudeTmuxClient *self,
    gboolean            skip
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    if (self->skip_permissions != skip)
    {
        self->skip_permissions = skip;
        g_object_notify_by_pspec(G_OBJECT(self),
            properties[PROP_SKIP_PERMISSIONS]);
    }
}

const gchar *
ai_claude_tmux_client_get_tmux_path(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), NULL);
    return self->tmux_path;
}

void
ai_claude_tmux_client_set_tmux_path(
    AiClaudeTmuxClient *self,
    const gchar        *path
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    g_free(self->tmux_path);
    self->tmux_path = g_strdup(path);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TMUX_PATH]);
}

const gchar *
ai_claude_tmux_client_get_claude_project_dir(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), NULL);
    return self->claude_project_dir;
}

void
ai_claude_tmux_client_set_claude_project_dir(
    AiClaudeTmuxClient *self,
    const gchar        *path
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    g_free(self->claude_project_dir);
    self->claude_project_dir = g_strdup(path);
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_CLAUDE_PROJECT_DIR]);
}

gint
ai_claude_tmux_client_get_turn_timeout_ms(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), 0);
    return self->turn_timeout_ms;
}

void
ai_claude_tmux_client_set_turn_timeout_ms(
    AiClaudeTmuxClient *self,
    gint                timeout_ms
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    g_return_if_fail(timeout_ms > 0);
    self->turn_timeout_ms = timeout_ms;
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_TURN_TIMEOUT_MS]);
}

gint
ai_claude_tmux_client_get_startup_timeout_ms(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), 0);
    return self->startup_timeout_ms;
}

void
ai_claude_tmux_client_set_startup_timeout_ms(
    AiClaudeTmuxClient *self,
    gint                timeout_ms
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    g_return_if_fail(timeout_ms > 0);
    self->startup_timeout_ms = timeout_ms;
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_STARTUP_TIMEOUT_MS]);
}

gboolean
ai_claude_tmux_client_get_keep_artifacts(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), FALSE);
    return self->keep_artifacts;
}

void
ai_claude_tmux_client_set_keep_artifacts(
    AiClaudeTmuxClient *self,
    gboolean            keep
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    if (self->keep_artifacts != keep)
    {
        self->keep_artifacts = keep;
        g_object_notify_by_pspec(G_OBJECT(self),
            properties[PROP_KEEP_ARTIFACTS]);
    }
}

gboolean
ai_claude_tmux_client_get_debug_preserve_tmux(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), FALSE);
    return self->debug_preserve_tmux;
}

void
ai_claude_tmux_client_set_debug_preserve_tmux(
    AiClaudeTmuxClient *self,
    gboolean            preserve
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    if (self->debug_preserve_tmux != preserve)
    {
        self->debug_preserve_tmux = preserve;
        g_object_notify_by_pspec(G_OBJECT(self),
            properties[PROP_DEBUG_PRESERVE_TMUX]);
    }
}

gint
ai_claude_tmux_client_get_prompt_resend_interval_ms(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), 0);
    return self->prompt_resend_interval_ms;
}

void
ai_claude_tmux_client_set_prompt_resend_interval_ms(
    AiClaudeTmuxClient *self,
    gint                interval_ms
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    g_return_if_fail(interval_ms > 0);
    self->prompt_resend_interval_ms = interval_ms;
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_PROMPT_RESEND_INTERVAL_MS]);
}

gint
ai_claude_tmux_client_get_max_prompt_send_attempts(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), 0);
    return self->max_prompt_send_attempts;
}

void
ai_claude_tmux_client_set_max_prompt_send_attempts(
    AiClaudeTmuxClient *self,
    gint                attempts
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    g_return_if_fail(attempts > 0);
    self->max_prompt_send_attempts = attempts;
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_MAX_PROMPT_SEND_ATTEMPTS]);
}

gboolean
ai_claude_tmux_client_get_dismiss_resume_prompt(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), FALSE);
    return self->dismiss_resume_prompt;
}

void
ai_claude_tmux_client_set_dismiss_resume_prompt(
    AiClaudeTmuxClient *self,
    gboolean            dismiss
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    self->dismiss_resume_prompt = dismiss;
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_DISMISS_RESUME_PROMPT]);
}

gboolean
ai_claude_tmux_client_get_prompt_send_exponential_backoff(
    AiClaudeTmuxClient *self
){
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), FALSE);
    return self->prompt_send_exponential_backoff;
}

void
ai_claude_tmux_client_set_prompt_send_exponential_backoff(
    AiClaudeTmuxClient *self,
    gboolean            backoff
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    self->prompt_send_exponential_backoff = backoff;
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_PROMPT_SEND_EXPONENTIAL_BACKOFF]);
}

gint
ai_claude_tmux_client_get_command_timeout_ms(AiClaudeTmuxClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self), 0);
    return self->command_timeout_ms;
}

void
ai_claude_tmux_client_set_command_timeout_ms(
    AiClaudeTmuxClient *self,
    gint                timeout_ms
){
    g_return_if_fail(AI_IS_CLAUDE_TMUX_CLIENT(self));
    g_return_if_fail(timeout_ms >= 0);
    self->command_timeout_ms = timeout_ms;
    g_object_notify_by_pspec(G_OBJECT(self),
        properties[PROP_COMMAND_TIMEOUT_MS]);
}
