/*
 * ai.c - command-line front-end for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A small, dependency-free CLI to drive every ai-glib provider by hand:
 * pick a provider and model, send a prompt (from argv or stdin), and print
 * the response to stdout. Built primarily to exercise the providers --
 * including the Ollama-as-transport rewrite for the claude-code /
 * claude-tmux CLI providers (model "ollama/<name>") -- outside the cmacs
 * and libreclaw integrations.
 *
 * Usage:
 *   ai [OPTIONS] [PROMPT]
 *   echo "PROMPT" | ai [OPTIONS]
 *
 * Notable flags:
 *   --dry-run         print the argv that WOULD be spawned (CLI providers)
 *   --list-providers  list known provider names
 *   --stream          stream tokens live (providers that support it)
 *   --interactive     multi-turn REPL keeping history
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai-glib.h"

/* Private seams: assert/print the exact argv, incl. the Ollama rewrite. */
#include "providers/ai-claude-code-client-internal.h"
#include "providers/ai-claude-tmux-client-internal.h"
#include "providers/ai-claude-launch.h"

/* ---------------------------------------------------------------- */
/* Options                                                           */
/* ---------------------------------------------------------------- */

static gchar    *opt_provider        = NULL;
static gchar    *opt_model           = NULL;
static gchar    *opt_system          = NULL;
static gchar    *opt_effort          = NULL;
static gint      opt_max_tokens      = 4096;
static gdouble   opt_temperature     = -1.0;  /* <0 => leave provider default */
static gboolean  opt_stream          = FALSE;
static gboolean  opt_skip_perms      = FALSE;
static gboolean  opt_dry_run         = FALSE;
static gboolean  opt_list_providers  = FALSE;
static gboolean  opt_interactive     = FALSE;
static gboolean  opt_version         = FALSE;

