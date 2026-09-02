/*
 * test-model-malformed.c - The model layer against JSON whose shapes are
 * wrong
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Two entry points in `src/model/` read JSON that nobody in this library
 * wrote, and both read it by presence:
 *
 *   - ai_message_new_from_json() is public API. An embedder hands it a
 *     document it parsed from a session file, a queue, or a request
 *     body, and gets back an #AiMessage.
 *   - ai_tool_use_get_input_*() read a tool call's arguments, which are
 *     whatever the model wrote. `{"command": {}}` is a thing a model
 *     emits by mistake several times a week.
 *
 * json-glib answers a type mismatch two ways, and the second is why this
 * file exists rather than a note in a comment:
 *
 *   - json_object_get_string_member() on an object or an array logs a
 *     Json-CRITICAL, which is fatal under GTest and under
 *     `G_DEBUG=fatal-warnings`;
 *   - json_object_get_string_member_with_default() on a *number* returns
 *     NULL --- not the default, and with nothing logged --- because its
 *     guard is on the node type and not on the value type. Every caller
 *     that wrote `x[0] != '\0'` after one of those was a segfault away
 *     from a badly typed field, on every build, sanitizers or not.
 *
 * So the assertions here are the absence of an abort and the absence of
 * a NULL where a string was promised.
 */

#include <glib.h>
#include <json-glib/json-glib.h>

#include "model/ai-message.h"
#include "model/ai-tool-use.h"
#include "core/ai-enums.h"
#include "core/ai-error.h"

/* ------------------------------------------------------------------ */
/* ai_message_new_from_json()                                          */
/* ------------------------------------------------------------------ */

static const gchar *malformed_messages[] = {
	/* The role, which decides whether there is a message at all. */
	"{\"role\":7}",
	"{\"role\":{}}",
	"{\"role\":[]}",
	"{\"role\":null}",
	"{\"role\":true}",

	/* The content shorthand and the block array. */
	"{\"role\":\"user\",\"content\":7}",
	"{\"role\":\"user\",\"content\":{}}",
	"{\"role\":\"user\",\"content\":null}",
	"{\"role\":\"user\",\"content\":[1,2,3]}",
	"{\"role\":\"user\",\"content\":[null]}",
	"{\"role\":\"user\",\"content\":[{\"type\":9}]}",
	"{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":[]}]}",
	"{\"role\":\"assistant\",\"content\":"
		"[{\"type\":\"tool_use\",\"id\":7,\"name\":{},\"input\":9}]}",
	"{\"role\":\"user\",\"content\":"
		"[{\"type\":\"tool_result\",\"tool_use_id\":[],\"content\":9,"
		"\"is_error\":\"yes\"}]}",
	NULL
};

/*
 * A document with a usable role must produce a message; one without must
 * produce an error. Both must produce exactly one of the two --- the
 * shape that used to hand back neither.
 */
static void
test_message_from_malformed_json(void)
{
	gsize i;

	for (i = 0; malformed_messages[i] != NULL; i++)
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(AiMessage)  message = NULL;
		g_autoptr(GError)     error = NULL;
		JsonNode             *root;

		g_assert_true(json_parser_load_from_data(
			parser, malformed_messages[i], -1, NULL));

		root = json_parser_get_root(parser);
		message = ai_message_new_from_json(root, &error);

		if ((message != NULL) == (error != NULL))
		{
			g_error("%s: answered neither a message nor an error",
			        malformed_messages[i]);
		}
	}
}

/*
 * A role of the wrong type is a missing role.
 *
 * Worth its own case because the two shapes reached that conclusion
 * differently before: an object criticalled on the way, and a number
 * arrived at ai_role_from_string() as a NULL that it was never told to
 * expect.
 */
static void
test_message_role_of_wrong_type_is_missing(void)
{
	const gchar *documents[] = { "{\"role\":7}", "{\"role\":{}}", NULL };
	gsize        i;

	for (i = 0; documents[i] != NULL; i++)
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(AiMessage)  message = NULL;
		g_autoptr(GError)     error = NULL;

		g_assert_true(json_parser_load_from_data(parser, documents[i], -1,
		                                         NULL));

		message = ai_message_new_from_json(json_parser_get_root(parser),
		                                   &error);

		g_assert_null(message);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	}
}

