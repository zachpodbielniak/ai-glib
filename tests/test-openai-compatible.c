/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-glib.h"
#include "test-server.h"

typedef enum { CHAT, STREAM, MODELS, IMAGE, EMBED, N_OPERATIONS } Operation;
static const gchar *names[] = { "chat", "stream", "models", "image", "embed" };
static const gchar *paths[] = {
	"/chat/completions", "/chat/completions", "/models", "/images/generations", "/embeddings"
};
static const gchar *bodies[] = {
	"{\"id\":\"c1\",\"model\":\"custom\",\"choices\":[{\"message\":{\"content\":\"hello\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2}}",
	": keepalive\r\ndata:{\"id\":\"c1\",\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\r\n\r\ndata: [DONE]\r\n\r\n",
	"{\"data\":[{\"id\":\"custom\"},null,7,{\"id\":7},{\"id\":\"embed\"}]}",
	"{\"created\":1,\"data\":[{\"b64_json\":\"aGVsbG8=\"}]}",
	"{\"model\":\"embed\",\"data\":[{\"index\":0,\"embedding\":[3,4]}]}"
};

typedef struct
{
	GMainLoop *loop;
	Operation operation;
	gpointer result;
	GError *error;
	guint calls;
} Result;

static void
done(GObject *source, GAsyncResult *async_result, gpointer user_data)
{
	Result *result = user_data;

	result->calls++;
	switch (result->operation)
	{
	case CHAT: result->result = ai_provider_chat_finish(AI_PROVIDER(source), async_result, &result->error); break;
	case STREAM: result->result = ai_streamable_chat_stream_finish(AI_STREAMABLE(source), async_result, &result->error); break;
	case MODELS: result->result = ai_provider_list_models_finish(AI_PROVIDER(source), async_result, &result->error); break;
	case IMAGE: result->result = ai_image_generator_generate_image_finish(AI_IMAGE_GENERATOR(source), async_result, &result->error); break;
	case EMBED: result->result = ai_embedder_embed_finish(AI_EMBEDDER(source), async_result, &result->error); break;
	default: g_assert_not_reached();
	}
	g_main_loop_quit(result->loop);
}

static void
clear_result(Result *result)
{
	if (result->operation == MODELS)
		g_list_free_full(result->result, g_free);
	else if (result->operation == IMAGE)
		g_clear_pointer(&result->result, ai_image_response_free);
	else if (result->operation == EMBED)
		g_clear_pointer(&result->result, ai_embedding_unref);
	else
		g_clear_object(&result->result);
	g_clear_error(&result->error);
}

static Result
run(gpointer client, Operation operation, GCancellable *cancellable)
{
	Result result = { NULL, operation, NULL, NULL, 0 };
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(AiMessage) message = ai_message_new_user("hello");
	g_autoptr(AiImageRequest) request = ai_image_request_new("hello");
	GList messages = { message, NULL, NULL };
	const gchar *texts[] = { "hello", NULL };

	result.loop = loop;
	switch (operation)
	{
	case CHAT: ai_provider_chat_async(AI_PROVIDER(client), &messages, "system", 25, NULL, cancellable, done, &result); break;
	case STREAM: ai_streamable_chat_stream_async(AI_STREAMABLE(client), &messages, "system", 25, NULL, cancellable, done, &result); break;
	case MODELS: ai_provider_list_models_async(AI_PROVIDER(client), cancellable, done, &result); break;
	case IMAGE: ai_image_generator_generate_image_async(AI_IMAGE_GENERATOR(client), request, cancellable, done, &result); break;
	case EMBED: ai_embedder_embed_async(AI_EMBEDDER(client), texts, NULL, cancellable, done, &result); break;
	default: g_assert_not_reached();
	}
	g_main_loop_run(loop);
	g_assert_cmpuint(result.calls, ==, 1);
	result.loop = NULL;
	return result;
}

static AiOpenAICompatibleClient *
client_new(const gchar *url)
{
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(AiOpenAICompatibleClient) client = NULL;

	ai_config_set_max_retries(config, 0);
	client = g_object_new(AI_TYPE_OPENAI_COMPATIBLE_CLIENT,
		"config", config, "base-url", url, "api-key", "private-token",
		"model", "custom", "image-model", "image", "embedding-model", "embed", NULL);
	return g_steal_pointer(&client);
}

