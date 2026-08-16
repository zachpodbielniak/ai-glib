/*
 * test-resource-registry.c - Scanning, precedence, and watching
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The whole suite runs inside a sandbox: main() creates a temporary
 * directory and points HOME and XDG_CONFIG_HOME at it before GLib has
 * cached either. Nothing here reads the real ~/.claude, which matters for
 * two reasons -- the suite has to pass on a machine that has never run
 * claude-code, and a developer's own sixteen command files must not be
 * able to make a test pass that would otherwise fail.
 */

#include <ai-glib.h>

#include <glib/gstdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Sandbox
 * ---------------------------------------------------------------- */

static gchar *sandbox_home = NULL;
static gchar *sandbox_project = NULL;

static void
rm_rf(const gchar *path)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", path);

	g_assert_cmpint(system(cmd), ==, 0);
}

/* Empty the sandbox so each test starts from nothing. */
static void
reset(void)
{
	g_autofree gchar *config = g_build_filename(sandbox_home, ".config", NULL);

	rm_rf(sandbox_project);

	{
		g_autofree gchar *claude =
			g_build_filename(sandbox_home, ".claude", NULL);
		g_autofree gchar *opencode =
			g_build_filename(sandbox_home, ".opencode", NULL);
		g_autofree gchar *grok =
			g_build_filename(sandbox_home, ".grok", NULL);

		rm_rf(claude);
		rm_rf(opencode);
		rm_rf(grok);
	}

	rm_rf(config);

	g_assert_cmpint(g_mkdir_with_parents(sandbox_project, 0755), ==, 0);
}

/* Write @contents to @relative under @base, creating directories. */
static void
write_file(const gchar *base, const gchar *relative, const gchar *contents)
{
	g_autofree gchar *path = g_build_filename(base, relative, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	GError           *error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);
}

static void
write_home(const gchar *relative, const gchar *contents)
{
	write_file(sandbox_home, relative, contents);
}

static void
write_project(const gchar *relative, const gchar *contents)
{
	write_file(sandbox_project, relative, contents);
}

static AiResourceRegistry *
fresh_registry(void)
{
	AiResourceRegistry *registry = ai_resource_registry_new();

	ai_resource_registry_set_working_directory(registry, sandbox_project);
	ai_resource_registry_scan(registry);

	return registry;
}

/* ----------------------------------------------------------------
 * Finding things
 * ---------------------------------------------------------------- */

static void
test_empty_registry(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	GList                        *list;

	reset();
	registry = fresh_registry();

	g_assert_null(ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                          "anything"));

	list = ai_resource_registry_list(registry, AI_RESOURCE_COMMAND);
	g_assert_null(list);

	list = ai_resource_registry_list_shadowed(registry);
	g_assert_null(list);
}

static void
test_finds_user_command(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *resource;

	reset();
	write_home(".claude/commands/deploy.md",
	           "---\ndescription: Ship it\n---\nDeploy the thing.\n");

	registry = fresh_registry();

	resource = ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                       "deploy");
	g_assert_nonnull(resource);
	g_assert_cmpstr(ai_resource_get_description(resource), ==, "Ship it");
	g_assert_cmpstr(ai_resource_get_origin(resource), ==, "claude");
	g_assert_cmpint(ai_resource_get_scope(resource), ==,
	                AI_RESOURCE_SCOPE_USER);
}

static void
test_finds_project_command(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *resource;

	reset();
	write_project(".claude/commands/local.md", "Body\n");

	registry = fresh_registry();

	resource = ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                       "local");
	g_assert_nonnull(resource);
	g_assert_cmpint(ai_resource_get_scope(resource), ==,
	                AI_RESOURCE_SCOPE_PROJECT);
}

static void
test_finds_nested_skill(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *resource;

	reset();
	write_home(".claude/skills/podomation-dsl/SKILL.md",
	           "---\ndescription: The DSL\n---\nHow to write it.\n");

	registry = fresh_registry();

	resource = ai_resource_registry_lookup(registry, AI_RESOURCE_SKILL,
	                                       "podomation-dsl");
	g_assert_nonnull(resource);
	g_assert_cmpstr(ai_resource_get_description(resource), ==, "The DSL");
}

