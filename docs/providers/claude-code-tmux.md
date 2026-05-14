# Claude Code (tmux) Provider

The Claude Code (tmux) provider (`AiClaudeTmuxClient`) drives the `claude` CLI in its **native interactive TUI** by spawning it inside an ephemeral [tmux](https://github.com/tmux/tmux) session. Unlike [`AiClaudeCodeClient`](claude-code.md) — which uses `claude --print` (the Agent SDK / non-interactive path) — this client speaks to a fully interactive `claude` process and so consumes the user's **normal subscription budget** rather than the Agent SDK credit pool.

This was introduced because Anthropic's Agent SDK billing path is changing 2026-06-15, and subscription-budget users wanted a way to continue routing automated workloads through their existing plan.

## Overview

| Property | Value |
|----------|-------|
| Type | `AI_PROVIDER_CLAUDE_TMUX` |
| Name | "Claude Code (tmux)" |
| Default Model | `sonnet` |
| Interface | CLI (subprocess wrapped in tmux) |
| Billing path | Subscription (TUI), not Agent SDK |

## Requirements

- The `claude` CLI must be installed, logged in, and on `$PATH` (or pointed at via `CLAUDE_CODE_PATH`).
- The `tmux` binary must be installed and on `$PATH` (or pointed at via `ai_claude_tmux_client_set_tmux_path()`).
- A writable `~/.claude/projects/` directory (or override via `set_claude_project_dir()`).

## Environment Variables

| Variable | Description |
|----------|-------------|
| `CLAUDE_CODE_PATH` | Override the path to the `claude` executable |

## Available Models

The Claude Code CLI uses simplified aliases — same as the non-tmux client:

| Define | Model Alias |
|--------|-------------|
| `AI_CLAUDE_TMUX_MODEL_OPUS` | `"opus"` |
| `AI_CLAUDE_TMUX_MODEL_SONNET` | `"sonnet"` |
| `AI_CLAUDE_TMUX_MODEL_HAIKU` | `"haiku"` |
| `AI_CLAUDE_TMUX_DEFAULT_MODEL` | `"sonnet"` |

## How It Works

Each call to `ai_provider_chat_sync()` (or its async equivalent) goes through this sequence:

1. **Generate a session UUID** (or reuse the configured one).
2. **Write the prompt** to a temp file atomically so the TUI can ingest it via Claude Code's `@<path>` file-reference syntax.
3. **Spawn** `tmux new-session -d -s <name> claude --session-id <uuid> --settings '<inline-json>' [args]`. The inline settings install a `Stop` hook that touches a per-session sentinel file when claude finishes a turn, and a `SessionStart` hook that touches a ready sentinel when claude's TUI is initialized.
4. **Wait** for the JSONL transcript file to appear under `~/.claude/projects/<encoded-cwd>/<uuid>.jsonl`.
5. **Capture the byte offset** of the JSONL file as `jsonl_size_before` — this is the high-water mark for "everything written by previous turns."
6. **Send the prompt** via `tmux send-keys -l '@<prompt-path>'` followed by Enter.
7. **Poll the JSONL** waiting for a **terminal assistant entry** to land past `jsonl_size_before` (see below).
8. **Parse** the JSONL: extract the last assistant entry's text blocks and usage, return as `AiResponse`.
9. **Tear down**: kill the tmux session, unlink the prompt + sentinel files. (Optionally preserved — see `debug-preserve-tmux` below.)

### Stop-hook / JSONL flush race

A subtle gotcha worth documenting: the Stop hook fires when `claude` has generated its assistant response **in memory**, but the on-disk JSONL flush of that response lags the flush of the user-message + attachment lines by up to ~2 seconds. A naive "wait for the file to grow past `jsonl_size_before`" check fires too early and reads the **previous** turn's last assistant entry, returning a stale response.

The poll loop therefore parses the slice of JSONL past `jsonl_size_before` and waits for an entry where:

- `type == "assistant"`, **and**
- `message.stop_reason` is set and is **not** `"tool_use"`.

`tool_use` stop reasons are intermediate; the Stop hook only fires on terminal reasons (`end_turn`, `stop_sequence`, `max_tokens`), so we're guaranteed to converge.

## Usage

### Basic Usage

```c
#include <ai-glib.h>

int main(void)
{
    g_autoptr(AiClaudeTmuxClient) client = ai_claude_tmux_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello, Claude!");
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;
    GList *messages = g_list_append(NULL, msg);

    response = ai_provider_chat_sync(
        AI_PROVIDER(client),
        messages,
        "You are a helpful assistant.",
        4096,
        NULL,
        NULL,
        &error
    );

    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }

    g_print("%s\n", ai_response_get_text(response));
    g_list_free(messages);
    return 0;
}
```

### Setting the Model

```c
g_autoptr(AiClaudeTmuxClient) client = ai_claude_tmux_client_new();

ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_CLAUDE_TMUX_MODEL_OPUS);
```

### Tuning Timeouts

The two relevant timeouts:

- **Startup timeout** — how long to wait for the JSONL transcript to appear after launch. Default: 15s.
- **Turn timeout** — how long to wait for the Stop-hook sentinel before declaring the turn timed out. Default: 120s.

```c
ai_claude_tmux_client_set_startup_timeout_ms(client, 30000);  /* 30s */
ai_claude_tmux_client_set_turn_timeout_ms(client, 180000);    /* 3m  */
```

### Skip Permissions

For automated/headless workloads where you don't want claude to prompt for tool-permission approvals:

```c
ai_claude_tmux_client_set_skip_permissions(client, TRUE);
```

This passes `--dangerously-skip-permissions` to `claude`. **Only use this in trusted environments** — it bypasses claude's normal "ask before destructive actions" gating.

### Debug Mode: preserve tmux + artifacts

When debugging a stuck or misbehaving turn, you can prevent the client from cleaning up after itself:

```c
ai_claude_tmux_client_set_debug_preserve_tmux(client, TRUE);
```

This:

- Skips `tmux kill-session` after the turn — you can `tmux attach -t claudetmux-<uuid>` to see the interactive session.
- Skips unlinking the prompt file and sentinel files.
- Implies `keep-artifacts` behaviour.

Default is `FALSE`. Production callers should leave it off — the per-turn artifacts pile up otherwise.

A lighter alternative if you only want to keep the temp files (without preserving the tmux session):

```c
ai_claude_tmux_client_set_keep_artifacts(client, TRUE);
```

### Overriding tmux / claude paths (testing)

For tests with a stand-in script:

```c
ai_claude_tmux_client_set_tmux_path(client, "/tmp/fake-tmux.sh");
ai_claude_tmux_client_set_claude_project_dir(client, "/tmp/test-projects");
```

Production callers should leave both `NULL` (search `$PATH` / use `$HOME/.claude/projects`).

### Getting Cost Information

The TUI doesn't always emit a cost field; missing values surface as `0.0`.

```c
gdouble cost = ai_claude_tmux_client_get_total_cost(client);
g_print("Last turn cost: $%.4f\n", cost);
```

## Comparison vs `AiClaudeCodeClient`

| Aspect | `AiClaudeCodeClient` | `AiClaudeTmuxClient` |
|--------|----------------------|----------------------|
| Spawn mode | `claude --print` | Interactive TUI inside tmux |
| Billing | Agent SDK credits | Normal subscription |
| Latency | Lower (no TUI startup) | Higher (~3-5s startup overhead) |
| Failure modes | stdout/stderr parsing | JSONL flush race, tmux session lifecycle |
| Best for | One-shot CLI batch work | Long-lived assistants, subscription budgets |

## Provider Type String

Configuration files and `ai_provider_type_from_string()` accept any of:

- `"claude-tmux"` / `"claude_tmux"` (canonical)
- `"claude-code-tmux"` / `"claude_code_tmux"` (alias, parallels `"claude-code"`)

All four map to `AI_PROVIDER_CLAUDE_TMUX`.

## See Also

- [Claude Code provider](claude-code.md) — the Agent-SDK / `--print` sibling.
- [Providers overview](index.md)