static void
respond(TServer *server, Operation operation)
{
	tserver_set_response_full(server, 200,
		operation == STREAM ? "text/event-stream" : "application/json", bodies[operation]);
}

static void
test_routes(gconstpointer data)
{
	Operation operation = GPOINTER_TO_INT(data);
	const gchar *roots[] = { "", "/", "/v1", "/v1/", "/gateway/v1/", "/custom/v2" };
	const gchar *expected[] = { "/v1", "/v1", "/v1", "/v1", "/gateway/v1", "/custom/v2" };
	TServer *server = tserver_new();
	guint i;

	respond(server, operation);
	for (i = 0; i < G_N_ELEMENTS(roots); i++)
	{
		g_autofree gchar *url = g_strconcat(server->base_url, roots[i], NULL);
		g_autofree gchar *expected_path = g_strconcat(expected[i], paths[operation], NULL);
		g_autoptr(AiOpenAICompatibleClient) client = client_new(url);
		g_autofree gchar *path = NULL;
		g_autofree gchar *auth = NULL;
		Result result = run(client, operation, NULL);

		g_assert_no_error(result.error);
		g_assert_nonnull(result.result);
		path = tserver_dup_last_path(server);
		auth = tserver_dup_header(server, "Authorization");
		g_assert_cmpstr(path, ==, expected_path);
		g_assert_cmpstr(auth, ==, "Bearer private-token");
		if (operation == CHAT || operation == STREAM)
		{
			g_autofree gchar *text = ai_response_get_text(result.result);
			g_autoptr(JsonParser) parser = NULL;
			JsonObject *body = tserver_last_json(server, &parser);
			g_assert_cmpstr(text, ==, "hello");
			g_assert_cmpstr(json_object_get_string_member(body, "model"), ==, "custom");
			g_assert_cmpint(json_object_get_int_member(body, "max_tokens"), ==, 25);
			g_assert_cmpuint(json_array_get_length(json_object_get_array_member(body, "messages")), ==, 2);
			if (operation == STREAM)
				g_assert_true(json_object_get_boolean_member(body, "stream"));
		}
		if (operation == MODELS)
		{
			g_assert_cmpuint(g_list_length(result.result), ==, 2);
			g_assert_cmpstr(((GList *)result.result)->data, ==, "custom");
		}
		if (operation == EMBED)
		{
			g_assert_cmpuint(ai_embedding_get_dimensions(result.result), ==, 2);
			g_assert_cmpfloat_with_epsilon(ai_embedding_get_vector(result.result, 0)[0], 0.6, 0.00001);
		}
		clear_result(&result);
	}
	tserver_free(server);
}

