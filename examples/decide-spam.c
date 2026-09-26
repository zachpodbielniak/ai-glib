/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>

int
main(int argc, char **argv)
{
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = NULL;
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *url = g_getenv("LAYA_BASE_URL");
	request = ai_decision_request_new(argc > 1 ? argv[1] : "Claim your free prize!");
	if (url != NULL) g_object_set(client, "base-url", url, NULL);
	g_object_set(client, "model", AI_LAYA_MODEL_ENGLISH,
		"api-key", g_getenv("LAYA_API_KEY"), NULL);
	if (!ai_decision_request_add_boolean(request, "spam", "Is this message spam?", &error)) goto failed;
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	if (response == NULL) goto failed;
	g_print("Spam probability: %.4f\n", ai_decision_response_get_probability(response, "spam", NULL));
	return 0;
failed:
	g_printerr("Decision failed: %s\nUse an already provisioned Laya server; this example does not install it.\n", error->message);
	return 1;
}
