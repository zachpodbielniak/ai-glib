/*
 * test-claude-tmux-client.c - Unit tests for AiClaudeTmuxClient
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Coverage focus: the helpers that can be exercised without spawning
 * the real `claude` binary.  End-to-end tmux orchestration is
 * verified by manual integration testing — these tests cover the
 * pieces that determine whether the response we hand back to the
 * caller (and thus to matrix) is correct:
 *
 *   - cwd-encoding for transcript subdirectory lookup
 *   - JSONL path assembly (with and without project-dir override)
 *   - JSONL parser for every assistant-entry shape we've observed
 *     in real claude transcripts (text-only, tool_use mixed in,
 *     thinking blocks, missing usage, multiple entries, etc.)
 *   - Class plumbing (properties, defaults, provider interface)
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "providers/ai-claude-tmux-client.h"
#include "core/ai-provider.h"
#include "core/ai-config.h"
#include "core/ai-error.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-text-content.h"
#include "model/ai-usage.h"

/* ================================================================== */
/* Class / type basics                                                 */
/* ================================================================== */

static void
test_new(void)
{
    g_autoptr(AiClaudeTmuxClient) client = ai_claude_tmux_client_new();
    g_assert_nonnull(client);
    g_assert_true(AI_IS_CLAUDE_TMUX_CLIENT(client));
    g_assert_true(AI_IS_CLI_CLIENT(client));
    g_assert_true(AI_IS_PROVIDER(client));
}

static void
test_new_with_config(void)
{
    g_autoptr(AiConfig) cfg = ai_config_new();
    g_autoptr(AiClaudeTmuxClient) client =
        ai_claude_tmux_client_new_with_config(cfg);
    g_assert_nonnull(client);
    g_assert_true(AI_IS_CLAUDE_TMUX_CLIENT(client));
}

static void
test_default_model(void)
{
    g_autoptr(AiClaudeTmuxClient) client = ai_claude_tmux_client_new();
    const gchar *m = ai_cli_client_get_model(AI_CLI_CLIENT(client));
    g_assert_cmpstr(m, ==, AI_CLAUDE_TMUX_DEFAULT_MODEL);
    g_assert_cmpstr(m, ==, "sonnet");
}

static void
test_provider_interface(void)
{
    g_autoptr(AiClaudeTmuxClient) client = ai_claude_tmux_client_new();
    AiProvider *p = AI_PROVIDER(client);

    g_assert_cmpstr(ai_provider_get_default_model(p), ==, "sonnet");
    g_assert_nonnull(ai_provider_get_name(p));
    /* Distinct from CLAUDE_CODE — billing model differs. */
    g_assert_cmpint(ai_provider_get_provider_type(p), ==,
                    AI_PROVIDER_CLAUDE_TMUX);

    /* Round-trip the enum value through the string helpers — guards
     * against the string-table drifting out of sync with the enum. */
    g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_CLAUDE_TMUX),
                    ==, "claude-tmux");
    g_assert_cmpint(ai_provider_type_from_string("claude-tmux"), ==,
                    AI_PROVIDER_CLAUDE_TMUX);
    g_assert_cmpint(ai_provider_type_from_string("claude_tmux"), ==,
                    AI_PROVIDER_CLAUDE_TMUX);
    g_assert_cmpint(ai_provider_type_from_string("CLAUDE-TMUX"), ==,
                    AI_PROVIDER_CLAUDE_TMUX);
}

/* ================================================================== */
/* Property setters / getters                                          */
/* ================================================================== */

static void
test_tmux_path_property(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();

    /* Default: NULL (search PATH). */
    g_assert_null(ai_claude_tmux_client_get_tmux_path(c));

    ai_claude_tmux_client_set_tmux_path(c, "/opt/local/bin/tmux");
    g_assert_cmpstr(ai_claude_tmux_client_get_tmux_path(c), ==,
                    "/opt/local/bin/tmux");

    /* Setting to NULL restores default. */
    ai_claude_tmux_client_set_tmux_path(c, NULL);
    g_assert_null(ai_claude_tmux_client_get_tmux_path(c));
}

static void
test_claude_project_dir_property(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();

    g_assert_null(ai_claude_tmux_client_get_claude_project_dir(c));

    ai_claude_tmux_client_set_claude_project_dir(c, "/tmp/fake-claude");
    g_assert_cmpstr(ai_claude_tmux_client_get_claude_project_dir(c), ==,
                    "/tmp/fake-claude");

    ai_claude_tmux_client_set_claude_project_dir(c, NULL);
    g_assert_null(ai_claude_tmux_client_get_claude_project_dir(c));
}

