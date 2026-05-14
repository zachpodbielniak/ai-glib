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

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "core/ai-error.h"
#include "core/ai-provider.h"
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
 * Build the JSON blob we pass to claude via --settings.  Configures
 * a Stop hook that touches our sentinel file.
 */
static gchar *
build_settings_json(const gchar *sentinel_path)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) gen = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *cmd = NULL;

    /* Use printf "" >> as a portable touch.  The Stop hook command
     * runs in a shell so we have to single-quote the path.  Embedded
     * single quotes in the path itself would be a problem — we use
     * a UUID-named sentinel so this never happens in practice. */
    cmd = g_strdup_printf(": > '%s'", sentinel_path);

    builder = json_builder_new();
    json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "hooks");
        json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "Stop");
            json_builder_begin_array(builder);
                json_builder_begin_object(builder);
                    json_builder_set_member_name(builder, "matcher");
                    json_builder_add_string_value(builder, "");
                    json_builder_set_member_name(builder, "hooks");
                    json_builder_begin_array(builder);
                        json_builder_begin_object(builder);
                            json_builder_set_member_name(builder, "type");
                            json_builder_add_string_value(builder, "command");
                            json_builder_set_member_name(builder, "command");
                            json_builder_add_string_value(builder, cmd);
                        json_builder_end_object(builder);
                    json_builder_end_array(builder);
                json_builder_end_object(builder);
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

    g_main_loop_run(loop);

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
 * Run a one-shot child process synchronously, capturing exit status.
 * Returns TRUE on exit-zero, FALSE otherwise.  When @capture_stderr
 * is non-NULL, stderr is captured (used for diagnostics on failure).
 */
