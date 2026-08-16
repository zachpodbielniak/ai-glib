/*
 * simple-chat-grok-build.c - Simple chat example using the Grok Build CLI
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates basic usage of ai-glib with xAI's `grok` CLI
 * in headless mode. The prompt is piped on stdin and read back by grok
 * through `--prompt-file /dev/stdin`, so a large conversation never runs
 * into ARG_MAX.
 *
 * Requirements:
 *   - The `grok` CLI must be installed and authenticated (`grok login`)
 *   - Either in PATH or set the GROK_PATH environment variable
 *
 * Usage:
 *   ./simple-chat-grok-build [prompt]
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

	printf("Cost: $%.6f\n",
	       ai_grok_build_client_get_total_cost(AI_GROK_BUILD_CLIENT(source)));

	g_main_loop_quit(loop);
}

int
main(
	int   argc,
	char *argv[]
){
	g_autoptr(AiGrokBuildClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	GList *messages = NULL;
	const gchar *prompt;
	const gchar *model_env;

	/* Get prompt from command line or use default */
	prompt = (argc > 1) ? argv[1] : "What is the capital of France?";

	printf("User: %s\n\n", prompt);

	/* Create client (uses the `grok` CLI from PATH or GROK_PATH) */
	client = ai_grok_build_client_new();

	/*
	 * Allow setting the model via environment variable for easy testing:
	 *   GROK_BUILD_MODEL=grok-4.5 ./simple-chat-grok-build
	 */
	model_env = g_getenv("GROK_BUILD_MODEL");
	if (model_env != NULL && model_env[0] != '\0')
	{
		ai_cli_client_set_model(AI_CLI_CLIENT(client), model_env);
		printf("Using model: %s\n\n", model_env);
	}
	else
	{
		printf("Using model: %s\n\n", AI_GROK_BUILD_DEFAULT_MODEL);
	}

	/*
	 * Reasoning effort. grok accepts low, medium, high and xhigh; the
	 * library folds AI_EFFORT_MAX onto xhigh.
	 */
	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "low");

	/*
	 * Run without tool-use prompts. This grants the model full tool
	 * access -- bound what it can reach with the working directory
	 * (ai_cli_client_set_working_directory) or a sandbox profile.
	 */
	ai_grok_build_client_set_skip_permissions(client, TRUE);

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
		4096,  /* max tokens (ignored: grok has no such flag) */
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