static const GOptionEntry option_entries[] = {
	{ "provider", 'p', 0, G_OPTION_ARG_STRING, &opt_provider,
	  "Provider: claude, openai, gemini, grok, ollama, claude-code, "
	  "claude-tmux, opencode (default: $AI_PROVIDER or claude)", "NAME" },
	{ "model", 'm', 0, G_OPTION_ARG_STRING, &opt_model,
	  "Model id (default: provider default). For claude-code/claude-tmux, "
	  "an \"ollama/<model>\" id routes via `ollama launch claude`.", "ID" },
	{ "system", 's', 0, G_OPTION_ARG_STRING, &opt_system,
	  "System prompt", "TEXT" },
	{ "effort", 0, 0, G_OPTION_ARG_STRING, &opt_effort,
	  "Effort/thinking level for CLI providers", "LEVEL" },
	{ "max-tokens", 0, 0, G_OPTION_ARG_INT, &opt_max_tokens,
	  "Max output tokens (default: 4096)", "N" },
	{ "temperature", 0, 0, G_OPTION_ARG_DOUBLE, &opt_temperature,
	  "Sampling temperature (HTTP providers)", "F" },
	{ "stream", 0, 0, G_OPTION_ARG_NONE, &opt_stream,
	  "Stream the response as it arrives (when supported)", NULL },
	{ "skip-permissions", 0, 0, G_OPTION_ARG_NONE, &opt_skip_perms,
	  "Pass --dangerously-skip-permissions (claude-code/claude-tmux)", NULL },
	{ "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run,
	  "Print the command that would be spawned, do not run it", NULL },
	{ "list-providers", 0, 0, G_OPTION_ARG_NONE, &opt_list_providers,
	  "List known provider names and exit", NULL },
	{ "interactive", 'i', 0, G_OPTION_ARG_NONE, &opt_interactive,
	  "Interactive multi-turn REPL (keeps history)", NULL },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &opt_version,
	  "Print version and exit", NULL },
	{ NULL, 0, 0, 0, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */

/*
 * Read all of stdin into a newly-allocated NUL-terminated string.
 * Returns NULL (and *len 0) on a read error. (transfer full)
 */
static gchar *
read_all_stdin(void)
{
	GString *buf = g_string_new(NULL);
	gchar    chunk[4096];
	gsize    n;

	while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
	{
		g_string_append_len(buf, chunk, n);
	}

	return g_string_free(buf, FALSE);
}

/* Print an argv (NULL-terminated) as a single, lightly-quoted line. */
static void
print_argv(gchar **argv)
{
	gint i;

	for (i = 0; argv != NULL && argv[i] != NULL; i++)
	{
		const gchar *a = argv[i];
		gboolean needs_quote = (a[0] == '\0') || strpbrk(a, " \t\"'\\") != NULL;

		if (i > 0)
		{
			fputc(' ', stdout);
		}
		if (needs_quote)
		{
			g_autofree gchar *esc = g_strescape(a, NULL);
			printf("\"%s\"", esc);
		}
		else
		{
			fputs(a, stdout);
		}
	}
	fputc('\n', stdout);
}

/*
 * Create the concrete provider object for @ptype using @config and apply
 * the common knobs (model, system prompt, max tokens, temperature, effort,
 * skip-permissions). Returns a floating-free owned GObject, or NULL on a
 * genuinely unknown provider type. (transfer full)
 */
static GObject *
make_provider(AiConfig *config, AiProviderType ptype)
{
	GObject *provider = NULL;

	switch (ptype)
	{
	case AI_PROVIDER_CLAUDE:
		provider = G_OBJECT(ai_claude_client_new_with_config(config));
		break;
	case AI_PROVIDER_OPENAI:
		provider = G_OBJECT(ai_openai_client_new_with_config(config));
		break;
	case AI_PROVIDER_GEMINI:
		provider = G_OBJECT(ai_gemini_client_new_with_config(config));
		break;
	case AI_PROVIDER_GROK:
		provider = G_OBJECT(ai_grok_client_new_with_config(config));
		break;
	case AI_PROVIDER_OLLAMA:
		provider = G_OBJECT(ai_ollama_client_new_with_config(config));
		break;
	case AI_PROVIDER_CLAUDE_CODE:
		provider = G_OBJECT(ai_claude_code_client_new_with_config(config));
		break;
	case AI_PROVIDER_CLAUDE_TMUX:
		provider = G_OBJECT(ai_claude_tmux_client_new_with_config(config));
		break;
	case AI_PROVIDER_OPENCODE:
		provider = G_OBJECT(ai_opencode_client_new_with_config(config));
		break;
	default:
		return NULL;
	}

	/* Apply the knobs to whichever base class this provider is. */
	if (AI_IS_CLIENT(provider))
	{
		AiClient *c = AI_CLIENT(provider);
		if (opt_model != NULL)
			ai_client_set_model(c, opt_model);
		if (opt_system != NULL)
			ai_client_set_system_prompt(c, opt_system);
		ai_client_set_max_tokens(c, opt_max_tokens);
		if (opt_temperature >= 0.0)
			ai_client_set_temperature(c, opt_temperature);
	}
	else if (AI_IS_CLI_CLIENT(provider))
	{
		AiCliClient *c = AI_CLI_CLIENT(provider);
		if (opt_model != NULL)
			ai_cli_client_set_model(c, opt_model);
		if (opt_system != NULL)
			ai_cli_client_set_system_prompt(c, opt_system);
		if (opt_effort != NULL)
			ai_cli_client_set_effort_level(c, opt_effort);
	}

	/* Provider-specific knobs. */
	if (AI_IS_CLAUDE_CODE_CLIENT(provider))
		ai_claude_code_client_set_skip_permissions(
			AI_CLAUDE_CODE_CLIENT(provider), opt_skip_perms);
	else if (AI_IS_CLAUDE_TMUX_CLIENT(provider))
		ai_claude_tmux_client_set_skip_permissions(
			AI_CLAUDE_TMUX_CLIENT(provider), opt_skip_perms);

	return provider;
}

/* ---------------------------------------------------------------- */
/* --list-providers                                                  */
/* ---------------------------------------------------------------- */

static void
list_providers(void)
{
	static const AiProviderType all[] = {
		AI_PROVIDER_CLAUDE, AI_PROVIDER_OPENAI, AI_PROVIDER_GEMINI,
		AI_PROVIDER_GROK, AI_PROVIDER_OLLAMA, AI_PROVIDER_CLAUDE_CODE,
		AI_PROVIDER_CLAUDE_TMUX, AI_PROVIDER_OPENCODE
	};
	gsize i;

	printf("Providers:\n");
	for (i = 0; i < G_N_ELEMENTS(all); i++)
	{
		const gchar *name = ai_provider_type_to_string(all[i]);
		const gchar *kind;
		const gchar *note = "";

		switch (all[i])
		{
		case AI_PROVIDER_CLAUDE_CODE:
		case AI_PROVIDER_CLAUDE_TMUX:
			kind = "CLI";
			note = "  (model \"ollama/<name>\" => ollama launch claude)";
			break;
		case AI_PROVIDER_OPENCODE:
			kind = "CLI";
			break;
		default:
			kind = "HTTP";
			break;
		}
		printf("  %-14s %-4s%s\n", name, kind, note);
	}
}

/* ---------------------------------------------------------------- */
/* --dry-run                                                         */
/* ---------------------------------------------------------------- */

/*
 * Print the command that would be spawned. For the claude-code /
 * claude-tmux CLI providers this is the real argv (with the executable
 * resolved and the Ollama transport applied); for other providers it is a
 * short summary, since they do not spawn a CLI we can introspect.
 */
static int
dry_run(GObject *provider, AiProviderType ptype, GList *messages)
{
	if (ptype == AI_PROVIDER_CLAUDE_CODE)
	{
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) argv = NULL;
		g_autofree gchar *exe = NULL;

		argv = ai_claude_code_client_build_argv(
			AI_CLI_CLIENT(provider), messages, opt_system,
			opt_max_tokens, opt_stream);
		exe = ai_cli_client_resolve_executable(AI_CLI_CLIENT(provider),
		                                       &error);
		if (exe != NULL)
		{
			g_free(argv[0]);
			argv[0] = g_steal_pointer(&exe);
		}
		else
		{
			g_printerr("note: %s (printing unresolved placeholder)\n",
			           error->message);
		}
		print_argv(argv);
		return 0;
	}

	if (ptype == AI_PROVIDER_CLAUDE_TMUX)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GPtrArray) argv = NULL;
		g_autofree gchar *claude_exe = NULL;
		g_autofree gchar *cwd = g_get_current_dir();
		const gchar *tmux_bin = g_getenv("TMUX_PATH");

		claude_exe = ai_cli_client_resolve_executable(
			AI_CLI_CLIENT(provider), &error);
		if (claude_exe == NULL)
		{
			g_printerr("note: %s (printing literal \"claude\")\n",
			           error->message);
			claude_exe = g_strdup("claude");
		}
		if (tmux_bin == NULL || tmux_bin[0] == '\0')
			tmux_bin = "tmux";

		/* Representative session: a fresh session-id, sample paths. */
		argv = ai_claude_tmux_client_build_session_argv(
			tmux_bin, "ai-glib-dry-run", cwd, claude_exe,
			/* resuming */ FALSE, "<session-id>", "<settings.json>",
			opt_model, opt_effort, opt_skip_perms);
		print_argv((gchar **) argv->pdata);
		return 0;
	}

	printf("[dry-run] provider=%s model=%s (no CLI subprocess for this "
	       "provider)\n",
	       ai_provider_type_to_string(ptype),
	       opt_model ? opt_model : "(default)");
	return 0;
}