static void
test_timeouts(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();

    g_assert_cmpint(ai_claude_tmux_client_get_turn_timeout_ms(c), ==,
                    600000);
    g_assert_cmpint(ai_claude_tmux_client_get_startup_timeout_ms(c), ==,
                    15000);

    ai_claude_tmux_client_set_turn_timeout_ms(c, 1234);
    g_assert_cmpint(ai_claude_tmux_client_get_turn_timeout_ms(c), ==, 1234);

    ai_claude_tmux_client_set_startup_timeout_ms(c, 5678);
    g_assert_cmpint(ai_claude_tmux_client_get_startup_timeout_ms(c), ==,
                    5678);
}

static void
test_skip_permissions(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();
    g_assert_false(ai_claude_tmux_client_get_skip_permissions(c));
    ai_claude_tmux_client_set_skip_permissions(c, TRUE);
    g_assert_true(ai_claude_tmux_client_get_skip_permissions(c));
    ai_claude_tmux_client_set_skip_permissions(c, FALSE);
    g_assert_false(ai_claude_tmux_client_get_skip_permissions(c));
}

static void
test_keep_artifacts(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();
    g_assert_false(ai_claude_tmux_client_get_keep_artifacts(c));
    ai_claude_tmux_client_set_keep_artifacts(c, TRUE);
    g_assert_true(ai_claude_tmux_client_get_keep_artifacts(c));
}

static void
test_prompt_resend_interval(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();

    /* Default: 2 sec — the per-attempt wait for a user entry to show
     * up in the transcript before re-pressing Enter. */
    g_assert_cmpint(ai_claude_tmux_client_get_prompt_resend_interval_ms(c),
                    ==, 2000);

    ai_claude_tmux_client_set_prompt_resend_interval_ms(c, 750);
    g_assert_cmpint(ai_claude_tmux_client_get_prompt_resend_interval_ms(c),
                    ==, 750);
}

static void
test_max_prompt_send_attempts(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();

    /* Default: 5 Enter keystrokes before the turn fails. */
    g_assert_cmpint(ai_claude_tmux_client_get_max_prompt_send_attempts(c),
                    ==, 5);

    ai_claude_tmux_client_set_max_prompt_send_attempts(c, 12);
    g_assert_cmpint(ai_claude_tmux_client_get_max_prompt_send_attempts(c),
                    ==, 12);
}

static void
test_dismiss_resume_prompt(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();

    /* Default: on — claude's resume-mode picker is auto-dismissed. */
    g_assert_true(ai_claude_tmux_client_get_dismiss_resume_prompt(c));

    ai_claude_tmux_client_set_dismiss_resume_prompt(c, FALSE);
    g_assert_false(ai_claude_tmux_client_get_dismiss_resume_prompt(c));

    ai_claude_tmux_client_set_dismiss_resume_prompt(c, TRUE);
    g_assert_true(ai_claude_tmux_client_get_dismiss_resume_prompt(c));
}

static void
test_prompt_send_exponential_backoff(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();

    /* Default: on — each retry doubles the per-attempt wait. */
    g_assert_true(
        ai_claude_tmux_client_get_prompt_send_exponential_backoff(c));

    ai_claude_tmux_client_set_prompt_send_exponential_backoff(c, FALSE);
    g_assert_false(
        ai_claude_tmux_client_get_prompt_send_exponential_backoff(c));

    ai_claude_tmux_client_set_prompt_send_exponential_backoff(c, TRUE);
    g_assert_true(
        ai_claude_tmux_client_get_prompt_send_exponential_backoff(c));
}

static void
test_prompt_send_exponential_backoff_property(void)
{
    /*
     * Property-bag round-trip — confirms the GParamSpec is installed
     * and the get_property/set_property handlers reach the right
     * field.  Necessary because g_object_set/get is how libreclaw
     * (and external introspection) drives this knob.
     */
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();
    gboolean v;

    g_object_set(c, "prompt-send-exponential-backoff", FALSE, NULL);
    g_object_get(c, "prompt-send-exponential-backoff", &v, NULL);
    g_assert_false(v);

    g_object_set(c, "prompt-send-exponential-backoff", TRUE, NULL);
    g_object_get(c, "prompt-send-exponential-backoff", &v, NULL);
    g_assert_true(v);
}