static void
test_finds_flat_and_nested_skills_together(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	GList                        *list;

	/*
	 * ~/.config/opencode/skills really does hold both shapes side by side
	 * -- skill-agent-writer/ and skill-agent-writer.md. Supporting only
	 * one of them would silently drop half the directory.
	 */
	reset();
	write_home(".config/opencode/skills/alpha/SKILL.md", "Nested.\n");
	write_home(".config/opencode/skills/beta.md", "Flat.\n");

	registry = fresh_registry();

	g_assert_nonnull(ai_resource_registry_lookup(registry, AI_RESOURCE_SKILL,
	                                             "alpha"));
	g_assert_nonnull(ai_resource_registry_lookup(registry, AI_RESOURCE_SKILL,
	                                             "beta"));

	list = ai_resource_registry_list(registry, AI_RESOURCE_SKILL);
	g_assert_cmpint(g_list_length(list), ==, 2);
	g_list_free(list);
}

static void
test_namespaced_command(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *resource;

	reset();
	write_home(".claude/commands/git/status.md", "Show the status.\n");

	registry = fresh_registry();

	/* claude spells a command in git/status.md as /git:status. */
	resource = ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                       "git:status");
	g_assert_nonnull(resource);

	/* And not under the bare stem, which would collide with a top-level
	 * command of the same name. */
	g_assert_null(ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                          "status"));
}

static void
test_skill_marker_is_not_listed_twice(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	GList                        *list;

	reset();
	write_home(".claude/skills/thing/SKILL.md", "Body\n");

	registry = fresh_registry();

	list = ai_resource_registry_list(registry, AI_RESOURCE_SKILL);

	/* Not "thing" and "thing:SKILL". */
	g_assert_cmpint(g_list_length(list), ==, 1);
	g_assert_cmpstr(ai_resource_get_name(list->data), ==, "thing");
	g_list_free(list);
}

static void
test_kinds_do_not_collide(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;

	reset();
	write_home(".claude/commands/review.md", "A command.\n");
	write_home(".claude/agents/review.md", "An agent.\n");

	registry = fresh_registry();

	/* Lookup is keyed on kind as well as name, so both survive. */
	g_assert_nonnull(ai_resource_registry_lookup(registry,
	                                             AI_RESOURCE_COMMAND,
	                                             "review"));
	g_assert_nonnull(ai_resource_registry_lookup(registry, AI_RESOURCE_AGENT,
	                                             "review"));
	g_assert_null(ai_resource_registry_list_shadowed(registry));
}

static void
test_agents_from_both_harnesses(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	GList                        *list;

	reset();
	write_home(".claude/agents/security-auditor.md",
	           "---\nname: security-auditor\ntools: Bash, Read\n---\nAudit.\n");
	write_home(".config/opencode/agents/agent-code-reviewer.md",
	           "# Agent: Reviewer\n\nReviews C code.\n");

	registry = fresh_registry();

	list = ai_resource_registry_list(registry, AI_RESOURCE_AGENT);
	g_assert_cmpint(g_list_length(list), ==, 2);
	g_list_free(list);

	{
		AiResource *auditor = ai_resource_registry_lookup(
			registry, AI_RESOURCE_AGENT, "security-auditor");
		g_auto(GStrv) tools = NULL;

		g_assert_nonnull(auditor);
		tools = ai_resource_get_meta_list(auditor, "tools");
		g_assert_cmpint(g_strv_length(tools), ==, 2);
	}

	{
		AiResource *reviewer = ai_resource_registry_lookup(
			registry, AI_RESOURCE_AGENT, "agent-code-reviewer");

		g_assert_nonnull(reviewer);
		g_assert_cmpstr(ai_resource_get_origin(reviewer), ==, "opencode");
		g_assert_cmpstr(ai_resource_get_description(reviewer), ==,
		                "Reviews C code.");
	}
}

/* ----------------------------------------------------------------
 * Precedence
 * ---------------------------------------------------------------- */

static void
test_project_shadows_user(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *winner;
	GList                        *shadowed;

	reset();
	write_home(".claude/commands/build.md",
	           "---\ndescription: from user\n---\nx\n");
	write_project(".claude/commands/build.md",
	              "---\ndescription: from project\n---\nx\n");

	registry = fresh_registry();

	winner = ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                     "build");
	g_assert_cmpstr(ai_resource_get_description(winner), ==, "from project");

	/* The loser is kept, so a listing can say *why* the user's file is not
	 * the one running rather than omitting both halves of the problem. */
	shadowed = ai_resource_registry_list_shadowed(registry);
	g_assert_cmpint(g_list_length(shadowed), ==, 1);
	g_assert_cmpstr(ai_resource_get_description(shadowed->data), ==,
	                "from user");
	g_assert_nonnull(ai_resource_get_path(shadowed->data));
	g_list_free(shadowed);
}

