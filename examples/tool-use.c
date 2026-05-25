/*
 * tool-use.c - Tool/function calling example, runnable against any provider
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Demonstrates a manually-driven multi-turn tool-use loop. The same code
 * works against any HTTP provider (claude, openai, grok, gemini, ollama)
 * thanks to per-provider wire-format serialization. For an easier
 * register-and-go integration, see tool-use-executor.c.
 *
 * Usage:
 *   export ANTHROPIC_API_KEY="..."   (or OPENAI_API_KEY / GEMINI_API_KEY / ...)
 *   ./tool-use <provider> [prompt]
 *
 *   <provider> is one of: claude (default), openai, grok, gemini, ollama
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ai-glib.h"

typedef struct {
	GMainLoop   *loop;
	AiProvider  *provider;
	GList       *messages;
	GList       *tools;
	const gchar *system_prompt;
	gint         turn_count;
	gint         max_turns;
} ToolContext;

/* Forward declaration */
static void send_chat_request(ToolContext *ctx);

/*
 * execute_get_weather: simulate fetching weather data.
 */
static gchar *
execute_get_weather(const gchar *location, const gchar *unit)
{
	gdouble temp_c;
	gdouble temp;
	const gchar *unit_str;

	if (g_str_has_prefix(location, "San") || g_str_has_prefix(location, "Los"))
		temp_c = 22.0;
	else if (g_str_has_prefix(location, "New York") || g_str_has_prefix(location, "Chicago"))
		temp_c = 8.0;
	else if (g_str_has_prefix(location, "Miami") || g_str_has_prefix(location, "Houston"))
		temp_c = 28.0;
	else
		temp_c = 15.0;

	if (unit != NULL && g_ascii_strcasecmp(unit, "fahrenheit") == 0)
	{
		temp = (temp_c * 9.0 / 5.0) + 32.0;
		unit_str = "F";
	}
	else
	{
		temp = temp_c;
		unit_str = "C";
	}

	return g_strdup_printf(
		"{\"location\": \"%s\", \"temperature\": %.1f, \"unit\": \"%s\", "
		"\"conditions\": \"partly cloudy\", \"humidity\": 65}",
		location, temp, unit_str
	);
}

/*
 * execute_calculate: perform a math operation.
 */
static gchar *
execute_calculate(const gchar *operation, gdouble a, gdouble b)
{
	gdouble result;
	gboolean error = FALSE;
	const gchar *error_msg = NULL;

	if (g_strcmp0(operation, "add") == 0)
		result = a + b;
	else if (g_strcmp0(operation, "subtract") == 0)
		result = a - b;
	else if (g_strcmp0(operation, "multiply") == 0)
		result = a * b;
	else if (g_strcmp0(operation, "divide") == 0)
	{
		if (fabs(b) < 1e-10) { error = TRUE; error_msg = "Division by zero"; }
		else                   result = a / b;
	}
	else { error = TRUE; error_msg = "Unknown operation"; }

	if (error)
		return g_strdup_printf("{\"error\": \"%s\"}", error_msg);

	return g_strdup_printf(
		"{\"operation\": \"%s\", \"a\": %g, \"b\": %g, \"result\": %g}",
		operation, a, b, result
	);
}

static gchar *
execute_tool(AiToolUse *tool_use)
{
	const gchar *name = ai_tool_use_get_name(tool_use);

	if (g_strcmp0(name, "get_weather") == 0)
	{
		const gchar *location = ai_tool_use_get_input_string(tool_use, "location");
		const gchar *unit = ai_tool_use_get_input_string(tool_use, "unit");

		printf("  [Executing get_weather: location=\"%s\", unit=\"%s\"]\n",
		       location ? location : "(null)",
		       unit ? unit : "celsius");

		return execute_get_weather(location ? location : "", unit);
	}
	else if (g_strcmp0(name, "calculate") == 0)
	{
		const gchar *operation = ai_tool_use_get_input_string(tool_use, "operation");
		gdouble a = ai_tool_use_get_input_double(tool_use, "a", 0.0);
		gdouble b = ai_tool_use_get_input_double(tool_use, "b", 0.0);

		printf("  [Executing calculate: %s(%g, %g)]\n", operation, a, b);

		return execute_calculate(operation, a, b);
	}

	return g_strdup("{\"error\": \"Unknown tool\"}");
}

