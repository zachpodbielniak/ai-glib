/*
 * simple-chat-opencode.c - Simple chat example using OpenCode CLI
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates basic usage of ai-glib with the OpenCode CLI.
 * OpenCode supports multiple providers (Anthropic, OpenAI, Google) via a single CLI.
 *
 * Requirements:
 *   - The `opencode` CLI must be installed and configured
 *   - Either in PATH or set OPENCODE_PATH environment variable
 *
 * Usage:
 *   ./simple-chat-opencode [prompt]
 */

#include <stdio.h>
#include <stdlib.h>

#include "ai-glib.h"

static void
on_chat_complete(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GMainLoop *loop = user_data;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	const AiUsage *usage;

	response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

	if (error != NULL)
	{
		g_printerr("Error: %s\n", error->message);
		g_main_loop_quit(loop);
		return;
	}

	text = ai_response_get_text(response);
	printf("Assistant: %s\n", text);

	usage = ai_response_get_usage(response);
	if (usage != NULL)
	{
		printf("\nUsage: %d input tokens, %d output tokens\n",
		       ai_usage_get_input_tokens(usage),
		       ai_usage_get_output_tokens(usage));
	}

	g_main_loop_quit(loop);
}

int
main(
	int   argc,
	char *argv[]
){
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	GList *messages = NULL;
	const gchar *prompt;
	const gchar *model_env;

	/* Get prompt from command line or use default */
	prompt = (argc > 1) ? argv[1] : "What is the capital of France?";

	printf("User: %s\n\n", prompt);

	/* Create client (uses `opencode` CLI from PATH or OPENCODE_PATH) */
	client = ai_opencode_client_new();

	/*
	 * Allow setting the model via environment variable for easy testing.
	 * Examples:
	 *   OPENCODE_MODEL=openai/gpt-4o ./simple-chat-opencode
	 *   OPENCODE_MODEL=google/gemini-2.0-flash ./simple-chat-opencode
	 */
	model_env = g_getenv("OPENCODE_MODEL");
	if (model_env != NULL && model_env[0] != '\0')
	{
		ai_cli_client_set_model(AI_CLI_CLIENT(client), model_env);
		printf("Using model: %s\n\n", model_env);
	}
	else
	{
		printf("Using model: %s\n\n", AI_OPENCODE_DEFAULT_MODEL);
	}

	/* Create message */
	msg = ai_message_new_user(prompt);
	messages = g_list_append(NULL, msg);

	/* Set up main loop for async operation */
	loop = g_main_loop_new(NULL, FALSE);

	/* Send chat request */
	ai_provider_chat_async(
		AI_PROVIDER(client),
		messages,
		NULL,  /* system prompt */
		4096,  /* max tokens */
		NULL,  /* tools */
		NULL,  /* cancellable */
		on_chat_complete,
		loop
	);

	/* Run until complete */
	g_main_loop_run(loop);

	g_list_free(messages);

	return 0;
}
