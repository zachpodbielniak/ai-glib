/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Shared frontend option parsing; all inference lives in AiDecider. */
#pragma once
#include <ai-glib.h>
#include <stdio.h>
#include <unistd.h>

typedef struct
{
	AiLayaClient *client;
	AiDecisionRequest *request;
	gchar *help;
	gboolean json;
} DecideOptions;
static inline void
decide_options_clear(DecideOptions *options)
{
	g_clear_object(&options->client);
	g_clear_object(&options->request);
	g_clear_pointer(&options->help, g_free);
}

static inline gboolean
decide_options_parse(DecideOptions *options, gchar **arguments, gboolean allow_stdin, const gchar *cwd, GError **error)
{
	g_autoptr(GOptionContext) context = g_option_context_new("[TEXT] — classify text with Laya");
	g_auto(GStrv) argv = g_strdupv(arguments);
	g_auto(GStrv) remaining = NULL;
	g_autofree gchar *question = NULL, *request_file = NULL, *model = NULL;
	g_autofree gchar *provider = NULL, *endpoint = NULL, *id = NULL, *input = NULL;
	gboolean help = FALSE, version = FALSE, license = FALSE;
	gint timeout = 30000;
	gsize length = 0;
	GOptionEntry entries[] = {
		{ "provider", 'p', 0, G_OPTION_ARG_STRING, &provider, "Decision provider (laya)", "NAME" },
		{ "model", 'm', 0, G_OPTION_ARG_STRING, &model, "english, multilingual, or typed-decisions", "NAME" },
		{ "base-url", 0, 0, G_OPTION_ARG_STRING, &endpoint, "Laya server base URL (or LAYA_BASE_URL)", "URL" },
		{ "question", 'q', 0, G_OPTION_ARG_STRING, &question, "Boolean question", "QUESTION" },
		{ "id", 0, 0, G_OPTION_ARG_STRING, &id, "Question identifier (default: answer)", "ID" },
		{ "request", 0, 0, G_OPTION_ARG_FILENAME, &request_file, "JSON request file; '-' reads stdin in CLI", "FILE" },
		{ "timeout-ms", 0, 0, G_OPTION_ARG_INT, &timeout, "Whole-operation deadline (default: 30000)", "MS" },
		{ "json", 0, 0, G_OPTION_ARG_NONE, &options->json, "Print response JSON", NULL },
		{ "help", 'h', 0, G_OPTION_ARG_NONE, &help, "Show help", NULL },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &version, "Show version", NULL },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &license, "Show license", NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &remaining, NULL, "TEXT" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};
	g_option_context_set_help_enabled(context, FALSE);
	g_option_context_add_main_entries(context, entries, NULL);
	g_option_context_set_summary(context, "ai decide / TUI /decide: calls an existing server. Never downloads weights.\n"
		"Examples:\n  ai decide --id spam -q 'Is this spam?' 'Win a prize'\n"
		"  cat message.txt | ai decide -q 'Is this spam?' --json\n"
		"  ai decide --request triage.json\n"
		"  /decide -q 'Is this spam?' 'Win a prize'\n"
		"LAYA_API_KEY supplies an optional bearer token.");
	if (!g_option_context_parse_strv(context, &argv, error)) return FALSE;
	if (help || (!allow_stdin && question == NULL && request_file == NULL && remaining == NULL && !version && !license))
	{
		options->help = g_option_context_get_help(context, TRUE, NULL);
		return TRUE;
	}
	if (version || license)
	{
		options->help = version ? g_strdup_printf("ai-glib %d.%d.%d\n", AI_VERSION_MAJOR, AI_VERSION_MINOR, AI_VERSION_MICRO)
			: g_strdup("ai-glib is licensed under GNU AGPL version 3 or later.\nhttps://www.gnu.org/licenses/agpl-3.0.html\n");
		return TRUE;
	}
	if ((provider != NULL && !g_str_equal(provider, "laya")) || timeout <= 0 ||
		(request_file != NULL && (question != NULL || remaining != NULL || id != NULL)) ||
		(request_file == NULL && question == NULL) || (remaining != NULL && remaining[1] != NULL))
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
			"Use --question QUESTION [TEXT] or --request FILE, provider laya, and a positive timeout");
		return FALSE;
	}
	if (request_file != NULL && !g_str_equal(request_file, "-"))
	{
		g_autofree gchar *path = cwd != NULL && !g_path_is_absolute(request_file)
			? g_build_filename(cwd, request_file, NULL) : g_strdup(request_file);
		if (!g_file_get_contents(path, &input, &length, error)) return FALSE;
	}
	else if (remaining != NULL)
	{
		input = g_strdup(remaining[0]); length = strlen(input);
	}
	else
	{
		g_autoptr(GString) buffer = g_string_new(NULL);
		gchar chunk[4096];
		gsize count;
		if (!allow_stdin || isatty(STDIN_FILENO))
		{
			g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Supply text or a request file; CLI also accepts piped stdin");
			return FALSE;
		}
		while ((count = fread(chunk, 1, sizeof(chunk), stdin)) > 0)
		{
			g_string_append_len(buffer, chunk, count);
			if (buffer->len > 2 * 1024 * 1024) break;
		}
		if (ferror(stdin))
		{
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read stdin");
			return FALSE;
		}
		length = buffer->len;
		input = g_string_free(g_steal_pointer(&buffer), FALSE);
	}
	if (length > 2 * 1024 * 1024 || memchr(input, '\0', length) != NULL || !g_utf8_validate(input, length, NULL))
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Input must be UTF-8 without NUL bytes and at most 2 MiB");
		return FALSE;
	}
	if (request_file != NULL)
	{
		options->request = ai_decision_request_new_from_json(input, error);
		if (options->request == NULL) return FALSE;
	}
	else
	{
		options->request = ai_decision_request_new(input);
		if (!ai_decision_request_add_boolean(options->request, id != NULL ? id : "answer", question, error)) return FALSE;
	}
	options->client = ai_laya_client_new();
	if (endpoint == NULL) endpoint = g_strdup(g_getenv("LAYA_BASE_URL"));
	if (endpoint != NULL) g_object_set(options->client, "base-url", endpoint, NULL);
	g_object_set(options->client, "model", model, "timeout-ms", (guint)timeout, "api-key", g_getenv("LAYA_API_KEY"), NULL);
	return TRUE;
}

static inline gint
decide_cli_main(gchar **argv)
{
	DecideOptions options = { NULL, NULL, NULL, FALSE };
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	gint status = 0;
	if (!decide_options_parse(&options, argv, TRUE, NULL, &error)) status = 2;
	else if (options.help != NULL) g_print("%s", options.help);
	else
	{
		response = ai_decider_decide(AI_DECIDER(options.client), options.request, NULL, &error);
		if (response == NULL) status = 1;
		else
		{
			output = options.json ? ai_decision_response_dup_json(response) : ai_decision_response_format(response);
			g_print("%s%s", output, options.json ? "\n" : "");
		}
	}
	if (error != NULL) g_printerr("ai decide: %s\n", error->message);
	decide_options_clear(&options);
	return status;
}
