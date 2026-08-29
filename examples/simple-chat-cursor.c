/*
 * simple-chat-cursor.c - Simple chat example using the Cursor Agent CLI
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates basic usage of ai-glib with Cursor's
 * `cursor-agent` CLI in `--print` mode. The prompt is piped on stdin
 * so a large conversation never runs into ARG_MAX and never appears
 * in ps. `--print` is a boolean, not a prompt-taking flag.
 *
 * Requirements:
 *   - The `cursor-agent` CLI must be installed and authenticated
 *   - Either in PATH or set CURSOR_AGENT_PATH / CURSOR_PATH
 *
 * Usage:
 *   ./simple-chat-cursor [prompt]
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
	g_autoptr(AiCursorClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	GList *messages = NULL;
	const gchar *prompt;
	const gchar *model_env;

	prompt = (argc > 1) ? argv[1] : "What is the capital of France?";

	printf("User: %s\n\n", prompt);

	client = ai_cursor_client_new();

	model_env = g_getenv("CURSOR_MODEL");
	if (model_env != NULL && model_env[0] != '\0')
	{
		ai_cli_client_set_model(AI_CLI_CLIENT(client), model_env);
		printf("Using model: %s\n\n", model_env);
	}
	else
	{
		printf("Using model: %s\n\n", AI_CURSOR_DEFAULT_MODEL);
	}

	ai_cursor_client_set_skip_permissions(client, TRUE);

	msg = ai_message_new_user(prompt);
	messages = g_list_append(NULL, msg);

	loop = g_main_loop_new(NULL, FALSE);

	ai_provider_chat_async(
		AI_PROVIDER(client),
		messages,
		NULL,
		4096,
		NULL,
		NULL,
		on_chat_complete,
		loop
	);

	g_main_loop_run(loop);

	g_list_free(messages);

	return 0;
}