static void
test_total_cost_initial(void)
{
    g_autoptr(AiClaudeTmuxClient) c = ai_claude_tmux_client_new();
    g_assert_cmpfloat(ai_claude_tmux_client_get_total_cost(c), ==, 0.0);
}

/* ================================================================== */
/* encode_cwd                                                          */
/* ================================================================== */

static void
test_encode_cwd_typical(void)
{
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("/home/zach/work");
    g_assert_cmpstr(out, ==, "-home-zach-work");
}

static void
test_encode_cwd_var_home(void)
{
    /* The real-world path on Fedora Silverblue, the exact pattern
     * claude uses for clawd's workspace. */
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("/var/home/zach/Documents/clawd");
    g_assert_cmpstr(out, ==, "-var-home-zach-Documents-clawd");
}

static void
test_encode_cwd_root(void)
{
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("/");
    g_assert_cmpstr(out, ==, "-");
}

static void
test_encode_cwd_empty(void)
{
    /* Edge case: empty string in == empty string out.  Not a valid
     * cwd but the encoder is total. */
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("");
    g_assert_cmpstr(out, ==, "");
}

static void
test_encode_cwd_trailing_slash(void)
{
    /* We do NOT normalize trailing slashes — exact mirror only. */
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("/foo/bar/");
    g_assert_cmpstr(out, ==, "-foo-bar-");
}

static void
test_encode_cwd_double_slash(void)
{
    /* Double slashes pass through (each becomes a dash). */
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("/foo//bar");
    g_assert_cmpstr(out, ==, "-foo--bar");
}

static void
test_encode_cwd_dots_and_dashes_passthrough(void)
{
    /* Non-slash characters are byte-copied verbatim. */
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("/home/.config/my-app");
    g_assert_cmpstr(out, ==, "-home-.config-my-app");
}

static void
test_encode_cwd_unicode_passthrough(void)
{
    /* UTF-8 in directory names passes through unchanged. */
    g_autofree gchar *out;
    out = ai_claude_tmux_client_encode_cwd("/home/\xC3\xA9");
    g_assert_cmpstr(out, ==, "-home-\xC3\xA9");
}

/* ================================================================== */
/* compute_jsonl_path                                                  */
/* ================================================================== */

static void
test_compute_jsonl_path_default_root(void)
{
    /* When project_dir is NULL we fall back to $HOME/.claude/projects. */
    g_autofree gchar *path;
    g_autofree gchar *expected;
    const gchar *home = g_get_home_dir();

    path = ai_claude_tmux_client_compute_jsonl_path(
        NULL, "/home/zach/work", "abc-uuid");

    expected = g_build_filename(home, ".claude", "projects",
                                "-home-zach-work",
                                "abc-uuid.jsonl", NULL);
    g_assert_cmpstr(path, ==, expected);
}

static void
test_compute_jsonl_path_empty_project_dir_uses_default(void)
{
    /* Empty-string project_dir behaves like NULL. */
    g_autofree gchar *a;
    g_autofree gchar *b;
    a = ai_claude_tmux_client_compute_jsonl_path(NULL, "/x", "u");
    b = ai_claude_tmux_client_compute_jsonl_path("",   "/x", "u");
    g_assert_cmpstr(a, ==, b);
}

static void
test_compute_jsonl_path_explicit_root(void)
{
    g_autofree gchar *path;
    path = ai_claude_tmux_client_compute_jsonl_path(
        "/tmp/fake-claude", "/var/home/zach/Documents/clawd",
        "deadbeef-1234");
    g_assert_cmpstr(path, ==,
        "/tmp/fake-claude/-var-home-zach-Documents-clawd/"
        "deadbeef-1234.jsonl");
}

static void
test_compute_jsonl_path_root_cwd(void)
{
    g_autofree gchar *path;
    path = ai_claude_tmux_client_compute_jsonl_path(
        "/tmp/r", "/", "uu");
    g_assert_cmpstr(path, ==, "/tmp/r/-/uu.jsonl");
}

/* ================================================================== */
/* parse_jsonl                                                         */
/* ================================================================== */

/*
 * The transcript shape (from real claude transcripts on this host):
 *   {"type":"assistant","sessionId":"...",
 *    "message":{"role":"assistant","content":[{"type":"text","text":"..."}],
 *               "model":"...","stop_reason":"end_turn",
 *               "usage":{"input_tokens":N,"output_tokens":N}},
 *    ...}
 *
 * The parser must:
 *   - skip non-assistant entries (queue-operation, user, attachment, ...)
 *   - tolerate malformed lines (partial writes)
 *   - prefer the LAST assistant entry (turn may have multiple)
 *   - concatenate every text block; ignore tool_use / thinking blocks
 *   - extract usage from message.usage
 *   - extract cost from either top-level or message-level total_cost_usd
 */

