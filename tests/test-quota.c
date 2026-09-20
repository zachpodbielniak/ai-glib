/*
 * test-quota.c - The shared account-allowance cache
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * src/core/ai-quota.h is read by ai-tui's session panel and ai-gui's
 * header indicator alike, so the direction rule and the display rounding
 * are asserted here once. A terminal saying 75% remaining while a window
 * says 25% for the same account is the failure this file exists to stop.
 */
#include <stdio.h>
#include <ai-glib.h>

#include "core/ai-quota.h"

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
drain(AiQuota *usage)
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
cached_remaining(AiQuota *usage)
{
	JsonArray *entries = ai_json_get_array(ai_quota_object(usage->data), "entries");
	g_assert_nonnull(entries);
	g_assert_cmpuint(json_array_get_length(entries), ==, 1);
	return ai_quota_remaining(ai_quota_object(json_array_get_element(entries, 0)));
}

/*
 * Rounding must not invent an exhausted allowance.
 *
 * "%.0f" turns 0.4% remaining into "0%", which reads as "you are out"
 * when you are not -- the same invented zero the report layer refuses to
 * produce from missing data. The two ends are therefore named.
 */
static void
test_format_percent(void)
{
	const struct { gdouble value; const gchar *expected; } cases[] = {
		{ 0,      "0%"   },
		{ 0.4,    "<1%"  },
		{ 0.01,   "<1%"  },
		{ 0.5,    "<1%"  },
		{ 0.99,   "<1%"  },
		{ 1,      "1%"   },
		{ 25,     "25%"  },
		{ 74.6,   "75%"  },
		{ 99.4,   "99%"  },
		{ 99.6,   ">99%" },
		{ 100,    "100%" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *text = ai_quota_format_percent(cases[i].value);

		g_assert_cmpstr(text, ==, cases[i].expected);
	}

	/* An unknown direction has no text at all, so a caller cannot print
	 * it by accident. */
	g_assert_null(ai_quota_format_percent(NAN));
}

/* Build a cache holding a literal report body, without a provider. */
static void
quota_seed(AiQuota *quota, const gchar *json)
{
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	g_clear_pointer(&quota->data, json_node_unref);
	quota->data = json_node_copy(json_parser_get_root(parser));
}

/*
 * One number for the header has to be the allowance about to stop the
 * conversation, not whichever the provider listed first.
 */
static void
test_lowest(void)
{
	AiQuota quota = { 0 };

	g_assert_false(isfinite(ai_quota_lowest(&quota)));

	quota_seed(&quota, "{\"entries\":["
		"{\"label\":\"weekly\",\"remaining_percent\":80},"
		"{\"label\":\"5-hour\",\"used_percent\":95},"
		"{\"label\":\"unknown\",\"percent\":1}]}");
	g_assert_cmpfloat(ai_quota_lowest(&quota), ==, 5);

	/* Every row direction-unknown is not zero, it is no answer. */
	quota_seed(&quota, "{\"entries\":[{\"percent\":1},{\"used\":3}]}");
	g_assert_false(isfinite(ai_quota_lowest(&quota)));

	quota_seed(&quota, "{\"entries\":[]}");
	g_assert_false(isfinite(ai_quota_lowest(&quota)));

	ai_quota_clear(&quota);
}

/*
 * A snapshot that is stale, refreshing or partial must say so. Both
 * front-ends print this string, so the precedence lives in one place.
 */
static void
test_heading(void)
{
	AiQuota quota = { 0 };

	g_assert_cmpstr(ai_quota_heading(&quota), ==, "ACCOUNT REMAINING");

	quota_seed(&quota, "{\"entries\":[],\"availability\":\"available\"}");
	g_assert_cmpstr(ai_quota_heading(&quota), ==, "ACCOUNT REMAINING");

	quota_seed(&quota, "{\"entries\":[],\"availability\":\"partial\"}");
	g_assert_cmpstr(ai_quota_heading(&quota), ==, "REMAINING (partial)");

	quota.pending = TRUE;
	g_assert_cmpstr(ai_quota_heading(&quota), ==, "REMAINING (refreshing)");

	/* Stale outranks refreshing: the figure on screen is the old one
	 * either way, and "stale" is the fact that matters. */
	quota.failed = TRUE;
	g_assert_cmpstr(ai_quota_heading(&quota), ==, "REMAINING (stale)");

	quota.pending = FALSE;
	g_assert_cmpstr(ai_quota_heading(&quota), ==, "REMAINING (stale)");

	ai_quota_clear(&quota);
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
		gdouble actual = ai_quota_remaining(ai_quota_object(node));
		if (cases[i].expected < 0) g_assert_true(isnan(actual));
		else g_assert_cmpfloat(actual, ==, cases[i].expected);
	}
	{
		g_autoptr(JsonObject) row = json_object_new();
		json_object_set_double_member(row, "remaining_percent", INFINITY);
		g_assert_true(isnan(ai_quota_remaining(row)));
		json_object_set_double_member(row, "remaining_percent", NAN);
		g_assert_true(isnan(ai_quota_remaining(row)));
	}
}

