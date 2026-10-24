/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>

/*
 * One Noul against TypeSafe Jev.
 *
 * ai_typesafe_client_new() reads TYPESAFE_API_KEY, TYPESAFE_BASE_URL and
 * TYPESAFE_DEFAULT_MODEL. This program does not contact the API when the
 * key is missing: that failure is reported and nothing is invented.
 */
int
main(int argc, char **argv)
{
	g_autoptr(AiTypesafeClient) client = ai_typesafe_client_new();
	g_autoptr(AiDecisionRequest) request = NULL;
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *text = argc > 1 ? argv[1] : "Please refund the duplicate charge.";

	request = ai_decision_request_new(text);
	if (!ai_decision_request_add_noul(request, "refund", "Is a refund requested?",
			"asks for money back", "does not ask", &error))
		goto failed;
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	if (response == NULL)
		goto failed;
	g_print("Refund probability: %.4f\n",
		ai_decision_response_get_probability(response, "refund", NULL));
	return 0;
failed:
	g_printerr("Decision failed: %s\nSet TYPESAFE_API_KEY. This example does not embed a key.\n",
		error != NULL ? error->message : "unknown error");
	return 1;
}
