/*
 * ai-claude-tmux-client.h - Claude Code CLI client driven via tmux
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiClaudeTmuxClient drives the `claude` CLI in its native TUI
 * (interactive) mode by spawning it inside an ephemeral tmux session.
 * Unlike AiClaudeCodeClient, which uses `claude --print` (the
 * "Agent SDK" / non-interactive billing path), this client speaks
 * to a fully interactive claude process and so consumes the user's
 * normal subscription budget rather than the Agent SDK credit pool.
 *
 * High-level flow:
 *   1. Generate a UUID for the session if one isn't already set.
 *   2. Write the prompt to a temp file (atomically) so the TUI can
 *      ingest it via Claude Code's @<path> file-reference syntax.
 *   3. Spawn `tmux new-session -d -s <name> claude --session-id <uuid>
 *      --settings '<inline-json-with-Stop-hook>' [args]`.  The Stop
 *      hook touches a per-session sentinel file when claude finishes
 *      a turn.
 *   4. Wait for claude to create the JSONL transcript file.
 *   5. `tmux send-keys -l '@<prompt-path>'` then Enter.
 *   6. Block on a GFileMonitor watching the sentinel directory.
 *   7. Parse the JSONL transcript, extract the last assistant entry's
 *      text blocks and usage, return as AiResponse.
 *   8. Kill the tmux session and clean up artifacts.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "core/ai-cli-client.h"
#include "core/ai-config.h"
#include "model/ai-response.h"

G_BEGIN_DECLS

#define AI_TYPE_CLAUDE_TMUX_CLIENT (ai_claude_tmux_client_get_type())

G_DECLARE_FINAL_TYPE(AiClaudeTmuxClient, ai_claude_tmux_client,
                     AI, CLAUDE_TMUX_CLIENT, AiCliClient)

/**
 * AI_CLAUDE_TMUX_DEFAULT_MODEL:
 *
 * The default model for Claude (tmux) clients.
 */
#define AI_CLAUDE_TMUX_DEFAULT_MODEL "sonnet"

/**
 * AI_CLAUDE_TMUX_MODEL_OPUS:
 * AI_CLAUDE_TMUX_MODEL_SONNET:
 * AI_CLAUDE_TMUX_MODEL_HAIKU:
 */
#define AI_CLAUDE_TMUX_MODEL_OPUS    "opus"
#define AI_CLAUDE_TMUX_MODEL_SONNET  "sonnet"
#define AI_CLAUDE_TMUX_MODEL_HAIKU   "haiku"

/**
 * ai_claude_tmux_client_new:
 *
 * Creates a new #AiClaudeTmuxClient.  Both `claude` and `tmux` must
 * be available in PATH (or via the corresponding *_PATH environment
 * variable overrides).
 *
 * Returns: (transfer full): a new #AiClaudeTmuxClient
 */
AiClaudeTmuxClient *
ai_claude_tmux_client_new(void);

/**
 * ai_claude_tmux_client_new_with_config:
 * @config: an #AiConfig
 *
 * Returns: (transfer full): a new #AiClaudeTmuxClient with @config attached
 */
AiClaudeTmuxClient *
ai_claude_tmux_client_new_with_config(AiConfig *config);

/**
 * ai_claude_tmux_client_get_total_cost:
 * @self: an #AiClaudeTmuxClient
 *
 * Gets the total cost reported by claude for the last turn.  The TUI
 * does not always emit a cost field; missing values surface as 0.0.
 *
 * Returns: total cost in USD, or 0.0 if the transcript did not carry
 *   a usable cost.
 */
