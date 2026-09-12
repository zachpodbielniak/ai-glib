/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <ai-glib.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Private native-launch projection, deliberately independent of build_argv.
 * Frontends map the type BEFORE constructing/configuring the CLI object.
 * No persistence, timeout, output-stream or prompt-spill machinery belongs here.
 * Explicit values equal to GParamSpec defaults cannot be distinguished from
 * untouched defaults. In particular, medium effort on OpenCode's TUI is omitted.
 *
 * Native mappings beyond model/effort/prompt/session/permissions:
 * Claude: agent(s), append-system-prompt, settings/sandbox, tool lists, MCP,
 *         plugins, fallback-model, betas, debug, autocompact, bare/safe-mode;
 *         fork requires resume/continue; max-budget-usd requires print mode.
 * OpenCode: agent, pure, logging, port, fork; title/files/attach/share/thinking
 *           require print mode (run). Interactive effort has no native flag.
 * Grok: agent, rules, sandbox, allow/deny, max-turns, disable-web-search.
 * agy: agent, project/new-project, mode, sandbox, add-dir, log-file;
 *      print-timeout is print-only, without the wrapper's automatic timeout.
 * Cursor: model-params, mode, sandbox, workspace, endpoint, add-dir, plugins,
 *         worktree options, auto-review, approve-mcps, trust; headers exec-only.
 * Codex: profile, search, add-dir, explicit sandbox, TOML effort/instructions.
 *         No automatic ephemeral, skip-git-repo-check or approval policy.
 *
 * There is no sandbox-mode property in the current wrappers: use sandbox.
 * OpenCode/agy/Cursor have no native system-prompt override; reject rather than
 * pretend a user-message prefix is a system instruction. Cursor effort belongs
 * in the model ID. json-schema is refused: these modes promise plain output.
 * Non-environment tool endpoints require the ordinary wrapper machinery.
 */
static inline AiProviderType
ai_launch_provider_type(AiProviderType type)
{
	return type == AI_PROVIDER_CLAUDE || type == AI_PROVIDER_CLAUDE_TMUX
		? AI_PROVIDER_CLAUDE_CODE : type;
}

/* Read only known scalar/strv types, never transform arbitrary boxed values.
 * The seen set also lets the final audit refuse unsupported explicit knobs.
 */
static inline gchar *
ai_launch_property(GObject *object, GHashTable *seen, const gchar *name)
{
	GParamSpec *pspec;
	GValue value = G_VALUE_INIT;
	gchar *result = NULL;
	gchar number[G_ASCII_DTOSTR_BUF_SIZE];

	g_hash_table_add(seen, g_strdup(name));
	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), name);
	if (pspec == NULL || !(pspec->flags & G_PARAM_READABLE))
		return NULL;
	g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(pspec));
	g_object_get_property(object, name, &value);
	if (G_VALUE_HOLDS_STRING(&value))
		result = g_value_dup_string(&value);
	else if (G_VALUE_HOLDS_BOOLEAN(&value))
		result = g_value_get_boolean(&value) ? g_strdup("true") : NULL;
	else if (G_VALUE_HOLDS_INT(&value) && g_value_get_int(&value) != 0)
		result = g_strdup_printf("%d", g_value_get_int(&value));
	else if (G_VALUE_HOLDS_UINT(&value) && g_value_get_uint(&value) != 0)
		result = g_strdup_printf("%u", g_value_get_uint(&value));
	else if (G_VALUE_HOLDS_DOUBLE(&value) && g_value_get_double(&value) != 0)
		result = g_strdup(g_ascii_dtostr(number, sizeof number, g_value_get_double(&value)));
	g_value_unset(&value);
	return result;
}

static inline void
ai_launch_arg(GPtrArray *args, const gchar *text)
{
	g_ptr_array_add(args, g_strdup(text));
}

static inline void
ai_launch_pair(GPtrArray *args, const gchar *flag, const gchar *text)
{
	/* Optional-value parsers otherwise mistake a leading dash for a flag. */
	if (*text == '-')
		g_ptr_array_add(args, g_strconcat(flag, "=", text, NULL));
	else
	{
		ai_launch_arg(args, flag);
		ai_launch_arg(args, text);
	}
}

