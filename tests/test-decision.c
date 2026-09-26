/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include "test-server.h"

static const gchar *reply =
	"{\"model\":\"laya-rl-agent\",\"routing\":{\"model\":\"english\",\"repo\":\"checkpoint\"},"
	"\"answers\":{\"spam\":{\"type\":\"noul\",\"noul\":0.93}}}";

static AiDecisionRequest *
request_new(void)
{
	g_autoptr(AiDecisionRequest) request = ai_decision_request_new("A free prize");
	g_autoptr(GError) error = NULL;
	g_assert_true(ai_decision_request_add_boolean(request, "spam", "Is this spam?", &error));
	g_assert_no_error(error);
	return g_steal_pointer(&request);
}

static void
test_wire(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;

	tserver_set_response(server, 200, reply);
	g_object_set(client, "base-url", server->base_url, "model", AI_LAYA_MODEL_ENGLISH,
		"api-key", "test-key", NULL);
	g_assert_cmpuint(server->hits, ==, 0);
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_cmpfloat(ai_decision_response_get_probability(response, "spam", NULL), ==, 0.93);
	g_assert_cmpstr(ai_decision_response_get_model(response), ==, "laya-rl-agent");
	g_assert_cmpstr(ai_decision_response_get_checkpoint(response), ==, "english");
	g_assert_cmpfloat(ai_decision_response_get_elapsed_ms(response), >=, 0);
	g_assert_cmpstr(server->last_path, ==, "/v1/systemone");
	g_assert_cmpstr(g_hash_table_lookup(server->last_headers, "authorization"), ==, "Bearer test-key");
	g_assert_true(json_parser_load_from_data(parser, server->last_body, -1, &error));
	root = json_node_get_object(json_parser_get_root(parser));
	g_assert_cmpstr(json_object_get_string_member(root, "state"), ==, "A free prize");
	g_assert_cmpstr(json_object_get_string_member(root, "model"), ==, "english");
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(
		json_object_get_object_member(root, "questions"), "spam"), "type"), ==, "noul");
	tserver_free(server);
}

static void
test_malformed(void)
{
	const gchar *bad[] = { "null", "[]", "{}", "{\"model\":7}",
		"{\"model\":\"x\",\"answers\":{\"spam\":{\"type\":\"noul\",\"noul\":2}}}",
		"{\"model\":\"x\",\"answers\":{\"spam\":{\"type\":\"noul\",\"noul\":\"0.9\"}}}", NULL };
	g_autoptr(AiDecisionRequest) request = request_new();
	guint i;
	for (i = 0; bad[i] != NULL; i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AiDecisionResponse) response = ai_decision_response_new_from_json(request, bad[i], 1, &error);
		g_assert_null(response);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	}
}

static void
test_timeout(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	tserver_set_response(server, 200, reply);
	tserver_set_delay(server, 150);
	g_object_set(client, "base-url", server->base_url, "timeout-ms", 20u, NULL);
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	tserver_free(server);
}

static void
test_service_down(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(client, "base-url", server->base_url, NULL);
	tserver_free(server);
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_nonnull(error);
}