static void
test_ai_glib_wins_within_a_scope(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *winner;

	reset();
	write_home(".claude/commands/thing.md",
	           "---\ndescription: from claude\n---\nx\n");
	write_home(".config/ai-glib/commands/thing.md",
	           "---\ndescription: from ai-glib\n---\nx\n");

	registry = fresh_registry();

	/* A file written specifically for ai-glib beats one it is merely
	 * borrowing from another harness. */
	winner = ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                     "thing");
	g_assert_cmpstr(ai_resource_get_description(winner), ==, "from ai-glib");
	g_assert_cmpstr(ai_resource_get_origin(winner), ==, "ai-glib");
}

static void
test_scope_beats_table_order(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *winner;

	reset();

	/* ai-glib at user scope against claude at project scope: scope wins,
	 * even though ai-glib comes first in the table. */
	write_home(".config/ai-glib/commands/thing.md",
	           "---\ndescription: user ai-glib\n---\nx\n");
	write_project(".claude/commands/thing.md",
	              "---\ndescription: project claude\n---\nx\n");

	registry = fresh_registry();

	winner = ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                     "thing");
	g_assert_cmpstr(ai_resource_get_description(winner), ==,
	                "project claude");
}

static void
test_opencode_home_and_xdg_skills(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	AiResource                   *winner;

	reset();
	write_home(".opencode/skills/dup.md", "---\ndescription: home\n---\nx\n");
	write_home(".config/opencode/skills/dup.md",
	           "---\ndescription: xdg\n---\nx\n");

	registry = fresh_registry();

	/* Both directories are searched; the XDG one is listed first. */
	winner = ai_resource_registry_lookup(registry, AI_RESOURCE_SKILL, "dup");
	g_assert_cmpstr(ai_resource_get_description(winner), ==, "xdg");

	{
		GList *shadowed = ai_resource_registry_list_shadowed(registry);

		g_assert_cmpint(g_list_length(shadowed), ==, 1);
		g_list_free(shadowed);
	}
}

/* ----------------------------------------------------------------
 * Robustness
 * ---------------------------------------------------------------- */

static void
test_missing_directories_are_normal(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;

	/* Most of the search table will not exist on any given machine.
	 * Scanning must be silent about it, not noisy and not fatal. */
	reset();
	registry = fresh_registry();

	g_assert_null(ai_resource_registry_list(registry, AI_RESOURCE_COMMAND));
}

static void
test_file_where_a_directory_was_expected(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;

	reset();
	write_home(".claude/commands", "not a directory\n");

	registry = fresh_registry();

	g_assert_null(ai_resource_registry_list(registry, AI_RESOURCE_COMMAND));
}

static void
test_unreadable_directory_is_skipped(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autofree gchar             *locked = NULL;

	reset();
	write_home(".claude/commands/good.md", "Fine.\n");
	write_home(".claude/skills/locked/SKILL.md", "Hidden.\n");

	locked = g_build_filename(sandbox_home, ".claude", "skills", NULL);
	g_assert_cmpint(g_chmod(locked, 0), ==, 0);

	registry = fresh_registry();

	/* The scan continues past it; the readable command is still found. */
	g_assert_nonnull(ai_resource_registry_lookup(registry,
	                                             AI_RESOURCE_COMMAND,
	                                             "good"));

	g_assert_cmpint(g_chmod(locked, 0755), ==, 0);
}

static void
test_one_bad_file_does_not_hide_the_others(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	GList                        *list;
	g_autofree gchar             *bad = NULL;
	GError                       *error = NULL;

	/*
	 * The case the whole log-level rule exists for. A file that cannot be
	 * decoded must cost exactly itself, not the sixteen good files beside
	 * it -- and must not abort a suite running with fatal warnings, which
	 * this one is.
	 */
	reset();
	write_home(".claude/commands/one.md", "First.\n");
	write_home(".claude/commands/two.md", "Second.\n");
	write_home(".claude/commands/three.md", "Third.\n");

	bad = g_build_filename(sandbox_home, ".claude", "commands", "bad.md",
	                       NULL);
	g_file_set_contents(bad, "\xff\xfe\xff\xfe", 4, &error);
	g_assert_no_error(error);

	registry = fresh_registry();

	list = ai_resource_registry_list(registry, AI_RESOURCE_COMMAND);
	g_assert_cmpint(g_list_length(list), ==, 3);
	g_list_free(list);
}

