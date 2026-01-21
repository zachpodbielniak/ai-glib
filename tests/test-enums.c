/*
 * test-enums.c - Unit tests for AiEnums
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "core/ai-enums.h"

static void
test_provider_type_gtype(void)
{
	GType type;

	type = ai_provider_type_get_type();
	g_assert_true(G_TYPE_IS_ENUM(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiProviderType");
}

static void
test_provider_type_to_string(void)
{
	g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_CLAUDE), ==, "claude");
	g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_OPENAI), ==, "openai");
	g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_GEMINI), ==, "gemini");
	g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_GROK), ==, "grok");
	g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_OLLAMA), ==, "ollama");
}

static void
test_provider_type_from_string(void)
{
	g_assert_cmpint(ai_provider_type_from_string("claude"), ==, AI_PROVIDER_CLAUDE);
	g_assert_cmpint(ai_provider_type_from_string("anthropic"), ==, AI_PROVIDER_CLAUDE);
	g_assert_cmpint(ai_provider_type_from_string("openai"), ==, AI_PROVIDER_OPENAI);
	g_assert_cmpint(ai_provider_type_from_string("gpt"), ==, AI_PROVIDER_OPENAI);
	g_assert_cmpint(ai_provider_type_from_string("gemini"), ==, AI_PROVIDER_GEMINI);
	g_assert_cmpint(ai_provider_type_from_string("google"), ==, AI_PROVIDER_GEMINI);
	g_assert_cmpint(ai_provider_type_from_string("grok"), ==, AI_PROVIDER_GROK);
	g_assert_cmpint(ai_provider_type_from_string("xai"), ==, AI_PROVIDER_GROK);
	g_assert_cmpint(ai_provider_type_from_string("ollama"), ==, AI_PROVIDER_OLLAMA);
	g_assert_cmpint(ai_provider_type_from_string(NULL), ==, AI_PROVIDER_CLAUDE);
}

static void
test_role_gtype(void)
{
	GType type;

	type = ai_role_get_type();
	g_assert_true(G_TYPE_IS_ENUM(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiRole");
}

static void
test_role_to_string(void)
{
	g_assert_cmpstr(ai_role_to_string(AI_ROLE_USER), ==, "user");
	g_assert_cmpstr(ai_role_to_string(AI_ROLE_ASSISTANT), ==, "assistant");
	g_assert_cmpstr(ai_role_to_string(AI_ROLE_SYSTEM), ==, "system");
	g_assert_cmpstr(ai_role_to_string(AI_ROLE_TOOL), ==, "tool");
}

static void
test_role_from_string(void)
{
	g_assert_cmpint(ai_role_from_string("user"), ==, AI_ROLE_USER);
	g_assert_cmpint(ai_role_from_string("assistant"), ==, AI_ROLE_ASSISTANT);
	g_assert_cmpint(ai_role_from_string("system"), ==, AI_ROLE_SYSTEM);
	g_assert_cmpint(ai_role_from_string("tool"), ==, AI_ROLE_TOOL);
	g_assert_cmpint(ai_role_from_string(NULL), ==, AI_ROLE_USER);
	g_assert_cmpint(ai_role_from_string("invalid"), ==, AI_ROLE_USER);
}

static void
test_stop_reason_gtype(void)
{
	GType type;

	type = ai_stop_reason_get_type();
	g_assert_true(G_TYPE_IS_ENUM(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiStopReason");
}

static void
test_stop_reason_to_string(void)
{
	g_assert_cmpstr(ai_stop_reason_to_string(AI_STOP_REASON_NONE), ==, "none");
	g_assert_cmpstr(ai_stop_reason_to_string(AI_STOP_REASON_END_TURN), ==, "end_turn");
	g_assert_cmpstr(ai_stop_reason_to_string(AI_STOP_REASON_MAX_TOKENS), ==, "max_tokens");
	g_assert_cmpstr(ai_stop_reason_to_string(AI_STOP_REASON_TOOL_USE), ==, "tool_use");
	g_assert_cmpstr(ai_stop_reason_to_string(AI_STOP_REASON_CONTENT_FILTER), ==, "content_filter");
}

static void
test_stop_reason_from_string(void)
{
	g_assert_cmpint(ai_stop_reason_from_string("end_turn"), ==, AI_STOP_REASON_END_TURN);
	g_assert_cmpint(ai_stop_reason_from_string("stop"), ==, AI_STOP_REASON_END_TURN);
	g_assert_cmpint(ai_stop_reason_from_string("max_tokens"), ==, AI_STOP_REASON_MAX_TOKENS);
	g_assert_cmpint(ai_stop_reason_from_string("length"), ==, AI_STOP_REASON_MAX_TOKENS);
	g_assert_cmpint(ai_stop_reason_from_string("tool_use"), ==, AI_STOP_REASON_TOOL_USE);
	g_assert_cmpint(ai_stop_reason_from_string("tool_calls"), ==, AI_STOP_REASON_TOOL_USE);
	g_assert_cmpint(ai_stop_reason_from_string("content_filter"), ==, AI_STOP_REASON_CONTENT_FILTER);
	g_assert_cmpint(ai_stop_reason_from_string(NULL), ==, AI_STOP_REASON_NONE);
}

static void
test_content_type_gtype(void)
{
	GType type;

	type = ai_content_type_get_type();
	g_assert_true(G_TYPE_IS_ENUM(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiContentType");
}

static void
test_content_type_to_string(void)
{
	g_assert_cmpstr(ai_content_type_to_string(AI_CONTENT_TYPE_TEXT), ==, "text");
	g_assert_cmpstr(ai_content_type_to_string(AI_CONTENT_TYPE_TOOL_USE), ==, "tool_use");
	g_assert_cmpstr(ai_content_type_to_string(AI_CONTENT_TYPE_TOOL_RESULT), ==, "tool_result");
	g_assert_cmpstr(ai_content_type_to_string(AI_CONTENT_TYPE_IMAGE), ==, "image");
}

static void
test_content_type_from_string(void)
{
	g_assert_cmpint(ai_content_type_from_string("text"), ==, AI_CONTENT_TYPE_TEXT);
	g_assert_cmpint(ai_content_type_from_string("tool_use"), ==, AI_CONTENT_TYPE_TOOL_USE);
	g_assert_cmpint(ai_content_type_from_string("tool_result"), ==, AI_CONTENT_TYPE_TOOL_RESULT);
	g_assert_cmpint(ai_content_type_from_string("image"), ==, AI_CONTENT_TYPE_IMAGE);
	g_assert_cmpint(ai_content_type_from_string("image_url"), ==, AI_CONTENT_TYPE_IMAGE);
	g_assert_cmpint(ai_content_type_from_string(NULL), ==, AI_CONTENT_TYPE_TEXT);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/enums/provider-type/gtype", test_provider_type_gtype);
	g_test_add_func("/ai-glib/enums/provider-type/to-string", test_provider_type_to_string);
	g_test_add_func("/ai-glib/enums/provider-type/from-string", test_provider_type_from_string);

	g_test_add_func("/ai-glib/enums/role/gtype", test_role_gtype);
	g_test_add_func("/ai-glib/enums/role/to-string", test_role_to_string);
	g_test_add_func("/ai-glib/enums/role/from-string", test_role_from_string);

	g_test_add_func("/ai-glib/enums/stop-reason/gtype", test_stop_reason_gtype);
	g_test_add_func("/ai-glib/enums/stop-reason/to-string", test_stop_reason_to_string);
	g_test_add_func("/ai-glib/enums/stop-reason/from-string", test_stop_reason_from_string);

	g_test_add_func("/ai-glib/enums/content-type/gtype", test_content_type_gtype);
	g_test_add_func("/ai-glib/enums/content-type/to-string", test_content_type_to_string);
	g_test_add_func("/ai-glib/enums/content-type/from-string", test_content_type_from_string);

	return g_test_run();
}
