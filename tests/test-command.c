/*
 * test-command.c - Resolving and expanding slash commands
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Resources are added to the registry directly rather than written to
 * disk, so these cases say what they mean without a directory tree in the
 * way; the scanning half is covered in test-resource-registry.c. The
 * exception is the shell-substitution section, which needs a real working
 * directory to run commands in -- and which carries the one assertion in
 * this file that is a security boundary rather than a convenience.
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static gchar *sandbox = NULL;

static void
rm_rf(const gchar *path)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", path);

	g_assert_cmpint(system(cmd), ==, 0);
}

static AiCommandSet *
command_set_with(
	const gchar    *name,
	AiResourceKind  kind,
	const gchar    *contents
){
	AiResourceRegistry   *registry = ai_resource_registry_new();
	AiCommandSet         *set;
	g_autoptr(AiResource) resource = NULL;

	if (name != NULL)
	{
		resource = ai_resource_new_from_data(contents, -1, name, kind,
		                                     "claude",
		                                     AI_RESOURCE_SCOPE_USER, NULL);
		g_assert_nonnull(resource);
		ai_resource_registry_add(registry, resource);
	}

	set = ai_command_set_new(registry);
	g_object_unref(registry);

	return set;
}

/* Resolve @line and assert it produced a prompt; returns the prompt. */
static gchar *
resolve_prompt(AiCommandSet *set, const gchar *line)
{
	g_autoptr(AiCommandResult) result = NULL;
	GError                    *error = NULL;
	gchar                     *prompt;

	result = ai_command_set_resolve(set, line, sandbox, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(ai_command_result_get_outcome(result), ==,
	                AI_COMMAND_OUTCOME_PROMPT);

	prompt = g_strdup(ai_command_result_get_prompt(result));

	return prompt;
}

/* ----------------------------------------------------------------
 * Recognising a command line
 * ---------------------------------------------------------------- */

static void
test_is_command_line(void)
{
	g_assert_true(ai_command_set_is_command_line("/help"));
	g_assert_true(ai_command_set_is_command_line("/deploy now"));
	g_assert_true(ai_command_set_is_command_line("/git:status"));

	g_assert_false(ai_command_set_is_command_line(NULL));
	g_assert_false(ai_command_set_is_command_line(""));
	g_assert_false(ai_command_set_is_command_line("ordinary text"));
	g_assert_false(ai_command_set_is_command_line("/"));
	g_assert_false(ai_command_set_is_command_line("/ spaced"));

	/* A path is something a user types about far more often than they
	 * type a command called "usr". */
	g_assert_false(ai_command_set_is_command_line("/usr/bin/thing"));
	g_assert_false(ai_command_set_is_command_line("//comment"));
}

static void
test_ordinary_line_is_not_a_command(void)
{
	g_autoptr(AiCommandSet)    set = command_set_with(NULL, 0, NULL);
	g_autoptr(AiCommandResult) result = NULL;
	GError                    *error = NULL;

	/* Not an error, so a caller can run every line through resolve()
	 * unconditionally instead of pre-testing each one. */
	result = ai_command_set_resolve(set, "just a prompt", NULL, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpint(ai_command_result_get_outcome(result), ==,
	                AI_COMMAND_OUTCOME_NOT_A_COMMAND);
	g_assert_null(ai_command_result_get_prompt(result));
}

/* ----------------------------------------------------------------
 * Built-ins
 * ---------------------------------------------------------------- */

static void
test_builtins_exist_without_a_registry(void)
{
	g_autoptr(AiCommandSet) set = ai_command_set_new(NULL);
	g_autoptr(AiCommand)    help = NULL;
	GList                  *list;

	/* A command set with no registry still has something to complete,
	 * which is what an embedder that will not read the user's home
	 * directory gets. */
	help = ai_command_set_lookup(set, "help");
	g_assert_nonnull(help);
	g_assert_cmpint(ai_command_get_kind(help), ==, AI_COMMAND_BUILTIN);
	g_assert_cmpstr(ai_command_get_origin(help), ==, "ai-glib");

	list = ai_command_set_list(set);
	g_assert_cmpint(g_list_length(list), >=, 14);
	g_list_free_full(list, g_object_unref);
}

static void
test_builtin_resolves_to_builtin(void)
{
	g_autoptr(AiCommandSet)    set = command_set_with(NULL, 0, NULL);
	g_autoptr(AiCommandResult) result = NULL;
	GError                    *error = NULL;

	result = ai_command_set_resolve(set, "/model opus", NULL, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpint(ai_command_result_get_outcome(result), ==,
	                AI_COMMAND_OUTCOME_BUILTIN);
	g_assert_cmpstr(ai_command_result_get_name(result), ==, "model");
	g_assert_cmpstr(ai_command_result_get_arguments(result), ==, "opus");
	g_assert_null(ai_command_result_get_prompt(result));
}

static void
test_builtin_beats_a_file(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("quit", AI_RESOURCE_COMMAND,
		                 "---\ndescription: not the real one\n---\nbody\n");
	g_autoptr(AiCommand)    command = ai_command_set_lookup(set, "quit");
	GList                  *list;
	GList                  *iter;
	guint                   quits = 0;

	/*
	 * Not politeness. A stray quit.md in a scanned directory must not be
	 * able to take away the way out of the program.
	 */
	g_assert_cmpint(ai_command_get_kind(command), ==, AI_COMMAND_BUILTIN);

	/* And the shadowed file is not offered twice in a listing. */
	list = ai_command_set_list(set);

	for (iter = list; iter != NULL; iter = iter->next)
	{
		if (g_strcmp0(ai_command_get_name(iter->data), "quit") == 0)
		{
			quits++;
		}
	}

	g_assert_cmpuint(quits, ==, 1);
	g_list_free_full(list, g_object_unref);
}

static void
test_unknown_command_errors_with_suggestions(void)
{
	g_autoptr(AiCommandSet)    set = command_set_with(NULL, 0, NULL);
	AiCommandResult           *result;
	GError                    *error = NULL;

	result = ai_command_set_resolve(set, "/hel", NULL, NULL, &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "/hel"));

	/* Naming the near misses is the difference between an error a user
	 * can act on and one they have to go looking for. */
	g_assert_nonnull(strstr(error->message, "help"));
	g_clear_error(&error);
}

static void
test_unknown_command_with_no_near_misses(void)
{
	g_autoptr(AiCommandSet) set = command_set_with(NULL, 0, NULL);
	AiCommandResult        *result;
	GError                 *error = NULL;

	result = ai_command_set_resolve(set, "/zzzzz", NULL, NULL, &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_command_names_are_case_sensitive(void)
{
	g_autoptr(AiCommandSet) set = command_set_with(NULL, 0, NULL);
	AiCommandResult        *result;
	GError                 *error = NULL;

	/* Asserted so the rule is a decision rather than an accident. */
	{
		g_autoptr(AiCommand) lower = ai_command_set_lookup(set, "help");

		g_assert_nonnull(lower);
	}

	g_assert_null(ai_command_set_lookup(set, "Help"));

	result = ai_command_set_resolve(set, "/HELP", NULL, NULL, &error);
	g_assert_null(result);
	g_clear_error(&error);
}

/* ----------------------------------------------------------------
 * File-backed commands and skills
 * ---------------------------------------------------------------- */

static void
test_command_from_a_file(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("deploy", AI_RESOURCE_COMMAND,
		                 "---\ndescription: Ship it\nargument-hint: <env>\n"
		                 "---\nDeploy to production.\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/deploy");
	g_autoptr(AiCommand)    command = ai_command_set_lookup(set, "deploy");

	g_assert_cmpstr(prompt, ==, "Deploy to production.\n");
	g_assert_cmpstr(ai_command_get_description(command), ==, "Ship it");
	g_assert_cmpstr(ai_command_get_argument_hint(command), ==, "<env>");
	g_assert_cmpstr(ai_command_get_origin(command), ==, "claude");
	g_assert_nonnull(ai_command_get_resource(command));
}

static void
test_skill_resolves_like_a_command(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("skill-gtest-scaffold", AI_RESOURCE_SKILL,
		                 "---\ndescription: Scaffold tests\n---\n"
		                 "Generate a GTest file for $ARGUMENTS.\n");
	g_autofree gchar       *prompt =
		resolve_prompt(set, "/skill-gtest-scaffold src/foo.c");

	/* No skill-specific code anywhere: body first, then the arguments,
	 * exactly as for a command. */
	g_assert_cmpstr(prompt, ==, "Generate a GTest file for src/foo.c.\n");
}

static void
test_agent_resolves_to_agent(void)
{
	g_autoptr(AiCommandSet)    set =
		command_set_with("security-auditor", AI_RESOURCE_AGENT,
		                 "---\ntools: Bash, Read\n---\nAudit $ARGUMENTS.\n");
	g_autoptr(AiCommandResult) result = NULL;
	GError                    *error = NULL;

	result = ai_command_set_resolve(set, "/security-auditor src/", sandbox,
	                                NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpint(ai_command_result_get_outcome(result), ==,
	                AI_COMMAND_OUTCOME_AGENT);
	g_assert_cmpstr(ai_command_result_get_prompt(result), ==, "Audit src/.\n");
}

static void
test_empty_body_is_allowed(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("empty", AI_RESOURCE_COMMAND, "---\nname: empty\n---\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/empty");

	g_assert_cmpstr(prompt, ==, "");
}

/* ----------------------------------------------------------------
 * Argument splitting
 * ---------------------------------------------------------------- */

static void
test_split_arguments(void)
{
	g_auto(GStrv) plain = ai_command_split_arguments("a b c");
	g_auto(GStrv) runs = ai_command_split_arguments("  a    b  ");
	g_auto(GStrv) none = ai_command_split_arguments("");
	g_auto(GStrv) null_in = ai_command_split_arguments(NULL);

	g_assert_cmpint(g_strv_length(plain), ==, 3);
	g_assert_cmpstr(plain[1], ==, "b");

	g_assert_cmpint(g_strv_length(runs), ==, 2);
	g_assert_cmpstr(runs[0], ==, "a");

	g_assert_cmpint(g_strv_length(none), ==, 0);
	g_assert_cmpint(g_strv_length(null_in), ==, 0);
}

static void
test_split_quoting(void)
{
	g_auto(GStrv) dq = ai_command_split_arguments("\"one two\" three");
	g_auto(GStrv) sq = ai_command_split_arguments("'one two' three");
	g_auto(GStrv) esc = ai_command_split_arguments("one\\ two three");
	g_auto(GStrv) inner = ai_command_split_arguments("\"say \\\"hi\\\"\"");
	g_auto(GStrv) empty_arg = ai_command_split_arguments("a \"\" b");

	g_assert_cmpint(g_strv_length(dq), ==, 2);
	g_assert_cmpstr(dq[0], ==, "one two");

	g_assert_cmpint(g_strv_length(sq), ==, 2);
	g_assert_cmpstr(sq[0], ==, "one two");

	g_assert_cmpint(g_strv_length(esc), ==, 2);
	g_assert_cmpstr(esc[0], ==, "one two");

	g_assert_cmpint(g_strv_length(inner), ==, 1);
	g_assert_cmpstr(inner[0], ==, "say \"hi\"");

	/* An explicitly empty argument is an argument. */
	g_assert_cmpint(g_strv_length(empty_arg), ==, 3);
	g_assert_cmpstr(empty_arg[1], ==, "");
}

static void
test_split_unterminated_quote_recovers(void)
{
	g_auto(GStrv) open = ai_command_split_arguments("a \"still typing");

	/* The user is mid-keystroke; refusing the line would be useless. */
	g_assert_cmpint(g_strv_length(open), ==, 2);
	g_assert_cmpstr(open[1], ==, "still typing");
}

/* ----------------------------------------------------------------
 * Placeholder substitution
 * ---------------------------------------------------------------- */

static void
test_substitute_arguments(void)
{
	g_auto(GStrv)     argv = ai_command_split_arguments("one two");
	g_autofree gchar *out =
		ai_command_substitute("A: $ARGUMENTS, again: $ARGUMENTS",
		                      "one two", (const gchar *const *)argv);

	g_assert_cmpstr(out, ==, "A: one two, again: one two");
}

static void
test_substitute_positional(void)
{
	g_auto(GStrv)     argv = ai_command_split_arguments("alpha beta gamma");
	g_autofree gchar *out =
		ai_command_substitute("$3/$1/$2", "alpha beta gamma",
		                      (const gchar *const *)argv);

	g_assert_cmpstr(out, ==, "gamma/alpha/beta");
}

static void
test_substitute_missing_becomes_empty(void)
{
	g_auto(GStrv)     argv = ai_command_split_arguments("only");
	g_autofree gchar *out =
		ai_command_substitute("[$1][$2][$9]", "only",
		                      (const gchar *const *)argv);

	/* A command written for two arguments and invoked with one must not
	 * send the model a stray "$2" to puzzle over. */
	g_assert_cmpstr(out, ==, "[only][][]");
}

static void
test_substitute_no_arguments_at_all(void)
{
	g_autofree gchar *out =
		ai_command_substitute("[$ARGUMENTS][$1]", NULL, NULL);

	g_assert_cmpstr(out, ==, "[][]");
}

static void
test_substitute_leaves_shell_variables_alone(void)
{
	g_auto(GStrv)     argv = ai_command_split_arguments("x");
	g_autofree gchar *out =
		ai_command_substitute("$HOME and $0 and $ and $$ and $10", "x",
		                      (const gchar *const *)argv);

	/*
	 * Command bodies contain shell snippets. Rewriting $HOME inside one
	 * would be worse than useless, so only $ARGUMENTS, $1-$9 and $$ mean
	 * anything -- and $10 is $1 followed by a zero, which this pins down.
	 */
	g_assert_cmpstr(out, ==, "$HOME and $0 and $ and $ and x0");
}

static void
test_substitute_multibyte(void)
{
	g_auto(GStrv)     argv = ai_command_split_arguments("café");
	g_autofree gchar *out =
		ai_command_substitute("naïve $1 —", "café",
		                      (const gchar *const *)argv);

	g_assert_cmpstr(out, ==, "naïve café —");
	g_assert_true(g_utf8_validate(out, -1, NULL));
}

/* ----------------------------------------------------------------
 * Shell substitution --- the security boundary
 * ---------------------------------------------------------------- */

static void
test_shell_is_literal_without_opt_in(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("status", AI_RESOURCE_COMMAND,
		                 "---\nname: status\n---\nHere: !`echo RAN_IT`\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/status");

	/*
	 * The one assertion in this file that is a security boundary. Any
	 * process that can drop a file into ~/.claude/commands would
	 * otherwise be able to run code the next time a listing is built --
	 * and building a listing is something a frontend does at startup,
	 * unprompted.
	 */
	g_assert_cmpstr(prompt, ==, "Here: !`echo RAN_IT`\n");
	g_assert_null(strstr(prompt, "RAN_IT\n"));
}

static void
test_shell_runs_when_the_file_opts_in(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("status", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\nHere: !`echo RAN_IT`\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/status");

	g_assert_cmpstr(prompt, ==, "Here: RAN_IT\n");
}

static void
test_shell_policy_never_overrides_the_file(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("status", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n!`echo RAN_IT`\n");
	g_autofree gchar       *prompt = NULL;

	/* An embedder that does not trust the directories being scanned gets
	 * the final word; the file does not. */
	ai_command_set_set_shell_policy(set, AI_COMMAND_SHELL_NEVER);
	g_assert_cmpint(ai_command_set_get_shell_policy(set), ==,
	                AI_COMMAND_SHELL_NEVER);

	prompt = resolve_prompt(set, "/status");
	g_assert_cmpstr(prompt, ==, "!`echo RAN_IT`\n");
}

static void
test_shell_policy_always(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("status", AI_RESOURCE_COMMAND,
		                 "---\nname: status\n---\n!`echo RAN_IT`\n");
	g_autofree gchar       *prompt = NULL;

	ai_command_set_set_shell_policy(set, AI_COMMAND_SHELL_ALWAYS);
	prompt = resolve_prompt(set, "/status");

	g_assert_cmpstr(prompt, ==, "RAN_IT\n");
}

static void
test_shell_failure_does_not_fail_the_expansion(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n"
		                 "before !`exit 3` after\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s");

	/* A command body embedding `git status` in a repository with no
	 * commits should still produce a prompt. */
	g_assert_cmpstr(prompt, ==, "before  after\n");
}

static void
test_shell_stderr_is_not_folded_in(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n"
		                 "[!`echo out; echo err >&2`]\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s");

	/* Quietly pasting a warning into a prompt is worse than dropping it:
	 * the author asked for the command's output, not its diagnostics. */
	g_assert_cmpstr(prompt, ==, "[out]\n");
}

static void
test_shell_runs_in_the_working_directory(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n!`pwd`\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s");

	g_assert_nonnull(strstr(prompt, sandbox));
}

static void
test_shell_unterminated_backtick_is_literal(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\ntext !`never closed\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s");

	g_assert_cmpstr(prompt, ==, "text !`never closed\n");
}

static void
test_shell_multiple_substitutions(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n!`echo one`-!`echo two`\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s");

	g_assert_cmpstr(prompt, ==, "one-two\n");
}

static void
test_shell_sees_substituted_arguments(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n!`echo $1`\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s HELLO");

	/* An argument reaches the command as the shell's own $1, so a body
	 * can act on what it was given rather than only quoting it. */
	g_assert_cmpstr(prompt, ==, "HELLO\n");
}

static void
test_shell_sees_the_arguments_variable(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n!`echo $ARGUMENTS`\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s one two");

	/* The whole argument string reaches it too, as an environment
	 * variable rather than as text pasted into the script. */
	g_assert_cmpstr(prompt, ==, "one two\n");
}

/*
 * A body that opted into running *its own* commands did not thereby opt
 * into running the caller's.
 *
 * `shell: true` is a statement about the file: its author wrote the
 * backticks and meant them. Arguments were interpolated into the body
 * first and the *result* was scanned for `` !` ``, so anybody who could
 * invoke the command could put backticks in an argument and have them
 * executed by a file that contains none. /summarise is the shape that
 * makes it obvious -- a command whose entire body is "summarise this"
 * has no shell in it at all, and ran `id` anyway.
 */
static void
test_shell_does_not_run_from_an_argument(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("summarise", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n"
		                 "Please summarise: $ARGUMENTS\n");
	g_autofree gchar       *prompt =
		resolve_prompt(set, "/summarise !`echo PWNED` please");

	g_assert_cmpstr(prompt, ==,
	                "Please summarise: !`echo PWNED` please\n");
	g_assert_null(strstr(prompt, "PWNED\n"));
}

/* The same through $1, which is the other way argument text arrives. */
static void
test_shell_does_not_run_from_a_positional_argument(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n[$1]\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s '!`echo PWNED`'");

	g_assert_cmpstr(prompt, ==, "[!`echo PWNED`]\n");
}

/*
 * And an argument used *inside* the file's own command is one word to
 * the shell, not more of the script.
 *
 * Pasting the argument into the command text made `;`, `|` and `$( )`
 * the caller's to write; the argument is a positional parameter now, so
 * the shell never re-parses it.
 */
static void
test_shell_argument_cannot_extend_the_command(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n[!`echo $1`]\n");
	g_autofree gchar       *prompt =
		resolve_prompt(set, "/s 'x; echo PWNED'");

	g_assert_cmpstr(prompt, ==, "[x; echo PWNED]\n");
}

/*
 * What a command printed is text, not a template.
 *
 * Expanding placeholders after the shell ran would let a price list
 * saying "$5" lose it to an empty $5, and would spend an argument
 * wherever the command's own output happened to say $1.
 */
static void
test_shell_output_is_not_scanned_for_placeholders(void)
{
	g_autoptr(AiCommandSet) set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n!`echo 'costs $5'`\n");
	g_autofree gchar       *prompt = resolve_prompt(set, "/s alpha");

	g_assert_cmpstr(prompt, ==, "costs $5\n");
}

static void
test_shell_cancellation(void)
{
	g_autoptr(AiCommandSet)    set =
		command_set_with("s", AI_RESOURCE_COMMAND,
		                 "---\nshell: true\n---\n[!`sleep 30; echo late`]\n");
	g_autoptr(GCancellable)    cancellable = g_cancellable_new();
	g_autoptr(AiCommandResult) result = NULL;
	GError                    *error = NULL;

	g_cancellable_cancel(cancellable);

	result = ai_command_set_resolve(set, "/s", sandbox, cancellable, &error);

	/* Cancelling drops the substitution's output, and the rest of the
	 * body still comes through -- a half-expanded prompt is more useful
	 * than none. */
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpstr(ai_command_result_get_prompt(result), ==, "[]\n");
}

/* ----------------------------------------------------------------
 * Listing and lookup
 * ---------------------------------------------------------------- */

static void
test_list_includes_every_kind(void)
{
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiCommandSet)       set = NULL;
	GList                        *list;
	GList                        *iter;
	gboolean                      saw_command = FALSE;
	gboolean                      saw_skill = FALSE;
	gboolean                      saw_agent = FALSE;

	{
		const struct
		{
			const gchar   *name;
			AiResourceKind kind;
		} entries[] = {
			{ "a-command", AI_RESOURCE_COMMAND },
			{ "a-skill",   AI_RESOURCE_SKILL },
			{ "an-agent",  AI_RESOURCE_AGENT }
		};
		gsize i;

		for (i = 0; i < G_N_ELEMENTS(entries); i++)
		{
			g_autoptr(AiResource) resource =
				ai_resource_new_from_data("body", -1, entries[i].name,
				                          entries[i].kind, "claude",
				                          AI_RESOURCE_SCOPE_USER, NULL);

			ai_resource_registry_add(registry, resource);
		}
	}

	set = ai_command_set_new(registry);
	list = ai_command_set_list(set);

	for (iter = list; iter != NULL; iter = iter->next)
	{
		const gchar *name = ai_command_get_name(iter->data);

		if (g_strcmp0(name, "a-command") == 0)
		{
			saw_command = TRUE;
		}
		else if (g_strcmp0(name, "a-skill") == 0)
		{
			saw_skill = TRUE;
		}
		else if (g_strcmp0(name, "an-agent") == 0)
		{
			saw_agent = TRUE;
			g_assert_cmpint(ai_command_get_kind(iter->data), ==,
			                AI_COMMAND_AGENT);
		}
	}

	g_assert_true(saw_command);
	g_assert_true(saw_skill);
	g_assert_true(saw_agent);

	/* Sorted, so a /help listing is stable between runs. */
	g_assert_cmpstr(ai_command_get_name(list->data), <=,
	                ai_command_get_name(list->next->data));

	g_list_free_full(list, g_object_unref);
}

static void
test_command_wins_over_skill_of_the_same_name(void)
{
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiCommandSet)       set = NULL;
	g_autoptr(AiCommand)          found = NULL;

	{
		g_autoptr(AiResource) as_skill =
			ai_resource_new_from_data("---\ndescription: skill\n---\nS\n", -1,
			                          "dual", AI_RESOURCE_SKILL, "claude",
			                          AI_RESOURCE_SCOPE_USER, NULL);
		g_autoptr(AiResource) as_command =
			ai_resource_new_from_data("---\ndescription: command\n---\nC\n",
			                          -1, "dual", AI_RESOURCE_COMMAND,
			                          "claude", AI_RESOURCE_SCOPE_USER, NULL);

		ai_resource_registry_add(registry, as_skill);
		ai_resource_registry_add(registry, as_command);
	}

	set = ai_command_set_new(registry);
	found = ai_command_set_lookup(set, "dual");

	g_assert_cmpstr(ai_command_get_description(found), ==, "command");
}

static void
test_properties(void)
{
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiCommandSet)       set = ai_command_set_new(registry);
	g_autoptr(AiResourceRegistry) read_back = NULL;
	gint                          policy = -1;

	g_object_get(set, "registry", &read_back, "shell-policy", &policy, NULL);

	g_assert_true(read_back == registry);
	g_assert_cmpint(policy, ==, AI_COMMAND_SHELL_OPT_IN);
	g_assert_true(ai_command_set_get_registry(set) == registry);

	g_object_set(set, "shell-policy", AI_COMMAND_SHELL_NEVER, NULL);
	g_assert_cmpint(ai_command_set_get_shell_policy(set), ==,
	                AI_COMMAND_SHELL_NEVER);
}

int
main(int argc, char *argv[])
{
	GError *error = NULL;
	int     status;

	g_test_init(&argc, &argv, NULL);

	sandbox = g_dir_make_tmp("ai-glib-command-XXXXXX", &error);
	g_assert_no_error(error);

	g_test_add_func("/ai-glib/command/is-command-line", test_is_command_line);
	g_test_add_func("/ai-glib/command/not-a-command",
	                test_ordinary_line_is_not_a_command);

	g_test_add_func("/ai-glib/command/builtins-without-registry",
	                test_builtins_exist_without_a_registry);
	g_test_add_func("/ai-glib/command/builtin-outcome",
	                test_builtin_resolves_to_builtin);
	g_test_add_func("/ai-glib/command/builtin-beats-file",
	                test_builtin_beats_a_file);
	g_test_add_func("/ai-glib/command/unknown-suggests",
	                test_unknown_command_errors_with_suggestions);
	g_test_add_func("/ai-glib/command/unknown-no-suggestions",
	                test_unknown_command_with_no_near_misses);
	g_test_add_func("/ai-glib/command/case-sensitive",
	                test_command_names_are_case_sensitive);

	g_test_add_func("/ai-glib/command/from-file", test_command_from_a_file);
	g_test_add_func("/ai-glib/command/skill", test_skill_resolves_like_a_command);
	g_test_add_func("/ai-glib/command/agent", test_agent_resolves_to_agent);
	g_test_add_func("/ai-glib/command/empty-body", test_empty_body_is_allowed);

	g_test_add_func("/ai-glib/command/split", test_split_arguments);
	g_test_add_func("/ai-glib/command/split-quoting", test_split_quoting);
	g_test_add_func("/ai-glib/command/split-unterminated",
	                test_split_unterminated_quote_recovers);

	g_test_add_func("/ai-glib/command/substitute-arguments",
	                test_substitute_arguments);
	g_test_add_func("/ai-glib/command/substitute-positional",
	                test_substitute_positional);
	g_test_add_func("/ai-glib/command/substitute-missing",
	                test_substitute_missing_becomes_empty);
	g_test_add_func("/ai-glib/command/substitute-none",
	                test_substitute_no_arguments_at_all);
	g_test_add_func("/ai-glib/command/substitute-shell-vars",
	                test_substitute_leaves_shell_variables_alone);
	g_test_add_func("/ai-glib/command/substitute-multibyte",
	                test_substitute_multibyte);

	g_test_add_func("/ai-glib/command/shell-literal-by-default",
	                test_shell_is_literal_without_opt_in);
	g_test_add_func("/ai-glib/command/shell-opt-in",
	                test_shell_runs_when_the_file_opts_in);
	g_test_add_func("/ai-glib/command/shell-policy-never",
	                test_shell_policy_never_overrides_the_file);
	g_test_add_func("/ai-glib/command/shell-policy-always",
	                test_shell_policy_always);
	g_test_add_func("/ai-glib/command/shell-failure",
	                test_shell_failure_does_not_fail_the_expansion);
	g_test_add_func("/ai-glib/command/shell-stderr",
	                test_shell_stderr_is_not_folded_in);
	g_test_add_func("/ai-glib/command/shell-cwd",
	                test_shell_runs_in_the_working_directory);
	g_test_add_func("/ai-glib/command/shell-unterminated",
	                test_shell_unterminated_backtick_is_literal);
	g_test_add_func("/ai-glib/command/shell-multiple",
	                test_shell_multiple_substitutions);
	g_test_add_func("/ai-glib/command/shell-after-substitution",
	                test_shell_sees_substituted_arguments);
	g_test_add_func("/ai-glib/command/shell-arguments-variable",
	                test_shell_sees_the_arguments_variable);
	g_test_add_func("/ai-glib/command/shell-not-from-an-argument",
	                test_shell_does_not_run_from_an_argument);
	g_test_add_func("/ai-glib/command/shell-not-from-a-positional",
	                test_shell_does_not_run_from_a_positional_argument);
	g_test_add_func("/ai-glib/command/shell-argument-is-one-word",
	                test_shell_argument_cannot_extend_the_command);
	g_test_add_func("/ai-glib/command/shell-output-is-text",
	                test_shell_output_is_not_scanned_for_placeholders);
	g_test_add_func("/ai-glib/command/shell-cancel", test_shell_cancellation);

	g_test_add_func("/ai-glib/command/list-every-kind",
	                test_list_includes_every_kind);
	g_test_add_func("/ai-glib/command/command-beats-skill",
	                test_command_wins_over_skill_of_the_same_name);
	g_test_add_func("/ai-glib/command/properties", test_properties);

	status = g_test_run();

	rm_rf(sandbox);
	g_free(sandbox);

	return status;
}
