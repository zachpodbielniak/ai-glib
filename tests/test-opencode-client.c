/*
 * test-opencode-client.c - Unit tests for AiOpenCodeClient
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "providers/ai-opencode-client.h"
#include "core/ai-error.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-text-content.h"
#include "model/ai-usage.h"

/* ───────────────────────────────────────────────────────────────────
 * Helper: call parse_json_output via the virtual method table.
 * ─────────────────────────────────────────────────────────────────── */
static AiResponse *
call_parse_json_output(
	AiOpenCodeClient *client,
	const gchar      *json_output,
	GError          **error
)
{
	AiCliClientClass *klass;

	klass = AI_CLI_CLIENT_GET_CLASS(client);
	return klass->parse_json_output(AI_CLI_CLIENT(client), json_output, error);
}

/* ───────────────────────────────────────────────────────────────────
 * Helper: call build_stdin via the virtual method table.
 * ─────────────────────────────────────────────────────────────────── */
static gchar *
call_build_stdin(
	AiOpenCodeClient *client,
	GList            *messages
)
{
	AiCliClientClass *klass;

	klass = AI_CLI_CLIENT_GET_CLASS(client);
	return klass->build_stdin(AI_CLI_CLIENT(client), messages);
}

/* ───────────────────────────────────────────────────────────────────
 * Helper: call build_argv via the virtual method table.
 * ─────────────────────────────────────────────────────────────────── */
static gchar **
call_build_argv(
	AiOpenCodeClient *client,
	GList            *messages,
	const gchar      *system_prompt,
	gint              max_tokens,
	gboolean          streaming
)
{
	AiCliClientClass *klass;

	klass = AI_CLI_CLIENT_GET_CLASS(client);
	return klass->build_argv(AI_CLI_CLIENT(client), messages,
	                         system_prompt, max_tokens, streaming);
}

/* ───────────────────────────────────────────────────────────────────
 * Helpers: locate flags in a built argv.
 * ─────────────────────────────────────────────────────────────────── */
static gint
argv_index_of(gchar **argv, const gchar *needle)
{
	gint i;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			return i;
	return -1;
}

static gint
argv_count(gchar **argv, const gchar *needle)
{
	gint i, n = 0;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			n++;
	return n;
}

static const gchar *
argv_value_after(gchar **argv, const gchar *needle)
{
	gint i = argv_index_of(argv, needle);

	if (i < 0 || argv[i + 1] == NULL)
		return NULL;

	return argv[i + 1];
}

/* Build argv for a one-message turn against @client. */
static gchar **
build_argv_for(AiOpenCodeClient *client)
{
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	gchar **argv;

	argv = call_build_argv(client, messages, NULL, 4096, FALSE);

	g_list_free_full(messages, g_object_unref);
	return argv;
}

/* ───────────────────────────────────────────────────────────────────
 * Basic construction / interface tests
 * ─────────────────────────────────────────────────────────────────── */

static void
test_opencode_client_new(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;

	client = ai_opencode_client_new();
	g_assert_nonnull(client);
	g_assert_true(AI_IS_OPENCODE_CLIENT(client));
	g_assert_true(AI_IS_CLI_CLIENT(client));
}

static void
test_opencode_client_default_model(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	const gchar *model;

	client = ai_opencode_client_new();
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_DEFAULT_MODEL);
}

static void
test_opencode_client_provider_interface(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	AiProviderType type;
	const gchar *name;
	const gchar *model;

	client = ai_opencode_client_new();

	g_assert_true(AI_IS_PROVIDER(client));

	type = ai_provider_get_provider_type(AI_PROVIDER(client));
	g_assert_cmpint(type, ==, AI_PROVIDER_OPENCODE);

	name = ai_provider_get_name(AI_PROVIDER(client));
	g_assert_cmpstr(name, ==, "OpenCode");

	model = ai_provider_get_default_model(AI_PROVIDER(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_DEFAULT_MODEL);
}

static void
test_opencode_client_streamable_interface(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;

	client = ai_opencode_client_new();

	g_assert_true(AI_IS_STREAMABLE(client));
}

static void
test_opencode_client_model(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	const gchar *model;

	client = ai_opencode_client_new();

	/* Default model */
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_DEFAULT_MODEL);

	/* Custom model - Anthropic */
	ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_CLAUDE_OPUS_4);
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_MODEL_CLAUDE_OPUS_4);

	/* Custom model - OpenAI */
	ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_GPT_4O);
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_MODEL_GPT_4O);

	/* Custom model - Google */
	ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_GEMINI_2_FLASH);
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_MODEL_GEMINI_2_FLASH);
}