static void
test_mock(void)
{
	g_autoptr(AiMockDecider) mock = ai_mock_decider_new(reply);
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	response = ai_decider_decide(AI_DECIDER(mock), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpfloat(ai_decision_response_get_probability(response, "spam", NULL), ==, 0.93);
	g_clear_object(&response);
	g_cancellable_cancel(cancel);
	response = ai_decider_decide(AI_DECIDER(mock), request, cancel, &error);
	g_assert_null(response);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_choices_and_scores(void)
{
	const gchar *labels[] = { "billing", "other", NULL };
	const gchar *descriptions[] = { "Invoices", "Everything else", NULL };
	const gchar *levels[] = { "low", "medium", "high", NULL };
	const gchar *json = "{\"model\":\"test\",\"answers\":{"
		"\"route\":{\"type\":\"choice\",\"choice\":\"billing\",\"probabilities\":{\"billing\":0.8234,\"other\":0.1766}},"
		"\"urgency\":{\"type\":\"score\",\"score\":1.5,\"probabilities\":{\"0\":0.1,\"1\":0.3,\"2\":0.6}}}}";
	g_autoptr(AiDecisionRequest) request = ai_decision_request_new("Refund please");
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_assert_true(ai_decision_request_add_choice(request, "route", "Which team?", labels, descriptions, &error));
	g_assert_true(ai_decision_request_add_score(request, "urgency", "How urgent?", levels, &error));
	response = ai_decision_response_new_from_json(request, json, 2.5, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(ai_decision_response_get_choice(response, "route"), ==, "billing");
	g_assert_cmpfloat(ai_decision_response_get_probability(response, "route", "billing"), ==, 0.8234);
	g_assert_cmpfloat(ai_decision_response_get_score(response, "urgency"), ==, 1.5);
	g_assert_cmpfloat(ai_decision_response_get_probability(response, "urgency", "2"), ==, 0.6);
	text = ai_decision_response_format(response);
	g_assert_nonnull(strstr(text, "billing: 0.8234"));
}

static void
test_invalid_request(void)
{
	const gchar *bad[] = { "null", "{}", "{\"state\":1,\"questions\":{}}",
		"{\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"choice\",\"instructions\":\"x\",\"criteria\":[]}}}", NULL };
	guint i;
	for (i = 0; bad[i] != NULL; i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AiDecisionRequest) request = ai_decision_request_new_from_json(bad[i], &error);
		g_assert_null(request);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	}
	{
		g_autoptr(AiDecisionRequest) request = request_new();
		g_autoptr(GError) error = NULL;
		g_assert_false(ai_decision_request_add_boolean(request, "spam", "duplicate", &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	}
}

typedef struct { GMainLoop *loop; AiDecisionResponse *response; GError *error; } AsyncResult;
static void
completed(GObject *source, GAsyncResult *result, gpointer user_data)
{
	AsyncResult *data = user_data;
	data->response = ai_decider_decide_finish(AI_DECIDER(source), result, &data->error);
	g_main_loop_quit(data->loop);
}
static gboolean cancel_later(gpointer data)
{
	g_cancellable_cancel(data);
	return G_SOURCE_REMOVE;
}
static void
test_async_snapshot(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	AsyncResult data = { loop, NULL, NULL };
	tserver_set_response(server, 200, reply);
	g_object_set(client, "base-url", server->base_url, NULL);
	ai_decider_decide_async(AI_DECIDER(client), request, NULL, completed, &data);
	g_assert_true(ai_decision_request_add_boolean(request, "later", "Added after dispatch", NULL));
	g_clear_object(&request);
	g_clear_object(&client);
	g_main_loop_run(loop);
	g_assert_no_error(data.error);
	g_assert_nonnull(data.response);
	g_assert_null(strstr(server->last_body, "later"));
	g_object_unref(data.response);
	tserver_free(server);
}
static void
test_cancel_in_flight(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	AsyncResult data = { loop, NULL, NULL };
	tserver_set_response(server, 200, reply);
	tserver_set_delay(server, 150);
	g_object_set(client, "base-url", server->base_url, NULL);
	ai_decider_decide_async(AI_DECIDER(client), request, cancel, completed, &data);
	g_timeout_add(20, cancel_later, cancel);
	g_main_loop_run(loop);
	g_assert_null(data.response);
	g_assert_error(data.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&data.error);
	tserver_free(server);
}

static void
test_http_error(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	tserver_set_response(server, 401, "{\"detail\":\"unauthorized\"}");
	g_object_set(client, "base-url", server->base_url, NULL);
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_assert_cmpuint(server->hits, ==, 1);
	tserver_free(server);
}

static void
test_configuration_and_redirect(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiLayaClient) client = ai_laya_client_new();
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(client, "base-url", server->base_url, "model", "typo", NULL);
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_MODEL_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpuint(server->hits, ==, 0);
	g_object_set(client, "model", NULL, "base-url", "file:///tmp/no", NULL);
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
	g_object_set(client, "base-url", server->base_url, NULL);
	server->redirect_to = g_strconcat(server->base_url, "/redirect-target", NULL);
	tserver_set_response(server, 200, reply);
	response = ai_decider_decide(AI_DECIDER(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_nonnull(error);
	g_assert_cmpuint(server->hits, ==, 1);
	tserver_free(server);
}
static void
test_distribution_rejection(void)
{
	const gchar *bad[] = {
		"{\"type\":\"choice\",\"choice\":\"missing\",\"probabilities\":{\"a\":0.5,\"b\":0.5}}",
		"{\"type\":\"choice\",\"choice\":\"a\",\"probabilities\":{\"a\":0.5}}",
		"{\"type\":\"choice\",\"choice\":\"a\",\"probabilities\":{\"a\":0.9,\"b\":0.9}}",
		"{\"type\":\"choice\",\"choice\":\"a\",\"probabilities\":{\"a\":true,\"b\":0.5}}",
		"{\"type\":\"noul\",\"noul\":0.5}", NULL };
	const gchar *labels[] = { "a", "b", NULL };
	g_autoptr(AiDecisionRequest) request = ai_decision_request_new("x");
	guint i;
	g_assert_true(ai_decision_request_add_choice(request, "q", "Choose", labels, labels, NULL));
	for (i = 0; bad[i] != NULL; i++)
	{
		g_autofree gchar *json = g_strdup_printf("{\"model\":\"test\",\"answers\":{\"q\":%s}}", bad[i]);
		g_autoptr(GError) error = NULL;
		g_autoptr(AiDecisionResponse) response = ai_decision_response_new_from_json(request, json, 0, &error);
		g_assert_null(response);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	}
}

typedef struct { GObject parent_instance; } EmptyDecider;
typedef struct { GObjectClass parent_class; } EmptyDeciderClass;
static void empty_interface_init(AiDeciderInterface *iface) { (void)iface; }
G_DEFINE_TYPE_WITH_CODE(EmptyDecider, empty_decider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_DECIDER, empty_interface_init))
static void empty_decider_class_init(EmptyDeciderClass *klass) { (void)klass; }
static void empty_decider_init(EmptyDecider *self) { (void)self; }
static void
test_interface_and_mock_errors(void)
{
	g_autoptr(GObject) empty = g_object_new(empty_decider_get_type(), NULL);
	g_autoptr(AiDecisionRequest) request = request_new();
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(AiMockDecider) mock = ai_mock_decider_new(reply);
	g_autoptr(GError) injected = g_error_new_literal(AI_ERROR, AI_ERROR_SERVICE_UNAVAILABLE, "fixture unavailable");
	g_autoptr(GError) error = NULL;
	response = ai_decider_decide(AI_DECIDER(empty), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
	g_clear_error(&error);
	ai_mock_decider_set_error(mock, injected);
	g_clear_error(&injected);
	response = ai_decider_decide(AI_DECIDER(mock), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_SERVICE_UNAVAILABLE);
	g_clear_error(&error);
	ai_mock_decider_set_error(mock, NULL);
	response = ai_decider_decide(AI_DECIDER(mock), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/decision/wire", test_wire);
	g_test_add_func("/decision/interface-mock-errors", test_interface_and_mock_errors);
	g_test_add_func("/decision/configuration-redirect", test_configuration_and_redirect);
	g_test_add_func("/decision/distribution-rejection", test_distribution_rejection);
	g_test_add_func("/decision/choices-scores", test_choices_and_scores);
	g_test_add_func("/decision/invalid-request", test_invalid_request);
	g_test_add_func("/decision/async-snapshot", test_async_snapshot);
	g_test_add_func("/decision/cancel-in-flight", test_cancel_in_flight);
	g_test_add_func("/decision/http-error", test_http_error);
	g_test_add_func("/decision/malformed", test_malformed);
	g_test_add_func("/decision/timeout", test_timeout);
	g_test_add_func("/decision/service-down", test_service_down);
	g_test_add_func("/decision/mock", test_mock);
	return g_test_run();
}