static void
test_invalid(gconstpointer data)
{
	Operation operation = GPOINTER_TO_INT(data);
	const gchar *urls[] = { "", "not a URL", "ftp://host/v1", "http://", "http://u:secret@host/v1", "http://host/v1?token=secret", "http://host/v1#fragment", "http://host/\n", "http://[invalid" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(urls); i++)
	{
		g_autoptr(AiOpenAICompatibleClient) client = client_new(urls[i]);
		Result result = run(client, operation, NULL);
		g_assert_null(result.result);
		g_assert_error(result.error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_null(strstr(result.error->message, "secret"));
		clear_result(&result);
	}
	{
		g_autoptr(AiOpenAICompatibleClient) client = client_new("http://127.0.0.1:1/v1");
		Result result;
		g_object_set(client, "api-key", "secret\r\nX-Injected: yes", NULL);
		result = run(client, operation, NULL);
		g_assert_error(result.error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		clear_result(&result);
	}
}

static void
test_errors(gconstpointer data)
{
	Operation operation = GPOINTER_TO_INT(data);
	const guint statuses[] = { 400, 401, 403, 404, 429, 500, 503 };
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(statuses); i++)
	{
		Result result;
		tserver_set_response(server, statuses[i], "{\"error\":{\"message\":\"deployment failed\"}}");
		result = run(client, operation, NULL);
		g_assert_null(result.result);
		g_assert_nonnull(result.error);
		g_assert_cmpuint(result.error->domain, ==, AI_ERROR);
		g_assert_nonnull(strstr(result.error->message, "deployment failed"));
		if (statuses[i] == 401)
			g_assert_error(result.error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
		if (statuses[i] == 429)
			g_assert_error(result.error, AI_ERROR, AI_ERROR_RATE_LIMITED);
		clear_result(&result);
	}
	{
		g_autoptr(GCancellable) cancellable = g_cancellable_new();
		Result result;
		g_cancellable_cancel(cancellable);
		result = run(client, operation, cancellable);
		g_assert_null(result.result);
		g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		clear_result(&result);
	}
	tserver_free(server);
}

static void
test_configuration(void)
{
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(AiOpenAICompatibleClient) client = NULL;
	g_autoptr(GObject) factory = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *key = NULL;

	ai_config_set_base_url(config, AI_PROVIDER_OPENAI, "https://openai.example");
	ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "must-not-leak");
	/* Explicit empty settings mask the environment. */
	ai_config_set_base_url(config, AI_PROVIDER_OPENAI_COMPATIBLE, "");
	ai_config_set_api_key(config, AI_PROVIDER_OPENAI_COMPATIBLE, "");
	client = ai_openai_compatible_client_new_with_config(config);
	g_object_get(client, "base-url", &url, "api-key", &key, NULL);
	g_assert_cmpstr(url, ==, "");
	g_assert_cmpstr(key, ==, "");
	g_assert_null(ai_client_get_model(AI_CLIENT(client)));
	g_assert_true(AI_IS_STREAMABLE(client));
	g_assert_true(AI_IS_EVENT_SOURCE(client));
	g_assert_true(AI_IS_IMAGE_GENERATOR(client));
	g_assert_true(AI_IS_EMBEDDER(client));
	g_assert_null(ai_image_generator_list_image_models(AI_IMAGE_GENERATOR(client)));
	g_assert_null(ai_embedder_list_embedding_models(AI_EMBEDDER(client)));
	ai_config_set_base_url(config, AI_PROVIDER_OPENAI_COMPATIBLE, "http://local/v1");
	g_clear_pointer(&url, g_free);
	g_object_get(client, "base-url", &url, NULL);
	g_assert_cmpstr(url, ==, "http://local/v1");
	factory = ai_provider_factory_new_from_string("http", config, &error);
	g_assert_no_error(error);
	g_assert_true(AI_IS_OPENAI_COMPATIBLE_CLIENT(factory));
	g_assert_cmpint(ai_provider_get_provider_type(AI_PROVIDER(factory)), ==, AI_PROVIDER_OPENAI_COMPATIBLE);
	g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_OPENAI_COMPATIBLE), ==, "openai-compatible");
}

static void
test_no_auth_and_missing_models(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	g_autofree gchar *auth = NULL;
	Result result;
	Operation operation;

	g_object_set(client, "api-key", "", NULL);
	respond(server, CHAT);
	result = run(client, CHAT, NULL);
	g_assert_no_error(result.error);
	clear_result(&result);
	auth = tserver_dup_header(server, "Authorization");
	g_assert_null(auth);
	g_object_set(client, "model", "", "image-model", "", "embedding-model", "", NULL);
	for (operation = CHAT; operation < N_OPERATIONS; operation++)
	{
		if (operation == MODELS)
			continue;
		result = run(client, operation, NULL);
		g_assert_error(result.error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		clear_result(&result);
	}
	g_assert_cmpuint(tserver_hits(server), ==, 1);
	tserver_free(server);
}

static void
test_image_capabilities(void)
{
	g_autoptr(AiImageModelInfo) a = ai_image_model_info_new("same", "A", AI_PROVIDER_OPENAI_COMPATIBLE, AI_IMAGE_CAP_PIXEL_SIZE);
	g_autoptr(AiImageModelInfo) b = ai_image_model_info_new("same", "B", AI_PROVIDER_OPENAI_COMPATIBLE, AI_IMAGE_CAP_NONE);
	g_autoptr(AiOpenAICompatibleClient) ca = g_object_new(AI_TYPE_OPENAI_COMPATIBLE_CLIENT, "image-model-info", a, NULL);
	g_autoptr(AiOpenAICompatibleClient) cb = g_object_new(AI_TYPE_OPENAI_COMPATIBLE_CLIENT, "image-model-info", b, NULL);

	g_assert_true(ai_image_generator_supports(AI_IMAGE_GENERATOR(ca), "same", AI_IMAGE_CAP_PIXEL_SIZE));
	g_assert_false(ai_image_generator_supports(AI_IMAGE_GENERATOR(cb), "same", AI_IMAGE_CAP_PIXEL_SIZE));
	g_assert_true(ai_image_generator_supports(AI_IMAGE_GENERATOR(ca), "same", AI_IMAGE_CAP_PIXEL_SIZE));
}

static void
test_malformed(gconstpointer data)
{
	Operation operation = GPOINTER_TO_INT(data);
	const gchar *documents[] = { "", "not json", "null", "[]", "{\"data\":null}", "{\"data\":7}", "{\"data\":[7]}", "{\"error\":{\"message\":[]}}" };
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(documents); i++)
	{
		Result result;
		tserver_set_response(server, 200, documents[i]);
		result = run(client, operation, NULL);
		g_assert_null(result.result);
		if (operation == MODELS && i == 6)
			g_assert_no_error(result.error);
		else
			g_assert_nonnull(result.error);
		clear_result(&result);
	}
	tserver_free(server);
}