static inline void
ai_launch_value(GPtrArray *args, const gchar *flag, const gchar *text)
{
	if (text != NULL && *text != '\0')
		ai_launch_pair(args, flag, text);
}

/* Map booleans to switches, scalars to one value, CSV/strv to repeated flags.
 * CSV splitting matches the existing wrappers; strv entries stay indivisible.
 */
static inline void
ai_launch_option(GObject *object, GHashTable *seen, GPtrArray *args,
	const gchar *name, const gchar *flag, gboolean list)
{
	GParamSpec *pspec;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) values = NULL;
	gboolean strv;
	guint i;

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), name);
	strv = pspec != NULL && G_PARAM_SPEC_VALUE_TYPE(pspec) == G_TYPE_STRV;
	text = ai_launch_property(object, seen, name);
	if (strv && (pspec->flags & G_PARAM_READABLE))
		g_object_get(object, name, &values, NULL);
	else if (text == NULL || *text == '\0')
		return;
	else if (G_PARAM_SPEC_VALUE_TYPE(pspec) == G_TYPE_BOOLEAN)
	{
		ai_launch_arg(args, flag);
		return;
	}
	else if (list)
		values = g_strsplit(text, ",", -1);
	else
	{
		ai_launch_value(args, flag, text);
		return;
	}
	for (i = 0; values != NULL && values[i] != NULL; i++)
		ai_launch_value(args, flag, strv ? values[i] : g_strstrip(values[i]));
}

/* JSON string escaping is also valid for these TOML basic-string values. */
static inline void
ai_launch_config(GPtrArray *args, const gchar *key, const gchar *text)
{
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *assignment = NULL;

	if (text == NULL || *text == '\0')
		return;
	node = json_node_new(JSON_NODE_VALUE);
	json_node_set_string(node, text);
	quoted = json_to_string(node, FALSE);
	assignment = g_strconcat(key, "=", quoted, NULL);
	ai_launch_value(args, "--config", assignment);
}

/* Commands may rely on inherited secrets, but must never print new ones. */
static inline gboolean
ai_launch_environment(gchar ***env, GHashTable *table, gboolean command_only)
{
	GHashTableIter iter;
	gpointer key, value;

	if (table == NULL)
		return TRUE;
	g_hash_table_iter_init(&iter, table);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		const gchar *name = (const gchar *)key;
		const gchar *text = (const gchar *)value;

		if (name == NULL || *name == '\0' || strchr(name, '=') != NULL || text == NULL)
		{
			g_printerr("ai-glib launch: invalid environment entry\n");
			return FALSE;
		}
		if (command_only && g_strcmp0(g_environ_getenv(*env, name), text) != 0)
		{
			g_printerr("ai-glib launch: command output cannot represent custom environment safely; use inherited environment\n");
			return FALSE;
		}
		*env = g_environ_setenv(*env, name, text, TRUE);
	}
	return TRUE;
}

/**
 * ai_launch_run:
 * @provider: a configured CLI provider (HTTP providers are refused)
 * @command_only: print a shell command instead of replacing this process
 * @print_mode: use the native plain-text print mode instead of the TUI
 * @prompt: (nullable): initial prompt; NULL leaves stdin to the native CLI
 *
 * Inherits stdio. No shell interprets a supplied prompt. The sole generated
 * substitution is agy's no-prompt print command, whose --print consumes text.
 * Wrapper-only properties listed below are intentionally ignored, including
 * explicit values: callers wanting those semantics must use the normal harness.
 * Other unsupported nondefault properties fail rather than silently disappear.
 *
 * Returns: 0 for a printed command, 2 for configuration errors, 127 for a
 * missing executable, 126 for other exec/cwd failures; success does not return.
 */