static const gchar *VALID_TRANSCRIPT_SIMPLE =
    /* irrelevant queue entry (must be ignored) */
    "{\"type\":\"queue-operation\",\"operation\":\"x\","
        "\"sessionId\":\"abc\",\"timestamp\":\"t\"}\n"
    /* user turn */
    "{\"type\":\"user\",\"sessionId\":\"abc\","
        "\"message\":{\"role\":\"user\","
        "\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}}\n"
    /* assistant turn */
    "{\"type\":\"assistant\",\"sessionId\":\"abc\","
        "\"message\":{\"role\":\"assistant\","
        "\"content\":[{\"type\":\"text\",\"text\":\"hello!\"}],"
        "\"model\":\"claude-opus-4-7\","
        "\"stop_reason\":\"end_turn\","
        "\"usage\":{\"input_tokens\":12,\"output_tokens\":34}}}\n";

static void
test_parse_jsonl_simple(void)
{
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;
    AiUsage *usage;
    gdouble cost = -1.0;

    r = ai_claude_tmux_client_parse_jsonl(
        VALID_TRANSCRIPT_SIMPLE, NULL, &cost, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);

    g_assert_cmpstr(ai_response_get_id(r), ==, "abc");
    text = ai_response_get_text(r);
    g_assert_cmpstr(text, ==, "hello!");

    usage = ai_response_get_usage(r);
    g_assert_nonnull(usage);
    g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 12);
    g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 34);

    g_assert_cmpint(ai_response_get_stop_reason(r), ==,
                    AI_STOP_REASON_END_TURN);

    /* No cost field in the fixture. */
    g_assert_cmpfloat(cost, ==, 0.0);
}

