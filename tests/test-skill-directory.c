/*
 * test-skill-directory.c - Skill directory context at both model entry points
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <ai-glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

static gchar *sandbox;
static gchar *skill_dir;

static const gchar body[] = "Read references/guide.md for $ARGUMENTS ($1). !`printf BODY`\n";

static AiResourceRegistry *
load_registry(void)
{
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();

	ai_resource_registry_set_working_directory(registry, sandbox);
	ai_resource_registry_scan(registry);
	g_assert_nonnull(ai_resource_registry_lookup(registry, AI_RESOURCE_SKILL, "procedure"));
	return (AiResourceRegistry *)g_steal_pointer(&registry);
}

static void
test_skill_tool(void)
{
	g_autoptr(AiResourceRegistry) registry = load_registry();
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiToolUse) use = ai_tool_use_new_from_json_string(
		"skill-test", "skill", "{\"name\":\"procedure\"}");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	g_autofree gchar *expected = g_strdup_printf(
		"Base directory for this skill: %s\n\n%s", skill_dir, body);

	ai_tool_executor_set_working_directory(executor, sandbox);
	ai_tool_executor_set_resource_registry(executor, registry);
	result = ai_tool_executor_execute(executor, use, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, expected);
	g_assert_false(g_file_test("skill-dir-executed", G_FILE_TEST_EXISTS));
}

static void
test_skill_command(gconstpointer data)
{
	gboolean with_task = GPOINTER_TO_INT(data);
	g_autoptr(AiResourceRegistry) registry = load_registry();
	g_autoptr(AiCommandSet) set = ai_command_set_new(registry);
	g_autoptr(AiCommandResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *expected = g_strdup_printf(
		"Base directory for this skill: %s\n\n%s", skill_dir,
		with_task
		? "Read references/guide.md for parser extra (parser). BODY\n"
		  "\n\nApply the skill instructions above to this user request:\n\nparser extra"
		: "Read references/guide.md for  (). BODY\n");

	/* BODY proves shell expansion was enabled. Only the path must remain
	 * literal; prefixing before expansion would also create this marker. */
	result = ai_command_set_resolve(set,
		with_task ? "/procedure parser extra" : "/procedure", sandbox, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(ai_command_result_get_outcome(result), ==, AI_COMMAND_OUTCOME_PROMPT);
	g_assert_false(g_file_test("skill-dir-executed", G_FILE_TEST_EXISTS));
	g_assert_cmpstr(ai_command_result_get_prompt(result), ==, expected);
}

static void
test_helper_file_kinds(void)
{
	const AiResourceKind kinds[] = { AI_RESOURCE_SKILL, AI_RESOURCE_COMMAND, AI_RESOURCE_AGENT };
	g_autofree gchar *path = g_build_filename(skill_dir, "SKILL.md", NULL);
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AiResource) resource = ai_resource_new_from_file(
			path, "procedure", kinds[i], "agents", AI_RESOURCE_SCOPE_PROJECT, &error);
		g_autofree gchar *text = NULL;
		g_autofree gchar *empty = NULL;
		g_autofree gchar *expected = NULL;
		g_autofree gchar *expected_empty = NULL;

		g_assert_no_error(error);
		g_assert_nonnull(resource);
		text = ai_resource_prefix_skill_dir(resource, "Already expanded.");
		empty = ai_resource_prefix_skill_dir(resource, "");
		expected = kinds[i] == AI_RESOURCE_SKILL
			? g_strdup_printf("Base directory for this skill: %s\n\nAlready expanded.", skill_dir)
			: g_strdup("Already expanded.");
		expected_empty = kinds[i] == AI_RESOURCE_SKILL
			? g_strdup_printf("Base directory for this skill: %s\n\n", skill_dir)
			: g_strdup("");
		g_assert_cmpstr(text, ==, expected);
		g_assert_cmpstr(empty, ==, expected_empty);
		g_assert_cmpstr(ai_resource_get_body(resource), ==, body);
	}
}