static void
on_response(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	ToolContext *ctx = user_data;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	const AiUsage *usage;

	response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

	if (error != NULL)
	{
		g_printerr("Error: %s\n", error->message);
		g_main_loop_quit(ctx->loop);
		return;
	}

	ctx->turn_count++;

	if (ai_response_has_tool_use(response))
	{
		GList *tool_uses;
		GList *iter;
		g_autoptr(AiMessage) assistant_msg = NULL;
		g_autofree gchar *response_text = NULL;

		printf("\n[Turn %d: Model requested tool use]\n", ctx->turn_count);

		response_text = ai_response_get_text(response);
		if (response_text != NULL && strlen(response_text) > 0)
			printf("Assistant: %s\n", response_text);

		if (ctx->turn_count >= ctx->max_turns)
		{
			g_printerr("Max turns (%d) reached, stopping.\n", ctx->max_turns);
			g_main_loop_quit(ctx->loop);
			return;
		}

		/* Reconstruct the assistant message including tool_use blocks. */
		assistant_msg = ai_message_new(AI_ROLE_ASSISTANT);
		for (iter = ai_response_get_content_blocks(response);
		     iter != NULL;
		     iter = iter->next)
		{
			AiContentBlock *block = iter->data;
			ai_message_add_content_block(
				assistant_msg,
				(AiContentBlock *)g_object_ref(block)
			);
		}
		ctx->messages = g_list_append(ctx->messages, g_object_ref(assistant_msg));

		tool_uses = ai_response_get_tool_uses(response);
		for (iter = tool_uses; iter != NULL; iter = iter->next)
		{
			AiToolUse *tool_use = iter->data;
			g_autofree gchar *tool_result = NULL;
			g_autoptr(AiMessage) result_msg = NULL;
			const gchar *tool_id = ai_tool_use_get_id(tool_use);
			const gchar *tool_name = ai_tool_use_get_name(tool_use);

			tool_result = execute_tool(tool_use);

			printf("  [Result: %s]\n", tool_result);

			/* Use *_with_name so Gemini's functionResponse (keyed by name)
			 * round-trips correctly. For other providers the name is ignored. */
			result_msg = ai_message_new_tool_result_with_name(
				tool_id, tool_name, tool_result, FALSE);
			ctx->messages = g_list_append(ctx->messages, g_steal_pointer(&result_msg));
		}
		g_list_free(tool_uses);

		send_chat_request(ctx);
		return;
	}

	{
		g_autofree gchar *text = ai_response_get_text(response);
		printf("\nAssistant: %s\n", text != NULL ? text : "(no text)");
	}

	usage = ai_response_get_usage(response);
	if (usage != NULL)
	{
		printf("\nUsage: %d input tokens, %d output tokens\n",
		       ai_usage_get_input_tokens(usage),
		       ai_usage_get_output_tokens(usage));
	}

	printf("Conversation completed in %d turn(s).\n", ctx->turn_count);
	g_main_loop_quit(ctx->loop);
}

static void
send_chat_request(ToolContext *ctx)
{
	ai_provider_chat_async(
		ctx->provider,
		ctx->messages,
		ctx->system_prompt,
		4096,
		ctx->tools,
		NULL,
		on_response,
		ctx
	);
}

static GList *
create_tools(void)
{
	GList *tools = NULL;
	AiTool *weather_tool;
	AiTool *calc_tool;
	const gchar *unit_values[] = { "celsius", "fahrenheit", NULL };
	const gchar *op_values[] = { "add", "subtract", "multiply", "divide", NULL };

	weather_tool = ai_tool_new(
		"get_weather",
		"Get the current weather for a location. Returns temperature, "
		"conditions, and humidity."
	);
	ai_tool_add_parameter(weather_tool, "location", "string",
		"The city and state/country, e.g. 'San Francisco, CA' or 'London, UK'", TRUE);
	ai_tool_add_enum_parameter(weather_tool, "unit",
		"Temperature unit to use", unit_values, FALSE);
	tools = g_list_append(tools, weather_tool);

	calc_tool = ai_tool_new("calculate",
		"Perform a mathematical operation on two numbers.");
	ai_tool_add_enum_parameter(calc_tool, "operation",
		"The operation to perform", op_values, TRUE);
	ai_tool_add_parameter(calc_tool, "a", "number", "The first operand", TRUE);
	ai_tool_add_parameter(calc_tool, "b", "number", "The second operand", TRUE);
	tools = g_list_append(tools, calc_tool);

	return tools;
}

/* Construct an AiProvider for the requested provider name. */
static AiProvider *
make_provider(const gchar *name)
{
	if (g_strcmp0(name, "openai") == 0)
		return AI_PROVIDER(ai_openai_client_new());
	if (g_strcmp0(name, "grok") == 0)
		return AI_PROVIDER(ai_grok_client_new());
	if (g_strcmp0(name, "gemini") == 0)
		return AI_PROVIDER(ai_gemini_client_new());
	if (g_strcmp0(name, "ollama") == 0)
		return AI_PROVIDER(ai_ollama_client_new());
	if (g_strcmp0(name, "claude") == 0 || name == NULL || name[0] == '\0')
		return AI_PROVIDER(ai_claude_client_new());

	g_printerr("Unknown provider '%s'. Valid: claude, openai, grok, gemini, ollama.\n",
	           name);
	return NULL;
}

int
main(int argc, char *argv[])
{
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	ToolContext ctx = { 0 };
	const gchar *provider_name = (argc > 1) ? argv[1] : "claude";
	const gchar *prompt = (argc > 2)
		? argv[2]
		: "What's the weather in San Francisco and New York? "
		  "Also, calculate 15 * 7 + 23 for me.";

	provider = make_provider(provider_name);
	if (provider == NULL)
		return 1;

	printf("Provider: %s\n", provider_name);
	printf("User: %s\n", prompt);

	loop = g_main_loop_new(NULL, FALSE);
	ctx.loop = loop;
	ctx.provider = provider;
	ctx.tools = create_tools();
	ctx.system_prompt = "You are a helpful assistant with access to weather "
	                    "and calculation tools. Use them when needed to "
	                    "answer the user's questions accurately.";
	ctx.turn_count = 0;
	ctx.max_turns = 10;

	msg = ai_message_new_user(prompt);
	ctx.messages = g_list_append(NULL, g_object_ref(msg));

	send_chat_request(&ctx);
	g_main_loop_run(loop);

	g_list_free_full(ctx.messages, g_object_unref);
	g_list_free_full(ctx.tools, g_object_unref);

	return 0;
}
