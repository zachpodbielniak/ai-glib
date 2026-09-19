/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <string.h>

static gchar *sandbox = NULL;

static void
changed(GObject *object, GParamSpec *spec, gpointer data)
{
	guint *count = data;
	(*count)++;
}

static void
batch(void)
{
	g_autoptr(AiPromptQueue) queue = ai_prompt_queue_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	guint notifications = 0;
	g_signal_connect(queue, "notify::length", G_CALLBACK(changed), &notifications);
	g_assert_true(ai_prompt_queue_push(queue, "one", NULL, FALSE, &error));
	g_assert_true(ai_prompt_queue_push(queue, "two", NULL, FALSE, &error));
	g_assert_true(ai_prompt_queue_push(queue, "three", NULL, FALSE, &error));
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 3);
	g_assert_cmpstr(ai_prompt_queue_peek(queue, NULL), ==, "one");
	g_assert_cmpuint(notifications, ==, 3);
	text = ai_prompt_queue_pop(queue, NULL);
	g_assert_cmpstr(text, ==, "one\n\ntwo\n\nthree");
	g_assert_cmpuint(notifications, ==, 4);
	g_assert_null(ai_prompt_queue_pop(queue, NULL));
	g_assert_null(ai_prompt_queue_peek(queue, NULL));
	g_assert_no_error(error);
}

