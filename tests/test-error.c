/*
 * test-error.c - Unit tests for AiError
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "core/ai-error.h"

static void
test_error_quark(void)
{
	GQuark quark;

	quark = ai_error_quark();
	g_assert_cmpuint(quark, !=, 0);
	g_assert_cmpstr(g_quark_to_string(quark), ==, "ai-glib-error-quark");
}

static void
test_error_type(void)
{
	GType type;

	type = ai_error_get_type();
	g_assert_true(G_TYPE_IS_ENUM(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiError");
}

static void
test_error_values(void)
{
	/* Verify some error code values and that the enum is non-empty */
	g_assert_cmpint(AI_ERROR_INVALID_API_KEY, ==, 1);
	g_assert_cmpint(AI_ERROR_RATE_LIMITED, ==, 2);
	g_assert_cmpint(AI_ERROR_NETWORK_ERROR, ==, 3);
	g_assert_cmpint(AI_ERROR_CONTENT_FILTERED, ==, 9);
	g_assert_cmpint(AI_ERROR_UNKNOWN, >, 0);
}

static void
test_error_creation(void)
{
	g_autoptr(GError) error = NULL;

	g_set_error(&error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
	            "Test error message");

	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_assert_cmpstr(error->message, ==, "Test error message");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/error/quark", test_error_quark);
	g_test_add_func("/ai-glib/error/type", test_error_type);
	g_test_add_func("/ai-glib/error/values", test_error_values);
	g_test_add_func("/ai-glib/error/creation", test_error_creation);

	return g_test_run();
}
