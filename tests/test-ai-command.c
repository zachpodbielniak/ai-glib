/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <string.h>

static void
test_model_command_offers_a_picker(void)
{
	g_autoptr(AiCommandSet) set = ai_command_set_new(NULL);
	g_autoptr(AiCommand) command = ai_command_set_lookup(set, "model");

	g_assert_nonnull(command);
	g_assert_cmpint(ai_command_get_kind(command), ==, AI_COMMAND_BUILTIN);
	g_assert_nonnull(strstr(ai_command_get_description(command), "picker"));
	g_assert_cmpstr(ai_command_get_argument_hint(command), ==, "[model]");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/command/model-picker", test_model_command_offers_a_picker);
	return g_test_run();
}
