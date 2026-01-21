/*
 * simple-chat-grok.c - Simple chat example using xAI Grok
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates basic usage of ai-glib with the Grok provider.
 * It sends a simple message and prints the response.
 *
 * Usage:
 *   export XAI_API_KEY="your-api-key"
 *   ./simple-chat-grok
 *
 * Alternative:
 *   export GROK_API_KEY="your-api-key"
 *   ./simple-chat-grok
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
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	GList *messages = NULL;
	const gchar *prompt;

	/* Get prompt from command line or use default */
	prompt = (argc > 1) ? argv[1] : "What is the capital of France?";

	printf("User: %s\n\n", prompt);

	/* Create client (uses XAI_API_KEY or GROK_API_KEY from environment) */
	client = ai_grok_client_new();

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