static void
test_stream_error(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	Result result;

	tserver_set_response_full(server, 200, "text/event-stream",
		"data:{\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
		"data:{\"error\":{\"message\":\"stream failed\"}}\n\n");
	result = run(client, STREAM, NULL);
	g_assert_null(result.result);
	g_assert_error(result.error, AI_ERROR, AI_ERROR_SERVER_ERROR);
	g_assert_nonnull(strstr(result.error->message, "stream failed"));
	clear_result(&result);
	tserver_free(server);
}

static void
test_embedding_batches(void)
{
	const gchar *invalid[] = {
		"{\"data\":[{\"embedding\":[\"bad\",1]}]}",
		"{\"data\":[{\"embedding\":[null]}]}",
		"{\"data\":[{\"embedding\":[{}]}]}",
		"{\"data\":[{\"embedding\":[]}]}",
		"{\"data\":[{\"embedding\":[1e300]}]}",
		"{\"data\":[{\"index\":-1,\"embedding\":[1]}]}",
		"{\"data\":[{\"index\":0.5,\"embedding\":[1]}]}",
		"{\"data\":[]}",
		"{\"data\":[{\"embedding\":[1]},{\"embedding\":[1]}]}"
	};
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	g_autoptr(AiEmbedding) embedding = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *texts[] = { "first", "second", NULL };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(invalid); i++)
	{
		Result result;
		tserver_set_response(server, 200, invalid[i]);
		result = run(client, EMBED, NULL);
		g_assert_null(result.result);
		g_assert_error(result.error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
		clear_result(&result);
	}
	tserver_set_response(server, 200,
		"{\"data\":[{\"index\":1,\"embedding\":[0,1]},{\"index\":0,\"embedding\":[1,0]}]}");
	embedding = ai_embedder_embed(AI_EMBEDDER(client), texts, "deployment-embed", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(ai_embedding_get_n_vectors(embedding), ==, 2);
	g_assert_cmpstr(ai_embedding_get_model(embedding), ==, "deployment-embed");
	g_assert_cmpfloat(ai_embedding_get_vector(embedding, 0)[0], ==, 1);
	g_assert_cmpfloat(ai_embedding_get_vector(embedding, 1)[1], ==, 1);
	g_clear_pointer(&embedding, ai_embedding_unref);
	tserver_set_response(server, 200,
		"{\"data\":[{\"index\":0,\"embedding\":[1,0]},{\"index\":0,\"embedding\":[0,1]}]}");
	embedding = ai_embedder_embed(AI_EMBEDDER(client), texts, "embed", NULL, &error);
	g_assert_null(embedding);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	tserver_free(server);
}

static void
test_image_operations(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	g_autoptr(AiImageRequest) request = ai_image_request_new("edit this");
	g_autoptr(AiImage) image = ai_image_new_from_data((const guint8 *)"image-bytes", 11, "image/png");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *content_type = NULL;

	respond(server, IMAGE);
	ai_image_request_set_model(request, "custom-edit");
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_EDIT);
	ai_image_request_add_reference_image(request, image);
	ai_image_request_set_mask(request, image);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(ai_image_response_get_image_count(response), ==, 1);
	path = tserver_dup_last_path(server);
	body = tserver_dup_last_body(server);
	content_type = tserver_dup_header(server, "Content-Type");
	g_assert_cmpstr(path, ==, "/v1/images/edits");
	g_assert_true(g_str_has_prefix(content_type, "multipart/form-data;"));
	g_assert_nonnull(strstr(body, "custom-edit"));
	g_assert_nonnull(strstr(body, "image-bytes"));
	g_assert_nonnull(strstr(body, "name=\"mask\""));
	g_clear_pointer(&response, ai_image_response_free);
	g_clear_pointer(&path, g_free);
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_VARIATION);
	ai_image_request_set_mask(request, NULL);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client), request, NULL, &error);
	g_assert_no_error(error);
	path = tserver_dup_last_path(server);
	g_assert_cmpstr(path, ==, "/v1/images/variations");
	tserver_free(server);
}

