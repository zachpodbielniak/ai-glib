/*
 * streaming-chat.c - Streaming chat example
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates the AiStreamable interface with Claude.
 * Text is printed in real-time as it arrives from the API.
 *
 * Usage:
 *   export ANTHROPIC_API_KEY="your-api-key"
 *   ./streaming-chat [prompt]
 */

#include <stdio.h>
#include <stdlib.h>

#include "ai-glib.h"

typedef struct {
	GMainLoop *loop;
	gboolean   first_delta;
} StreamContext;

/*
 * on_stream_start:
 *
 * Called when streaming begins. Prints the "Assistant: " prefix.
 */
static void
on_stream_start(
	AiStreamable  *streamable,
	StreamContext *ctx
){
	(void)streamable;

	printf("Assistant: ");
	fflush(stdout);
	ctx->first_delta = TRUE;
}

/*
 * on_delta:
 *
 * Called for each text chunk received. Prints the text immediately.
 */
static void
on_delta(
	AiStreamable  *streamable,
	const gchar   *text,
	StreamContext *ctx
){
	(void)streamable;
	(void)ctx;

	printf("%s", text);
	fflush(stdout);
}

/*
 * on_stream_end:
 *
 * Called when streaming ends. Prints a newline to finish the response.
 */
static void
on_stream_end(
	AiStreamable  *streamable,
	AiResponse    *response,
	StreamContext *ctx
){
	(void)streamable;
	(void)response;
	(void)ctx;

	printf("\n");
	fflush(stdout);
}

/*
 * on_stream_complete:
 *
 * Called when the async operation completes. Prints usage stats and quits.
 */
static void
on_stream_complete(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	StreamContext *ctx = user_data;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	const AiUsage *usage;

	response = ai_streamable_chat_stream_finish(
		AI_STREAMABLE(source),
		result,
		&error
	);

	if (error != NULL)
	{
		g_printerr("\nError: %s\n", error->message);
		g_main_loop_quit(ctx->loop);
		return;
	}

	/* Print usage statistics */
	usage = ai_response_get_usage(response);
	if (usage != NULL)
	{
		printf("\nUsage: %d input tokens, %d output tokens\n",
		       ai_usage_get_input_tokens(usage),
		       ai_usage_get_output_tokens(usage));
	}

	g_main_loop_quit(ctx->loop);
}

int
main(
	int   argc,
	char *argv[]
){
	g_autoptr(AiClaudeClient) client = NULL;
	g_autoptr(AiMessage) msg = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	StreamContext ctx = { 0 };
	GList *messages = NULL;
	const gchar *prompt;

	/* Get prompt from command line or use default */
	prompt = (argc > 1) ? argv[1] : "Tell me a short story about a brave robot.";

	printf("User: %s\n\n", prompt);

	/* Create client (uses ANTHROPIC_API_KEY from environment) */
	client = ai_claude_client_new();

	/* Set up main loop for async operation */
	loop = g_main_loop_new(NULL, FALSE);
	ctx.loop = loop;
	ctx.first_delta = FALSE;

	/* Connect to streaming signals */
	g_signal_connect(
		client,
		"stream-start",
		G_CALLBACK(on_stream_start),
		&ctx
	);
	g_signal_connect(
		client,
		"delta",
		G_CALLBACK(on_delta),
		&ctx
	);
	g_signal_connect(
		client,
		"stream-end",
		G_CALLBACK(on_stream_end),
		&ctx
	);

	/* Create message */
	msg = ai_message_new_user(prompt);
	messages = g_list_append(NULL, msg);

	/* Send streaming chat request */
	ai_streamable_chat_stream_async(
		AI_STREAMABLE(client),
		messages,
		NULL,  /* system prompt */
		4096,  /* max tokens */
		NULL,  /* tools */
		NULL,  /* cancellable */
		on_stream_complete,
		&ctx
	);

	/* Run until complete */
	g_main_loop_run(loop);

	g_list_free(messages);

	return 0;
}
