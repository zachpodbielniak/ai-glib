/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <stdio.h>
#include "../bin/ai-tui-usage.h"

static gchar *self_path;

/* Only reporting methods are accepted: a sidebar must never send a turn. */
static gint
native_stub(gint argc, gchar **argv)
{
	gchar line[4096];
	g_assert_cmpint(argc, ==, 4);
	g_assert_cmpstr(argv[1], ==, "app-server");
	g_assert_cmpstr(argv[2], ==, "--listen");
	g_assert_cmpstr(argv[3], ==, "stdio://");
	g_assert_nonnull(fgets(line, sizeof line, stdin));
	g_assert_nonnull(strstr(line, "\"method\":\"initialize\""));
	puts("{\"id\":1,\"result\":{}}"); fflush(stdout);
	g_assert_nonnull(fgets(line, sizeof line, stdin));
	g_assert_nonnull(strstr(line, "\"method\":\"initialized\""));
	g_assert_nonnull(fgets(line, sizeof line, stdin));
	g_assert_nonnull(strstr(line, "\"method\":\"account/rateLimits/read\""));
	if (g_strcmp0(g_getenv("TUI_USAGE_MODE"), "stall") == 0)
		g_usleep(10000000);
	if (g_strcmp0(g_getenv("TUI_USAGE_MODE"), "error") == 0)
		puts("{\"id\":2,\"error\":{\"code\":-32601}}");
	else
		printf("{\"id\":2,\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":%s}}}}\n",
			g_getenv("TUI_USAGE_USED"));
	fflush(stdout);
	return 0;
}

static void
count_changed(gpointer data)
{
	guint *count = data;
	(*count)++;
}