static void
test_opencode_client_executable_path(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	const gchar *path;

	client = ai_opencode_client_new();

	/* Default path (NULL means search PATH) */
	path = ai_cli_client_get_executable_path(AI_CLI_CLIENT(client));
	g_assert_null(path);

	/* Set custom path */
	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), "/usr/local/bin/opencode");
	path = ai_cli_client_get_executable_path(AI_CLI_CLIENT(client));
	g_assert_cmpstr(path, ==, "/usr/local/bin/opencode");
}

static void
test_opencode_client_gtype(void)
{
	GType type;

	type = ai_opencode_client_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiOpenCodeClient");
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: text events (normal case)
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_text_only(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *text;
	const gchar *ndjson =
		"{\"type\":\"step_start\",\"sessionID\":\"s1\"}\n"
		"{\"type\":\"text\",\"part\":{\"text\":\"Hello \"}}\n"
		"{\"type\":\"text\",\"part\":{\"text\":\"world!\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":10,\"output\":5}}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* Should have one text content block with concatenated text */
	blocks = ai_response_get_content_blocks(response);
	g_assert_nonnull(blocks);
	g_assert_cmpuint(g_list_length(blocks), ==, 1);
	g_assert_true(AI_IS_TEXT_CONTENT(blocks->data));

	text = ai_text_content_get_text(AI_TEXT_CONTENT(blocks->data));
	g_assert_cmpstr(text, ==, "Hello world!");
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: single text event
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_single_text_event(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	const gchar *ndjson =
		"{\"type\":\"text\",\"part\":{\"text\":\"Only one chunk.\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":1,\"output\":1}}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	text = ai_response_get_text(response);
	g_assert_cmpstr(text, ==, "Only one chunk.");
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: token usage from step_finish
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_usage_tokens(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	AiUsage *usage;
	const gchar *ndjson =
		"{\"type\":\"text\",\"part\":{\"text\":\"hi\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":42,\"output\":17}}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	usage = ai_response_get_usage(response);
	g_assert_nonnull(usage);
	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 42);
	g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 17);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: sessionID capture
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_session_id_capture(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *sid;
	const gchar *ndjson =
		"{\"type\":\"step_start\",\"sessionID\":\"ses-abc-123\"}\n"
		"{\"type\":\"text\",\"part\":{\"text\":\"ok\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":1,\"output\":1}}}\n";

	client = ai_opencode_client_new();

	/* Session persistence is on by default */
	g_assert_true(ai_cli_client_get_session_persistence(AI_CLI_CLIENT(client)));

	response = call_parse_json_output(client, ndjson, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);

	sid = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));
	g_assert_cmpstr(sid, ==, "ses-abc-123");
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: sessionID NOT captured when persistence disabled
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_session_id_no_persist(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *sid;
	const gchar *ndjson =
		"{\"type\":\"step_start\",\"sessionID\":\"ses-xyz\"}\n"
		"{\"type\":\"text\",\"part\":{\"text\":\"ok\"}}\n";

	client = ai_opencode_client_new();
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);

	response = call_parse_json_output(client, ndjson, &error);
	g_assert_no_error(error);

	sid = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));
	g_assert_null(sid);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: error event returns NULL + GError
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_error_event(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	AiResponse *response = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *ndjson =
		"{\"error\":\"rate limit exceeded\"}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "rate limit exceeded"));
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: tool_use events only (completed tool)
 *
 * When the AI only makes tool calls and no text event follows,
 * parse_json_output should store the tool summary on the client
 * but NOT add it as a content block — leaving the response empty
 * so the caller can attempt a re-prompt.
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_tool_use_only_completed(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"step_start\",\"sessionID\":\"s1\"}\n"
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":\"ls -la\"},"
		"\"output\":\"total 48\\ndrwxr-xr-x 8 user\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"reason\":\"tool-calls\","
		"\"tokens\":{\"input\":20,\"output\":10}}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* Response should have NO content blocks (tool summary stored on client) */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);

	/* Usage should still be captured */
	g_assert_nonnull(ai_response_get_usage(response));
	g_assert_cmpint(ai_usage_get_input_tokens(ai_response_get_usage(response)), ==, 20);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: tool_use events only (failed tool)
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_tool_use_only_error(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"error\","
		"\"error\":\"permission denied\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"reason\":\"tool-calls\"}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* No content blocks — tool summary stored on client for re-prompt */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: multiple tool_use events
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_multiple_tool_calls(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":\"ls\"},"
		"\"output\":\"file1\\nfile2\"}}}\n"
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"error\","
		"\"error\":\"cat: no such file\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"reason\":\"tool-calls\"}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* Still no content blocks — both tool summaries stored, not surfaced */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: text + tool_use events — text wins
 *
 * When the AI produces both tool calls AND text, the text
 * should appear as a content block normally.  The tool summary
 * should NOT be stored since we already have text.
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_text_and_tool_use(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	const gchar *ndjson =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":\"ls\"},"
		"\"output\":\"file.txt\"}}}\n"
		"{\"type\":\"text\",\"part\":{\"text\":\"I found file.txt.\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":5,\"output\":3}}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* Text block should be present */
	text = ai_response_get_text(response);
	g_assert_cmpstr(text, ==, "I found file.txt.");
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: completely empty output
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_empty_output(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;

	client = ai_opencode_client_new();

	/* Empty string → triggers "no text or tool events" warning */
	response = call_parse_json_output(client, "", &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(response);

	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: only whitespace / blank lines
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_blank_lines(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;

	client = ai_opencode_client_new();

	response = call_parse_json_output(client, "\n\n\n", &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(response);

	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: malformed JSON lines are skipped gracefully
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_malformed_json(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	const gchar *ndjson =
		"this is not json\n"
		"{\"bad: json\n"
		"{\"type\":\"text\",\"part\":{\"text\":\"survived\"}}\n"
		"[1,2,3]\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* The one valid text event should be captured */
	text = ai_response_get_text(response);
	g_assert_cmpstr(text, ==, "survived");
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: unknown event types are silently ignored
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_unknown_event_type(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	const gchar *ndjson =
		"{\"type\":\"heartbeat\",\"data\":\"ping\"}\n"
		"{\"type\":\"text\",\"part\":{\"text\":\"ok\"}}\n"
		"{\"type\":\"debug\",\"level\":\"info\"}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	text = ai_response_get_text(response);
	g_assert_cmpstr(text, ==, "ok");
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: text event with empty text field
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_empty_text_event(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"text\",\"part\":{\"text\":\"\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":0,\"output\":0}}}\n";

	client = ai_opencode_client_new();

	response = call_parse_json_output(client, ndjson, &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* Empty accumulated text → no content blocks */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: text event missing "part" member
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_text_event_no_part(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"text\"}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();

	response = call_parse_json_output(client, ndjson, &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(response);

	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: tool_use with no "state" member
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_tool_use_no_state(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();

	response = call_parse_json_output(client, ndjson, &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* No state → tool not recorded, no content blocks, no crash */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: tool_use with no "part" member at all
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_tool_use_no_part(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"tool_use\"}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();

	response = call_parse_json_output(client, ndjson, &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* No crash, no content */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: tool_use completed with no "command" in input
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_tool_use_no_command(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"read\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"path\":\"/etc/hosts\"},"
		"\"output\":\"127.0.0.1 localhost\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* Tool summary stored but no command field — should still work */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: step_finish with no "tokens" member
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_step_finish_no_tokens(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	AiUsage *usage;
	const gchar *ndjson =
		"{\"type\":\"text\",\"part\":{\"text\":\"ok\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* No tokens → no usage set */
	usage = ai_response_get_usage(response);
	g_assert_null(usage);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: error event with missing error string
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_error_event_no_message(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	AiResponse *response = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *ndjson =
		"{\"error\":\"\"}\n";

	client = ai_opencode_client_new();

	/* An empty error string should still be treated as an error:
	 * json_object_has_member returns TRUE for "error":"" */
	response = call_parse_json_output(client, ndjson, &error);

	g_assert_null(response);
	g_assert_nonnull(error);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: tool_use with unknown status is ignored
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_tool_use_unknown_status(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *blocks;
	const gchar *ndjson =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"pending\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();

	response = call_parse_json_output(client, ndjson, &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(response);

	/* Unknown status → not recorded, no crash */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: calling twice replaces stored tool summary
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_tool_summary_replaced(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) r1 = NULL;
	g_autoptr(AiResponse) r2 = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *ndjson1 =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":\"echo first\"},"
		"\"output\":\"first\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";
	const gchar *ndjson2 =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":\"echo second\"},"
		"\"output\":\"second\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();

	/* First parse stores a tool summary */
	r1 = call_parse_json_output(client, ndjson1, &error);
	g_assert_no_error(error);
	g_assert_null(ai_response_get_content_blocks(r1));

	/* Second parse should replace it without leak */
	r2 = call_parse_json_output(client, ndjson2, &error);
	g_assert_no_error(error);
	g_assert_null(ai_response_get_content_blocks(r2));

	/* No crash, no leak (valgrind would catch) */
}

/* ───────────────────────────────────────────────────────────────────
 * parse_json_output: text after tool clears tool-only path
 *
 * If a prior call stored a tool summary but a subsequent call
 * produces text, the text path should be taken.
 * ─────────────────────────────────────────────────────────────────── */

static void
test_parse_text_after_tool_only(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiResponse) r1 = NULL;
	g_autoptr(AiResponse) r2 = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	const gchar *tool_only =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":\"ls\"},\"output\":\"a\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";
	const gchar *text_response =
		"{\"type\":\"text\",\"part\":{\"text\":\"Here is the summary.\"}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();

	r1 = call_parse_json_output(client, tool_only, &error);
	g_assert_no_error(error);
	g_assert_null(ai_response_get_content_blocks(r1));

	r2 = call_parse_json_output(client, text_response, &error);
	g_assert_no_error(error);
	text = ai_response_get_text(r2);
	g_assert_cmpstr(text, ==, "Here is the summary.");
}

/* ───────────────────────────────────────────────────────────────────
 * build_stdin: includes plain text instruction
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_stdin_plain_text_instruction(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autofree gchar *stdin_text = NULL;
	GList *messages = NULL;

	client = ai_opencode_client_new();
	msg = ai_message_new_user("What is 2+2?");
	messages = g_list_append(messages, msg);

	stdin_text = call_build_stdin(client, messages);

	g_assert_nonnull(stdin_text);
	g_assert_nonnull(strstr(stdin_text, "IMPORTANT:"));
	g_assert_nonnull(strstr(stdin_text, "plain text response"));
	g_assert_nonnull(strstr(stdin_text, "Never end your turn on tool calls alone"));

	g_list_free(messages);
}

/* ───────────────────────────────────────────────────────────────────
 * build_stdin: system prompt wrapped in <system> tags
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_stdin_system_prompt(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autofree gchar *stdin_text = NULL;
	GList *messages = NULL;

	client = ai_opencode_client_new();
	ai_cli_client_set_system_prompt(AI_CLI_CLIENT(client), "Be helpful.");

	msg = ai_message_new_user("Hi");
	messages = g_list_append(messages, msg);

	stdin_text = call_build_stdin(client, messages);

	g_assert_nonnull(stdin_text);
	g_assert_nonnull(strstr(stdin_text, "<system>"));
	g_assert_nonnull(strstr(stdin_text, "Be helpful."));
	g_assert_nonnull(strstr(stdin_text, "</system>"));

	g_list_free(messages);
}

/* ───────────────────────────────────────────────────────────────────
 * build_stdin: no system prompt → no <system> tags
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_stdin_no_system_prompt(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autofree gchar *stdin_text = NULL;
	GList *messages = NULL;

	client = ai_opencode_client_new();
	msg = ai_message_new_user("Hello");
	messages = g_list_append(messages, msg);

	stdin_text = call_build_stdin(client, messages);

	g_assert_nonnull(stdin_text);
	g_assert_null(strstr(stdin_text, "<system>"));

	/* But the plain text instruction should still be there */
	g_assert_nonnull(strstr(stdin_text, "IMPORTANT:"));

	g_list_free(messages);
}

/* ───────────────────────────────────────────────────────────────────
 * build_stdin: empty message list
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_stdin_empty_messages(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autofree gchar *stdin_text = NULL;

	client = ai_opencode_client_new();
	stdin_text = call_build_stdin(client, NULL);

	/* Should still contain the plain text instruction even with no messages */
	g_assert_nonnull(stdin_text);
	g_assert_nonnull(strstr(stdin_text, "IMPORTANT:"));
}

/* ───────────────────────────────────────────────────────────────────
 * build_stdin: assistant message gets "Previous assistant response"
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_stdin_assistant_message(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiMessage) user_msg = NULL;
	g_autoptr(AiMessage) asst_msg = NULL;
	g_autofree gchar *stdin_text = NULL;
	GList *messages = NULL;

	client = ai_opencode_client_new();

	user_msg = ai_message_new_user("What?");
	asst_msg = ai_message_new_assistant("I said hello.");
	messages = g_list_append(messages, user_msg);
	messages = g_list_append(messages, asst_msg);

	stdin_text = call_build_stdin(client, messages);

	g_assert_nonnull(stdin_text);
	g_assert_nonnull(strstr(stdin_text, "What?"));
	g_assert_nonnull(strstr(stdin_text, "Previous assistant response: I said hello."));

	g_list_free(messages);
}

/* ───────────────────────────────────────────────────────────────────
 * build_argv: session flag included when session ID is set
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_argv_with_session(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_auto(GStrv) argv = NULL;
	gboolean found_session = FALSE;
	gint i;

	client = ai_opencode_client_new();
	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "test-sess-42");

	argv = call_build_argv(client, NULL, NULL, 0, FALSE);

	g_assert_nonnull(argv);

	for (i = 0; argv[i] != NULL; i++)
	{
		if (g_strcmp0(argv[i], "--session") == 0 && argv[i + 1] != NULL)
		{
			g_assert_cmpstr(argv[i + 1], ==, "test-sess-42");
			found_session = TRUE;
			break;
		}
	}

	g_assert_true(found_session);
}

/* ───────────────────────────────────────────────────────────────────
 * build_argv: no session flag when session ID is NULL
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_argv_no_session(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_auto(GStrv) argv = NULL;
	gint i;

	client = ai_opencode_client_new();

	argv = call_build_argv(client, NULL, NULL, 0, FALSE);
	g_assert_nonnull(argv);

	for (i = 0; argv[i] != NULL; i++)
	{
		g_assert_cmpstr(argv[i], !=, "--session");
	}
}

/* ───────────────────────────────────────────────────────────────────
 * build_argv: --format json and --model present
 * ─────────────────────────────────────────────────────────────────── */

static void
test_build_argv_basic_flags(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_auto(GStrv) argv = NULL;
	gboolean found_format = FALSE;
	gboolean found_model = FALSE;
	gint i;

	client = ai_opencode_client_new();
	argv = call_build_argv(client, NULL, NULL, 0, FALSE);

	g_assert_nonnull(argv);

	for (i = 0; argv[i] != NULL; i++)
	{
		if (g_strcmp0(argv[i], "--format") == 0 && argv[i + 1] != NULL)
		{
			g_assert_cmpstr(argv[i + 1], ==, "json");
			found_format = TRUE;
		}
		if (g_strcmp0(argv[i], "--model") == 0 && argv[i + 1] != NULL)
		{
			g_assert_cmpstr(argv[i + 1], ==, AI_OPENCODE_DEFAULT_MODEL);
			found_model = TRUE;
		}
	}

	g_assert_true(found_format);
	g_assert_true(found_model);
}

/* ───────────────────────────────────────────────────────────────────
 * Finalize: creating and destroying a client that stored a
 * tool summary should not leak.
 * ─────────────────────────────────────────────────────────────────── */

static void
test_finalize_with_tool_summary(void)
{
	AiOpenCodeClient *client;
	AiResponse *response;
	GError *error = NULL;
	const gchar *ndjson =
		"{\"type\":\"tool_use\",\"part\":{\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":\"pwd\"},\"output\":\"/home\"}}}\n"
		"{\"type\":\"step_finish\",\"part\":{}}\n";

	client = ai_opencode_client_new();

	/* Parse tool-only output → stores tool_summary on client */
	response = call_parse_json_output(client, ndjson, &error);
	g_assert_no_error(error);
	g_object_unref(response);

	/* Destroy the client — finalize should g_free(last_tool_summary) */
	g_object_unref(client);
	/* If we get here without a crash/leak, the test passes */
}

/* ───────────────────────────────────────────────────────────────────
 * Finalize: client with no tool summary (NULL) is safe
 * ─────────────────────────────────────────────────────────────────── */

static void
test_finalize_without_tool_summary(void)
{
	AiOpenCodeClient *client;

	client = ai_opencode_client_new();
	/* No parsing done — last_tool_summary is NULL */
	g_object_unref(client);
	/* g_free(NULL) is a no-op, should not crash */
}

/* ───────────────────────────────────────────────────────────────────
 * main
 * ─────────────────────────────────────────────────────────────────── */

/*
 * opencode's own errors nest the human-readable text two levels down, in
 * error.data.message, with the class in error.name. Reading only
 * error.message meant the caller got the whole object serialised back as
 * the message -- raw JSON where a sentence belonged.
 *
 * This is a real payload from opencode 1.18.18.
 */
static void
test_parse_error_nested_data_message(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = call_parse_json_output(client,
		"{\"type\":\"error\",\"timestamp\":1786847476620,"
		"\"sessionID\":\"ses_x\",\"error\":{\"name\":\"UnknownError\","
		"\"data\":{\"message\":\"Unexpected server error.\","
		"\"ref\":\"err_5f996935\"}}}",
		&error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "Unexpected server error."));
	/* The class name is worth keeping. */
	g_assert_nonnull(strstr(error->message, "UnknownError"));
	/* And the raw object must not be what the caller reads. */
	g_assert_null(strstr(error->message, "\"timestamp\""));
}

/* The flatter shapes must keep working. */
static void
test_parse_error_flat_shapes(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_autoptr(GError) string_error = NULL;
	g_autoptr(GError) object_error = NULL;

	g_assert_null(call_parse_json_output(client,
		"{\"error\":\"plain string\"}", &string_error));
	g_assert_nonnull(strstr(string_error->message, "plain string"));

	g_assert_null(call_parse_json_output(client,
		"{\"error\":{\"message\":\"one level\"}}", &object_error));
	g_assert_nonnull(strstr(object_error->message, "one level"));
}

/* ───────────────────────────────────────────────────────────────────
 * build_argv: the `opencode run` options
 * ─────────────────────────────────────────────────────────────────── */

/*
 * skip-permissions emits --auto, opencode's equivalent of Claude Code's
 * --dangerously-skip-permissions. Before this it relied solely on the
 * OPENCODE_PERMISSION environment variable, which the inherited
 * synchronous path never set -- so a sync call with skip-permissions on
 * still stopped for approval.
 */
static void
test_build_argv_auto(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	argv = build_argv_for(client);
	g_assert_cmpint(argv_index_of(argv, "--auto"), ==, -1);
	g_strfreev(g_steal_pointer(&argv));

	ai_opencode_client_set_skip_permissions(client, TRUE);
	argv = build_argv_for(client);
	g_assert_cmpint(argv_index_of(argv, "--auto"), >, 0);
}

static void
test_build_argv_agent(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "agent", "build", NULL);
	argv = build_argv_for(client);

	g_assert_cmpstr(argv_value_after(argv, "--agent"), ==, "build");
}

static void
test_build_argv_title_and_share(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "title", "nightly run", "share", TRUE, NULL);
	argv = build_argv_for(client);

	g_assert_cmpstr(argv_value_after(argv, "--title"), ==, "nightly run");
	g_assert_cmpint(argv_index_of(argv, "--share"), >, 0);
}

/* Sharing is off unless asked for: a session can contain anything. */
static void
test_build_argv_share_defaults_off(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = build_argv_for(client);

	g_assert_cmpint(argv_index_of(argv, "--share"), ==, -1);
}

/* Each file becomes its own --file pair; blank items are dropped. */
static void
test_build_argv_files(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "files", "a.txt, b.txt, ,", NULL);
	argv = build_argv_for(client);

	g_assert_cmpint(argv_count(argv, "--file"), ==, 2);
	g_assert_cmpint(argv_index_of(argv, "a.txt"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "b.txt"), >, 0);
}

/* An explicit session id wins over --continue, which names a different one. */
static void
test_build_argv_continue_vs_session(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	argv = build_argv_for(client);
	g_assert_cmpint(argv_index_of(argv, "--continue"), >, 0);
	g_strfreev(g_steal_pointer(&argv));

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "ses_123");
	argv = build_argv_for(client);
	g_assert_cmpstr(argv_value_after(argv, "--session"), ==, "ses_123");
	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

/* --fork needs something to fork; it is not emitted on a fresh session. */
static void
test_build_argv_fork_needs_a_session(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "fork-session", TRUE, NULL);
	argv = build_argv_for(client);
	g_assert_cmpint(argv_index_of(argv, "--fork"), ==, -1);
	g_strfreev(g_steal_pointer(&argv));

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "ses_123");
	argv = build_argv_for(client);
	g_assert_cmpint(argv_index_of(argv, "--fork"), >, 0);
}

/*
 * The working directory reaches opencode as --dir as well as being the
 * child's cwd: --dir is what tells opencode which project it is in, and
 * therefore which session store to use.
 */
static void
test_build_argv_working_directory(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	argv = build_argv_for(client);
	g_assert_cmpint(argv_index_of(argv, "--dir"), ==, -1);
	g_strfreev(g_steal_pointer(&argv));

	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), "/srv/work");
	argv = build_argv_for(client);
	g_assert_cmpstr(argv_value_after(argv, "--dir"), ==, "/srv/work");
}

static void
test_build_argv_attach_and_port(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "attach", "http://localhost:4096",
	             "port", 4097,
	             NULL);
	argv = build_argv_for(client);

	g_assert_cmpstr(argv_value_after(argv, "--attach"),
	                ==, "http://localhost:4096");
	g_assert_cmpstr(argv_value_after(argv, "--port"), ==, "4097");
}

/* Port 0 means "let opencode choose", not "--port 0". */
static void
test_build_argv_port_zero(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = build_argv_for(client);

	g_assert_cmpint(argv_index_of(argv, "--port"), ==, -1);
}

static void
test_build_argv_thinking_pure_logs(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "thinking", TRUE,
	             "pure", TRUE,
	             "print-logs", TRUE,
	             "log-level", "DEBUG",
	             NULL);
	argv = build_argv_for(client);

	g_assert_cmpint(argv_index_of(argv, "--thinking"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--pure"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--print-logs"), >, 0);
	g_assert_cmpstr(argv_value_after(argv, "--log-level"), ==, "DEBUG");
}

/*
 * opencode rejects the whole invocation on a bad option value, printing
 * its usage text, so an unknown log level is dropped with one clear
 * warning instead.
 */
static void
test_build_argv_log_level_invalid(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "log-level", "verbose", NULL);

	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
	                      "*unknown log level 'verbose'*");
	argv = build_argv_for(client);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--log-level"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "verbose"), ==, -1);
}

/* Log levels are upper-case in opencode's own vocabulary. */
static void
test_build_argv_log_level_case_sensitive(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "log-level", "debug", NULL);

	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
	                      "*unknown log level 'debug'*");
	argv = build_argv_for(client);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--log-level"), ==, -1);
}

/* Everything at once still produces one well-formed command. */
static void
test_build_argv_all_options(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "ses_1");
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), "/w");
	ai_opencode_client_set_skip_permissions(client, TRUE);
	g_object_set(client,
	             "agent", "build",
	             "title", "t",
	             "files", "x.txt",
	             "attach", "http://h:1",
	             "log-level", "WARN",
	             "port", 1234,
	             "share", TRUE,
	             "fork-session", TRUE,
	             "thinking", TRUE,
	             "pure", TRUE,
	             "print-logs", TRUE,
	             NULL);

	argv = build_argv_for(client);

	/* The invariants that must survive every combination. */
	g_assert_cmpstr(argv[0], ==, "opencode");
	g_assert_cmpstr(argv[1], ==, "run");
	g_assert_cmpstr(argv_value_after(argv, "--format"), ==, "json");
	g_assert_cmpint(argv_count(argv, "--model"), ==, 1);
	g_assert_cmpint(argv_count(argv, "--session"), ==, 1);
	/* No positional prompt: it is piped. */
	g_assert_cmpint(argv_index_of(argv, "hi"), ==, -1);
}

/* Property plumbing: the GObject path bindings use. */
static void
test_opencode_property_round_trip(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	g_autofree gchar *agent = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *files = NULL;
	g_autofree gchar *attach = NULL;
	g_autofree gchar *log_level = NULL;
	gboolean share = FALSE, fork = FALSE, cont = FALSE;
	gboolean thinking = FALSE, pure = FALSE, print_logs = FALSE;
	gint port = 0;

	g_object_set(client,
	             "agent", "build",
	             "title", "t",
	             "files", "a.txt",
	             "attach", "http://h:1",
	             "log-level", "INFO",
	             "port", 4096,
	             "share", TRUE,
	             "fork-session", TRUE,
	             "continue-session", TRUE,
	             "thinking", TRUE,
	             "pure", TRUE,
	             "print-logs", TRUE,
	             NULL);

	g_object_get(client,
	             "agent", &agent,
	             "title", &title,
	             "files", &files,
	             "attach", &attach,
	             "log-level", &log_level,
	             "port", &port,
	             "share", &share,
	             "fork-session", &fork,
	             "continue-session", &cont,
	             "thinking", &thinking,
	             "pure", &pure,
	             "print-logs", &print_logs,
	             NULL);

	g_assert_cmpstr(agent, ==, "build");
	g_assert_cmpstr(title, ==, "t");
	g_assert_cmpstr(files, ==, "a.txt");
	g_assert_cmpstr(attach, ==, "http://h:1");
	g_assert_cmpstr(log_level, ==, "INFO");
	g_assert_cmpint(port, ==, 4096);
	g_assert_true(share && fork && cont && thinking && pure && print_logs);
}

int
main(
	int   argc,
	char *argv[]
)
{
	g_test_init(&argc, &argv, NULL);

	/* Basic construction / interfaces */
	g_test_add_func("/ai-glib/opencode-client/new", test_opencode_client_new);
	g_test_add_func("/ai-glib/opencode-client/default-model", test_opencode_client_default_model);
	g_test_add_func("/ai-glib/opencode-client/provider-interface", test_opencode_client_provider_interface);
	g_test_add_func("/ai-glib/opencode-client/streamable-interface", test_opencode_client_streamable_interface);
	g_test_add_func("/ai-glib/opencode-client/model", test_opencode_client_model);
	g_test_add_func("/ai-glib/opencode-client/executable-path", test_opencode_client_executable_path);
	g_test_add_func("/ai-glib/opencode-client/gtype", test_opencode_client_gtype);

	/* parse_json_output — text events */
	g_test_add_func("/ai-glib/opencode-client/parse/text-only", test_parse_text_only);
	g_test_add_func("/ai-glib/opencode-client/parse/single-text-event", test_parse_single_text_event);
	g_test_add_func("/ai-glib/opencode-client/parse/empty-text-event", test_parse_empty_text_event);
	g_test_add_func("/ai-glib/opencode-client/parse/text-event-no-part", test_parse_text_event_no_part);

	/* parse_json_output — usage and session */
	g_test_add_func("/ai-glib/opencode-client/parse/usage-tokens", test_parse_usage_tokens);
	g_test_add_func("/ai-glib/opencode-client/parse/session-id-capture", test_parse_session_id_capture);
	g_test_add_func("/ai-glib/opencode-client/parse/session-id-no-persist", test_parse_session_id_no_persist);
	g_test_add_func("/ai-glib/opencode-client/parse/step-finish-no-tokens", test_parse_step_finish_no_tokens);

	/* parse_json_output — error handling */
	g_test_add_func("/ai-glib/opencode-client/parse/error-event", test_parse_error_event);
	g_test_add_func("/ai-glib/opencode-client/parse/error-event-no-message", test_parse_error_event_no_message);
	g_test_add_func("/ai-glib/opencode-client/parse/malformed-json", test_parse_malformed_json);
	g_test_add_func("/ai-glib/opencode-client/parse/unknown-event-type", test_parse_unknown_event_type);

	/* parse_json_output — empty / degenerate input */
	g_test_add_func("/ai-glib/opencode-client/parse/empty-output", test_parse_empty_output);
	g_test_add_func("/ai-glib/opencode-client/parse/blank-lines", test_parse_blank_lines);

	/* parse_json_output — tool_use events */
	g_test_add_func("/ai-glib/opencode-client/parse/tool-use-only-completed", test_parse_tool_use_only_completed);
	g_test_add_func("/ai-glib/opencode-client/parse/tool-use-only-error", test_parse_tool_use_only_error);
	g_test_add_func("/ai-glib/opencode-client/parse/multiple-tool-calls", test_parse_multiple_tool_calls);
	g_test_add_func("/ai-glib/opencode-client/parse/text-and-tool-use", test_parse_text_and_tool_use);
	g_test_add_func("/ai-glib/opencode-client/parse/tool-use-no-state", test_parse_tool_use_no_state);
	g_test_add_func("/ai-glib/opencode-client/parse/tool-use-no-part", test_parse_tool_use_no_part);
	g_test_add_func("/ai-glib/opencode-client/parse/tool-use-no-command", test_parse_tool_use_no_command);
	g_test_add_func("/ai-glib/opencode-client/parse/tool-use-unknown-status", test_parse_tool_use_unknown_status);
	g_test_add_func("/ai-glib/opencode-client/parse/tool-summary-replaced", test_parse_tool_summary_replaced);
	g_test_add_func("/ai-glib/opencode-client/parse/text-after-tool-only", test_parse_text_after_tool_only);

	/* build_stdin */
	g_test_add_func("/ai-glib/opencode-client/build-stdin/plain-text-instruction", test_build_stdin_plain_text_instruction);
	g_test_add_func("/ai-glib/opencode-client/build-stdin/system-prompt", test_build_stdin_system_prompt);
	g_test_add_func("/ai-glib/opencode-client/build-stdin/no-system-prompt", test_build_stdin_no_system_prompt);
	g_test_add_func("/ai-glib/opencode-client/build-stdin/empty-messages", test_build_stdin_empty_messages);
	g_test_add_func("/ai-glib/opencode-client/build-stdin/assistant-message", test_build_stdin_assistant_message);

	/* build_argv */
	g_test_add_func("/ai-glib/opencode-client/build-argv/with-session", test_build_argv_with_session);
	g_test_add_func("/ai-glib/opencode-client/build-argv/no-session", test_build_argv_no_session);
	g_test_add_func("/ai-glib/opencode-client/build-argv/basic-flags", test_build_argv_basic_flags);
	g_test_add_func("/ai-glib/opencode-client/parse/error-nested-data", test_parse_error_nested_data_message);
	g_test_add_func("/ai-glib/opencode-client/parse/error-flat-shapes", test_parse_error_flat_shapes);
	g_test_add_func("/ai-glib/opencode-client/build-argv/auto", test_build_argv_auto);
	g_test_add_func("/ai-glib/opencode-client/build-argv/agent", test_build_argv_agent);
	g_test_add_func("/ai-glib/opencode-client/build-argv/title-and-share", test_build_argv_title_and_share);
	g_test_add_func("/ai-glib/opencode-client/build-argv/share-defaults-off", test_build_argv_share_defaults_off);
	g_test_add_func("/ai-glib/opencode-client/build-argv/files", test_build_argv_files);
	g_test_add_func("/ai-glib/opencode-client/build-argv/continue-vs-session", test_build_argv_continue_vs_session);
	g_test_add_func("/ai-glib/opencode-client/build-argv/fork-needs-session", test_build_argv_fork_needs_a_session);
	g_test_add_func("/ai-glib/opencode-client/build-argv/working-directory", test_build_argv_working_directory);
	g_test_add_func("/ai-glib/opencode-client/build-argv/attach-and-port", test_build_argv_attach_and_port);
	g_test_add_func("/ai-glib/opencode-client/build-argv/port-zero", test_build_argv_port_zero);
	g_test_add_func("/ai-glib/opencode-client/build-argv/thinking-pure-logs", test_build_argv_thinking_pure_logs);
	g_test_add_func("/ai-glib/opencode-client/build-argv/log-level-invalid", test_build_argv_log_level_invalid);
	g_test_add_func("/ai-glib/opencode-client/build-argv/log-level-case", test_build_argv_log_level_case_sensitive);
	g_test_add_func("/ai-glib/opencode-client/build-argv/all-options", test_build_argv_all_options);
	g_test_add_func("/ai-glib/opencode-client/property-round-trip", test_opencode_property_round_trip);

	/* Finalize / cleanup */
	g_test_add_func("/ai-glib/opencode-client/finalize/with-tool-summary", test_finalize_with_tool_summary);
	g_test_add_func("/ai-glib/opencode-client/finalize/without-tool-summary", test_finalize_without_tool_summary);

	return g_test_run();
}