static void
test_non_markdown_files_ignored(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;

	GList *list;

	reset();
	write_home(".claude/commands/notes.txt", "Not a command.\n");
	write_home(".claude/commands/config.json", "{}\n");
	write_home(".claude/commands/real.md", "A command.\n");

	registry = fresh_registry();

	list = ai_resource_registry_list(registry, AI_RESOURCE_COMMAND);
	g_assert_cmpint(g_list_length(list), ==, 1);
	g_list_free(list);
}

static void
test_directory_without_a_marker_is_ignored(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;

	reset();
	write_home(".claude/skills/empty-dir/README.txt", "nothing here\n");

	registry = fresh_registry();

	g_assert_null(ai_resource_registry_lookup(registry, AI_RESOURCE_SKILL,
	                                          "empty-dir"));
}

static void
test_symlink_loop_terminates(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autofree gchar             *dir = NULL;
	g_autofree gchar             *loop = NULL;

	reset();
	write_home(".claude/commands/real.md", "A command.\n");

	dir = g_build_filename(sandbox_home, ".claude", "commands", NULL);
	loop = g_build_filename(dir, "loop", NULL);

	if (symlink(dir, loop) != 0)
	{
		g_test_skip("cannot create symlinks here");
		return;
	}

	/* The depth cap is what makes this terminate rather than walk
	 * forever. If it did not, this test would hang, not fail. */
	registry = fresh_registry();

	g_assert_nonnull(ai_resource_registry_lookup(registry,
	                                             AI_RESOURCE_COMMAND,
	                                             "real"));
}

static void
test_resource_with_empty_name_is_skipped(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autoptr(AiResource)         nameless = NULL;

	reset();
	registry = fresh_registry();

	nameless = ai_resource_new_from_data("body", -1, "", AI_RESOURCE_COMMAND,
	                                     "ai-glib",
	                                     AI_RESOURCE_SCOPE_BUILTIN, NULL);
	ai_resource_registry_add(registry, nameless);

	g_assert_null(ai_resource_registry_list(registry, AI_RESOURCE_COMMAND));
}

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

static void
test_scan_is_idempotent(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	GList                        *list;

	reset();
	write_home(".claude/commands/a.md", "One.\n");

	registry = fresh_registry();
	ai_resource_registry_scan(registry);
	ai_resource_registry_scan(registry);

	list = ai_resource_registry_list(registry, AI_RESOURCE_COMMAND);
	g_assert_cmpint(g_list_length(list), ==, 1);
	g_list_free(list);

	g_assert_null(ai_resource_registry_list_shadowed(registry));
}

static void
test_deleted_file_disappears(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autofree gchar             *path = NULL;

	reset();
	write_home(".claude/commands/temp.md", "Here for now.\n");

	registry = fresh_registry();
	g_assert_nonnull(ai_resource_registry_lookup(registry,
	                                             AI_RESOURCE_COMMAND,
	                                             "temp"));

	path = g_build_filename(sandbox_home, ".claude", "commands", "temp.md",
	                        NULL);
	g_assert_cmpint(g_unlink(path), ==, 0);

	ai_resource_registry_scan(registry);
	g_assert_null(ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                          "temp"));
}

static void
test_changing_working_directory_rescans(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autofree gchar             *other = NULL;

	reset();
	write_project(".claude/commands/here.md", "In the project.\n");

	other = g_build_filename(sandbox_home, "elsewhere", NULL);
	g_assert_cmpint(g_mkdir_with_parents(other, 0755), ==, 0);

	registry = fresh_registry();
	g_assert_nonnull(ai_resource_registry_lookup(registry,
	                                             AI_RESOURCE_COMMAND,
	                                             "here"));

	/* Moving to another project changes which commands apply, so the set
	 * has to be rebuilt rather than kept. */
	ai_resource_registry_set_working_directory(registry, other);
	g_assert_null(ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND,
	                                          "here"));

	ai_resource_registry_set_working_directory(registry, sandbox_project);
	g_assert_nonnull(ai_resource_registry_lookup(registry,
	                                             AI_RESOURCE_COMMAND,
	                                             "here"));
}

