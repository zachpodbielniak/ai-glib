/*
 * test-cli-client.c - Unit tests for AiCliClient base class
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "core/ai-cli-client.h"
#include "core/ai-config.h"

/*
 * Test that AiCliClient is a valid GType.
 */
static void
test_cli_client_gtype(void)
{
	GType type;

	type = ai_cli_client_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiCliClient");
}

/*
 * Test that AiCliClient is derivable (cannot be instantiated directly).
 */
static void
test_cli_client_derivable(void)
{
	GType type;

	type = ai_cli_client_get_type();
	/* AiCliClient is derivable, not instantiable directly */
	g_assert_true(G_TYPE_IS_DERIVABLE(type));
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/cli-client/gtype", test_cli_client_gtype);
	g_test_add_func("/ai-glib/cli-client/derivable", test_cli_client_derivable);

	return g_test_run();
}
