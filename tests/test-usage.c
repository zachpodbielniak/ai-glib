/*
 * test-usage.c - Unit tests for AiUsage
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "model/ai-usage.h"

static void
test_usage_new(void)
{
	g_autoptr(AiUsage) usage = NULL;

	usage = ai_usage_new(100, 50);
	g_assert_nonnull(usage);
}

static void
test_usage_tokens(void)
{
	g_autoptr(AiUsage) usage = NULL;

	usage = ai_usage_new(100, 50);

	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 100);
	g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 50);
	g_assert_cmpint(ai_usage_get_total_tokens(usage), ==, 150);
}

static void
test_usage_copy(void)
{
	g_autoptr(AiUsage) usage = NULL;
	g_autoptr(AiUsage) copy = NULL;

	usage = ai_usage_new(200, 100);
	copy = ai_usage_copy(usage);

	g_assert_nonnull(copy);
	g_assert_true(usage != copy);
	g_assert_cmpint(ai_usage_get_input_tokens(copy), ==, 200);
	g_assert_cmpint(ai_usage_get_output_tokens(copy), ==, 100);
}

static void
test_usage_gtype(void)
{
	GType type;

	type = ai_usage_get_type();
	g_assert_true(G_TYPE_IS_BOXED(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiUsage");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/usage/new", test_usage_new);
	g_test_add_func("/ai-glib/usage/tokens", test_usage_tokens);
	g_test_add_func("/ai-glib/usage/copy", test_usage_copy);
	g_test_add_func("/ai-glib/usage/gtype", test_usage_gtype);

	return g_test_run();
}