static void
test_list_is_transfer_container(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	GList                        *list;
	AiResource                   *first;

	reset();
	write_home(".claude/commands/a.md", "One.\n");
	write_home(".claude/commands/b.md", "Two.\n");

	registry = fresh_registry();

	list = ai_resource_registry_list(registry, AI_RESOURCE_COMMAND);
	g_assert_cmpint(g_list_length(list), ==, 2);

	/* Sorted by name, so a listing is stable between runs. */
	g_assert_cmpstr(ai_resource_get_name(list->data), ==, "a");
	g_assert_cmpstr(ai_resource_get_name(list->next->data), ==, "b");

	first = list->data;
	g_list_free(list);

	/* Freeing the list must not have freed the resources. */
	g_assert_cmpstr(ai_resource_get_name(first), ==, "a");
}

static void
test_add_follows_first_writer_wins(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autoptr(AiResource)         one = NULL;
	g_autoptr(AiResource)         two = NULL;

	reset();
	registry = fresh_registry();

	one = ai_resource_new_from_data("---\ndescription: first\n---\nx\n", -1,
	                                "dup", AI_RESOURCE_COMMAND, "ai-glib",
	                                AI_RESOURCE_SCOPE_BUILTIN, NULL);
	two = ai_resource_new_from_data("---\ndescription: second\n---\nx\n", -1,
	                                "dup", AI_RESOURCE_COMMAND, "ai-glib",
	                                AI_RESOURCE_SCOPE_BUILTIN, NULL);

	ai_resource_registry_add(registry, one);
	ai_resource_registry_add(registry, two);

	g_assert_cmpstr(ai_resource_get_description(
		ai_resource_registry_lookup(registry, AI_RESOURCE_COMMAND, "dup")),
		==, "first");

	{
		GList *shadowed = ai_resource_registry_list_shadowed(registry);

		g_assert_cmpint(g_list_length(shadowed), ==, 1);
		g_list_free(shadowed);
	}
}

static void
test_search_paths_are_reportable(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_auto(GStrv)                 paths = NULL;
	guint                         i;
	gboolean                      saw_project = FALSE;
	gboolean                      saw_user = FALSE;

	reset();
	registry = fresh_registry();

	/* What a /commands listing shows when the answer to "why is my file
	 * not found" is that it is in the wrong directory. */
	paths = ai_resource_registry_get_search_paths(registry,
	                                              AI_RESOURCE_COMMAND);
	g_assert_nonnull(paths);
	g_assert_cmpint(g_strv_length(paths), >, 4);

	for (i = 0; paths[i] != NULL; i++)
	{
		if (g_str_has_prefix(paths[i], sandbox_project))
		{
			saw_project = TRUE;

			/* Project paths come first, in precedence order. */
			g_assert_false(saw_user);
		}
		else if (g_str_has_prefix(paths[i], sandbox_home))
		{
			saw_user = TRUE;
		}
	}

	g_assert_true(saw_project);
	g_assert_true(saw_user);
}

/* ----------------------------------------------------------------
 * Signals and watching
 * ---------------------------------------------------------------- */

typedef struct
{
	guint       changed;
	GMainLoop  *loop;
} Watcher;

static void
on_changed(AiResourceRegistry *registry, gpointer user_data)
{
	Watcher *w = user_data;

	w->changed++;

	if (w->loop != NULL && g_main_loop_is_running(w->loop))
	{
		g_main_loop_quit(w->loop);
	}
}

static void
test_changed_fires_once_per_scan(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	Watcher                       w = { 0, NULL };

	reset();
	write_home(".claude/commands/a.md", "One.\n");
	write_home(".claude/commands/b.md", "Two.\n");
	write_home(".claude/commands/c.md", "Three.\n");

	registry = ai_resource_registry_new();
	g_signal_connect(registry, "changed", G_CALLBACK(on_changed), &w);

	/* Three files, one notification. A frontend rebuilding a completion
	 * list wants one rebuild for a `git pull`, not thirty. */
	ai_resource_registry_scan(registry);
	g_assert_cmpuint(w.changed, ==, 1);

	ai_resource_registry_scan(registry);
	g_assert_cmpuint(w.changed, ==, 2);
}

