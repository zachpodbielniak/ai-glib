/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>

int
main(int argc, char **argv)
{
	g_autoptr(AiOpenAICompatibleClient) client = ai_openai_compatible_client_new();
	g_autoptr(AiMessage) message = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	GList messages = { NULL, NULL, NULL };

	if (argc != 3)
	{
		g_printerr("Usage: %s MODEL PROMPT\nSet OPENAI_COMPATIBLE_BASE_URL and optionally OPENAI_COMPATIBLE_API_KEY.\n", argv[0]);
		return 1;
	}
	ai_client_set_model(AI_CLIENT(client), argv[1]);
	message = ai_message_new_user(argv[2]);
	messages.data = message;
	response = ai_client_chat_sync(AI_CLIENT(client), &messages, NULL, &error);
	if (response == NULL)
	{
		g_printerr("%s\n", error->message);
		return 1;
	}
	text = ai_response_get_text(response);
	g_print("%s\n", text);
	return 0;
}
