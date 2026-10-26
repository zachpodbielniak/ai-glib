/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Shared frontend option parsing; all inference lives in AiDecider. */
#pragma once
#include <ai-glib.h>
#include <stdio.h>
#include <unistd.h>

typedef struct
{
	GObject *client;
	AiDecisionRequest *request;
	gchar *help;
	gchar *provider_label;
	gboolean json;
} DecideOptions;
static inline void
decide_options_clear(DecideOptions *options)
{
	g_clear_object(&options->client);
	g_clear_object(&options->request);
	g_clear_pointer(&options->help, g_free);
	g_clear_pointer(&options->provider_label, g_free);
}

/* Explicit -p wins. Otherwise a TypeSafe key means Jev, an existing Laya
 * URL means Laya, and the remaining case is a local Ollama decision model. */
static inline gboolean
decide_provider_name(const gchar *requested, gchar **name, GError **error)
{
	const gchar *key;

	if (requested == NULL || *requested == '\0')
	{
		key = g_getenv("TYPESAFE_API_KEY");
		if (key != NULL && *key != '\0')
			*name = g_strdup("typesafe");
		else if (g_getenv("LAYA_BASE_URL") != NULL)
			*name = g_strdup("laya");
		else
			*name = g_strdup("ollama");
		return TRUE;
	}
	if (g_str_equal(requested, "laya") || g_str_equal(requested, "ollama") ||
		g_str_equal(requested, "typesafe") || g_str_equal(requested, "jev"))
	{
		*name = g_strdup(g_str_equal(requested, "jev") ? "typesafe" : requested);
		return TRUE;
	}
	g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		"Decision provider must be typesafe, jev, ollama or laya");
	return FALSE;
}

static inline gboolean
decide_options_parse(DecideOptions *options, gchar **arguments, gboolean allow_stdin, const gchar *cwd, GError **error)
{
	g_autoptr(GOptionContext) context = g_option_context_new("[TEXT] — typed decisions");
	g_auto(GStrv) argv = g_strdupv(arguments);
	g_auto(GStrv) remaining = NULL;
	g_autofree gchar *question = NULL, *request_file = NULL, *model = NULL;
	g_autofree gchar *provider = NULL, *endpoint = NULL, *id = NULL, *input = NULL;
	g_autofree gchar *api_key = NULL, *resolved = NULL;
	gboolean help = FALSE, version = FALSE, license = FALSE, long_context = FALSE;
	gint timeout = 30000;
	gsize length = 0;
	GOptionEntry entries[] = {
		{ "provider", 'p', 0, G_OPTION_ARG_STRING, &provider, "Decision provider: typesafe, jev, ollama or laya", "NAME" },
		{ "model", 'm', 0, G_OPTION_ARG_STRING, &model, "jev-latest, tev1, english, or another id that provider accepts", "NAME" },
		{ "base-url", 0, 0, G_OPTION_ARG_STRING, &endpoint, "Server origin (TYPESAFE_BASE_URL, OLLAMA_HOST or LAYA_BASE_URL)", "URL" },
		{ "api-key", 0, 0, G_OPTION_ARG_STRING, &api_key, "Bearer token (or TYPESAFE_API_KEY, OLLAMA_API_KEY, LAYA_API_KEY)", "KEY" },
		{ "long-context", 0, 0, G_OPTION_ARG_NONE, &long_context, "TypeSafe long-context header", NULL },
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
	g_option_context_set_summary(context, "ai decide / TUI /decide / GUI /decide: calls an existing server. Never downloads weights.\n"
		"Examples:\n  ai decide -p typesafe -q 'Is a refund requested?' 'Please refund the duplicate charge.'\n"
		"  ai decide -p ollama -m tev1 -q 'Is this spam?' 'Win a prize'\n"
		"  ai decide -p laya -m english -q 'Is this spam?' 'Win a prize'\n"
		"  cat message.txt | ai decide -q 'Is this spam?' --json\n"
		"  ai decide --request triage.json\n"
		"  /decide -p ollama -q 'Is this spam?' 'Win a prize'\n"
		"TYPESAFE_API_KEY selects TypeSafe when -p is omitted. LAYA_BASE_URL still selects Laya. Otherwise the decision runs on local Ollama.");
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
	if (timeout <= 0 ||
		(request_file != NULL && (question != NULL || remaining != NULL || id != NULL)) ||
		(request_file == NULL && question == NULL) || (remaining != NULL && remaining[1] != NULL) ||
		!decide_provider_name(provider, &resolved, error))
	{
		if (error == NULL || *error == NULL)
			g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
				"Use --question QUESTION [TEXT] or --request FILE, and a positive timeout");
		return FALSE;
	}
	if (long_context && !g_str_equal(resolved, "typesafe"))
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
			"--long-context applies to the TypeSafe provider");
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
	if (g_str_equal(resolved, "typesafe"))
	{
		options->client = ai_provider_factory_new_decider("typesafe", NULL, error);
		if (options->client == NULL) return FALSE;
		options->provider_label = g_strdup("TypeSafe");
		if (endpoint != NULL)
			g_object_set(options->client, "base-url", endpoint, NULL);
		if (model != NULL)
			g_object_set(options->client, "model", model, NULL);
		if (api_key != NULL)
			g_object_set(options->client, "api-key", api_key, NULL);
		g_object_set(options->client, "timeout-ms", (guint)timeout, "long-context", long_context, NULL);
	}
	else if (g_str_equal(resolved, "ollama"))
	{
		g_autoptr(AiConfig) config = ai_config_new();
		const gchar *key = api_key != NULL ? api_key : g_getenv("OLLAMA_API_KEY");

		if (endpoint != NULL)
			ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, endpoint);
		if (key != NULL && *key != '\0')
			ai_config_set_api_key(config, AI_PROVIDER_OLLAMA, key);
		options->client = ai_provider_factory_new_decider("ollama", config, error);
		if (options->client == NULL) return FALSE;
		options->provider_label = g_strdup("Ollama");
		if (model != NULL)
			g_object_set(options->client, "decision-model", model, NULL);
		g_object_set(options->client, "decision-timeout-ms", (guint)timeout, NULL);
	}
	else
	{
		const gchar *key = api_key != NULL ? api_key : g_getenv("LAYA_API_KEY");

		options->client = ai_provider_factory_new_decider("laya", NULL, error);
		if (options->client == NULL) return FALSE;
		options->provider_label = g_strdup("Laya");
		if (endpoint == NULL)
			endpoint = g_strdup(g_getenv("LAYA_BASE_URL"));
		if (endpoint != NULL)
			g_object_set(options->client, "base-url", endpoint, NULL);
		g_object_set(options->client, "model", model, "timeout-ms", (guint)timeout, "api-key", key, NULL);
	}
	return TRUE;
}

static inline gint
decide_cli_main(gchar **argv)
{
	DecideOptions options = { NULL, NULL, NULL, NULL, FALSE };
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