static void
test_setting_working_directory_emits_changed(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	Watcher                       w = { 0, NULL };

	reset();
	registry = ai_resource_registry_new();
	g_signal_connect(registry, "changed", G_CALLBACK(on_changed), &w);

	ai_resource_registry_set_working_directory(registry, sandbox_project);
	g_assert_cmpuint(w.changed, ==, 1);

	/* Setting the same value again is a no-op, not a second rescan. */
	ai_resource_registry_set_working_directory(registry, sandbox_project);
	g_assert_cmpuint(w.changed, ==, 1);
}

static gboolean
give_up(gpointer user_data)
{
	GMainLoop *loop = user_data;

	g_main_loop_quit(loop);

	return G_SOURCE_REMOVE;
}

static void
test_watching_reports_a_new_file(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autoptr(GMainLoop)          loop = g_main_loop_new(NULL, FALSE);
	Watcher                       w = { 0, NULL };
	guint                         timeout_id;

	reset();
	write_home(".claude/commands/existing.md", "Already here.\n");

	registry = fresh_registry();
	ai_resource_registry_set_watching(registry, TRUE);
	g_assert_true(ai_resource_registry_get_watching(registry));

	w.loop = loop;
	g_signal_connect(registry, "changed", G_CALLBACK(on_changed), &w);

	write_home(".claude/commands/appeared.md", "New.\n");

	/* A bounded wait, so a monitor that never fires fails the test
	 * instead of hanging the suite. */
	timeout_id = g_timeout_add_seconds(5, give_up, loop);
	g_main_loop_run(loop);
	g_source_remove(timeout_id);

	g_assert_cmpuint(w.changed, >, 0);
	g_assert_nonnull(ai_resource_registry_lookup(registry,
	                                             AI_RESOURCE_COMMAND,
	                                             "appeared"));

	ai_resource_registry_set_watching(registry, FALSE);
	g_assert_false(ai_resource_registry_get_watching(registry));
}

static void
test_watching_survives_finalization_with_events_pending(void)
{
	AiResourceRegistry *registry;

	reset();
	write_home(".claude/commands/a.md", "One.\n");

	registry = ai_resource_registry_new();
	ai_resource_registry_set_working_directory(registry, sandbox_project);
	ai_resource_registry_set_watching(registry, TRUE);

	/* Touch a watched directory, then destroy the registry before the
	 * debounce fires. A timeout that outlived the object would run
	 * against freed memory -- which is exactly what ASAN is here for. */
	write_home(".claude/commands/b.md", "Two.\n");

	g_object_unref(registry);

	{
		guint i;

		for (i = 0; i < 50; i++)
		{
			g_main_context_iteration(NULL, FALSE);
		}
	}
}

static void
test_watching_toggles_cleanly(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;

	reset();
	registry = fresh_registry();

	g_assert_false(ai_resource_registry_get_watching(registry));

	ai_resource_registry_set_watching(registry, TRUE);
	ai_resource_registry_set_watching(registry, TRUE);
	ai_resource_registry_set_watching(registry, FALSE);
	ai_resource_registry_set_watching(registry, FALSE);

	g_assert_false(ai_resource_registry_get_watching(registry));
}

