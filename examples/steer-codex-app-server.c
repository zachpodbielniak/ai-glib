/*
 * steer-codex-app-server.c - Steer a running codex turn over `codex app-server`
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The check for AiCodexCliClient's app-server transport against a real
 * codex.  It was written from codex's published protocol and has only
 * run against a fake server, so this is how to find out whether the
 * installed codex agrees:
 *
 *   make examples
 *   ./build/release/examples/steer-codex-app-server
 *
 * It starts one streamed turn that runs `sleep 15` before answering,
 * steers it when the command starts, and prints every event, the
 * steer's answer and the reply.  It passes when the steer prints
 * "delivered" and the reply contains STEERED-OK.  Any failure prints
 * codex's own words; G_MESSAGES_DEBUG=all adds ai-glib's debug log.
 *
 * Requirements: `codex` on PATH (or CODEX_PATH) and `codex login` done.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ai-glib.h"

#define STEER_EXAMPLE_LICENSE \
	"steer-codex-app-server - part of ai-glib\n" \
	"Copyright (C) 2026\n" \
	"License AGPLv3+: GNU AGPL version 3 or later <https://www.gnu.org/licenses/agpl.html>\n" \
	"This is free software: you are free to change and redistribute it.\n" \
	"There is NO WARRANTY, to the extent permitted by law.\n"

typedef struct
{
	GMainLoop        *loop;
	AiCodexCliClient *client;
	gboolean          steered;
	gboolean          turn_done;
	gboolean          steer_done;
	AiSteerResult     steer_result;
	gint              status;
} SteerExample;

/* Quit once both the turn and the steer have answered. */
static void
steer_example_maybe_quit(SteerExample *example)
{
	if (example->turn_done && (!example->steered || example->steer_done))
		g_main_loop_quit(example->loop);
}

static void
on_steered(GObject *source, GAsyncResult *result, gpointer user_data)
{
	SteerExample *example = user_data;
	g_autoptr(GError) error = NULL;

	example->steer_result = ai_steerable_steer_finish(AI_STEERABLE(source), result, &error);
	example->steer_done = TRUE;
	printf("steer: %s%s%s\n", ai_steer_result_to_string(example->steer_result),
	       error != NULL ? " -- " : "", error != NULL ? error->message : "");
	steer_example_maybe_quit(example);
}

/* Print each event; steer when the first command starts. */
static void
on_event(AiEventSource *source, AiEvent *event, gpointer user_data)
{
	SteerExample *example = user_data;
	AiEventKind kind = ai_event_get_kind(event);

	(void)source;

	switch (kind)
	{
		case AI_EVENT_TEXT_DELTA:
			printf("%s", ai_event_get_text(event));
			fflush(stdout);
			break;
		case AI_EVENT_TOOL_STARTED:
			printf("\n[tool started: %s]\n",
			       ai_tool_use_get_name(ai_event_get_tool_use(event)));
			break;
		case AI_EVENT_TOOL_FINISHED:
			printf("\n[tool finished]\n");
			break;
		case AI_EVENT_STATUS:
			printf("\n[status: %s]\n", ai_event_get_text(event));
			break;
		default:
			break;
	}

	if (kind == AI_EVENT_TOOL_STARTED && !example->steered)
	{
		g_autofree gchar *turn_id = ai_steerable_dup_turn_id(AI_STEERABLE(example->client));

		example->steered = TRUE;
		printf("[steering turn %s]\n", turn_id != NULL ? turn_id : "(none)");
		ai_steerable_steer_async(AI_STEERABLE(example->client), turn_id,
		                         "When the command finishes, reply with exactly STEERED-OK and nothing else.",
		                         NULL, on_steered, example);
	}
}

static void
on_turn(GObject *source, GAsyncResult *result, gpointer user_data)
{
	SteerExample *example = user_data;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source), result, &error);
	example->turn_done = TRUE;
	if (response == NULL)
	{
		g_printerr("\nturn failed: %s\n", error->message);
		example->status = EXIT_FAILURE;
		steer_example_maybe_quit(example);
		return;
	}
	text = ai_response_get_text(response);
	printf("\n\nreply: %s\n", text != NULL ? text : "");
	if (text == NULL || g_strstr_len(text, -1, "STEERED-OK") == NULL)
		example->status = EXIT_FAILURE;
	steer_example_maybe_quit(example);
}

int
main(int argc, char *argv[])
{
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCodexCliClient) client = NULL;
	g_autoptr(AiMessage) message = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autofree gchar *model = NULL;
	gboolean license = FALSE;
	GList *messages = NULL;
	SteerExample example = { 0 };
	GOptionEntry entries[] = {
		{ "model", 'm', 0, G_OPTION_ARG_STRING, &model, "Model id (default: the client's)", "MODEL" },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &license, "Show the license", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

	context = g_option_context_new("- steer a running codex turn over app-server");
	g_option_context_add_main_entries(context, entries, NULL);
	g_option_context_set_description(context,
		"Examples:\n"
		"  steer-codex-app-server\n"
		"  steer-codex-app-server --model gpt-5.5\n"
		"  CODEX_PATH=/opt/codex/bin/codex steer-codex-app-server\n"
		"\n"
		"Passes (exit 0) when the steer is delivered and the reply contains STEERED-OK.\n");
	if (!g_option_context_parse(context, &argc, &argv, &error))
	{
		g_printerr("%s\n", error->message);
		return EXIT_FAILURE;
	}
	if (license)
	{
		printf("%s", STEER_EXAMPLE_LICENSE);
		return EXIT_SUCCESS;
	}

	/* The steerable transport; read-only is enough to run `sleep`. */
	client = ai_codex_cli_client_new();
	ai_codex_cli_client_set_transport(client, AI_CODEX_CLI_TRANSPORT_APP_SERVER);
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(client), 300000);
	if (model != NULL)
		ai_cli_client_set_model(AI_CLI_CLIENT(client), model);
	printf("can_steer: %s\n",
	       ai_steerable_can_steer(AI_STEERABLE(client)) ? "yes" : "no");

	loop = g_main_loop_new(NULL, FALSE);
	example.loop = loop;
	example.client = client;
	example.status = EXIT_SUCCESS;
	g_signal_connect(client, "event", G_CALLBACK(on_event), &example);

	message = ai_message_new_user(
		"Run the shell command `sleep 15`, then reply with one short sentence "
		"saying that it finished.");
	messages = g_list_append(messages, message);
	ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages, NULL, 0, NULL,
	                                NULL, on_turn, &example);
	g_main_loop_run(loop);
	g_list_free(messages);

	if (!example.steered)
	{
		g_printerr("no command started, so nothing was steered\n");
		return EXIT_FAILURE;
	}
	if (example.steer_result != AI_STEER_DELIVERED)
		example.status = EXIT_FAILURE;
	return example.status;
}