static void
test_parse_jsonl_explicit_model_override(void)
{
    /* When the caller passes a model, the AiResponse records it
     * (instead of the model field from the transcript). */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    r = ai_claude_tmux_client_parse_jsonl(
        VALID_TRANSCRIPT_SIMPLE, "sonnet", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    g_assert_cmpstr(ai_response_get_model(r), ==, "sonnet");
}

static void
test_parse_jsonl_no_assistant(void)
{
    /* Transcript that has user entries but no assistant entry should
     * fail — there's no response to return. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    const gchar *t =
        "{\"type\":\"user\",\"sessionId\":\"x\",\"message\":{\"role\":\"user\","
        "\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}}\n";

    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_null(r);
    g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_parse_jsonl_empty(void)
{
    /* Empty transcript → no assistant entry → error. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    r = ai_claude_tmux_client_parse_jsonl("", NULL, NULL, &error);
    g_assert_null(r);
    g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_parse_jsonl_skips_corrupt_lines(void)
{
    /* A half-written line in the middle of the file must be skipped
     * without aborting parsing.  This race actually happens — claude
     * writes the transcript incrementally and we tail-read it. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        /* A truncated line — claude was mid-write when we read.  Note
         * the trailing \n so g_strsplit sees this as its own line. */
        "{\"type\":\"queue-operation\",\n"
        /* Well-formed line that must still be parsed. */
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"survived\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    text = ai_response_get_text(r);
    g_assert_cmpstr(text, ==, "survived");
}

static void
test_parse_jsonl_blank_lines(void)
{
    /* Blank lines must be tolerated.  Empty input → no assistant, but
     * blank lines mixed with valid content must not break parsing. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        "\n\n"
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}\n"
        "\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_no_error(error);
    text = ai_response_get_text(r);
    g_assert_cmpstr(text, ==, "ok");
}

static void
test_parse_jsonl_multiple_assistant_entries(void)
{
    /* A single turn may produce multiple assistant entries when the
     * model calls tools mid-turn.  We want the LAST one (it carries
     * the final text reply). */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"thinking aloud...\"},"
                         "{\"type\":\"tool_use\",\"id\":\"u1\","
                          "\"name\":\"Bash\",\"input\":{}}],"
            "\"stop_reason\":\"tool_use\"}}\n"
        "{\"type\":\"user\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"user\","
            "\"content\":[{\"type\":\"tool_result\","
                          "\"tool_use_id\":\"u1\","
                          "\"content\":\"output\"}]}}\n"
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"FINAL answer\"}],"
            "\"stop_reason\":\"end_turn\","
            "\"usage\":{\"input_tokens\":100,\"output_tokens\":5}}}\n";

    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_no_error(error);
    text = ai_response_get_text(r);
    g_assert_cmpstr(text, ==, "FINAL answer");
}

static void
test_parse_jsonl_tool_use_only(void)
{
    /* Assistant entry with only a tool_use block (no text).  Parser
     * should still produce a (textless) AiResponse, not an error. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"tool_use\",\"id\":\"u1\","
                          "\"name\":\"Bash\",\"input\":{}}],"
            "\"stop_reason\":\"tool_use\"}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    /* Empty / no text content. */
    text = ai_response_get_text(r);
    g_assert_true(text == NULL || text[0] == '\0');
}

static void
test_parse_jsonl_multiple_text_blocks(void)
{
    /* Multiple text blocks in the same message — concatenate in order. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"first \"},"
                         "{\"type\":\"text\",\"text\":\"second \"},"
                         "{\"type\":\"text\",\"text\":\"third\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    text = ai_response_get_text(r);
    g_assert_cmpstr(text, ==, "first second third");
}

static void
test_parse_jsonl_ignores_thinking(void)
{
    /* Thinking blocks are internal-only — never relayed to the user.
     * Same goes for redacted_thinking and any future internal block. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"thinking\","
                          "\"text\":\"reasoning here\"},"
                         "{\"type\":\"text\",\"text\":\"hi user\"},"
                         "{\"type\":\"redacted_thinking\","
                          "\"text\":\"hidden\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    text = ai_response_get_text(r);
    g_assert_cmpstr(text, ==, "hi user");
}

static void
test_parse_jsonl_multiline_text(void)
{
    /* Newlines inside text blocks are preserved verbatim — matrix
     * formatting depends on them. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\","
                          "\"text\":\"line one\\nline two\\nline three\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    text = ai_response_get_text(r);
    g_assert_cmpstr(text, ==, "line one\nline two\nline three");
}

static void
test_parse_jsonl_utf8_text(void)
{
    /* Non-ASCII content must round-trip cleanly. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    /* "café — résumé ✓" expressed as \uXXXX so the source file stays
     * plain ASCII. */
    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\","
                          "\"text\":\"caf\\u00e9 \\u2014 r\\u00e9sum\\u00e9 \\u2713\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    text = ai_response_get_text(r);
    /* Should decode to UTF-8 bytes: caf<C3 A9> <E2 80 94> r<C3 A9>sum<C3 A9> <E2 9C 93> */
    g_assert_cmpstr(text, ==,
        "caf\xC3\xA9 \xE2\x80\x94 r\xC3\xA9sum\xC3\xA9 \xE2\x9C\x93");
}

static void
test_parse_jsonl_usage_missing(void)
{
    /* If the assistant entry has no usage field, the AiResponse has
     * no usage attached.  Caller treats absence as "unknown cost". */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    g_assert_null(ai_response_get_usage(r));
}

static void
test_parse_jsonl_cost_toplevel(void)
{
    /* Some transcripts attach total_cost_usd at the top level, some
     * at the message level — parser must accept both. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    gdouble cost = -1.0;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"total_cost_usd\":0.0042,"
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, &cost, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(cost, >=, 0.0042 - 1e-9);
    g_assert_cmpfloat(cost, <=, 0.0042 + 1e-9);
}

static void
test_parse_jsonl_cost_message_level(void)
{
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    gdouble cost = -1.0;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"total_cost_usd\":0.123,"
            "\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, &cost, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(cost, >=, 0.123 - 1e-9);
    g_assert_cmpfloat(cost, <=, 0.123 + 1e-9);
}

static void
test_parse_jsonl_cost_absent(void)
{
    /* No cost field anywhere → cost_out is set to 0.0 (not left
     * uninitialized). */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    gdouble cost = -1.0;

    r = ai_claude_tmux_client_parse_jsonl(
        VALID_TRANSCRIPT_SIMPLE, NULL, &cost, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(cost, ==, 0.0);
}

static void
test_parse_jsonl_cost_out_null(void)
{
    /* Cost output parameter is optional. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    r = ai_claude_tmux_client_parse_jsonl(
        VALID_TRANSCRIPT_SIMPLE, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
}

static void
test_parse_jsonl_session_id_from_assistant(void)
{
    /* sessionId at the top level of the assistant entry is captured. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"my-uuid-123\","
            "\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(ai_response_get_id(r), ==, "my-uuid-123");
}

static void
test_parse_jsonl_message_with_no_content_array(void)
{
    /* Defensive: an assistant entry whose message has no content
     * field at all.  Still a valid entry shape — produce a textless
     * response, not an error. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"assistant\","
            "\"stop_reason\":\"end_turn\"}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    text = ai_response_get_text(r);
    g_assert_true(text == NULL || text[0] == '\0');
}

static void
test_parse_jsonl_wrong_role_skipped(void)
{
    /* Defensive: a type:"assistant" entry whose role isn't actually
     * "assistant" must be skipped (some future shape might use type
     * to mean something else). */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    const gchar *t =
        "{\"type\":\"assistant\",\"sessionId\":\"s\","
            "\"message\":{\"role\":\"system\","
            "\"content\":[{\"type\":\"text\",\"text\":\"nope\"}]}}\n";
    r = ai_claude_tmux_client_parse_jsonl(t, NULL, NULL, &error);
    /* No valid assistant entry → error. */
    g_assert_null(r);
    g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_parse_jsonl_null_content(void)
{
    /* The C contract says content must not be NULL.  Cover the
     * defensive return-NULL behaviour. */
    g_autoptr(AiResponse) r = NULL;
    g_autoptr(GError) error = NULL;

    /* Programmatic NULL is rejected by g_return_val_if_fail; using
     * a critical handler trap makes the test deterministic. */
    if (g_test_subprocess())
    {
        r = ai_claude_tmux_client_parse_jsonl(NULL, NULL, NULL, &error);
        g_assert_null(r);
        return;
    }
    g_test_trap_subprocess(NULL, 0,
        G_TEST_SUBPROCESS_INHERIT_STDOUT |
        G_TEST_SUBPROCESS_INHERIT_STDERR);
    g_test_trap_assert_failed();
}

/* ================================================================== */
/* jsonl_has_accepted_prompt                                           */
/* ================================================================== */

/*
 * The submit-Enter loop polls this to decide whether the Enter
 * keystroke registered.  Acceptance has two shapes: a real
 * (non-compaction-summary) type:"user" entry, OR a
 * queue-operation/enqueue entry — the prompt was submitted while
 * claude was busy (e.g. auto-compacting a large resumed session) and
 * is safely queued.  Mistaking the queued case for a swallowed
 * keystroke is what caused us to retry 5x and kill claude
 * mid-compaction.
 */

static void
test_accepted_prompt_user_entry(void)
{
    /* claude was idle: the prompt became a real user turn. */
    const gchar *slice =
        "{\"type\":\"user\",\"message\":{\"role\":\"user\","
        "\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}}\n";
    g_assert_true(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

static void
test_accepted_prompt_enqueued(void)
{
    /* claude was busy: the prompt was queued, not lost.  This is the
     * exact entry shape that previously read as "not ingested". */
    const gchar *slice =
        "{\"type\":\"queue-operation\",\"operation\":\"enqueue\","
        "\"content\":\"Current time: ...\\n\\nclawdbot: you alive?\"}\n";
    g_assert_true(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

static void
test_accepted_prompt_compact_summary_excluded(void)
{
    /* A finished auto-compaction logs its summary as type:"user" with
     * isCompactSummary:true — claude talking to itself, NOT our
     * prompt landing.  Must not count as acceptance. */
    const gchar *slice =
        "{\"type\":\"system\",\"subtype\":\"compact_boundary\","
        "\"compactMetadata\":{\"trigger\":\"auto\"}}\n"
        "{\"type\":\"user\",\"isCompactSummary\":true,"
        "\"message\":{\"role\":\"user\",\"content\":\"<summary>\"}}\n";
    g_assert_false(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

static void
test_accepted_prompt_dequeue_not_accepted(void)
{
    /* Only enqueue is acceptance; a dequeue marker alone is not. */
    const gchar *slice =
        "{\"type\":\"queue-operation\",\"operation\":\"dequeue\"}\n";
    g_assert_false(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

static void
test_accepted_prompt_metadata_only(void)
{
    /* Resume-time metadata churn with no submission — not accepted. */
    const gchar *slice =
        "{\"type\":\"ai-title\",\"aiTitle\":\"Some title\"}\n"
        "{\"type\":\"permission-mode\","
        "\"permissionMode\":\"bypassPermissions\"}\n";
    g_assert_false(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

static void
test_accepted_prompt_regression_slice(void)
{
    /* The actual post-watermark slice from the logged failure: a
     * queue-operation/enqueue followed by ai-title + permission-mode
     * metadata.  The old check looked only for type:"user" and so
     * declared the prompt lost; this must now read as accepted. */
    const gchar *slice =
        "{\"type\":\"queue-operation\",\"operation\":\"enqueue\","
        "\"timestamp\":\"2026-05-14T18:58:08.875Z\","
        "\"content\":\"clawdbot: you alive buddy?\"}\n"
        "{\"type\":\"ai-title\","
        "\"aiTitle\":\"Merge attachment download feature\"}\n"
        "{\"type\":\"permission-mode\","
        "\"permissionMode\":\"bypassPermissions\"}\n";
    g_assert_true(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

static void
test_accepted_prompt_tolerates_corrupt_line(void)
{
    /* A half-written line (we tail-read a file claude writes
     * incrementally) must be skipped, not abort the scan. */
    const gchar *slice =
        "{\"type\":\"queue-operation\",\"operati\n"   /* truncated */
        "{\"type\":\"queue-operation\",\"operation\":\"enqueue\","
        "\"content\":\"x\"}\n";
    g_assert_true(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

static void
test_accepted_prompt_empty(void)
{
    /* Empty slice — nothing written past the watermark yet. */
    g_assert_false(ai_claude_tmux_client_jsonl_has_accepted_prompt(""));
}

static void
test_accepted_prompt_summary_then_real_prompt(void)
{
    /* A compaction finishes (summary) and THEN our queued prompt is
     * dequeued into a real user turn — acceptance, found past the
     * compaction summary that precedes it. */
    const gchar *slice =
        "{\"type\":\"user\",\"isCompactSummary\":true,"
        "\"message\":{\"role\":\"user\",\"content\":\"<summary>\"}}\n"
        "{\"type\":\"user\",\"message\":{\"role\":\"user\","
        "\"content\":[{\"type\":\"text\",\"text\":\"the real prompt\"}]}}\n";
    g_assert_true(ai_claude_tmux_client_jsonl_has_accepted_prompt(slice));
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* class basics */
    g_test_add_func("/claude-tmux/new", test_new);
    g_test_add_func("/claude-tmux/new-with-config", test_new_with_config);
    g_test_add_func("/claude-tmux/default-model", test_default_model);
    g_test_add_func("/claude-tmux/provider-interface", test_provider_interface);

    /* properties */
    g_test_add_func("/claude-tmux/prop/tmux-path", test_tmux_path_property);
    g_test_add_func("/claude-tmux/prop/claude-project-dir",
                    test_claude_project_dir_property);
    g_test_add_func("/claude-tmux/prop/timeouts", test_timeouts);
    g_test_add_func("/claude-tmux/prop/skip-permissions",
                    test_skip_permissions);
    g_test_add_func("/claude-tmux/prop/keep-artifacts", test_keep_artifacts);
    g_test_add_func("/claude-tmux/prop/prompt-resend-interval",
                    test_prompt_resend_interval);
    g_test_add_func("/claude-tmux/prop/max-prompt-send-attempts",
                    test_max_prompt_send_attempts);
    g_test_add_func("/claude-tmux/prop/dismiss-resume-prompt",
                    test_dismiss_resume_prompt);
    g_test_add_func("/claude-tmux/prop/prompt-send-exponential-backoff",
                    test_prompt_send_exponential_backoff);
    g_test_add_func("/claude-tmux/prop/prompt-send-exp-backoff-gobject",
                    test_prompt_send_exponential_backoff_property);
    g_test_add_func("/claude-tmux/prop/total-cost-initial",
                    test_total_cost_initial);

    /* encode_cwd */
    g_test_add_func("/claude-tmux/encode-cwd/typical",
                    test_encode_cwd_typical);
    g_test_add_func("/claude-tmux/encode-cwd/var-home",
                    test_encode_cwd_var_home);
    g_test_add_func("/claude-tmux/encode-cwd/root", test_encode_cwd_root);
    g_test_add_func("/claude-tmux/encode-cwd/empty", test_encode_cwd_empty);
    g_test_add_func("/claude-tmux/encode-cwd/trailing-slash",
                    test_encode_cwd_trailing_slash);
    g_test_add_func("/claude-tmux/encode-cwd/double-slash",
                    test_encode_cwd_double_slash);
    g_test_add_func("/claude-tmux/encode-cwd/dots-and-dashes",
                    test_encode_cwd_dots_and_dashes_passthrough);
    g_test_add_func("/claude-tmux/encode-cwd/unicode",
                    test_encode_cwd_unicode_passthrough);

    /* compute_jsonl_path */
    g_test_add_func("/claude-tmux/jsonl-path/default-root",
                    test_compute_jsonl_path_default_root);
    g_test_add_func("/claude-tmux/jsonl-path/empty-project-dir",
                    test_compute_jsonl_path_empty_project_dir_uses_default);
    g_test_add_func("/claude-tmux/jsonl-path/explicit-root",
                    test_compute_jsonl_path_explicit_root);
    g_test_add_func("/claude-tmux/jsonl-path/root-cwd",
                    test_compute_jsonl_path_root_cwd);

    /* parse_jsonl */
    g_test_add_func("/claude-tmux/parse/simple", test_parse_jsonl_simple);
    g_test_add_func("/claude-tmux/parse/explicit-model-override",
                    test_parse_jsonl_explicit_model_override);
    g_test_add_func("/claude-tmux/parse/no-assistant",
                    test_parse_jsonl_no_assistant);
    g_test_add_func("/claude-tmux/parse/empty", test_parse_jsonl_empty);
    g_test_add_func("/claude-tmux/parse/skips-corrupt-lines",
                    test_parse_jsonl_skips_corrupt_lines);
    g_test_add_func("/claude-tmux/parse/blank-lines",
                    test_parse_jsonl_blank_lines);
    g_test_add_func("/claude-tmux/parse/multiple-assistant",
                    test_parse_jsonl_multiple_assistant_entries);
    g_test_add_func("/claude-tmux/parse/tool-use-only",
                    test_parse_jsonl_tool_use_only);
    g_test_add_func("/claude-tmux/parse/multiple-text-blocks",
                    test_parse_jsonl_multiple_text_blocks);
    g_test_add_func("/claude-tmux/parse/ignores-thinking",
                    test_parse_jsonl_ignores_thinking);
    g_test_add_func("/claude-tmux/parse/multiline-text",
                    test_parse_jsonl_multiline_text);
    g_test_add_func("/claude-tmux/parse/utf8-text",
                    test_parse_jsonl_utf8_text);
    g_test_add_func("/claude-tmux/parse/usage-missing",
                    test_parse_jsonl_usage_missing);
    g_test_add_func("/claude-tmux/parse/cost-toplevel",
                    test_parse_jsonl_cost_toplevel);
    g_test_add_func("/claude-tmux/parse/cost-message-level",
                    test_parse_jsonl_cost_message_level);
    g_test_add_func("/claude-tmux/parse/cost-absent",
                    test_parse_jsonl_cost_absent);
    g_test_add_func("/claude-tmux/parse/cost-out-null",
                    test_parse_jsonl_cost_out_null);
    g_test_add_func("/claude-tmux/parse/session-id-from-assistant",
                    test_parse_jsonl_session_id_from_assistant);
    g_test_add_func("/claude-tmux/parse/no-content-array",
                    test_parse_jsonl_message_with_no_content_array);
    g_test_add_func("/claude-tmux/parse/wrong-role-skipped",
                    test_parse_jsonl_wrong_role_skipped);
    g_test_add_func("/claude-tmux/parse/null-content-rejected",
                    test_parse_jsonl_null_content);

    /* jsonl_has_accepted_prompt */
    g_test_add_func("/claude-tmux/accepted-prompt/user-entry",
                    test_accepted_prompt_user_entry);
    g_test_add_func("/claude-tmux/accepted-prompt/enqueued",
                    test_accepted_prompt_enqueued);
    g_test_add_func("/claude-tmux/accepted-prompt/compact-summary-excluded",
                    test_accepted_prompt_compact_summary_excluded);
    g_test_add_func("/claude-tmux/accepted-prompt/dequeue-not-accepted",
                    test_accepted_prompt_dequeue_not_accepted);
    g_test_add_func("/claude-tmux/accepted-prompt/metadata-only",
                    test_accepted_prompt_metadata_only);
    g_test_add_func("/claude-tmux/accepted-prompt/regression-slice",
                    test_accepted_prompt_regression_slice);
    g_test_add_func("/claude-tmux/accepted-prompt/tolerates-corrupt-line",
                    test_accepted_prompt_tolerates_corrupt_line);
    g_test_add_func("/claude-tmux/accepted-prompt/empty",
                    test_accepted_prompt_empty);
    g_test_add_func("/claude-tmux/accepted-prompt/summary-then-real-prompt",
                    test_accepted_prompt_summary_then_real_prompt);

    return g_test_run();
}