/* ------------------------------------------------------------------ */
/* ai_tool_use_get_input_*()                                           */
/* ------------------------------------------------------------------ */

static AiToolUse *
tool_use_with_input(const gchar *input_json)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode             *node = NULL;

	if (input_json != NULL)
	{
		g_assert_true(json_parser_load_from_data(parser, input_json, -1,
		                                         NULL));
		node = json_parser_get_root(parser);
	}

	/* ai_tool_use_new() copies the node, so the parser may go. */
	return ai_tool_use_new("id-1", "bash", node);
}

/*
 * Every accessor asked for every wrong type.
 *
 * The table is the cross product on purpose: each accessor used to test
 * only that the key was there, so which accessor and which type met is
 * exactly what decided between a critical, a silent NULL and the right
 * answer.
 */
static void
test_tool_use_input_wrong_types(void)
{
	static const gchar *inputs[] = {
		"{\"p\":\"text\"}",
		"{\"p\":7}",
		"{\"p\":1.5}",
		"{\"p\":true}",
		"{\"p\":null}",
		"{\"p\":{}}",
		"{\"p\":[]}",
		"{\"p\":[1,2]}",
		"{}",
		NULL
	};
	gsize i;

	for (i = 0; inputs[i] != NULL; i++)
	{
		g_autoptr(AiToolUse) tool_use = tool_use_with_input(inputs[i]);
		const gchar         *s;

		s = ai_tool_use_get_input_string(tool_use, "p");

		/*
		 * NULL is a legitimate answer for "there is no such string".
		 * What must not happen is a NULL where the value *is* a string,
		 * or a critical on the way to either.
		 */
		if (g_strcmp0(inputs[i], "{\"p\":\"text\"}") == 0)
			g_assert_cmpstr(s, ==, "text");

		g_assert_cmpint(ai_tool_use_get_input_int(tool_use, "p", -1), >=, -1);
		ai_tool_use_get_input_double(tool_use, "p", -1.0);
		ai_tool_use_get_input_boolean(tool_use, "p", FALSE);
	}
}

/* The fallbacks are the whole contract for a member of another type. */
static void
test_tool_use_input_falls_back(void)
{
	g_autoptr(AiToolUse) tool_use =
		tool_use_with_input("{\"p\":{\"nested\":1}}");

	g_assert_null(ai_tool_use_get_input_string(tool_use, "p"));
	g_assert_cmpint(ai_tool_use_get_input_int(tool_use, "p", 42), ==, 42);
	g_assert_cmpfloat(ai_tool_use_get_input_double(tool_use, "p", 1.5), ==,
	                  1.5);
	g_assert_true(ai_tool_use_get_input_boolean(tool_use, "p", TRUE));
}

/* An input that is not an object at all, which a model also writes. */
static void
test_tool_use_input_not_an_object(void)
{
	const gchar *inputs[] = { "[1,2,3]", "\"a string\"", "7", "null", NULL };
	gsize        i;

	for (i = 0; inputs[i] != NULL; i++)
	{
		g_autoptr(AiToolUse) tool_use = tool_use_with_input(inputs[i]);

		g_assert_null(ai_tool_use_get_input_string(tool_use, "p"));
		g_assert_cmpint(ai_tool_use_get_input_int(tool_use, "p", 42), ==, 42);
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* See test-http-malformed.c: the criticals are the subject here. */
	g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL |
	                       G_LOG_LEVEL_WARNING);

	g_test_add_func("/ai-glib/model-malformed/message/table",
	                test_message_from_malformed_json);
	g_test_add_func("/ai-glib/model-malformed/message/role-wrong-type",
	                test_message_role_of_wrong_type_is_missing);
	g_test_add_func("/ai-glib/model-malformed/tool-use/wrong-types",
	                test_tool_use_input_wrong_types);
	g_test_add_func("/ai-glib/model-malformed/tool-use/fallbacks",
	                test_tool_use_input_falls_back);
	g_test_add_func("/ai-glib/model-malformed/tool-use/not-an-object",
	                test_tool_use_input_not_an_object);

	return g_test_run();
}