static void
test_retry(gconstpointer data)
{
	Operation operation = GPOINTER_TO_INT(data);
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	g_autofree gchar *auth = NULL;
	Result result;

	ai_config_set_max_retries(ai_client_get_config(AI_CLIENT(client)), 1);
	respond(server, operation);
	tserver_set_failures(server, 1, 503);
	result = run(client, operation, NULL);
	g_assert_no_error(result.error);
	g_assert_nonnull(result.result);
	g_assert_cmpuint(tserver_hits(server), ==, 2);
	auth = tserver_dup_header(server, "Authorization");
	g_assert_cmpstr(auth, ==, "Bearer private-token");
	clear_result(&result);
	tserver_free(server);
}

static void
test_tools_and_vision(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	g_autoptr(AiMessage) user = ai_message_new_user("describe");
	g_autoptr(AiMessage) assistant = ai_message_new_assistant("");
	g_autoptr(AiToolUse) call = ai_tool_use_new_from_json_string("call1", "lookup", "{\"query\":\"a\"}");
	g_autoptr(AiMessage) answer = ai_message_new_tool_result("call1", "found", FALSE);
	g_autoptr(AiTool) tool = ai_tool_new("lookup", "Look up a value");
	g_autoptr(AiImage) image = ai_image_new_from_data((const guint8 *)"image", 5, "image/png");
	g_autoptr(AiImageContent) content = ai_image_content_new(image);
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	GList tools = { tool, NULL, NULL };
	g_autolist(AiMessage) messages = NULL;
	Result result = { loop, CHAT, NULL, NULL, 0 };
	JsonObject *body;
	JsonArray *wire_messages;

	ai_message_add_content_block(user, AI_CONTENT_BLOCK(g_steal_pointer(&content)));
	ai_message_add_content_block(assistant, AI_CONTENT_BLOCK(g_steal_pointer(&call)));
	messages = g_list_append(messages, g_steal_pointer(&user));
	messages = g_list_append(messages, g_steal_pointer(&assistant));
	messages = g_list_append(messages, g_steal_pointer(&answer));
	tserver_set_response(server, 200,
		"{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call2\",\"function\":{\"name\":\"lookup\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}");
	ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 42, &tools, NULL, done, &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	g_assert_cmpint(ai_response_get_stop_reason(result.result), ==, AI_STOP_REASON_TOOL_USE);
	body = tserver_last_json(server, &parser);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(body, "tools")), ==, 1);
	wire_messages = json_object_get_array_member(body, "messages");
	g_assert_cmpuint(json_array_get_length(wire_messages), ==, 3);
	g_assert_nonnull(json_object_get_array_member(json_array_get_object_element(wire_messages, 0), "content"));
	g_assert_nonnull(json_object_get_array_member(json_array_get_object_element(wire_messages, 1), "tool_calls"));
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(wire_messages, 2), "tool_call_id"), ==, "call1");
	clear_result(&result);
	tserver_free(server);
}

static gchar *ai_binary;