/* ---------------------------------------------------------------- */
/* Streaming                                                         */
/* ---------------------------------------------------------------- */

static void
on_delta(AiStreamable *s, const gchar *text, gpointer user_data)
{
	(void) s;
	(void) user_data;
	fputs(text, stdout);
	fflush(stdout);
}

static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GMainLoop          *loop  = user_data;
	g_autoptr(AiResponse) resp = NULL;
	g_autoptr(GError)     error = NULL;

	resp = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
	                                        result, &error);
	fputc('\n', stdout);
	if (error != NULL)
		g_printerr("Error: %s\n", error->message);

	g_main_loop_quit(loop);
}

/*
 * Stream the response to stdout. Returns TRUE on success. Caller has
 * verified AI_IS_STREAMABLE(provider).
 */
static gboolean
stream_chat(GObject *provider, GList *messages)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	gulong delta_id;

	delta_id = g_signal_connect(provider, "delta",
	                            G_CALLBACK(on_delta), NULL);

	ai_streamable_chat_stream_async(
		AI_STREAMABLE(provider), messages, opt_system, opt_max_tokens,
		NULL, NULL, on_stream_done, loop);

	g_main_loop_run(loop);
	g_signal_handler_disconnect(provider, delta_id);
	return TRUE;
}

/* ---------------------------------------------------------------- */
/* Synchronous chat                                                  */
/* ---------------------------------------------------------------- */

/*
 * Run one synchronous turn and return the response (transfer full), or
 * NULL on error (with *error set). Dispatches to the right base class.
 */
static AiResponse *
sync_chat(GObject *provider, GList *messages, GError **error)
{
	if (AI_IS_CLIENT(provider))
		return ai_client_chat_sync(AI_CLIENT(provider), messages,
		                           NULL, error);
	if (AI_IS_CLI_CLIENT(provider))
		return ai_cli_client_chat_sync(AI_CLI_CLIENT(provider), messages,
		                               NULL, error);

	g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
	                    "Unknown provider base type");
	return NULL;
}

/* ---------------------------------------------------------------- */
/* Interactive REPL                                                  */
/* ---------------------------------------------------------------- */

