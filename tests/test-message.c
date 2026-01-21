/*
 * test-message.c - Unit tests for AiMessage
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "model/ai-message.h"
#include "model/ai-text-content.h"
#include "core/ai-enums.h"

static void
test_message_new_user(void)
{
	g_autoptr(AiMessage) msg = NULL;

	msg = ai_message_new_user("Hello, world!");
	g_assert_nonnull(msg);
	g_assert_true(AI_IS_MESSAGE(msg));
	g_assert_cmpint(ai_message_get_role(msg), ==, AI_ROLE_USER);
}

static void
test_message_new_assistant(void)
{
	g_autoptr(AiMessage) msg = NULL;

	msg = ai_message_new_assistant("Hello!");
	g_assert_nonnull(msg);
	g_assert_cmpint(ai_message_get_role(msg), ==, AI_ROLE_ASSISTANT);
}

static void
test_message_new_system(void)
{
	g_autoptr(AiMessage) msg = NULL;

	/* No ai_message_new_system, create with role and add text */
	msg = ai_message_new(AI_ROLE_SYSTEM);
	ai_message_add_text(msg, "You are helpful.");
	g_assert_nonnull(msg);
	g_assert_cmpint(ai_message_get_role(msg), ==, AI_ROLE_SYSTEM);
}

static void
test_message_content_blocks(void)
{
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(AiTextContent) text = NULL;
	GList *blocks;

	msg = ai_message_new(AI_ROLE_USER);
	g_assert_nonnull(msg);

	/* Initially empty */
	blocks = ai_message_get_content_blocks(msg);
	g_assert_null(blocks);

	/* Add content */
	text = ai_text_content_new("Test content");
	ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&text));

	blocks = ai_message_get_content_blocks(msg);
	g_assert_nonnull(blocks);
	g_assert_cmpuint(g_list_length(blocks), ==, 1);
}

static void
test_message_get_text(void)
{
	g_autoptr(AiMessage) msg = NULL;
	g_autofree gchar *text = NULL;

	msg = ai_message_new_user("Hello, world!");
	text = ai_message_get_text(msg);

	g_assert_cmpstr(text, ==, "Hello, world!");
}

static void
test_message_to_json(void)
{
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(JsonNode) json = NULL;
	JsonObject *obj;

	msg = ai_message_new_user("Test message");
	json = ai_message_to_json(msg);

	g_assert_nonnull(json);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(json));

	obj = json_node_get_object(json);
	g_assert_cmpstr(json_object_get_string_member(obj, "role"), ==, "user");
}

static void
test_message_gtype(void)
{
	GType type;

	type = ai_message_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiMessage");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/message/new-user", test_message_new_user);
	g_test_add_func("/ai-glib/message/new-assistant", test_message_new_assistant);
	g_test_add_func("/ai-glib/message/new-system", test_message_new_system);
	g_test_add_func("/ai-glib/message/content-blocks", test_message_content_blocks);
	g_test_add_func("/ai-glib/message/get-text", test_message_get_text);
	g_test_add_func("/ai-glib/message/to-json", test_message_to_json);
	g_test_add_func("/ai-glib/message/gtype", test_message_gtype);

	return g_test_run();
}