static gboolean
run_command_sync(
    const gchar * const  *argv,
    gchar               **capture_stderr,
    GError              **error
){
    g_autoptr(GSubprocess) sub = NULL;
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_SILENCE;

    if (capture_stderr != NULL)
    {
        flags |= G_SUBPROCESS_FLAGS_STDERR_PIPE;
    }
    else
    {
        flags |= G_SUBPROCESS_FLAGS_STDERR_SILENCE;
    }

    sub = g_subprocess_newv(argv, flags, error);
    if (sub == NULL)
    {
        return FALSE;
    }
    if (!g_subprocess_communicate_utf8(sub, NULL, NULL, NULL,
                                       capture_stderr, error))
    {
        return FALSE;
    }
    return g_subprocess_get_successful(sub);
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

static void
ai_claude_tmux_client_class_init(AiClaudeTmuxClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize     = ai_claude_tmux_client_finalize;
    object_class->get_property = ai_claude_tmux_client_get_property;
    object_class->set_property = ai_claude_tmux_client_set_property;

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
        1, G_MAXINT, 30000,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_SKIP_PERMISSIONS] = g_param_spec_boolean(
        "skip-permissions", "Skip Permissions",
        "Pass --dangerously-skip-permissions to claude",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_KEEP_ARTIFACTS] = g_param_spec_boolean(
        "keep-artifacts", "Keep Artifacts",
        "Leave prompt/sentinel files on disk after the turn",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

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
    self->startup_timeout_ms = 30000;   /* 30 sec */
    self->skip_permissions = FALSE;
    self->keep_artifacts = FALSE;
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
 */
static gchar *
resolve_session_cwd(AiClaudeTmuxClient *self)
{
    const gchar *wd;
    wd = ai_cli_client_get_working_directory(AI_CLI_CLIENT(self));
    if (wd != NULL && wd[0] != '\0')
    {
        return g_strdup(wd);
    }
    return g_get_current_dir();
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
    g_autofree gchar *sentinel_path = NULL;
    g_autofree gchar *tmux_session_name = NULL;
    g_autofree gchar *cwd = NULL;
    g_autofree gchar *jsonl_path = NULL;
    g_autofree gchar *settings_json = NULL;
    g_autofree gchar *prompt_text = NULL;
    g_autofree gchar *send_arg = NULL;
    g_autofree gchar *jsonl_contents = NULL;
    g_autofree gchar *claude_exec_path = NULL;
    g_autoptr(AiResponse) response = NULL;
    const gchar *tmux_bin;
    const gchar *configured_session_id;
    gdouble cost = 0.0;

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

    {
        g_autofree gchar *base = g_strconcat("prompt-", session_id, ".md", NULL);
        prompt_path = g_build_filename(runtime_dir, base, NULL);
    }
    {
        g_autofree gchar *base = g_strconcat("done-", session_id, NULL);
        sentinel_path = g_build_filename(runtime_dir, base, NULL);
    }

    tmux_session_name = g_strconcat("claudetmux-", session_id, NULL);

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

    /* ---------- assemble argv for `tmux new-session -d -s NAME -- CMD ARGS` ---------- */
    settings_json = build_settings_json(sentinel_path);

    {
        g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
        gboolean ok;

        g_ptr_array_add(argv, g_strdup(tmux_bin));
        g_ptr_array_add(argv, g_strdup("new-session"));
        g_ptr_array_add(argv, g_strdup("-d"));
        g_ptr_array_add(argv, g_strdup("-s"));
        g_ptr_array_add(argv, g_strdup(tmux_session_name));
        g_ptr_array_add(argv, g_strdup("--"));
        g_ptr_array_add(argv, g_strdup(claude_exec_path));
        g_ptr_array_add(argv, g_strdup("--session-id"));
        g_ptr_array_add(argv, g_strdup(session_id));
        g_ptr_array_add(argv, g_strdup("--settings"));
        g_ptr_array_add(argv, g_strdup(settings_json));

        {
            const gchar *model = ai_cli_client_get_model(AI_CLI_CLIENT(self));
            if (model != NULL && model[0] != '\0')
            {
                g_ptr_array_add(argv, g_strdup("--model"));
                g_ptr_array_add(argv, g_strdup(model));
            }
        }
        {
            const gchar *effort = ai_cli_client_get_effort_level(AI_CLI_CLIENT(self));
            if (effort != NULL && effort[0] != '\0')
            {
                g_ptr_array_add(argv, g_strdup("--effort"));
                g_ptr_array_add(argv, g_strdup(effort));
            }
        }
        if (self->skip_permissions)
        {
            g_ptr_array_add(argv, g_strdup("--dangerously-skip-permissions"));
        }
        g_ptr_array_add(argv, NULL);

        ok = run_command_sync(
            (const gchar * const *)argv->pdata, NULL, error);
        if (!ok)
        {
            g_prefix_error(error, "Failed to start tmux session: ");
            if (!self->keep_artifacts)
            {
                g_unlink(prompt_path);
            }
            return NULL;
        }
    }

    /* ---------- wait for JSONL to exist ---------- */
    if (!wait_for_file(jsonl_path, self->startup_timeout_ms,
                       cancellable, error))
    {
        g_prefix_error(error,
                       "claude failed to create transcript at '%s': ",
                       jsonl_path);
        goto cleanup_and_fail;
    }

    /* ---------- send the prompt ---------- */
    send_arg = g_strconcat("@", prompt_path, NULL);

    {
        const gchar *argv[] = {
            tmux_bin, "send-keys", "-t", tmux_session_name,
            "-l", send_arg, NULL
        };
        if (!run_command_sync(argv, NULL, error))
        {
            g_prefix_error(error, "tmux send-keys (literal) failed: ");
            goto cleanup_and_fail;
        }
    }
    {
        const gchar *argv[] = {
            tmux_bin, "send-keys", "-t", tmux_session_name,
            "Enter", NULL
        };
        if (!run_command_sync(argv, NULL, error))
        {
            g_prefix_error(error, "tmux send-keys (Enter) failed: ");
            goto cleanup_and_fail;
        }
    }

    /* ---------- wait for Stop hook sentinel ---------- */
    if (!wait_for_file(sentinel_path, self->turn_timeout_ms,
                       cancellable, error))
    {
        g_prefix_error(error,
                       "Stop hook sentinel '%s' never appeared: ",
                       sentinel_path);
        goto cleanup_and_fail;
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
    {
        const gchar *argv[] = {
            tmux_bin, "kill-session", "-t", tmux_session_name, NULL
        };
        /* Best-effort: ignore errors — the session may have already
         * exited on its own. */
        run_command_sync(argv, NULL, NULL);
    }

    if (!self->keep_artifacts)
    {
        g_unlink(prompt_path);
        g_unlink(sentinel_path);
    }

    return g_steal_pointer(&response);

cleanup_and_fail:
    {
        const gchar *argv[] = {
            tmux_bin, "kill-session", "-t", tmux_session_name, NULL
        };
        run_command_sync(argv, NULL, NULL);
    }
    if (!self->keep_artifacts)
    {
        g_unlink(prompt_path);
        g_unlink(sentinel_path);
    }
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

    task = g_task_new(provider, NULL, callback, user_data);
    models = g_list_append(models, g_strdup("opus"));
    models = g_list_append(models, g_strdup("sonnet"));
    models = g_list_append(models, g_strdup("haiku"));
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