static int
run_interactive(GObject *provider)
{
	GList    *history = NULL;  /* element-type AiMessage (owned) */
	gchar     line[8192];
	int       status = 0;

	fprintf(stderr, "ai interactive (provider=%s). Ctrl-D to exit.\n",
	        opt_provider ? opt_provider : "claude");

	for (;;)
	{
		AiMessage           *user_msg;
		g_autoptr(AiResponse) resp = NULL;
		g_autoptr(GError)     error = NULL;
		g_autofree gchar     *text = NULL;

		fputs("> ", stderr);
		fflush(stderr);
		if (fgets(line, sizeof line, stdin) == NULL)
		{
			fputc('\n', stderr);
			break;  /* EOF */
		}
		g_strchomp(line);
		if (line[0] == '\0')
			continue;

		user_msg = ai_message_new_user(line);
		history = g_list_append(history, user_msg);

		resp = sync_chat(provider, history, &error);
		if (resp == NULL)
		{
			g_printerr("Error: %s\n",
			           error ? error->message : "unknown");
			status = 1;
			continue;
		}

		text = ai_response_get_text(resp);
		printf("%s\n", text ? text : "");
		fflush(stdout);

		/* Keep the assistant turn in history for context. */
		history = g_list_append(history,
		                        ai_message_new_assistant(text ? text : ""));
	}

	g_list_free_full(history, g_object_unref);
	return status;
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	g_autoptr(GOptionContext) ctx = NULL;
	g_autoptr(GError)         error = NULL;
	g_autoptr(AiConfig)       config = NULL;
	GObject                  *provider = NULL;
	AiProviderType            ptype;
	const gchar              *provider_name;
	g_autofree gchar         *prompt = NULL;
	AiMessage                *msg;
	GList                    *messages = NULL;
	int                       status = 0;

	ctx = g_option_context_new("[PROMPT] - chat with an AI provider");
	g_option_context_add_main_entries(ctx, option_entries, NULL);
	g_option_context_set_summary(
		ctx,
		"Send PROMPT (from the argument or stdin) to an AI provider and "
		"print the reply to stdout.");
	if (!g_option_context_parse(ctx, &argc, &argv, &error))
	{
		g_printerr("ai: %s\n", error->message);
		return 2;
	}

	if (opt_version)
	{
		printf("ai (ai-glib) %d.%d.%d\n", AI_VERSION_MAJOR,
		       AI_VERSION_MINOR, AI_VERSION_MICRO);
		return 0;
	}

	if (opt_list_providers)
	{
		list_providers();
		return 0;
	}

	/* Resolve provider type. */
	provider_name = opt_provider;
	if (provider_name == NULL)
		provider_name = g_getenv("AI_PROVIDER");
	if (provider_name == NULL)
		provider_name = "claude";
	ptype = ai_provider_type_from_string(provider_name);

	config = ai_config_new();
	provider = make_provider(config, ptype);
	if (provider == NULL)
	{
		g_printerr("ai: unknown provider '%s'\n", provider_name);
		return 2;
	}

	if (opt_interactive)
	{
		status = run_interactive(provider);
		g_object_unref(provider);
		return status;
	}

	/* Resolve the prompt: positional arg(s) win, else stdin. */
	if (argc > 1)
	{
		prompt = g_strjoinv(" ", &argv[1]);
	}
	else if (!isatty(STDIN_FILENO))
	{
		prompt = read_all_stdin();
		if (prompt != NULL)
			g_strchomp(prompt);
	}

	if (prompt == NULL || prompt[0] == '\0')
	{
		/* For dry-run we still want a representative message. */
		if (opt_dry_run)
		{
			g_free(prompt);
			prompt = g_strdup("(prompt)");
		}
		else
		{
			g_printerr("ai: no prompt given (pass it as an argument, "
			           "pipe it on stdin, or use --interactive)\n");
			g_object_unref(provider);
			return 2;
		}
	}

	msg = ai_message_new_user(prompt);
	messages = g_list_append(NULL, msg);

	if (opt_dry_run)
	{
		status = dry_run(provider, ptype, messages);
	}
	else if (opt_stream && AI_IS_STREAMABLE(provider))
	{
		stream_chat(provider, messages);
	}
	else
	{
		g_autoptr(AiResponse) resp = NULL;
		g_autofree gchar     *text = NULL;

		if (opt_stream)
			g_printerr("note: provider '%s' does not support streaming; "
			           "using a single response\n", provider_name);

		resp = sync_chat(provider, messages, &error);
		if (resp == NULL)
		{
			g_printerr("ai: %s\n", error ? error->message : "request failed");
			status = 1;
		}
		else
		{
			text = ai_response_get_text(resp);
			printf("%s\n", text ? text : "");
		}
	}

	g_list_free_full(messages, g_object_unref);
	g_object_unref(provider);
	return status;
}