static void
test_cache(void)
{
	g_autoptr(AiCliClient) client = AI_CLI_CLIENT(ai_codex_cli_client_new());
	g_autoptr(AiCliClient) unsupported = AI_CLI_CLIENT(ai_cursor_client_new());
	AiQuota usage = { 0 };
	guint changes = 0;
	usage.changed = count_changed;
	usage.user_data = &changes;
	ai_cli_client_set_executable_path(client, self_path);
	ai_cli_client_set_process_timeout_ms(client, 1500);
	ai_cli_client_set_env(client, "TUI_USAGE_USED", "25");
	g_assert_false(ai_quota_refresh(&usage, G_OBJECT(client), FALSE));
	g_assert_null(usage.provider);
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	/* Mutating the chat client cannot alter the in-flight report snapshot. */
	ai_cli_client_set_env(client, "TUI_USAGE_USED", "90");
	g_assert_false(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	drain(&usage);
	g_assert_cmpuint(changes, ==, 1);
	g_assert_cmpfloat(cached_remaining(&usage), ==, 75);
	g_assert_false(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	/* Expired snapshots refresh once; failure preserves an explicitly stale value. */
	usage.next_refresh = 0;
	ai_cli_client_set_env(client, "TUI_USAGE_MODE", "error");
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	drain(&usage);
	g_assert_true(usage.failed);
	g_assert_cmpfloat(cached_remaining(&usage), ==, 75);
	g_assert_false(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	/* Changing a model invalidates cached values before querying. */
	ai_cli_client_unset_env(client, "TUI_USAGE_MODE");
	ai_cli_client_set_model(client, "another-model");
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	g_assert_null(usage.data);
	drain(&usage);
	g_assert_cmpfloat(cached_remaining(&usage), ==, 10);
	/* Cwd also participates in the cache identity. */
	ai_cli_client_set_working_directory(client, g_get_tmp_dir());
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	g_assert_null(usage.data);
	drain(&usage);
	/* Providers without reporting show unavailable and respect retry backoff. */
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(unsupported), TRUE));
	drain(&usage);
	g_assert_null(usage.data);
	g_assert_true(usage.failed);
	g_assert_false(ai_quota_refresh(&usage, G_OBJECT(unsupported), TRUE));
	ai_quota_stop(&usage);
	ai_quota_clear(&usage);
}

static void
test_cancel_and_stale(void)
{
	g_autoptr(AiCliClient) client = AI_CLI_CLIENT(ai_codex_cli_client_new());
	g_autoptr(AiClient) replacement = AI_CLIENT(ai_claude_client_new());
	AiQuota usage = { 0 };
	guint changes = 0;
	usage.changed = count_changed;
	usage.user_data = &changes;
	ai_cli_client_set_executable_path(client, self_path);
	ai_cli_client_set_env(client, "TUI_USAGE_MODE", "stall");
	ai_cli_client_set_env(client, "TUI_USAGE_USED", "25");
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	/* A switch retires the pending generation; its completion cannot populate
	 * the replacement provider's panel, even if cancellation races success. */
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(replacement), TRUE));
	drain(&usage);
	g_assert_null(usage.data);
	g_assert_true(usage.failed);
	g_assert_cmpint(usage.next_refresh, ==, 0);
	g_assert_true(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	ai_quota_stop(&usage);
	drain(&usage);
	g_assert_cmpuint(changes, ==, 1);
	g_assert_false(ai_quota_refresh(&usage, G_OBJECT(client), TRUE));
	ai_quota_clear(&usage);
}

int
main(int argc, char **argv)
{
	gint result;
	if (argc > 1 && g_str_equal(argv[1], "app-server")) return native_stub(argc, argv);
	g_test_init(&argc, &argv, NULL);
	self_path = g_canonicalize_filename(argv[0], NULL);
	g_test_add_func("/ai-glib/quota/remaining", test_remaining);
	g_test_add_func("/ai-glib/quota/format-percent", test_format_percent);
	g_test_add_func("/ai-glib/quota/lowest", test_lowest);
	g_test_add_func("/ai-glib/quota/heading", test_heading);
	g_test_add_func("/ai-glib/quota/cache", test_cache);
	g_test_add_func("/ai-glib/quota/cancel-stale", test_cancel_and_stale);
	result = g_test_run();
	g_free(self_path);
	return result;
}
