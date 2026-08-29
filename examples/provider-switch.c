/*
 * provider-switch.c - Keep AiConversation context across providers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Sends one asynchronous turn to a provider, switches only after that turn
 * completes, then sends a second turn through another provider. The second
 * provider receives the first turn's canonical AiMessage history.
 *
 * Usage:
 *   export ANTHROPIC_API_KEY="..."
 *   export OPENAI_API_KEY="..."
 *   ./build/release/examples/provider-switch claude openai
 */

#include <ai-glib.h>

typedef struct
{
	GMainLoop      *loop;
	AiConversation *conversation;
	GObject        *second_provider;
	gint            status;
} SwitchState;

static void
on_second_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	SwitchState *state = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;

	if (!ai_conversation_send_finish(AI_CONVERSATION(source), result, &error))
	{
		g_printerr("Second turn failed: %s\n", error->message);
		state->status = 1;
		g_main_loop_quit(state->loop);
		return;
	}

	transcript = ai_transcript_to_text(
		ai_conversation_get_transcript(state->conversation), 0);

	g_print("\n--- shared transcript ---\n%s\n", transcript);
	g_print("\nPortable history now contains %u messages.\n",
		g_list_length(ai_conversation_get_messages(state->conversation)));

	g_main_loop_quit(state->loop);
}

static void
on_first_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	SwitchState *state = user_data;
	g_autoptr(GError) error = NULL;
	GObject *first_provider;

	if (!ai_conversation_send_finish(AI_CONVERSATION(source), result, &error))
	{
		g_printerr("First turn failed: %s\n", error->message);
		state->status = 1;
		g_main_loop_quit(state->loop);
		return;
	}

	first_provider = ai_conversation_get_provider(state->conversation);
	g_print("First turn completed with %s (%u messages).\n",
		ai_provider_get_name(AI_PROVIDER(first_provider)),
		g_list_length(ai_conversation_get_messages(state->conversation)));

	if (!ai_conversation_set_provider(state->conversation,
	                                  state->second_provider, &error))
	{
		g_printerr("Provider unchanged: %s\n", error->message);
		state->status = 1;
		g_main_loop_quit(state->loop);
		return;
	}

	g_print("Switched to %s; sending the next turn with shared context.\n",
		ai_provider_get_name(AI_PROVIDER(state->second_provider)));

	ai_conversation_send_async(
		state->conversation,
		"What code word did I ask the previous provider to remember? "
		"Answer with the word only.",
		NULL,
		on_second_sent,
		state
	);
}

int
main(
	int   argc,
	char *argv[]
){
	const gchar *first_name;
	const gchar *second_name;
	g_autoptr(GMainLoop) loop = NULL;
	g_autoptr(GObject) first_provider = NULL;
	g_autoptr(GObject) second_provider = NULL;
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GError) error = NULL;
	SwitchState state;

	first_name = argc > 1 ? argv[1] : "claude";
	second_name = argc > 2 ? argv[2] : "openai";

	first_provider =
		ai_provider_factory_new_from_string(first_name, NULL, &error);
	if (first_provider == NULL)
	{
		g_printerr("Could not create '%s': %s\n",
			first_name, error->message);
		return 2;
	}

	second_provider =
		ai_provider_factory_new_from_string(second_name, NULL, &error);
	if (second_provider == NULL)
	{
		g_printerr("Could not create '%s': %s\n",
			second_name, error->message);
		return 2;
	}

	loop = g_main_loop_new(NULL, FALSE);
	conversation = ai_conversation_new(first_provider);
	ai_conversation_set_system_prompt(
		conversation,
		"Follow the user's instructions and keep answers concise."
	);

	state.loop = loop;
	state.conversation = conversation;
	state.second_provider = second_provider;
	state.status = 0;

	g_print("First provider: %s\n",
		ai_provider_get_name(AI_PROVIDER(first_provider)));
	g_print("Second provider: %s\n\n",
		ai_provider_get_name(AI_PROVIDER(second_provider)));

	ai_conversation_send_async(
		conversation,
		"Remember the code word amber. Confirm it in one short sentence.",
		NULL,
		on_first_sent,
		&state
	);

	g_main_loop_run(loop);

	return state.status;
}
