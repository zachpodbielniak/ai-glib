/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>

static gchar *sandbox;
static gboolean done;
static GError *send_error;
static void
sent(GObject *source, GAsyncResult *result, gpointer data)
{
	(void)data;
	ai_conversation_send_finish(AI_CONVERSATION(source), result, &send_error);
	done = TRUE;
}

static void
test_provider_context(void)
{
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autofree gchar *text = NULL;
	GList *messages;

	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(conversation), "work-session"));
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/9", NULL));
	g_object_set(conversation, "work-session", work, "stream", FALSE, "local-tools", FALSE, NULL);
	done = FALSE;
	ai_conversation_send_async(conversation, "Check the linked issue", NULL, sent, NULL);
	while (!done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(send_error);
	messages = ai_mock_provider_get_last_messages(provider);
	g_assert_nonnull(messages);
	text = ai_message_get_text(g_list_last(messages)->data);
	g_assert_nonnull(strstr(text, "https://github.com/team/project/issues/9"));
	g_assert_nonnull(strstr(text, "Issue description from forge"));
	g_assert_nonnull(strstr(text, "Comment from maintainer"));
	g_assert_null(ai_mock_provider_get_last_system_prompt(provider));
}

static gchar *context_result;
static GError *context_error;
static void
context_ready(GObject *source, GAsyncResult *result, gpointer data)
{
	(void)data;
	context_result = ai_work_session_read_context_finish(AI_WORK_SESSION(source), result, &context_error);
	done = TRUE;
}
static gchar *
read_context(AiWorkSession *work)
{
	done = FALSE;
	ai_work_session_read_context_async(work, NULL, context_ready, NULL);
	while (!done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(context_error);
	return g_steal_pointer(&context_result);
}
static void
test_connectors_and_partial_failure(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autofree gchar *text = NULL;
	g_autofree gchar *manifest = NULL;
	g_assert_true(ai_work_session_add_link_full(work, "https://github.com/team/project/pull/9", "assigned", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://forge.test/other/repo/pulls/2", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://gitlab.test/group/sub/repo/-/merge_requests/3", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/private/issues/401", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/offline/issues/502", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://unknown.test/team/project/issues/9", NULL));
	manifest = ai_work_session_dup_link_manifest(work);
	g_assert_nonnull(strstr(manifest, "assigned"));
	g_assert_nonnull(strstr(manifest, "group/sub/repo"));
	text = read_context(work);
	g_assert_nonnull(strstr(text, "Issue description from forge"));
	g_assert_nonnull(strstr(text, "TEA_PULL_REQUEST"));
	g_assert_nonnull(strstr(text, "TEA_COMMENT"));
	g_assert_nonnull(strstr(text, "GITLAB_MERGE_REQUEST"));
	g_assert_nonnull(strstr(text, "GITLAB_COMMENT"));
	g_assert_nonnull(strstr(text, "authorization-failed"));
	g_assert_nonnull(strstr(text, "network-error"));
	g_assert_nonnull(strstr(text, "connector-unavailable"));
}
static void
test_resume_and_history(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autolist(AiWorkSession) saved = NULL;
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autofree gchar *directory = g_build_filename(sandbox, "registry", NULL);
	g_autofree gchar *text = NULL;
	guint i;
	g_assert_true(ai_work_session_add_link_full(work, "https://github.com/team/project/issues/9", "blocks", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://forge.test/other/repo/pulls/2", NULL));
	g_assert_true(ai_work_session_save(work, directory, FALSE, NULL));
	saved = ai_work_session_list(directory, NULL);
	g_assert_cmpuint(g_list_length(saved), ==, 1);
	g_assert_cmpstr(ai_work_session_get_link_relationship(saved->data,
		"https://github.com/team/project/issues/9"), ==, "blocks");
	g_object_set(conversation, "work-session", saved->data, "stream", FALSE, "local-tools", FALSE,
		"system-prompt", "Trusted system instructions", NULL);
	for (i = 0; i < 2; i++)
	{
		GList *messages;
		done = FALSE;
		ai_conversation_send_async(conversation, "Inspect the linked work", NULL, sent, NULL);
		while (!done) g_main_context_iteration(NULL, TRUE);
		g_assert_no_error(send_error);
		messages = ai_mock_provider_get_last_messages(provider);
		g_clear_pointer(&text, g_free);
		text = ai_message_get_text(g_list_last(messages)->data);
		g_assert_nonnull(strstr(text, "TEA_PULL_REQUEST"));
		g_assert_nonnull(strstr(text, "Issue description from forge"));
		g_assert_cmpstr(ai_mock_provider_get_last_system_prompt(provider), ==, "Trusted system instructions");
		g_clear_pointer(&text, g_free);
		text = ai_message_get_text(ai_conversation_get_messages(conversation)->data);
		g_assert_cmpstr(text, ==, "Inspect the linked work");
		ai_conversation_clear(conversation);
	}
}
static gboolean
cancel_fetch(gpointer data)
{
	g_cancellable_cancel(data);
	return G_SOURCE_REMOVE;
}
static void
test_cancellation(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GSource) timeout = g_timeout_source_new(30);
	gint64 start = g_get_monotonic_time();
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/999", NULL));
	g_object_set(conversation, "work-session", work, "stream", FALSE, NULL);
	g_main_context_push_thread_default(context);
	g_source_set_callback(timeout, cancel_fetch, cancel, NULL);
	g_source_attach(timeout, context);
	done = FALSE;
	ai_conversation_send_async(conversation, "Read linked work", cancel, sent, NULL);
	while (!done) g_main_context_iteration(context, TRUE);
	g_main_context_pop_thread_default(context);
	g_assert_error(send_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&send_error);
	g_assert_cmpuint(ai_mock_provider_get_call_count(provider), ==, 0);
	g_assert_false(ai_conversation_get_busy(conversation));
	g_assert_cmpint(g_get_monotonic_time() - start, <, 3 * G_USEC_PER_SEC);
}
static void
test_missing_and_malformed(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autofree gchar *text = read_context(work);
	g_assert_cmpstr(text, ==, "[]");
	g_clear_pointer(&text, g_free);
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/666", NULL));
	text = read_context(work);
	g_assert_nonnull(strstr(text, "invalid-response"));
	g_assert_false(ai_work_session_add_link_full(work, "https://github.com/team/project/issues/9", "bad\nrelationship", NULL));
}

static void
test_canonical_and_bounds(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_auto(GStrv) links = NULL;
	g_autofree gchar *body = g_strnfill(40000, 'x');
	g_autoptr(JsonObject) response = json_object_new();
	g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *json = NULL, *text = NULL;
	g_autofree gchar *file = g_build_filename(sandbox, "large.json", NULL);
	g_assert_true(ai_work_session_add_link(work, "https://GITHUB.COM:443/team/project/issues/777/", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/777", NULL));
	links = ai_work_session_dup_links(work);
	g_assert_cmpuint(g_strv_length(links), ==, 1);
	g_assert_cmpstr(links[0], ==, "https://github.com/team/project/issues/777");
	json_object_set_string_member(response, "title", "LARGE");
	json_object_set_string_member(response, "state", "open");
	json_object_set_string_member(response, "body", body);
	json_object_set_array_member(response, "comments", json_array_new());
	json_node_set_object(root, response);
	json = json_to_string(root, FALSE);
	g_assert_true(g_file_set_contents(file, json, -1, NULL));
	text = read_context(work);
	g_assert_nonnull(strstr(text, "\"truncated\":true"));
	g_assert_cmpuint(strlen(text), <, 34000);
	g_assert_true(ai_work_session_remove_link(work, "https://GITHUB.COM:443/team/project/issues/777/"));
	g_assert_false(ai_work_session_add_link(work, "https://github.com/team/../repo/issues/1", NULL));
}
static void
test_unavailable_connector(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autofree gchar *gh = g_build_filename(sandbox, "gh", NULL);
	g_autofree gchar *off = g_build_filename(sandbox, "gh-off", NULL);
	g_autofree gchar *text = NULL;
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/9", NULL));
	g_assert_cmpint(g_rename(gh, off), ==, 0);
	text = read_context(work);
	g_assert_cmpint(g_rename(off, gh), ==, 0);
	g_assert_nonnull(strstr(text, "connector-unavailable"));
	g_assert_nonnull(strstr(text, "https://github.com/team/project/issues/9"));
}
static void
test_comment_failure(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autofree gchar *text = NULL;
	g_assert_true(ai_work_session_add_link(work, "https://forge.test/other/repo/issues/4", NULL));
	text = read_context(work);
	g_assert_nonnull(strstr(text, "DESCRIPTION_RETAINED"));
	g_assert_nonnull(strstr(text, "Comments unavailable"));
	g_assert_nonnull(strstr(text, "authorization failed"));
	g_assert_nonnull(strstr(text, "\"status\":\"available\""));
}

static void
test_deadline(void)
{
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autofree gchar *text = NULL;
	gint64 start = g_get_monotonic_time();
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/998", NULL));
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/9", NULL));
	text = read_context(work);
	g_assert_nonnull(strstr(text, "\"status\":\"timeout\""));
	g_assert_nonnull(strstr(text, "Issue description from forge"));
	g_assert_cmpint(g_get_monotonic_time() - start, <, 25 * G_USEC_PER_SEC);
}

int
main(int argc, char **argv)
{
	g_autofree gchar *stub = NULL;
	g_autofree gchar *tea = NULL, *glab = NULL;
	sandbox = g_dir_make_tmp("ai-linked-work-XXXXXX", NULL);
	stub = g_build_filename(sandbox, "gh", NULL);
	tea = g_build_filename(sandbox, "tea", NULL);
	glab = g_build_filename(sandbox, "glab", NULL);
	g_test_init(&argc, &argv, NULL);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("PATH", sandbox, TRUE);
	g_assert_true(g_file_set_contents(stub,
		"#!/bin/sh\ncase \"$*\" in *issues/401*) echo 'HTTP 403' >&2; exit 1;; *issues/502*) echo 'dial tcp: no such host' >&2; exit 1;; *issues/666*) echo null; exit 0;; *issues/998*) exec /bin/sleep 40;; *issues/999*) /bin/sleep 10;; *issues/777*) /bin/cat \"$HOME/large.json\"; exit 0;; esac\nprintf '%s' '{\"title\":\"Fix parser\",\"state\":\"OPEN\",\"body\":\"Issue description from forge\",\"comments\":[{\"body\":\"Comment from maintainer\"}]}'\n", -1, NULL));
	g_chmod(stub, 0700);
	g_assert_true(g_file_set_contents(tea,
		"#!/bin/sh\ncase \"$*\" in \"login list --output json\") printf '%s' '[{\"name\":\"test\",\"url\":\"https://forge.test\"}]';;"
		"\"api --login test repos/other/repo/pulls/2\") printf '%s' '{\"title\":\"TEA_PULL_REQUEST\",\"state\":\"open\",\"body\":\"Fix it\"}';;"
		"\"api --login test repos/other/repo/issues/2/comments?limit=100&page=1\") printf '%s' '[{\"body\":\"TEA_COMMENT\"}]';;"
		"\"api --login test repos/other/repo/issues/4\") printf '%s' '{\"title\":\"DESCRIPTION_RETAINED\",\"state\":\"open\",\"body\":\"Body\"}';;"
		"\"api --login test repos/other/repo/issues/4/comments?limit=100&page=1\") echo 'HTTP 403' >&2; exit 1;;"
		"*) echo unexpected >&2; exit 1;; esac\n", -1, NULL));
	g_assert_true(g_file_set_contents(glab,
		"#!/bin/sh\ncase \"$*\" in \"api --hostname gitlab.test projects/group%2Fsub%2Frepo/merge_requests/3\") printf '%s' '{\"title\":\"GITLAB_MERGE_REQUEST\",\"state\":\"opened\",\"description\":\"Fix it\"}';;"
		"\"api --hostname gitlab.test projects/group%2Fsub%2Frepo/merge_requests/3/notes?per_page=100&sort=desc\") printf '%s' '[{\"body\":\"GITLAB_COMMENT\"}]';;"
		"*) echo unexpected >&2; exit 1;; esac\n", -1, NULL));
	g_chmod(tea, 0700); g_chmod(glab, 0700);
	g_test_add_func("/linked-work/connectors", test_connectors_and_partial_failure);
	g_test_add_func("/linked-work/resume-history", test_resume_and_history);
	g_test_add_func("/linked-work/cancellation", test_cancellation);
	g_test_add_func("/linked-work/missing-malformed", test_missing_and_malformed);
	g_test_add_func("/linked-work/canonical-bounds", test_canonical_and_bounds);
	g_test_add_func("/linked-work/unavailable-connector", test_unavailable_connector);
	g_test_add_func("/linked-work/comment-failure", test_comment_failure);
	g_test_add_func("/linked-work/provider-context", test_provider_context);
	g_test_add_func("/linked-work/deadline", test_deadline);
	return g_test_run();
}