static void
barriers(void)
{
	g_autoptr(AiPromptQueue) queue = ai_prompt_queue_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_assert_true(ai_prompt_queue_push(queue, "one", NULL, FALSE, NULL));
	g_assert_true(ai_prompt_queue_push(queue, "/model next", NULL, TRUE, NULL));
	g_assert_true(ai_prompt_queue_push(queue, "two", NULL, FALSE, NULL));
	text = ai_prompt_queue_pop(queue, NULL);
	g_assert_cmpstr(text, ==, "one");
	g_clear_pointer(&text, g_free);
	text = ai_prompt_queue_pop(queue, NULL);
	g_assert_cmpstr(text, ==, "/model next");
	g_clear_pointer(&text, g_free);
	text = ai_prompt_queue_pop(queue, NULL);
	g_assert_cmpstr(text, ==, "two");
	g_object_set(queue, "coalesce", FALSE, "max-length", 1, NULL);
	g_assert_true(ai_prompt_queue_push(queue, "kept", NULL, FALSE, NULL));
	g_assert_false(ai_prompt_queue_push(queue, "rejected", NULL, FALSE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	ai_prompt_queue_clear(queue);
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 0);
	g_assert_false(ai_prompt_queue_push(queue, " \t\n", NULL, FALSE, NULL));
}

static void
validation(void)
{
	g_autoptr(AiPromptQueue) queue = ai_prompt_queue_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *too_long = g_malloc(1024 * 1024 + 2);
	GList *not_an_image = g_list_append(NULL, queue);

	memset(too_long, 'a', 1024 * 1024 + 1);
	too_long[1024 * 1024 + 1] = '\0';
	g_assert_false(ai_prompt_queue_push(queue, NULL, NULL, FALSE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	g_assert_false(ai_prompt_queue_push(queue, "\xFF\xFE not utf-8", NULL, FALSE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	g_assert_false(ai_prompt_queue_push(queue, too_long, NULL, FALSE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	g_assert_false(ai_prompt_queue_push(queue, "picture", not_an_image, FALSE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 0);
	g_list_free(not_an_image);
}

static void
separate(void)
{
	g_autoptr(AiPromptQueue) queue = ai_prompt_queue_new();
	g_autofree gchar *text = NULL;
	g_object_set(queue, "coalesce", FALSE, NULL);
	ai_prompt_queue_push(queue, "one", NULL, FALSE, NULL);
	ai_prompt_queue_push(queue, "two", NULL, FALSE, NULL);
	text = ai_prompt_queue_pop(queue, NULL);
	g_assert_cmpstr(text, ==, "one");
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 1);
}

static void
images(void)
{
	g_autoptr(AiPromptQueue) queue = ai_prompt_queue_new();
	g_autoptr(GBytes) bytes = g_bytes_new_static("image", 5);
	g_autoptr(AiImageContent) image = ai_image_content_new_from_bytes(bytes, "image/png");
	g_autofree gchar *text = NULL;
	GList *input = g_list_append(NULL, image), *output = NULL;
	ai_prompt_queue_push(queue, "before", NULL, FALSE, NULL);
	ai_prompt_queue_push(queue, "picture", input, FALSE, NULL);
	ai_prompt_queue_push(queue, "after", NULL, FALSE, NULL);
	g_list_free(input);
	g_clear_object(&image);
	text = ai_prompt_queue_pop(queue, &output);
	g_assert_cmpstr(text, ==, "before");
	g_assert_null(output);
	g_clear_pointer(&text, g_free);
	g_assert_cmpstr(ai_prompt_queue_peek(queue, &output), ==, "picture");
	g_assert_cmpuint(g_list_length(output), ==, 1);
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 2);
	text = ai_prompt_queue_pop(queue, &output);
	g_assert_cmpstr(text, ==, "picture");
	g_assert_cmpuint(g_list_length(output), ==, 1);
	g_assert_true(g_bytes_equal(bytes, ai_image_get_bytes(ai_image_content_get_image(output->data))));
	g_list_free_full(output, g_object_unref);
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 1);
}

typedef struct { gboolean done; GError *error; } Result;
static void
sent(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *out = data;
	ai_conversation_send_finish(AI_CONVERSATION(source), result, &out->error);
	out->done = TRUE;
}

static void
wait_result(Result *result)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
	while (!result->done && g_get_monotonic_time() < deadline) {
		while (g_main_context_iteration(NULL, FALSE));
		g_usleep(1000);
	}
	g_assert_true(result->done);
}

static void
fork_context(void)
{
	g_autoptr(AiMockProvider) parent_provider = ai_mock_provider_new();
	g_autoptr(AiMockProvider) side_provider = ai_mock_provider_new();
	g_autoptr(AiConversation) parent = ai_conversation_new(G_OBJECT(parent_provider));
	g_autoptr(AiConversation) side = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	Result first = {0}, active = {0}, question = {0};
	GList *messages;
	ai_conversation_set_local_tools(parent, FALSE);
	ai_conversation_set_system_prompt(parent, "identity");
	ai_mock_provider_push_text(parent_provider, "first answer");
	ai_conversation_send_async(parent, "first question", NULL, sent, &first);
	wait_result(&first);
	g_assert_no_error(first.error);
	ai_mock_provider_set_delay_ms(parent_provider, 500);
	ai_conversation_send_async(parent, "unfinished work", NULL, sent, &active);
	side = ai_conversation_fork(parent, G_OBJECT(side_provider), &error);
	g_assert_no_error(error);
	g_assert_nonnull(side);
	g_assert_cmpuint(g_list_length(ai_conversation_get_messages(side)), ==, 2);
	ai_message_add_text(ai_conversation_get_messages(parent)->data, "parent mutation");
	g_assert_true(ai_conversation_get_executor(side) != ai_conversation_get_executor(parent));
	g_assert_null(ai_conversation_get_brigade(side));
	ai_mock_provider_push_text(side_provider, "side answer");
	ai_conversation_send_async(side, "side question", NULL, sent, &question);
	wait_result(&question);
	g_assert_no_error(question.error);
	g_assert_true(ai_conversation_get_busy(parent));
	g_assert_cmpstr(ai_mock_provider_get_last_system_prompt(side_provider), ==, "identity");
	messages = ai_mock_provider_get_last_messages(side_provider);
	g_assert_cmpuint(g_list_length(messages), ==, 3);
	text = ai_message_get_text(messages->data);
	g_assert_cmpstr(text, ==, "first question");
	g_assert_cmpuint(g_list_length(ai_conversation_get_messages(parent)), ==, 3);
	ai_conversation_cancel(parent);
	wait_result(&active);
	g_clear_error(&active.error);
	g_assert_cmpuint(g_list_length(ai_conversation_get_messages(side)), ==, 4);
}

static void
fork_reject_shared(void)
{
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(AiConversation) parent = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(GError) error = NULL;
	g_assert_null(ai_conversation_fork(parent, G_OBJECT(provider), &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
fork_cli_session(void)
{
	g_autoptr(AiGrokBuildClient) provider = ai_grok_build_client_new();
	g_autoptr(AiGrokBuildClient) other = ai_grok_build_client_new();
	g_autoptr(AiConversation) parent = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(AiConversation) side = NULL;
	gboolean resume = TRUE;
	g_object_set(provider, "session-id", "parent-session", "continue-session", TRUE, NULL);
	g_object_set(other, "session-id", "parent-session", "continue-session", TRUE, NULL);
	side = ai_conversation_fork(parent, G_OBJECT(other), NULL);
	g_assert_nonnull(side);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(provider)), ==, "parent-session");
	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(other)));
	g_object_get(other, "continue-session", &resume, NULL);
	g_assert_false(resume);
}

static void
fork_cancel_side(void)
{
	g_autoptr(AiMockProvider) main_provider = ai_mock_provider_new();
	g_autoptr(AiMockProvider) other = ai_mock_provider_new();
	g_autoptr(AiConversation) parent = ai_conversation_new(G_OBJECT(main_provider));
	g_autoptr(AiConversation) side = ai_conversation_fork(parent, G_OBJECT(other), NULL);
	Result main_result = {0}, side_result = {0};
	ai_conversation_set_local_tools(parent, FALSE);
	ai_mock_provider_set_delay_ms(main_provider, 150);
	ai_mock_provider_set_delay_ms(other, 500);
	ai_conversation_send_async(parent, "work", NULL, sent, &main_result);
	ai_conversation_send_async(side, "aside", NULL, sent, &side_result);
	ai_conversation_cancel(side);
	wait_result(&main_result);
	wait_result(&side_result);
	g_assert_no_error(main_result.error);
	g_assert_nonnull(side_result.error);
	g_clear_error(&side_result.error);
	g_assert_cmpuint(ai_mock_provider_get_call_count(main_provider), ==, 1);
}

int main(int argc, char **argv)
{
	sandbox = g_dir_make_tmp("ai-prompt-queue-XXXXXX", NULL);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_setenv("XDG_STATE_HOME", sandbox, TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/queue/batch", batch);
	g_test_add_func("/queue/images", images);
	g_test_add_func("/queue/validation", validation);
	g_test_add_func("/queue/fork-cli-session", fork_cli_session);
	g_test_add_func("/queue/fork-cancel-side", fork_cancel_side);
	g_test_add_func("/queue/barriers-limit-validation", barriers);
	g_test_add_func("/queue/separate", separate);
	g_test_add_func("/queue/fork-context", fork_context);
	g_test_add_func("/queue/fork-reject-shared", fork_reject_shared);
	return g_test_run();
}
