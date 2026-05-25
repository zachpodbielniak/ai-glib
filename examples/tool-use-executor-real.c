/*
 * tool-use-executor-real.c - End-to-end smoke test for built-in tools
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises the BUILT-IN tools shipped with AiToolExecutor (bash, write,
 * read) against any HTTP provider. The prompt asks the model to:
 *
 *   1. Run `ls -lah` and report the output.
 *   2. Write a marker line to a tmp file whose path we tell it.
 *   3. Read the tmp file back and report its contents.
 *
 * We print the tmp file path at the end so you can `cat` it manually and
 * confirm the model really did invoke the write tool (not just hallucinate
 * the output).
 *
 * Usage:
 *   export ANTHROPIC_API_KEY="..."   # or OPENAI_API_KEY / GEMINI_API_KEY / ...
 *   ./tool-use-executor-real <provider>
 *
 *   AI_MODEL=qwen3:14b ./tool-use-executor-real ollama
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai-glib.h"

static AiProvider *
make_provider(const gchar *name)
{
	if (g_strcmp0(name, "openai") == 0)  return AI_PROVIDER(ai_openai_client_new());
	if (g_strcmp0(name, "grok") == 0)    return AI_PROVIDER(ai_grok_client_new());
	if (g_strcmp0(name, "gemini") == 0)  return AI_PROVIDER(ai_gemini_client_new());
	if (g_strcmp0(name, "ollama") == 0)  return AI_PROVIDER(ai_ollama_client_new());
	if (g_strcmp0(name, "claude") == 0 || name == NULL || name[0] == '\0')
		return AI_PROVIDER(ai_claude_client_new());

	g_printerr("Unknown provider '%s'. Valid: claude, openai, grok, gemini, ollama.\n",
	           name);
	return NULL;
}

int
main(int argc, char *argv[])
{
	g_autoptr(AiProvider)     provider = NULL;
	g_autoptr(AiToolExecutor) exec     = NULL;
	g_autoptr(AiMessage)      msg      = NULL;
	g_autoptr(GError)         err      = NULL;
	g_autofree gchar         *answer   = NULL;
	g_autofree gchar         *tmp_path = NULL;
	g_autofree gchar         *prompt   = NULL;
	g_autofree gchar         *marker   = NULL;
	GList *messages = NULL;
	const gchar *provider_name = (argc > 1) ? argv[1] : "claude";

	provider = make_provider(provider_name);
	if (provider == NULL)
		return 1;

	{
		const gchar *override = g_getenv("AI_MODEL");
		if (override != NULL && AI_IS_CLIENT(provider))
			ai_client_set_model(AI_CLIENT(provider), override);
	}

	exec = ai_tool_executor_new();

	/* Build a unique tmp path the model has to write to.
	 * Including the provider + pid makes it easy to find later. */
	tmp_path = g_strdup_printf("%s/ai-glib-tool-use-real-%s-%d.txt",
	                           g_get_tmp_dir(), provider_name, (int)getpid());

	marker = g_strdup_printf(
		"Hello from %s, written by AI through the write tool.",
		provider_name);

	prompt = g_strdup_printf(
		"Please do three things, in order, by calling the provided tools:\n"
		"\n"
		"1. Run `ls -lah` and report a one-line summary of the output.\n"
		"\n"
		"2. Write EXACTLY the following text (no extra characters) to the "
		"file %s using the write tool:\n"
		"     %s\n"
		"\n"
		"3. Read that same file back using the read tool and quote its "
		"contents in your reply.\n"
		"\n"
		"Use the actual tools — do not guess or fabricate output.",
		tmp_path, marker);

	printf("Provider: %s\n", provider_name);
	printf("Tmp path: %s\n", tmp_path);
	printf("Marker:   %s\n\n", marker);

	msg = ai_message_new_user(prompt);
	messages = g_list_append(NULL, msg);

	answer = ai_tool_executor_run(exec, provider, messages,
	                              "You are a careful operator. Use the "
	                              "bash, write, and read tools to satisfy "
	                              "the user's request literally.",
	                              4096, NULL, &err);
	g_list_free(messages);

	if (err != NULL)
	{
		g_printerr("Error: %s\n", err->message);
		return 1;
	}

	printf("=== Model's final answer ===\n%s\n\n", answer != NULL ? answer : "(empty)");

	printf("=== Verify manually ===\n");
	printf("  cat '%s'\n", tmp_path);
	printf("  (expected contents: %s)\n", marker);

	return 0;
}