static void
test_pathless_skill(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResource) resource = ai_resource_new_from_data(body, -1,
		"memory", AI_RESOURCE_SKILL, "ai-glib", AI_RESOURCE_SCOPE_USER, &error);
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiToolUse) use = ai_tool_use_new_from_json_string(
		"memory-test", "skill", "{\"name\":\"memory\"}");
	g_autofree gchar *text = NULL;
	g_autofree gchar *result = NULL;

	g_assert_no_error(error);
	g_assert_null(ai_resource_get_path(resource));
	text = ai_resource_prefix_skill_dir(resource, "Expanded.");
	g_assert_cmpstr(text, ==, "Expanded.");
	ai_resource_registry_add(registry, resource);
	ai_tool_executor_set_working_directory(executor, sandbox);
	ai_tool_executor_set_resource_registry(executor, registry);
	result = ai_tool_executor_execute(executor, use, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, body);
}

static void
test_flat_command(void)
{
	g_autoptr(AiResourceRegistry) registry = load_registry();
	g_autoptr(AiCommandSet) set = ai_command_set_new(registry);
	g_autoptr(AiCommandResult) result = NULL;
	g_autoptr(GError) error = NULL;

	result = ai_command_set_resolve(set, "/flat parser extra", sandbox, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpstr(ai_command_result_get_prompt(result), ==, "Command parser extra.\n");
}

int
main(int argc, char *argv[])
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *old_cwd = g_get_current_dir();
	g_autofree gchar *installed = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *commands = NULL;
	g_autofree gchar *command_path = NULL;
	g_autofree gchar *skills = NULL;
	gchar *cleanup_argv[] = { "rm", "-rf", "--", NULL, NULL };
	gint cleanup_status;
	gint status;

	/* Set these before constructing any GLib objects that cache HOME. */
	sandbox = g_dir_make_tmp("ai-glib-skill-directory-XXXXXX", &error);
	g_assert_no_error(error);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_assert_cmpint(g_chdir(sandbox), ==, 0);
	g_test_init(&argc, &argv, NULL);

	installed = g_build_filename(sandbox, "installed", NULL);
	skills = g_build_filename(sandbox, ".agents", "skills", NULL);
	commands = g_build_filename(sandbox, ".claude", "commands", NULL);
	g_assert_cmpint(g_mkdir_with_parents(installed, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(skills, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(commands, 0700), ==, 0);
	path = g_build_filename(installed, "SKILL.md", NULL);
	contents = g_strconcat("---\nname: procedure\nshell: true\n---\n", body, NULL);
	g_assert_true(g_file_set_contents(path, contents, -1, &error));
	g_assert_no_error(error);
	skill_dir = g_build_filename(skills, "procedure $ARGUMENTS $1 !`touch skill-dir-executed` café", NULL);
	g_assert_cmpint(symlink(installed, skill_dir), ==, 0);
	command_path = g_build_filename(commands, "flat.md", NULL);
	g_assert_true(g_file_set_contents(command_path, "Command $ARGUMENTS.\n", -1, &error));
	g_assert_no_error(error);

	g_test_add_func("/ai-glib/skill-directory/tool", test_skill_tool);
	g_test_add_data_func("/ai-glib/skill-directory/command-task", GINT_TO_POINTER(TRUE), test_skill_command);
	g_test_add_data_func("/ai-glib/skill-directory/command-no-task", GINT_TO_POINTER(FALSE), test_skill_command);
	g_test_add_func("/ai-glib/skill-directory/file-kinds", test_helper_file_kinds);
	g_test_add_func("/ai-glib/skill-directory/pathless", test_pathless_skill);
	g_test_add_func("/ai-glib/skill-directory/flat-command", test_flat_command);
	status = g_test_run();

	g_assert_cmpint(g_chdir(old_cwd), ==, 0);
	cleanup_argv[3] = sandbox;
	g_assert_true(g_spawn_sync(NULL, cleanup_argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, NULL, NULL, &cleanup_status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(cleanup_status, &error));
	g_assert_no_error(error);
	g_free(skill_dir);
	g_free(sandbox);
	return status;
}