static void
test_properties(void)
{
	g_autoptr(AiResourceRegistry) registry = NULL;
	g_autofree gchar             *cwd = NULL;
	gboolean                      watching = TRUE;

	reset();
	registry = ai_resource_registry_new();

	g_object_get(registry,
	             "working-directory", &cwd,
	             "watching", &watching,
	             NULL);

	/* Defaults: the current directory, and no inotify watches left behind
	 * by a registry built to answer one question. */
	g_assert_nonnull(cwd);
	g_assert_false(watching);

	g_object_set(registry, "working-directory", sandbox_project, NULL);
	g_assert_cmpstr(ai_resource_registry_get_working_directory(registry), ==,
	                sandbox_project);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *config = NULL;
	GError           *error = NULL;
	int               status;

	/*
	 * HOME and XDG_CONFIG_HOME are redirected before g_test_init, because
	 * GLib caches both on first use and GTest itself may ask. Everything
	 * below therefore runs against a tree this file created.
	 */
	sandbox_home = g_dir_make_tmp("ai-glib-registry-XXXXXX", &error);
	g_assert_no_error(error);

	config = g_build_filename(sandbox_home, ".config", NULL);
	sandbox_project = g_build_filename(sandbox_home, "project", NULL);

	g_setenv("HOME", sandbox_home, TRUE);
	g_setenv("XDG_CONFIG_HOME", config, TRUE);

	g_test_init(&argc, &argv, NULL);

	/* If GLib had already cached a different home, every precedence test
	 * below would silently test nothing. Fail loudly instead. */
	g_assert_cmpstr(g_get_home_dir(), ==, sandbox_home);
	g_assert_cmpstr(g_get_user_config_dir(), ==, config);

	g_test_add_func("/ai-glib/registry/empty", test_empty_registry);
	g_test_add_func("/ai-glib/registry/user-command",
	                test_finds_user_command);
	g_test_add_func("/ai-glib/registry/project-command",
	                test_finds_project_command);
	g_test_add_func("/ai-glib/registry/nested-skill", test_finds_nested_skill);
	g_test_add_func("/ai-glib/registry/flat-and-nested",
	                test_finds_flat_and_nested_skills_together);
	g_test_add_func("/ai-glib/registry/namespaced", test_namespaced_command);
	g_test_add_func("/ai-glib/registry/marker-not-doubled",
	                test_skill_marker_is_not_listed_twice);
	g_test_add_func("/ai-glib/registry/kinds-coexist",
	                test_kinds_do_not_collide);
	g_test_add_func("/ai-glib/registry/agents", test_agents_from_both_harnesses);

	g_test_add_func("/ai-glib/registry/project-shadows-user",
	                test_project_shadows_user);
	g_test_add_func("/ai-glib/registry/ai-glib-first",
	                test_ai_glib_wins_within_a_scope);
	g_test_add_func("/ai-glib/registry/scope-beats-order",
	                test_scope_beats_table_order);
	g_test_add_func("/ai-glib/registry/opencode-two-roots",
	                test_opencode_home_and_xdg_skills);

	g_test_add_func("/ai-glib/registry/missing-dirs",
	                test_missing_directories_are_normal);
	g_test_add_func("/ai-glib/registry/file-not-dir",
	                test_file_where_a_directory_was_expected);
	g_test_add_func("/ai-glib/registry/unreadable-dir",
	                test_unreadable_directory_is_skipped);
	g_test_add_func("/ai-glib/registry/one-bad-file",
	                test_one_bad_file_does_not_hide_the_others);
	g_test_add_func("/ai-glib/registry/non-markdown",
	                test_non_markdown_files_ignored);
	g_test_add_func("/ai-glib/registry/dir-without-marker",
	                test_directory_without_a_marker_is_ignored);
	g_test_add_func("/ai-glib/registry/symlink-loop",
	                test_symlink_loop_terminates);
	g_test_add_func("/ai-glib/registry/empty-name",
	                test_resource_with_empty_name_is_skipped);

	g_test_add_func("/ai-glib/registry/idempotent", test_scan_is_idempotent);
	g_test_add_func("/ai-glib/registry/deleted", test_deleted_file_disappears);
	g_test_add_func("/ai-glib/registry/cwd-rescans",
	                test_changing_working_directory_rescans);
	g_test_add_func("/ai-glib/registry/list-container",
	                test_list_is_transfer_container);
	g_test_add_func("/ai-glib/registry/add-first-wins",
	                test_add_follows_first_writer_wins);
	g_test_add_func("/ai-glib/registry/search-paths",
	                test_search_paths_are_reportable);

	g_test_add_func("/ai-glib/registry/changed-once",
	                test_changed_fires_once_per_scan);
	g_test_add_func("/ai-glib/registry/cwd-emits",
	                test_setting_working_directory_emits_changed);
	g_test_add_func("/ai-glib/registry/watch-new-file",
	                test_watching_reports_a_new_file);
	g_test_add_func("/ai-glib/registry/watch-finalize",
	                test_watching_survives_finalization_with_events_pending);
	g_test_add_func("/ai-glib/registry/watch-toggle",
	                test_watching_toggles_cleanly);
	g_test_add_func("/ai-glib/registry/properties", test_properties);

	status = g_test_run();

	rm_rf(sandbox_home);
	g_free(sandbox_home);
	g_free(sandbox_project);

	return status;
}