gdouble
ai_claude_tmux_client_get_total_cost(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_get_skip_permissions:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: %TRUE if --dangerously-skip-permissions will be passed
 */
gboolean
ai_claude_tmux_client_get_skip_permissions(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_skip_permissions:
 * @self: an #AiClaudeTmuxClient
 * @skip: pass --dangerously-skip-permissions when spawning claude
 */
void
ai_claude_tmux_client_set_skip_permissions(
    AiClaudeTmuxClient *self,
    gboolean            skip
);

/**
 * ai_claude_tmux_client_get_tmux_path:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: (transfer none): path to the tmux executable; %NULL means
 *   "search PATH for `tmux`"
 */
const gchar *
ai_claude_tmux_client_get_tmux_path(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_tmux_path:
 * @self: an #AiClaudeTmuxClient
 * @path: (nullable): absolute path to tmux, or %NULL to search PATH
 *
 * Override the tmux binary used to wrap claude.  Primarily for
 * testing with a stand-in script; production callers can ignore.
 */
void
ai_claude_tmux_client_set_tmux_path(
    AiClaudeTmuxClient *self,
    const gchar        *path
);

/**
 * ai_claude_tmux_client_get_claude_project_dir:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: (transfer none) (nullable): the directory under which
 *   claude writes its per-cwd subdirectories of session transcripts.
 *   %NULL means the default `$HOME/.claude/projects`.
 */
const gchar *
ai_claude_tmux_client_get_claude_project_dir(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_claude_project_dir:
 * @self: an #AiClaudeTmuxClient
 * @path: (nullable): root directory for claude project transcripts
 *
 * Override the search root for transcript JSONL files.  Production
 * callers can ignore; tests use this to point at a temporary
 * directory.
 */
void
ai_claude_tmux_client_set_claude_project_dir(
    AiClaudeTmuxClient *self,
    const gchar        *path
);

/**
 * ai_claude_tmux_client_get_turn_timeout_ms:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: how long to wait for the Stop-hook sentinel before
 *   declaring the turn timed out (milliseconds).
 */
gint
ai_claude_tmux_client_get_turn_timeout_ms(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_turn_timeout_ms:
 * @self: an #AiClaudeTmuxClient
 * @timeout_ms: timeout in milliseconds; must be > 0
 */
void
ai_claude_tmux_client_set_turn_timeout_ms(
    AiClaudeTmuxClient *self,
    gint                timeout_ms
);

/**
 * ai_claude_tmux_client_get_startup_timeout_ms:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: how long to wait for the JSONL transcript file to appear
 *   after spawning claude before declaring startup failed.
 */
gint
ai_claude_tmux_client_get_startup_timeout_ms(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_startup_timeout_ms:
 * @self: an #AiClaudeTmuxClient
 * @timeout_ms: timeout in milliseconds; must be > 0
 */
void
ai_claude_tmux_client_set_startup_timeout_ms(
    AiClaudeTmuxClient *self,
    gint                timeout_ms
);

/**
 * ai_claude_tmux_client_get_keep_artifacts:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: %TRUE if prompt/sentinel artifacts will be kept after
 *   the turn finishes (for debugging).  Default %FALSE.
 */
gboolean
ai_claude_tmux_client_get_keep_artifacts(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_keep_artifacts:
 * @self: an #AiClaudeTmuxClient
 * @keep: %TRUE to leave per-turn temp files on disk
 */
void
ai_claude_tmux_client_set_keep_artifacts(
    AiClaudeTmuxClient *self,
    gboolean            keep
);

/**
 * ai_claude_tmux_client_get_debug_preserve_tmux:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: %TRUE if the tmux session and all per-turn artifacts
 *   will be left in place after the turn for post-mortem inspection.
 *   Implies keep-artifacts behaviour.  Default %FALSE.
 */
gboolean
ai_claude_tmux_client_get_debug_preserve_tmux(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_debug_preserve_tmux:
 * @self: an #AiClaudeTmuxClient
 * @reserve: %TRUE to skip tmux kill-session and artifact unlink
 */
void
ai_claude_tmux_client_set_debug_preserve_tmux(
    AiClaudeTmuxClient *self,
    gboolean            reserve
);

/**
 * ai_claude_tmux_client_get_prompt_resend_interval_ms:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: how long the client waits for a `user` entry to appear in
 *   the transcript after pressing Enter before deciding the keystroke
 *   was swallowed and re-sending it (milliseconds).
 */
gint
ai_claude_tmux_client_get_prompt_resend_interval_ms(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_prompt_resend_interval_ms:
 * @self: an #AiClaudeTmuxClient
 * @interval_ms: per-attempt wait in milliseconds; must be > 0
 *
 * tmux occasionally fails to land the submit Enter against the claude
 * TUI, leaving the pasted prompt un-submitted.  After pressing Enter
 * the client polls the transcript for a new `user` entry; if none
 * appears within @interval_ms it re-presses Enter.  Default 2000 ms.
 */
void
ai_claude_tmux_client_set_prompt_resend_interval_ms(
    AiClaudeTmuxClient *self,
    gint                interval_ms
);

/**
 * ai_claude_tmux_client_get_max_prompt_send_attempts:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: the maximum number of Enter keystrokes the client will
 *   deliver while trying to get the claude TUI to accept the pasted
 *   prompt, before failing the turn.
 */
gint
ai_claude_tmux_client_get_max_prompt_send_attempts(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_max_prompt_send_attempts:
 * @self: an #AiClaudeTmuxClient
 * @attempts: maximum Enter keystrokes; must be > 0
 *
 * If the prompt is still not ingested after this many Enter
 * keystrokes (each followed by a prompt-resend-interval-ms wait), the
 * turn fails with %AI_ERROR_CLI_EXECUTION rather than blocking on a
 * Stop hook that will never fire.  Default 5.
 */
void
ai_claude_tmux_client_set_max_prompt_send_attempts(
    AiClaudeTmuxClient *self,
    gint                attempts
);

/**
 * ai_claude_tmux_client_get_dismiss_resume_prompt:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: %TRUE if, when resuming an existing session, the client
 *   types "2" ("resume as-is") before delivering the prompt to clear
 *   claude's interactive resume-mode picker.  Default %TRUE.
 */
gboolean
ai_claude_tmux_client_get_dismiss_resume_prompt(AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_dismiss_resume_prompt:
 * @self: an #AiClaudeTmuxClient
 * @dismiss: %TRUE to type "2" ("resume as-is") on resume
 *
 * When resuming an existing session, claude's TUI can stop on an
 * interactive picker asking whether to resume "with a summary",
 * "as-is", or "clear" — which blocks prompt delivery.  With this
 * enabled the client types "2" after the TUI is ready, selecting
 * "resume as-is"; the picker's default ("with a summary") would
 * instead trigger a compaction that can run for minutes.  It then
 * waits prompt-resend-interval-ms for the picker to tear down and the
 * input box to settle before delivering the prompt.
 *
 * Only takes effect on resume; fresh sessions never show the picker.
 * Worst case — no picker is actually shown — the "2" lands as a
 * literal character prepended to the pasted prompt, an accepted
 * cosmetic trade-off.  Default %TRUE.
 */
void
ai_claude_tmux_client_set_dismiss_resume_prompt(
    AiClaudeTmuxClient *self,
    gboolean            dismiss
);

/**
 * ai_claude_tmux_client_get_prompt_send_exponential_backoff:
 * @self: an #AiClaudeTmuxClient
 *
 * Returns: %TRUE if the submit-Enter retry loop doubles its
 *   per-attempt wait each iteration, %FALSE if every attempt waits
 *   the same prompt-resend-interval-ms.  Default %TRUE.
 *
 * Since: 0.20.4
 */
gboolean
ai_claude_tmux_client_get_prompt_send_exponential_backoff(
    AiClaudeTmuxClient *self);

/**
 * ai_claude_tmux_client_set_prompt_send_exponential_backoff:
 * @self: an #AiClaudeTmuxClient
 * @backoff: %TRUE to double the per-attempt wait each retry
 *
 * When %TRUE (the default), the submit-Enter retry loop waits
 * `prompt-resend-interval-ms << (attempt - 1)` milliseconds between
 * attempts: 2 s, 4 s, 8 s, 16 s, 32 s by default — a total budget of
 * ~62 s across the default 5 attempts.  That window is large enough
 * to ride out claude auto-compacting a multi-megabyte resumed
 * transcript before the TUI accepts input again.
 *
 * When %FALSE, every attempt waits a flat prompt-resend-interval-ms
 * (the pre-0.20.4 behaviour), giving a fixed total budget of
 * prompt-resend-interval-ms × max-prompt-send-attempts.
 *
 * Since: 0.20.4
 */
void
ai_claude_tmux_client_set_prompt_send_exponential_backoff(
    AiClaudeTmuxClient *self,
    gboolean            backoff
);

/* ================================================================== */
/* Pure-function helpers — exposed primarily for unit testing.        */
/* ================================================================== */

/**
 * ai_claude_tmux_client_encode_cwd:
 * @cwd: an absolute filesystem path (must not be %NULL)
 *
 * Encodes a working-directory path into the form claude uses for its
 * per-project transcript subdirectory.  The rule is simple: replace
 * every `/` with `-`.  An absolute path therefore starts with `-`.
 *
 * Examples:
 *   "/home/zach/work"       → "-home-zach-work"
 *   "/var/home/zach/clawd"  → "-var-home-zach-clawd"
 *   "/"                     → "-"
 *
 * Returns: (transfer full): a newly-allocated encoded string.
 *   Free with g_free().
 */
gchar *
ai_claude_tmux_client_encode_cwd(const gchar *cwd);

/**
 * ai_claude_tmux_client_compute_jsonl_path:
 * @project_dir: (nullable): root directory under which claude stores
 *   per-cwd subdirectories.  %NULL defaults to `$HOME/.claude/projects`.
 * @cwd: an absolute working-directory path
 * @session_id: a UUID for the session (must not be %NULL)
 *
 * Returns: (transfer full): the absolute path where the JSONL
 *   transcript for this (cwd, session_id) pair lives.
 *   Free with g_free().
 */
gchar *
ai_claude_tmux_client_compute_jsonl_path(
    const gchar *project_dir,
    const gchar *cwd,
    const gchar *session_id
);

/**
 * ai_claude_tmux_client_parse_jsonl:
 * @content: full contents of the .jsonl transcript file
 * @model: (nullable): model name to record on the AiResponse;
 *   if %NULL the parser uses the model field from the assistant entry
 * @total_cost_out: (out) (nullable): receives total cost reported by
 *   claude, or 0.0 if absent
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a claude transcript JSONL into an AiResponse.  Walks every
 * line and tracks the most recent `type: "assistant"` entry; the
 * response's text content is the concatenation of all `text` blocks
 * from that entry, in order.  Tool-use, tool-result, and `thinking`
 * blocks are ignored — only the user-visible text is returned.
 *
 * Lines that fail to parse as JSON are silently skipped (the
 * transcript is line-oriented and tolerates partial writes between
 * processes); only a complete absence of any assistant entry is an
 * error.
 *
 * Returns: (transfer full) (nullable): the parsed #AiResponse, or
 *   %NULL on error (e.g. no assistant entry found).
 */
AiResponse *
ai_claude_tmux_client_parse_jsonl(
    const gchar  *content,
    const gchar  *model,
    gdouble      *total_cost_out,
    GError      **error
);

/**
 * ai_claude_tmux_client_jsonl_has_accepted_prompt:
 * @jsonl_slice: the transcript JSONL text written after the
 *   pre-prompt watermark (must not be %NULL)
 *
 * Returns %TRUE if @jsonl_slice shows claude accepted a
 * freshly-submitted prompt: either a real `type:"user"` entry (one
 * that is not a compaction summary), or a `type:"queue-operation"`
 * entry with `operation:"enqueue"` (the prompt was submitted while
 * claude was busy — e.g. auto-compacting a large resumed session —
 * and is safely queued).  Lines that fail to parse are skipped.
 *
 * This is the signal the submit-Enter loop polls on: its absence
 * means the Enter keystroke never registered and should be re-sent.
 *
 * Returns: %TRUE if the slice shows the prompt was accepted.
 */
gboolean
ai_claude_tmux_client_jsonl_has_accepted_prompt(const gchar *jsonl_slice);

/**
 * ai_claude_tmux_client_wait_for_sentinel_or_idle:
 * @sentinel_path: path the Stop hook touches when the turn finishes
 * @activity_path: the JSONL transcript whose growth signals progress
 * @idle_timeout_ms: maximum time with NO transcript growth and no
 *   sentinel before the wait gives up
 * @cancellable: (nullable): aborts the wait with %AI_ERROR_CANCELLED
 * @error: (out) (optional): return location for a #GError
 *
 * Waits for @sentinel_path to appear, bounding the wait by INACTIVITY
 * rather than a fixed wall-clock deadline: every time @activity_path
 * grows the idle clock is reset, so an actively-working (but long)
 * claude turn is never killed mid-flight.  Fails with %AI_ERROR_TIMEOUT
 * only after the transcript has been completely silent for
 * @idle_timeout_ms, or with %AI_ERROR_CANCELLED if @cancellable trips.
 *
 * Exposed for unit testing; production callers use it internally.
 *
 * Returns: %TRUE once the sentinel appears, %FALSE on timeout/cancel.
 */
gboolean
ai_claude_tmux_client_wait_for_sentinel_or_idle(
    const gchar  *sentinel_path,
    const gchar  *activity_path,
    gint          idle_timeout_ms,
    GCancellable *cancellable,
    GError      **error
);

G_END_DECLS