static void
drain(AiTuiUsage *usage)
{
	gint64 deadline = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
	while (usage->pending && g_get_monotonic_time() < deadline)
	{
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	g_assert_false(usage->pending);
}

static gdouble
cached_remaining(AiTuiUsage *usage)
{
	JsonArray *entries = ai_json_get_array(usage_object(usage->data), "entries");
	g_assert_nonnull(entries);
	g_assert_cmpuint(json_array_get_length(entries), ==, 1);
	return usage_remaining(usage_object(json_array_get_element(entries, 0)));
}

static void
test_remaining(void)
{
	const struct { const gchar *json; gdouble expected; } cases[] = {
		{ "{\"used_percent\":25}", 75 },
		{ "{\"remaining_percent\":75}", 75 },
		{ "{\"remaining_percent\":0,\"used_percent\":0}", 0 },
		{ "{\"used_percent\":0}", 100 },
		{ "{\"used_percent\":100}", 0 },
		{ "{\"remaining\":3,\"limit\":4}", 75 },
		{ "{\"used\":1,\"limit\":4,\"unit\":\"USD\"}", 75 },
		{ "{\"percent\":75}", -1 },
		{ "{\"used_percent\":null}", -1 },
		{ "{\"used_percent\":\"25\"}", -1 },
		{ "{\"remaining_percent\":101}", -1 },
		{ "{\"used_percent\":-1}", -1 },
		{ "{\"used\":0,\"limit\":0}", -1 },
		{ "{\"remaining\":5,\"limit\":4}", -1 },
		{ "{\"used\":1}", -1 },
		{ "null", -1 }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(JsonNode) node = json_from_string(cases[i].json, NULL);
		gdouble actual = usage_remaining(usage_object(node));
		if (cases[i].expected < 0) g_assert_true(isnan(actual));
		else g_assert_cmpfloat(actual, ==, cases[i].expected);
	}
	{
		g_autoptr(JsonObject) row = json_object_new();
		json_object_set_double_member(row, "remaining_percent", INFINITY);
		g_assert_true(isnan(usage_remaining(row)));
		json_object_set_double_member(row, "remaining_percent", NAN);
		g_assert_true(isnan(usage_remaining(row)));
	}
}

static void
test_cache(void)
{
	g_autoptr(AiCliClient) client = AI_CLI_CLIENT(ai_codex_cli_client_new());
	g_autoptr(AiCliClient) unsupported = AI_CLI_CLIENT(ai_cursor_client_new());
	AiTuiUsage usage = { 0 };
	guint changes = 0;
	usage.changed = count_changed;
	usage.user_data = &changes;
	ai_cli_client_set_executable_path(client, self_path);
	ai_cli_client_set_process_timeout_ms(client, 1500);
	ai_cli_client_set_env(client, "TUI_USAGE_USED", "25");
	g_assert_false(usage_refresh(&usage, G_OBJECT(client), FALSE));
	g_assert_null(usage.provider);
	g_assert_true(usage_refresh(&usage, G_OBJECT(client), TRUE));
	/* Mutating the chat client cannot alter the in-flight report snapshot. */
	ai_cli_client_set_env(client, "TUI_USAGE_USED", "90");
	g_assert_false(usage_refresh(&usage, G_OBJECT(client), TRUE));
	drain(&usage);
	g_assert_cmpuint(changes, ==, 1);
	g_assert_cmpfloat(cached_remaining(&usage), ==, 75);
	g_assert_false(usage_refresh(&usage, G_OBJECT(client), TRUE));
	/* Expired snapshots refresh once; failure preserves an explicitly stale value. */
	usage.next_refresh = 0;
	ai_cli_client_set_env(client, "TUI_USAGE_MODE", "error");
	g_assert_true(usage_refresh(&usage, G_OBJECT(client), TRUE));
	drain(&usage);
	g_assert_true(usage.failed);
	g_assert_cmpfloat(cached_remaining(&usage), ==, 75);
	g_assert_false(usage_refresh(&usage, G_OBJECT(client), TRUE));
	/* Changing a model invalidates cached values before querying. */
	ai_cli_client_unset_env(client, "TUI_USAGE_MODE");
	ai_cli_client_set_model(client, "another-model");
	g_assert_true(usage_refresh(&usage, G_OBJECT(client), TRUE));
	g_assert_null(usage.data);
	drain(&usage);
	g_assert_cmpfloat(cached_remaining(&usage), ==, 10);
	/* Cwd also participates in the cache identity. */
	ai_cli_client_set_working_directory(client, g_get_tmp_dir());
	g_assert_true(usage_refresh(&usage, G_OBJECT(client), TRUE));
	g_assert_null(usage.data);
	drain(&usage);
	/* Providers without reporting show unavailable and respect retry backoff. */
	g_assert_true(usage_refresh(&usage, G_OBJECT(unsupported), TRUE));
	drain(&usage);
	g_assert_null(usage.data);
	g_assert_true(usage.failed);
	g_assert_false(usage_refresh(&usage, G_OBJECT(unsupported), TRUE));
	usage_stop(&usage);
	usage_clear(&usage);
}

static void
test_cancel_and_stale(void)
{
	g_autoptr(AiCliClient) client = AI_CLI_CLIENT(ai_codex_cli_client_new());
	g_autoptr(AiClient) replacement = AI_CLIENT(ai_claude_client_new());
	AiTuiUsage usage = { 0 };
	guint changes = 0;
	usage.changed = count_changed;
	usage.user_data = &changes;
	ai_cli_client_set_executable_path(client, self_path);
	ai_cli_client_set_env(client, "TUI_USAGE_MODE", "stall");
	ai_cli_client_set_env(client, "TUI_USAGE_USED", "25");
	g_assert_true(usage_refresh(&usage, G_OBJECT(client), TRUE));
	/* A switch retires the pending generation; its completion cannot populate
	 * the replacement provider's panel, even if cancellation races success. */
	g_assert_true(usage_refresh(&usage, G_OBJECT(replacement), TRUE));
	drain(&usage);
	g_assert_null(usage.data);
	g_assert_true(usage.failed);
	g_assert_cmpint(usage.next_refresh, ==, 0);
	g_assert_true(usage_refresh(&usage, G_OBJECT(client), TRUE));
	usage_stop(&usage);
	drain(&usage);
	g_assert_cmpuint(changes, ==, 1);
	g_assert_false(usage_refresh(&usage, G_OBJECT(client), TRUE));
	usage_clear(&usage);
}

int
main(int argc, char **argv)
{
	gint result;
	if (argc > 1 && g_str_equal(argv[1], "app-server")) return native_stub(argc, argv);
	g_test_init(&argc, &argv, NULL);
	self_path = g_canonicalize_filename(argv[0], NULL);
	g_test_add_func("/tui/usage/remaining", test_remaining);
	g_test_add_func("/tui/usage/cache", test_cache);
	g_test_add_func("/tui/usage/cancel-stale", test_cancel_and_stale);
	result = g_test_run();
	g_free(self_path);
	return result;
}
