/*
 * test-response.c - Unit tests for AiResponse
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "model/ai-response.h"
#include "model/ai-text-content.h"
#include "model/ai-usage.h"
#include "core/ai-enums.h"

static void
test_response_new(void)
{
	g_autoptr(AiResponse) response = NULL;

	response = ai_response_new("msg_123", "claude-sonnet-4-20250514");
	g_assert_nonnull(response);
	g_assert_true(AI_IS_RESPONSE(response));
}

static void
test_response_properties(void)
{
	g_autoptr(AiResponse) response = NULL;
	const gchar *id;
	const gchar *model;

	response = ai_response_new("msg_456", "gpt-4o");

	id = ai_response_get_id(response);
	g_assert_cmpstr(id, ==, "msg_456");

	model = ai_response_get_model(response);
	g_assert_cmpstr(model, ==, "gpt-4o");
}

static void
test_response_stop_reason(void)
{
	g_autoptr(AiResponse) response = NULL;

	response = ai_response_new("msg_789", "claude-sonnet-4-20250514");

	/* Default is NONE */
	g_assert_cmpint(ai_response_get_stop_reason(response), ==, AI_STOP_REASON_NONE);

	ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
	g_assert_cmpint(ai_response_get_stop_reason(response), ==, AI_STOP_REASON_END_TURN);
}

static void
test_response_usage(void)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(AiUsage) usage = NULL;
	const AiUsage *retrieved;

	response = ai_response_new("msg_abc", "claude-sonnet-4-20250514");
	usage = ai_usage_new(100, 50);

	ai_response_set_usage(response, usage);

	retrieved = ai_response_get_usage(response);
	g_assert_nonnull(retrieved);
	g_assert_cmpint(ai_usage_get_input_tokens(retrieved), ==, 100);
	g_assert_cmpint(ai_usage_get_output_tokens(retrieved), ==, 50);
}

static void
test_response_content_blocks(void)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(AiTextContent) text = NULL;
	GList *blocks;

	response = ai_response_new("msg_def", "claude-sonnet-4-20250514");

	/* Initially empty */
	blocks = ai_response_get_content_blocks(response);
	g_assert_null(blocks);

	/* Add content */
	text = ai_text_content_new("Response text");
	ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&text));

	blocks = ai_response_get_content_blocks(response);
	g_assert_nonnull(blocks);
	g_assert_cmpuint(g_list_length(blocks), ==, 1);
}

static void
test_response_get_text(void)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(AiTextContent) text = NULL;
	g_autofree gchar *result = NULL;

	response = ai_response_new("msg_ghi", "claude-sonnet-4-20250514");
	text = ai_text_content_new("Hello from AI!");
	ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&text));

	result = ai_response_get_text(response);
	g_assert_cmpstr(result, ==, "Hello from AI!");
}

static void
test_response_gtype(void)
{
	GType type;

	type = ai_response_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiResponse");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/response/new", test_response_new);
	g_test_add_func("/ai-glib/response/properties", test_response_properties);
	g_test_add_func("/ai-glib/response/stop-reason", test_response_stop_reason);
	g_test_add_func("/ai-glib/response/usage", test_response_usage);
	g_test_add_func("/ai-glib/response/content-blocks", test_response_content_blocks);
	g_test_add_func("/ai-glib/response/get-text", test_response_get_text);
	g_test_add_func("/ai-glib/response/gtype", test_response_gtype);

	return g_test_run();
}