static void
test_cli_environment(void)
{
	TServer *server = tserver_new();
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *auth = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *url = g_strconcat(server->base_url, "/gateway/v1/", NULL);
	const gchar *args[] = { ai_binary, "-p", "http", "-m", "custom", "hello", NULL };

	respond(server, CHAT);
	g_subprocess_launcher_setenv(launcher, "OPENAI_COMPATIBLE_BASE_URL", url, TRUE);
	g_subprocess_launcher_setenv(launcher, "OPENAI_COMPATIBLE_API_KEY", "env-token", TRUE);
	g_subprocess_launcher_setenv(launcher, "OPENAI_API_KEY", "must-not-leak", TRUE);
	g_subprocess_launcher_setenv(launcher, "OPENAI_BASE_URL", "https://invalid.example", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &out, &err, &error));
	g_assert_no_error(error);
	g_test_message("CLI stderr: %s", err);
	g_assert_true(g_subprocess_get_successful(process));
	g_assert_nonnull(strstr(out, "hello"));
	auth = tserver_dup_header(server, "Authorization");
	path = tserver_dup_last_path(server);
	g_assert_cmpstr(auth, ==, "Bearer env-token");
	g_assert_cmpstr(path, ==, "/gateway/v1/chat/completions");
	tserver_free(server);
}

static gboolean
cancel_request(gpointer data)
{
	g_cancellable_cancel(data);
	return G_SOURCE_REMOVE;
}

static void
test_inflight_cancel(gconstpointer data)
{
	Operation operation = GPOINTER_TO_INT(data);
	TServer *server = tserver_new();
	g_autoptr(AiOpenAICompatibleClient) client = client_new(server->base_url);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GSource) timer = g_timeout_source_new(30);
	Result result;

	respond(server, operation);
	tserver_set_delay(server, 100);
	g_source_set_callback(timer, cancel_request, cancellable, NULL);
	g_source_attach(timer, NULL);
	result = run(client, operation, cancellable);
	g_source_destroy(timer);
	g_assert_null(result.result);
	g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	clear_result(&result);
	tserver_free(server);
}

int
main(int argc, char **argv)
{
	Operation operation;
	g_autofree gchar *dir = g_path_get_dirname(argv[0]);
	gint status;
	g_test_init(&argc, &argv, NULL);
	ai_binary = g_build_filename(dir, "..", "bin", "ai", NULL);
	for (operation = CHAT; operation < N_OPERATIONS; operation++)
	{
		g_autofree gchar *route = g_strdup_printf("/compatible/routes/%s", names[operation]);
		g_autofree gchar *invalid = g_strdup_printf("/compatible/invalid/%s", names[operation]);
		g_autofree gchar *errors = g_strdup_printf("/compatible/errors/%s", names[operation]);
		g_autofree gchar *cancel = g_strdup_printf("/compatible/cancel/%s", names[operation]);
		g_test_add_data_func(cancel, GINT_TO_POINTER(operation), test_inflight_cancel);
		g_test_add_data_func(route, GINT_TO_POINTER(operation), test_routes);
		g_test_add_data_func(invalid, GINT_TO_POINTER(operation), test_invalid);
		g_test_add_data_func(errors, GINT_TO_POINTER(operation), test_errors);
	}
	g_test_add_data_func("/compatible/malformed/models", GINT_TO_POINTER(MODELS), test_malformed);
	g_test_add_data_func("/compatible/malformed/image", GINT_TO_POINTER(IMAGE), test_malformed);
	g_test_add_data_func("/compatible/malformed/embed", GINT_TO_POINTER(EMBED), test_malformed);
	g_test_add_func("/compatible/stream-error", test_stream_error);
	g_test_add_func("/compatible/embedding-batches", test_embedding_batches);
	g_test_add_func("/compatible/image-operations", test_image_operations);
	g_test_add_func("/compatible/tools-vision", test_tools_and_vision);
	g_test_add_data_func("/compatible/retry/image", GINT_TO_POINTER(IMAGE), test_retry);
	g_test_add_data_func("/compatible/retry/embed", GINT_TO_POINTER(EMBED), test_retry);
	g_test_add_func("/compatible/configuration", test_configuration);
	g_test_add_func("/compatible/no-auth-missing-models", test_no_auth_and_missing_models);
	g_test_add_func("/compatible/image-capabilities", test_image_capabilities);
	g_test_add_func("/compatible/cli-environment", test_cli_environment);
	status = g_test_run();
	g_free(ai_binary);
	return status;
}
