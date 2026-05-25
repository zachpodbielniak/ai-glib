/*
 * tool-use-executor.c - Multi-turn tool loop with user-registered tools
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Demonstrates ai_tool_executor_register_callback(): the same multi-turn
 * loop that drives the built-in tools (bash/read/write/...) is reused for
 * application-supplied callbacks. Works against any HTTP provider.
 *
 * Usage:
 *   export ANTHROPIC_API_KEY="..."  # or OPENAI_API_KEY / GEMINI_API_KEY / ...
 *   ./tool-use-executor <provider> [prompt]
 *
 *   <provider>: claude (default), openai, grok, gemini, ollama
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ai-glib.h"

/* ----- Application-supplied tool implementations ----- */

static gchar *
on_get_weather(
	AiToolUse    *tool_use,
	GCancellable *cancellable,
	GError      **error,
	gpointer      user_data
){
	const gchar *location;
	const gchar *unit;
	gdouble temp_c;
	gdouble temp;
	const gchar *unit_str;

	(void)cancellable;
	(void)error;
	(void)user_data;

	location = ai_tool_use_get_input_string(tool_use, "location");
	unit     = ai_tool_use_get_input_string(tool_use, "unit");

	if (location == NULL)
		location = "";

	if (g_str_has_prefix(location, "San") || g_str_has_prefix(location, "Los"))
		temp_c = 22.0;
	else if (g_str_has_prefix(location, "Miami") || g_str_has_prefix(location, "Houston"))
		temp_c = 28.0;
	else if (g_str_has_prefix(location, "New York") || g_str_has_prefix(location, "Chicago"))
		temp_c = 8.0;
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

	printf("  [get_weather: %s -> %.1f %s]\n", location, temp, unit_str);

	return g_strdup_printf(
		"{\"location\":\"%s\",\"temperature\":%.1f,\"unit\":\"%s\","
		"\"conditions\":\"partly cloudy\"}",
		location, temp, unit_str
	);
}

static gchar *
on_calculate(
	AiToolUse    *tool_use,
	GCancellable *cancellable,
	GError      **error,
	gpointer      user_data
){
	const gchar *op;
	gdouble a, b, result;

	(void)cancellable;
	(void)error;
	(void)user_data;

	op = ai_tool_use_get_input_string(tool_use, "operation");
	a  = ai_tool_use_get_input_double(tool_use, "a", 0.0);
	b  = ai_tool_use_get_input_double(tool_use, "b", 0.0);

	if (g_strcmp0(op, "add") == 0)      result = a + b;
	else if (g_strcmp0(op, "sub") == 0) result = a - b;
	else if (g_strcmp0(op, "mul") == 0) result = a * b;
	else if (g_strcmp0(op, "div") == 0) result = (fabs(b) < 1e-10) ? NAN : a / b;
	else
		return g_strdup_printf("{\"error\":\"unknown op %s\"}", op ? op : "(null)");

	printf("  [calculate: %s(%g, %g) = %g]\n", op, a, b, result);

	return g_strdup_printf("{\"result\":%g}", result);
}

/* ----- Provider factory ----- */

static AiProvider *
make_provider(const gchar *name)
{
	if (g_strcmp0(name, "openai") == 0)  return AI_PROVIDER(ai_openai_client_new());
	if (g_strcmp0(name, "grok") == 0)    return AI_PROVIDER(ai_grok_client_new());
	if (g_strcmp0(name, "gemini") == 0)  return AI_PROVIDER(ai_gemini_client_new());
	if (g_strcmp0(name, "ollama") == 0)  return AI_PROVIDER(ai_ollama_client_new());
	if (g_strcmp0(name, "claude") == 0 || name == NULL || name[0] == '\0')
		return AI_PROVIDER(ai_claude_client_new());

	g_printerr("Unknown provider '%s'. Valid: claude, openai, grok, gemini, ollama.\n",
	           name);
	return NULL;
}

int
main(int argc, char *argv[])
{
	g_autoptr(AiProvider)     provider = NULL;
	g_autoptr(AiToolExecutor) exec     = NULL;
	g_autoptr(AiTool)         weather  = NULL;
	g_autoptr(AiTool)         calc     = NULL;
	g_autoptr(AiMessage)      msg      = NULL;
	g_autoptr(GError)         err      = NULL;
	g_autofree gchar         *answer   = NULL;
	GList *messages = NULL;
	const gchar *provider_name = (argc > 1) ? argv[1] : "claude";
	const gchar *prompt = (argc > 2)
		? argv[2]
		: "What's the weather in Miami in Fahrenheit, and what's 13 * 7?";
	const gchar *op_values[] = { "add", "sub", "mul", "div", NULL };

	provider = make_provider(provider_name);
	if (provider == NULL)
		return 1;

	/* Allow overriding the model via env, useful for picking Ollama tool-capable
	 * models (e.g. AI_MODEL=llama3.1:8b ./tool-use-executor ollama ...). */
	{
		const gchar *override = g_getenv ("AI_MODEL");
		if (override != NULL && AI_IS_CLIENT (provider))
			ai_client_set_model (AI_CLIENT (provider), override);
	}

	exec = ai_tool_executor_new();

	/* Define and register a custom weather tool. */
	weather = ai_tool_new(
		"get_weather",
		"Get the current weather for a city."
	);
	ai_tool_add_parameter(weather, "location", "string",
		"City name, e.g. 'Miami, FL'.", TRUE);
	ai_tool_add_parameter(weather, "unit", "string",
		"Temperature unit: 'celsius' or 'fahrenheit'.", FALSE);
	ai_tool_executor_register_callback(exec, weather, on_get_weather, NULL, NULL);

	/* Define and register a custom calculator tool. */
	calc = ai_tool_new("calculate", "Do basic arithmetic on two numbers.");
	ai_tool_add_enum_parameter(calc, "operation", "Operation",
		op_values, TRUE);
	ai_tool_add_parameter(calc, "a", "number", "First operand", TRUE);
	ai_tool_add_parameter(calc, "b", "number", "Second operand", TRUE);
	ai_tool_executor_register_callback(exec, calc, on_calculate, NULL, NULL);

	printf("Provider: %s\nUser: %s\n", provider_name, prompt);

	msg = ai_message_new_user(prompt);
	messages = g_list_append(NULL, msg);

	/* One call: runs the full multi-turn loop, dispatches our callbacks
	 * (and any built-in tools the model happens to use), returns the
	 * final text. */
	answer = ai_tool_executor_run(exec, provider, messages,
	                              "You are a helpful assistant. Use the "
	                              "provided tools whenever they would help.",
	                              4096, NULL, &err);

	g_list_free(messages);

	if (err != NULL)
	{
		g_printerr("Error: %s\n", err->message);
		return 1;
	}

	printf("\nAssistant: %s\n", answer != NULL ? answer : "(no text)");
	return 0;
}