static inline gint
ai_launch_run(GObject *provider, gboolean command_only, gboolean print_mode,
	const gchar *prompt)
{
	static const gchar * const wrapper_only[] = {
		"config", "max-tokens", "session-persistence", "process-timeout-ms",
		"splits-text-at-tool-use", "disable-slash-commands", "verbatim",
		"include-partial-messages", "include-hook-events", "forward-subagent-text",
		"tmux-path", "socket-name", "claude-project-dir", "turn-timeout-ms",
		"startup-timeout-ms", "keep-artifacts", "debug-preserve-tmux",
		"prompt-resend-interval-ms", "max-prompt-send-attempts",
		"max-prompt-delivery-passes", "dismiss-resume-prompt",
		"prompt-send-exponential-backoff", "command-timeout-ms", NULL
	};
	g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliClient) launch_client = NULL;
	g_autoptr(GString) command = g_string_new(NULL);
	g_auto(GStrv) env = g_get_environ();
	g_autofree gchar *exe = NULL;
	g_autofree gchar *model = NULL;
	g_autofree gchar *system = NULL;
	g_autofree gchar *effort = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *cont = NULL;
	g_autofree gchar *skip = NULL;
	g_autofree gchar *sandbox = NULL;
	g_autofree gchar *cwd = NULL;
	g_autofree gchar *api_key = NULL;
	g_autofree GParamSpec **properties = NULL;
	AiCliClient *client;
	AiCliClientClass *klass;
	AiProviderType type;
	const AiAgentEndpoint *endpoint;
	const gchar *explicit_path;
	gboolean claude, resume, ollama, stdin_substitution = FALSE;
	guint i, n_properties;
	gint saved_cwd = -1, saved_errno;

	if (!AI_IS_CLI_CLIENT(provider) || !AI_IS_PROVIDER(provider))
	{
		g_printerr("ai-glib launch: a native CLI provider is required\n");
		return 2;
	}
	client = AI_CLI_CLIENT(provider);
	klass = AI_CLI_CLIENT_GET_CLASS(client);
	type = ai_launch_provider_type(ai_provider_get_provider_type(AI_PROVIDER(provider)));
	claude = type == AI_PROVIDER_CLAUDE_CODE;
	if (!claude && type != AI_PROVIDER_OPENCODE && type != AI_PROVIDER_GROK_BUILD &&
		type != AI_PROVIDER_ANTIGRAVITY && type != AI_PROVIDER_CURSOR && type != AI_PROVIDER_CODEX_CLI)
	{
		g_printerr("ai-glib launch: unsupported native CLI provider\n");
		return 2;
	}
	for (i = 0; wrapper_only[i] != NULL; i++)
		g_hash_table_add(seen, g_strdup(wrapper_only[i]));
	model = ai_launch_property(provider, seen, "model");
	system = ai_launch_property(provider, seen, "system-prompt");
	effort = ai_launch_property(provider, seen, "effort-level");
	session = ai_launch_property(provider, seen, "session-id");
	cont = ai_launch_property(provider, seen, "continue-session");
	skip = ai_launch_property(provider, seen, "skip-permissions");
	sandbox = ai_launch_property(provider, seen, "sandbox");
	cwd = ai_launch_property(provider, seen, "working-directory");
	resume = (session != NULL && *session != '\0') || cont != NULL;
	ollama = claude && model != NULL && g_str_has_prefix(model, "ollama/") && model[7] != '\0';

	/* Endpoint env is public; file/config delivery is not safe to reconstruct. */
	g_hash_table_add(seen, g_strdup("environment"));
	g_hash_table_add(seen, g_strdup("tool-endpoint"));
	endpoint = ai_cli_client_get_tool_endpoint(client);
	if (endpoint != NULL && g_strcmp0(endpoint->kind, AI_ENDPOINT_KIND_ENV) != 0)
	{
		g_printerr("ai-glib launch: only environment tool endpoints are supported\n");
		return 2;
	}
	if (!ai_launch_environment(&env, ai_cli_client_get_environment(client), command_only) ||
		!ai_launch_environment(&env, endpoint != NULL ? endpoint->env : NULL, command_only))
		return 2;
	if (type == AI_PROVIDER_CURSOR)
	{
		api_key = ai_launch_property(provider, seen, "api-key");
		if (api_key != NULL && *api_key != '\0')
		{
			if (command_only && g_strcmp0(g_environ_getenv(env, "CURSOR_API_KEY"), api_key) != 0)
			{
				g_printerr("ai-glib launch: use inherited CURSOR_API_KEY for command output\n");
				return 2;
			}
			env = g_environ_setenv(env, "CURSOR_API_KEY", api_key, TRUE);
		}
	}
	g_hash_table_add(seen, g_strdup("executable-path"));
	explicit_path = ai_cli_client_get_executable_path(client);
	/* tmux's getter names Claude even for Ollama models. Use the mapped
	 * provider's getter/resolver without mutating the caller's object. */
	if (ollama && AI_IS_CLAUDE_TMUX_CLIENT(provider))
	{
		launch_client = g_object_new(AI_TYPE_CLAUDE_CODE_CLIENT,
			"model", model, "executable-path", explicit_path, NULL);
		client = launch_client;
		klass = AI_CLI_CLIENT_GET_CLASS(client);
	}
	if (command_only)
		exe = explicit_path != NULL && *explicit_path != '\0' ? g_strdup(explicit_path) :
			(klass->get_executable_path != NULL ? klass->get_executable_path(client) : NULL);
	else
		exe = ai_cli_client_resolve_executable(client, &error);
	if (exe == NULL || *exe == '\0')
	{
		g_printerr("ai-glib launch: %s\n", error != NULL ? error->message : "executable not configured");
		return 127;
	}
	/* Preserve resolver semantics for relative explicit paths across chdir. */
	if (!g_path_is_absolute(exe) && (strchr(exe, '/') != NULL ||
		(explicit_path != NULL && *explicit_path != '\0')))
	{
		gchar *absolute = g_canonicalize_filename(exe, NULL);
		g_free(exe);
		exe = absolute;
	}
	ai_launch_arg(args, exe);
	if (ollama)
	{
		ai_launch_arg(args, "launch");
		ai_launch_arg(args, "claude");
		ai_launch_value(args, "--model", model + 7);
		ai_launch_arg(args, "--");
	}
	if (type == AI_PROVIDER_OPENCODE && print_mode)
	{
		ai_launch_arg(args, "run");
		ai_launch_value(args, "--format", "default");
	}
	if (type == AI_PROVIDER_CODEX_CLI)
	{
		ai_launch_option(provider, seen, args, "profile", "--profile", FALSE);
		ai_launch_option(provider, seen, args, "search", "--search", FALSE);
		if (print_mode)
			ai_launch_arg(args, "exec");
	}
	if (type == AI_PROVIDER_CURSOR)
	{
		g_autofree gchar *params = ai_launch_property(provider, seen, "model-params");
		if (params != NULL && *params != '\0' && model != NULL && strchr(model, '[') == NULL)
		{
			gchar *combined = g_strdup_printf("%s[%s]", model, params);
			g_free(model);
			model = combined;
		}
	}
	if (!ollama)
		ai_launch_value(args, "--model", model);
	if (claude || type == AI_PROVIDER_ANTIGRAVITY)
		ai_launch_value(args, "--effort", type == AI_PROVIDER_ANTIGRAVITY &&
			(g_strcmp0(effort, "xhigh") == 0 || g_strcmp0(effort, "max") == 0) ? "high" : effort);
	else if (type == AI_PROVIDER_GROK_BUILD)
		ai_launch_value(args, "--reasoning-effort", g_strcmp0(effort, "max") == 0 ? "xhigh" : effort);
	else if (type == AI_PROVIDER_CODEX_CLI)
		ai_launch_config(args, "model_reasoning_effort", effort);
	else if (type == AI_PROVIDER_OPENCODE && print_mode)
		ai_launch_value(args, "--variant", effort);
	else if (effort != NULL && *effort != '\0' && strcmp(effort, "medium") != 0)
	{
		g_printerr("ai-glib launch: this native mode has no effort flag; use a model variant or print mode\n");
		return 2;
	}
	if (system != NULL && *system != '\0')
	{
		if (claude)
			ai_launch_value(args, "--system-prompt", system);
		else if (type == AI_PROVIDER_GROK_BUILD)
			ai_launch_value(args, "--system-prompt-override", system);
		else if (type == AI_PROVIDER_CODEX_CLI)
			ai_launch_config(args, "developer_instructions", system);
		else
		{
			g_printerr("ai-glib launch: this CLI has no native system-prompt flag; put instructions in the prompt or native configuration\n");
			return 2;
		}
	}
	/* All current skip-permissions properties default FALSE. Never manufacture
	 * bypass/approval flags from session-persistence or headless defaults. */
	if (skip != NULL)
	{
		if (claude || type == AI_PROVIDER_ANTIGRAVITY)
			ai_launch_arg(args, "--dangerously-skip-permissions");
		else if (type == AI_PROVIDER_CURSOR)
			ai_launch_arg(args, "--force");
		else if (type == AI_PROVIDER_CODEX_CLI)
			ai_launch_arg(args, "--dangerously-bypass-approvals-and-sandbox");
		else if (type == AI_PROVIDER_GROK_BUILD)
			ai_launch_value(args, "--permission-mode", "bypassPermissions");
		else
		{
			ai_launch_arg(args, "--auto");
		}
	}
	if (claude || type == AI_PROVIDER_GROK_BUILD)
	{
		g_autofree gchar *permission = ai_launch_property(provider, seen, "permission-mode");

		if (skip != NULL && permission != NULL && *permission != '\0' &&
			strcmp(permission, "bypassPermissions") != 0)
		{
			g_printerr("ai-glib launch: skip-permissions conflicts with permission-mode\n");
			return 2;
		}
		if (skip == NULL)
			ai_launch_value(args, "--permission-mode", permission);
		ai_launch_option(provider, seen, args, "allowed-tools", claude ? "--allowedTools" : "--allow", TRUE);
		ai_launch_option(provider, seen, args, "disallowed-tools", claude ? "--disallowedTools" : "--deny", TRUE);
	}
	if (type == AI_PROVIDER_CODEX_CLI && sandbox != NULL && *sandbox != '\0')
	{
		if ((strcmp(sandbox, "read-only") != 0 && strcmp(sandbox, "workspace-write") != 0 &&
			strcmp(sandbox, "danger-full-access") != 0) ||
			(skip != NULL && strcmp(sandbox, "danger-full-access") != 0))
		{
			g_printerr("ai-glib launch: invalid or conflicting Codex sandbox policy\n");
			return 2;
		}
	}
	if (claude)
	{
		g_autofree gchar *settings = ai_launch_property(provider, seen, "settings");
		if (sandbox != NULL && *sandbox != '\0')
		{
			g_autoptr(JsonParser) parser = json_parser_new();
			g_autofree gchar *contents = NULL;
			JsonNode *root, *sandbox_node;
			JsonObject *object, *box;
			const gchar *text = settings;
			gboolean enabled = strcmp(sandbox, "enabled") == 0;

			if (!enabled && strcmp(sandbox, "disabled") != 0)
			{
				g_printerr("ai-glib launch: Claude sandbox must be enabled or disabled\n");
				return 2;
			}
			while (text != NULL && g_ascii_isspace(*text)) text++;
			if (text != NULL && *text != '\0' && *text != '{')
			{
				if (!g_file_get_contents(text, &contents, NULL, &error))
				{
					g_printerr("ai-glib launch: cannot read sandbox settings: %s\n", error->message);
					return 2;
				}
				text = contents;
			}
			if (!json_parser_load_from_data(parser, text != NULL && *text ? text : "{}", -1, NULL) ||
				(root = json_parser_get_root(parser)) == NULL || !JSON_NODE_HOLDS_OBJECT(root))
			{
				g_printerr("ai-glib launch: sandbox settings must be a JSON object\n");
				return 2;
			}
			object = json_node_get_object(root);
			sandbox_node = json_object_get_member(object, "sandbox");
			if (sandbox_node != NULL && !JSON_NODE_HOLDS_NULL(sandbox_node) && !JSON_NODE_HOLDS_OBJECT(sandbox_node))
			{
				g_printerr("ai-glib launch: settings.sandbox must be an object\n");
				return 2;
			}
			box = sandbox_node != NULL && JSON_NODE_HOLDS_OBJECT(sandbox_node) ? json_node_get_object(sandbox_node) : NULL;
			if (box == NULL)
			{
				box = json_object_new();
				json_object_set_object_member(object, "sandbox", box);
			}
			json_object_set_boolean_member(box, "enabled", enabled);
			if (enabled)
			{
				json_object_set_boolean_member(box, "allowUnsandboxedCommands", FALSE);
				json_object_set_boolean_member(box, "failIfUnavailable", TRUE);
			}
			g_free(settings);
			settings = json_to_string(root, FALSE);
		}
		ai_launch_value(args, "--settings", settings);
		ai_launch_option(provider, seen, args, "append-system-prompt", "--append-system-prompt", FALSE);
		ai_launch_option(provider, seen, args, "agents-json", "--agents", FALSE);
		if (print_mode)
			ai_launch_option(provider, seen, args, "fallback-model", "--fallback-model", FALSE);
		ai_launch_option(provider, seen, args, "setting-sources", "--setting-sources", FALSE);
		{
			g_autofree gchar *tools = ai_launch_property(provider, seen, "tools");

			/* Empty is meaningful: --tools '' disables the native built-ins. */
			if (tools != NULL)
				ai_launch_pair(args, "--tools", tools);
		}
		ai_launch_option(provider, seen, args, "betas", "--betas", TRUE);
		ai_launch_option(provider, seen, args, "plugin-dirs", "--plugin-dir", TRUE);
		ai_launch_option(provider, seen, args, "plugin-urls", "--plugin-url", TRUE);
		ai_launch_option(provider, seen, args, "mcp-config-path", "--mcp-config", FALSE);
		ai_launch_option(provider, seen, args, "strict-mcp-config", "--strict-mcp-config", FALSE);
		ai_launch_option(provider, seen, args, "bare", "--bare", FALSE);
		ai_launch_option(provider, seen, args, "safe-mode", "--safe-mode", FALSE);
		ai_launch_option(provider, seen, args, "autocompact", "--autocompact", FALSE);
		ai_launch_option(provider, seen, args, "exclude-dynamic-system-prompt-sections", "--exclude-dynamic-system-prompt-sections", FALSE);
		ai_launch_option(provider, seen, args, "debug-file", "--debug-file", FALSE);
		{
			g_autofree gchar *filter = ai_launch_property(provider, seen, "debug-filter");
			g_autofree gchar *debug = ai_launch_property(provider, seen, "debug");

			if (filter != NULL && *filter != '\0')
				ai_launch_value(args, "--debug", filter);
			else if (debug != NULL)
				ai_launch_arg(args, "--debug");
		}
		if (resume)
			ai_launch_option(provider, seen, args, "fork-session", "--fork-session", FALSE);
		if (print_mode)
		{
			ai_launch_arg(args, "--print");
			ai_launch_option(provider, seen, args, "max-budget-usd", "--max-budget-usd", FALSE);
		}
	}
	else if (type == AI_PROVIDER_ANTIGRAVITY)
	{
		if (sandbox != NULL) ai_launch_arg(args, "--sandbox");
		ai_launch_option(provider, seen, args, "project", "--project", FALSE);
		ai_launch_option(provider, seen, args, "new-project", "--new-project", FALSE);
		ai_launch_option(provider, seen, args, "log-file", "--log-file", FALSE);
		if (print_mode)
			ai_launch_option(provider, seen, args, "print-timeout", "--print-timeout", FALSE);
	}
	else if (!(type == AI_PROVIDER_CODEX_CLI && skip != NULL))
		ai_launch_value(args, "--sandbox", sandbox);
	if (type != AI_PROVIDER_OPENCODE && type != AI_PROVIDER_GROK_BUILD)
		ai_launch_option(provider, seen, args, "additional-directories", "--add-dir", TRUE);
	if (type != AI_PROVIDER_CODEX_CLI && type != AI_PROVIDER_CURSOR)
		ai_launch_option(provider, seen, args, "agent", "--agent", FALSE);
	if (type == AI_PROVIDER_ANTIGRAVITY || type == AI_PROVIDER_CURSOR)
		ai_launch_option(provider, seen, args, "mode", "--mode", FALSE);
	if (type == AI_PROVIDER_OPENCODE)
	{
		ai_launch_option(provider, seen, args, "pure", "--pure", FALSE);
		ai_launch_option(provider, seen, args, "log-level", "--log-level", FALSE);
		ai_launch_option(provider, seen, args, "print-logs", "--print-logs", FALSE);
		ai_launch_option(provider, seen, args, "port", "--port", FALSE);
		if (resume)
			ai_launch_option(provider, seen, args, "fork-session", "--fork", FALSE);
		if (print_mode)
		{
			const gchar *names[] = { "username", "password" };
			const gchar *variables[] = { "OPENCODE_SERVER_USERNAME", "OPENCODE_SERVER_PASSWORD" };
			guint credential;
			for (credential = 0; credential < G_N_ELEMENTS(names); credential++)
			{
				g_autofree gchar *value = ai_launch_property(provider, seen, names[credential]);
				if (value == NULL) continue;
				if (command_only && g_strcmp0(g_environ_getenv(env, variables[credential]), value) != 0)
				{
					g_printerr("ai-glib launch: use inherited %s for command output\n", variables[credential]);
					return 2;
				}
				env = g_environ_setenv(env, variables[credential], value, TRUE);
			}
			ai_launch_option(provider, seen, args, "command", "--command", FALSE);
			ai_launch_option(provider, seen, args, "directory", "--dir", FALSE);
			ai_launch_option(provider, seen, args, "file-paths", "--file", TRUE);
			ai_launch_option(provider, seen, args, "title", "--title", FALSE);
			ai_launch_option(provider, seen, args, "files", "--file", TRUE);
			ai_launch_option(provider, seen, args, "attach", "--attach", FALSE);
			ai_launch_option(provider, seen, args, "share", "--share", FALSE);
			ai_launch_option(provider, seen, args, "thinking", "--thinking", FALSE);
		}
	}
	if (type == AI_PROVIDER_GROK_BUILD)
	{
		ai_launch_option(provider, seen, args, "rules", "--rules", FALSE);
		ai_launch_option(provider, seen, args, "max-turns", "--max-turns", FALSE);
		ai_launch_option(provider, seen, args, "disable-web-search", "--disable-web-search", FALSE);
		if (print_mode)
			ai_launch_value(args, "--output-format", "plain");
	}
	if (type == AI_PROVIDER_CURSOR)
	{
		g_autofree gchar *worktree = ai_launch_property(provider, seen, "worktree");
		g_autofree gchar *automatic = ai_launch_property(provider, seen, "worktree-auto");
		ai_launch_option(provider, seen, args, "workspace", "--workspace", FALSE);
		ai_launch_option(provider, seen, args, "endpoint", "--endpoint", FALSE);
		/* Headers can carry credentials, so leave them unsupported for display. */
		if (!command_only)
			ai_launch_option(provider, seen, args, "headers", "--header", TRUE);
		ai_launch_option(provider, seen, args, "plugin-dirs", "--plugin-dir", TRUE);
		if (worktree != NULL && *worktree != '\0')
			ai_launch_value(args, "--worktree", worktree);
		else if (automatic != NULL)
			ai_launch_arg(args, "--worktree");
		if ((worktree != NULL && *worktree != '\0') || automatic != NULL)
		{
			ai_launch_option(provider, seen, args, "worktree-base", "--worktree-base", FALSE);
			ai_launch_option(provider, seen, args, "skip-worktree-setup", "--skip-worktree-setup", FALSE);
		}
		ai_launch_option(provider, seen, args, "auto-review", "--auto-review", FALSE);
		ai_launch_option(provider, seen, args, "approve-mcps", "--approve-mcps", FALSE);
		ai_launch_option(provider, seen, args, "trust", "--trust", FALSE);
		if (print_mode) ai_launch_arg(args, "--print");
	}
	/* Subcommands follow their parent flags; session IDs cannot become options. */
	if (type == AI_PROVIDER_CODEX_CLI && resume)
	{
		ai_launch_arg(args, "resume");
		if (session == NULL || *session == '\0') ai_launch_arg(args, "--last");
		ai_launch_arg(args, "--");
		if (session != NULL && *session != '\0') ai_launch_arg(args, session);
	}
	else if (session != NULL && *session != '\0')
		ai_launch_value(args, type == AI_PROVIDER_OPENCODE ? "--session" :
			type == AI_PROVIDER_ANTIGRAVITY ? "--conversation" : "--resume", session);
	else if (cont != NULL)
		ai_launch_arg(args, "--continue");

	/* Audit before potentially consuming stdin for agy. */
	properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(provider), &n_properties);
	for (i = 0; i < n_properties; i++)
	{
		GParamSpec *pspec = properties[i];
		GValue value = G_VALUE_INIT;
		gboolean is_default;

		if (!(pspec->flags & G_PARAM_READABLE) || !(pspec->flags & G_PARAM_WRITABLE) ||
			g_hash_table_contains(seen, pspec->name)) continue;
		g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(pspec));
		g_object_get_property(provider, pspec->name, &value);
		is_default = g_param_value_defaults(pspec, &value);
		g_value_unset(&value);
		if (!is_default)
		{
			g_printerr("ai-glib launch: property '%s' is unsupported in this native mode\n", pspec->name);
			return 2;
		}
	}

	if (type == AI_PROVIDER_ANTIGRAVITY)
	{
		if (prompt != NULL)
			ai_launch_pair(args, print_mode ? "--print" : "--prompt-interactive", prompt);
		else if (print_mode)
		{
			if (command_only)
			{
				ai_launch_arg(args, "--print");
				stdin_substitution = TRUE;
			}
			else
			{
				g_autoptr(GString) input = g_string_new(NULL);
				gchar buffer[4096];
				ssize_t count;

				/* agy has no native plain-stdin print mode. Read directly,
				 * without a subprocess or a shell interpreting its contents. */
				while ((count = read(STDIN_FILENO, buffer, sizeof buffer)) != 0)
				{
					if (count < 0)
					{
						if (errno == EINTR) continue;
						g_printerr("ai-glib launch: reading stdin failed: %s\n", g_strerror(errno));
						return 126;
					}
					if (memchr(buffer, '\0', (gsize)count) != NULL)
					{
						g_printerr("ai-glib launch: prompt contains a NUL byte\n");
						return 2;
					}
					g_string_append_len(input, buffer, (gssize)count);
				}
				ai_launch_pair(args, "--print", input->str);
			}
		}
	}
	else if (type == AI_PROVIDER_OPENCODE && !print_mode)
	{
		if (prompt != NULL)
			ai_launch_pair(args, "--prompt", prompt);
	}
	else if (type == AI_PROVIDER_GROK_BUILD && print_mode)
	{
		if (prompt != NULL)
			ai_launch_pair(args, "--single", prompt);
		else
			ai_launch_value(args, "--prompt-file", "/dev/stdin");
	}
	else if (prompt != NULL || (type == AI_PROVIDER_CODEX_CLI && print_mode))
	{
		if (!(type == AI_PROVIDER_CODEX_CLI && resume)) ai_launch_arg(args, "--");
		ai_launch_arg(args, prompt != NULL ? prompt : "-");
	}

	if (command_only)
	{
		if (cwd != NULL && *cwd != '\0')
		{
			g_autofree gchar *quoted = g_shell_quote(cwd);
			g_string_append_printf(command, "(cd -- %s && ", quoted);
		}
		for (i = 0; i < args->len; i++)
		{
			g_autofree gchar *quoted = g_shell_quote((const gchar *)g_ptr_array_index(args, i));
			if (i != 0) g_string_append_c(command, ' ');
			g_string_append(command, quoted);
		}
		/* Attach stdin to the option so leading '-' bytes remain its value. */
		if (stdin_substitution) g_string_append(command, "=\"$(cat)\"");
		if (cwd != NULL && *cwd != '\0') g_string_append_c(command, ')');
		g_print("%s\n", command->str);
		return 0;
	}
	/* Keep a directory fd so a failed exec does not strand the caller elsewhere.
	 * Resolver returns a path, so execve needs no GNU execvpe declaration. */
	if (cwd != NULL && *cwd != '\0')
	{
		saved_cwd = open(".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
		if (saved_cwd < 0 || chdir(cwd) < 0)
		{
			saved_errno = errno;
			if (saved_cwd >= 0) close(saved_cwd);
			g_printerr("ai-glib launch: cannot change working directory: %s\n", g_strerror(saved_errno));
			return 126;
		}
	}
	if (type == AI_PROVIDER_OPENCODE && cwd != NULL && *cwd != '\0')
	{
		g_autofree gchar *actual_cwd = g_get_current_dir();
		env = g_environ_setenv(env, "PWD", actual_cwd, TRUE);
	}
	g_ptr_array_add(args, NULL);
	fflush(NULL);
	execve(exe, (gchar * const *)args->pdata, env);
	saved_errno = errno;
	if (saved_cwd >= 0)
	{
		if (fchdir(saved_cwd) < 0)
			g_printerr("ai-glib launch: cannot restore working directory: %s\n", g_strerror(errno));
		close(saved_cwd);
	}
	g_printerr("ai-glib launch: exec failed: %s%s\n", g_strerror(saved_errno),
		saved_errno == E2BIG ? "; prompt or environment exceeds the OS argument limit; shorten it or use native stdin where supported" : "");
	return saved_errno == ENOENT ? 127 : 126;
}
