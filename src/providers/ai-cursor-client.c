/*
 * ai-cursor-client.c - Cursor Agent CLI client (cursor-agent)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Wraps Cursor's `cursor-agent` binary (the `agent` CLI) in `--print`
 * mode. `--print` is a boolean, not a prompt-taking flag. The prompt is
 * piped on stdin so a long conversation never hits MAX_ARG_STRLEN, and
 * so it never appears in ps output.
 *
 * The default binary is `cursor-agent` rather than `agent`: grok also
 * ships an `agent` on PATH, and the Cursor install provides both names.
 */

#include "config.h"

#include <string.h>
#include <json-glib/json-glib.h>

#include "providers/ai-cursor-client.h"
#include "providers/ai-cursor-client-internal.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

static const gchar * const CURSOR_MODELS[] = {
	"auto",
	"gpt-5.3-codex-low",
	"gpt-5.3-codex-low-fast",
	"gpt-5.3-codex",
	"gpt-5.3-codex-fast",
	"gpt-5.3-codex-high",
	"gpt-5.3-codex-high-fast",
	"gpt-5.3-codex-xhigh",
	"gpt-5.3-codex-xhigh-fast",
	"gpt-5.2",
	"cursor-grok-4.6-high-fast",
	"composer-2.5",
	"claude-opus-5-thinking-high",
	"claude-opus-5-thinking-high-fast",
	"gpt-5.6-sol-high",
	"gpt-5.6-sol-high-fast",
	"gpt-5.6-sol-xhigh",
	"gpt-5.6-sol-xhigh-fast",
	"claude-fable-5-thinking-high",
	"claude-fable-5-thinking-xhigh",
	"cursor-grok-4.5-high",
	"cursor-grok-4.5-high-fast",
	"gemini-3.7-flash-high",
	"claude-sonnet-5-thinking-high",
	"claude-sonnet-5-thinking-xhigh",
	"gpt-5.6-luna-high",
	"cursor-grok-4.6-low",
	"cursor-grok-4.6-low-fast",
	"cursor-grok-4.6-medium",
	"cursor-grok-4.6-medium-fast",
	"cursor-grok-4.6-high",
	"cursor-grok-4.6-xhigh",
	"cursor-grok-4.6-xhigh-fast",
	"composer-2.5-fast",
	"claude-opus-5-low",
	"claude-opus-5-low-fast",
	"claude-opus-5-medium",
	"claude-opus-5-medium-fast",
	"claude-opus-5-high",
	"claude-opus-5-high-fast",
	"claude-opus-5-thinking-low",
	"claude-opus-5-thinking-low-fast",
	"claude-opus-5-thinking-medium",
	"claude-opus-5-thinking-medium-fast",
	"claude-opus-5-thinking-xhigh",
	"claude-opus-5-thinking-xhigh-fast",
	"claude-opus-5-thinking-max",
	"claude-opus-5-thinking-max-fast",
	"claude-opus-4-8-low",
	"claude-opus-4-8-low-fast",
	"claude-opus-4-8-medium",
	"claude-opus-4-8-medium-fast",
	"claude-opus-4-8-high",
	"claude-opus-4-8-high-fast",
	"claude-opus-4-8-xhigh",
	"claude-opus-4-8-xhigh-fast",
	"claude-opus-4-8-max",
	"claude-opus-4-8-max-fast",
	"claude-opus-4-8-thinking-low",
	"claude-opus-4-8-thinking-low-fast",
	"claude-opus-4-8-thinking-medium",
	"claude-opus-4-8-thinking-medium-fast",
	"claude-opus-4-8-thinking-high",
	"claude-opus-4-8-thinking-high-fast",
	"claude-opus-4-8-thinking-xhigh",
	"claude-opus-4-8-thinking-xhigh-fast",
	"claude-opus-4-8-thinking-max",
	"claude-opus-4-8-thinking-max-fast",
	"gpt-5.6-sol-none",
	"gpt-5.6-sol-none-fast",
	"gpt-5.6-sol-low",
	"gpt-5.6-sol-low-fast",
	"gpt-5.6-sol-medium",
	"gpt-5.6-sol-medium-fast",
	"gpt-5.6-sol-max",
	"gpt-5.6-sol-max-fast",
	"gpt-5.5-none",
	"gpt-5.5-none-fast",
	"gpt-5.5-low",
	"gpt-5.5-low-fast",
	"gpt-5.5-medium",
	"gpt-5.5-medium-fast",
	"gpt-5.5-high",
	"gpt-5.5-high-fast",
	"gpt-5.5-extra-high",
	"gpt-5.5-extra-high-fast",
	"claude-fable-5-low",
	"claude-fable-5-medium",
	"claude-fable-5-high",
	"claude-fable-5-xhigh",
	"claude-fable-5-max",
	"claude-fable-5-thinking-low",
	"claude-fable-5-thinking-medium",
	"claude-fable-5-thinking-max",
	"cursor-grok-4.5-low",
	"cursor-grok-4.5-low-fast",
	"cursor-grok-4.5-medium",
	"cursor-grok-4.5-medium-fast",
	"gemini-3.7-flash-low",
	"gemini-3.7-flash-medium",
	"gpt-5.6-terra-none",
	"gpt-5.6-terra-none-fast",
	"gpt-5.6-terra-low",
	"gpt-5.6-terra-low-fast",
	"gpt-5.6-terra-medium",
	"gpt-5.6-terra-medium-fast",
	"gpt-5.6-terra-high",
	"gpt-5.6-terra-high-fast",
	"gpt-5.6-terra-xhigh",
	"gpt-5.6-terra-xhigh-fast",
	"gpt-5.6-terra-max",
	"gpt-5.6-terra-max-fast",
	"claude-sonnet-5-low",
	"claude-sonnet-5-medium",
	"claude-sonnet-5-high",
	"claude-sonnet-5-xhigh",
	"claude-sonnet-5-max",
	"claude-sonnet-5-thinking-low",
	"claude-sonnet-5-thinking-medium",
	"claude-sonnet-5-thinking-max",
	"claude-4.6-sonnet-medium",
	"claude-4.6-sonnet-medium-thinking",
	"claude-opus-4-7-low",
	"claude-opus-4-7-low-fast",
	"claude-opus-4-7-medium",
	"claude-opus-4-7-medium-fast",
	"claude-opus-4-7-high",
	"claude-opus-4-7-high-fast",
	"claude-opus-4-7-xhigh",
	"claude-opus-4-7-xhigh-fast",
	"claude-opus-4-7-max",
	"claude-opus-4-7-max-fast",
	"claude-opus-4-7-thinking-low",
	"claude-opus-4-7-thinking-low-fast",
	"claude-opus-4-7-thinking-medium",
	"claude-opus-4-7-thinking-medium-fast",
	"claude-opus-4-7-thinking-high",
	"claude-opus-4-7-thinking-high-fast",
	"claude-opus-4-7-thinking-xhigh",
	"claude-opus-4-7-thinking-xhigh-fast",
	"claude-opus-4-7-thinking-max",
	"claude-opus-4-7-thinking-max-fast",
	"gpt-5.4-low",
	"gpt-5.4-medium",
	"gpt-5.4-medium-fast",
	"gpt-5.4-high",
	"gpt-5.4-high-fast",
	"gpt-5.4-xhigh",
	"gpt-5.4-xhigh-fast",
	"claude-4.6-opus-high",
	"claude-4.6-opus-max",
	"claude-4.6-opus-high-thinking",
	"claude-4.6-opus-max-thinking",
	"claude-4.5-opus-high",
	"claude-4.5-opus-high-thinking",
	"gpt-5.2-low",
	"gpt-5.2-low-fast",
	"gpt-5.2-fast",
	"gpt-5.2-high",
	"gpt-5.2-high-fast",
	"gpt-5.2-xhigh",
	"gpt-5.2-xhigh-fast",
	"gpt-5.6-luna-none",
	"gpt-5.6-luna-none-fast",
	"gpt-5.6-luna-low",
	"gpt-5.6-luna-low-fast",
	"gpt-5.6-luna-medium",
	"gpt-5.6-luna-medium-fast",
	"gpt-5.6-luna-high-fast",
	"gpt-5.6-luna-xhigh",
	"gpt-5.6-luna-xhigh-fast",
	"gpt-5.6-luna-max",
	"gpt-5.6-luna-max-fast",
	"gemini-3.6-flash-minimal",
	"gemini-3.6-flash-low",
	"gemini-3.6-flash-medium",
	"gemini-3.6-flash-high",
	"gemini-3.1-pro",
	"gpt-5.4-mini-none",
	"gpt-5.4-mini-low",
	"gpt-5.4-mini-medium",
	"gpt-5.4-mini-high",
	"gpt-5.4-mini-xhigh",
	"gpt-5.4-nano-none",
	"gpt-5.4-nano-low",
	"gpt-5.4-nano-medium",
	"gpt-5.4-nano-high",
	"gpt-5.4-nano-xhigh",
	"claude-4.5-sonnet",
	"claude-4.5-sonnet-thinking",
	"gpt-5.1-low",
	"gpt-5.1",
	"gpt-5.1-high",
	"gemini-3.5-flash",
	"claude-4-sonnet",
	"claude-4-sonnet-thinking",
	"gpt-5-mini",
	"kimi-k3-low",
	"kimi-k3-high",
	"kimi-k3-max",
	"kimi-k2.7-code",
	"glm-5.2-high",
	"glm-5.2-max",
	"gemini-3-flash",
	NULL
};


struct _AiCursorClient
{
	AiCliClient parent_instance;

	gboolean skip_permissions;
	gchar   *additional_directories;
	gchar   *plugin_dirs;
	gchar   *headers;
	gchar   *mode;
	gchar   *sandbox;
	gchar   *workspace;
	gchar   *endpoint;
	gchar   *api_key;
	gchar   *model_params;
	gchar   *worktree;
	gchar   *worktree_base;
	gboolean worktree_auto;
	gboolean skip_worktree_setup;
	gboolean auto_review;
	gboolean approve_mcps;
	gboolean trust;
	gboolean continue_session;

	gchar   *turn_system_prompt;
	gchar   *last_tool_summary;
};

static void ai_cursor_client_provider_init(AiProviderInterface *iface);
static void ai_cursor_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiCursorClient, ai_cursor_client, AI_TYPE_CLI_CLIENT,
			G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
					      ai_cursor_client_provider_init)
			G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
					      ai_cursor_client_streamable_init))

enum
{
	PROP_0,
	PROP_SKIP_PERMISSIONS,
	PROP_ADDITIONAL_DIRECTORIES,
	PROP_PLUGIN_DIRS,
	PROP_HEADERS,
	PROP_MODE,
	PROP_SANDBOX,
	PROP_WORKSPACE,
	PROP_ENDPOINT,
	PROP_API_KEY,
	PROP_MODEL_PARAMS,
	PROP_WORKTREE,
	PROP_WORKTREE_BASE,
	PROP_WORKTREE_AUTO,
	PROP_SKIP_WORKTREE_SETUP,
	PROP_AUTO_REVIEW,
	PROP_APPROVE_MCPS,
	PROP_TRUST,
	PROP_CONTINUE_SESSION,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_cursor_client_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiCursorClient *self = AI_CURSOR_CLIENT(object);

	switch (prop_id)
	{
		case PROP_SKIP_PERMISSIONS:
			g_value_set_boolean(value, self->skip_permissions);
			break;
		case PROP_ADDITIONAL_DIRECTORIES:
			g_value_set_string(value, self->additional_directories);
			break;
		case PROP_PLUGIN_DIRS:
			g_value_set_string(value, self->plugin_dirs);
			break;
		case PROP_HEADERS:
			g_value_set_string(value, self->headers);
			break;
		case PROP_MODE:
			g_value_set_string(value, self->mode);
			break;
		case PROP_SANDBOX:
			g_value_set_string(value, self->sandbox);
			break;
		case PROP_WORKSPACE:
			g_value_set_string(value, self->workspace);
			break;
		case PROP_ENDPOINT:
			g_value_set_string(value, self->endpoint);
			break;
		case PROP_API_KEY:
			g_value_set_string(value, self->api_key);
			break;
		case PROP_MODEL_PARAMS:
			g_value_set_string(value, self->model_params);
			break;
		case PROP_WORKTREE:
			g_value_set_string(value, self->worktree);
			break;
		case PROP_WORKTREE_BASE:
			g_value_set_string(value, self->worktree_base);
			break;
		case PROP_WORKTREE_AUTO:
			g_value_set_boolean(value, self->worktree_auto);
			break;
		case PROP_SKIP_WORKTREE_SETUP:
			g_value_set_boolean(value, self->skip_worktree_setup);
			break;
		case PROP_AUTO_REVIEW:
			g_value_set_boolean(value, self->auto_review);
			break;
		case PROP_APPROVE_MCPS:
			g_value_set_boolean(value, self->approve_mcps);
			break;
		case PROP_TRUST:
			g_value_set_boolean(value, self->trust);
			break;
		case PROP_CONTINUE_SESSION:
			g_value_set_boolean(value, self->continue_session);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void
ai_cursor_client_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiCursorClient *self = AI_CURSOR_CLIENT(object);

	switch (prop_id)
	{
		case PROP_SKIP_PERMISSIONS:
			self->skip_permissions = g_value_get_boolean(value);
			break;
		case PROP_ADDITIONAL_DIRECTORIES:
			g_free(self->additional_directories);
			self->additional_directories = g_value_dup_string(value);
			break;
		case PROP_PLUGIN_DIRS:
			g_free(self->plugin_dirs);
			self->plugin_dirs = g_value_dup_string(value);
			break;
		case PROP_HEADERS:
			g_free(self->headers);
			self->headers = g_value_dup_string(value);
			break;
		case PROP_MODE:
			g_free(self->mode);
			self->mode = g_value_dup_string(value);
			break;
		case PROP_SANDBOX:
			g_free(self->sandbox);
			self->sandbox = g_value_dup_string(value);
			break;
		case PROP_WORKSPACE:
			g_free(self->workspace);
			self->workspace = g_value_dup_string(value);
			break;
		case PROP_ENDPOINT:
			g_free(self->endpoint);
			self->endpoint = g_value_dup_string(value);
			break;
		case PROP_API_KEY:
			g_free(self->api_key);
			self->api_key = g_value_dup_string(value);
			break;
		case PROP_MODEL_PARAMS:
			g_free(self->model_params);
			self->model_params = g_value_dup_string(value);
			break;
		case PROP_WORKTREE:
			g_free(self->worktree);
			self->worktree = g_value_dup_string(value);
			break;
		case PROP_WORKTREE_BASE:
			g_free(self->worktree_base);
			self->worktree_base = g_value_dup_string(value);
			break;
		case PROP_WORKTREE_AUTO:
			self->worktree_auto = g_value_get_boolean(value);
			break;
		case PROP_SKIP_WORKTREE_SETUP:
			self->skip_worktree_setup = g_value_get_boolean(value);
			break;
		case PROP_AUTO_REVIEW:
			self->auto_review = g_value_get_boolean(value);
			break;
		case PROP_APPROVE_MCPS:
			self->approve_mcps = g_value_get_boolean(value);
			break;
		case PROP_TRUST:
			self->trust = g_value_get_boolean(value);
			break;
		case PROP_CONTINUE_SESSION:
			self->continue_session = g_value_get_boolean(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static const gchar *AI_CURSOR_MODES[] = { "plan", "ask", NULL };
static const gchar *AI_CURSOR_SANDBOXES[] = { "enabled", "disabled", NULL };

static gboolean
str_in_list(const gchar *value, const gchar **list)
{
	gsize i;

	for (i = 0; list[i] != NULL; i++)
	{
		if (g_strcmp0(value, list[i]) == 0)
			return TRUE;
	}

	return FALSE;
}

static void
emit_value_flag(GPtrArray *args, const gchar *flag, const gchar *value)
{
	if (value == NULL || value[0] == '\0')
		return;

	g_ptr_array_add(args, g_strdup(flag));
	g_ptr_array_add(args, g_strdup(value));
}

static void
emit_repeated_flag(GPtrArray *args, const gchar *flag, const gchar *csv)
{
	g_auto(GStrv) parts = NULL;
	gsize i;

	if (csv == NULL || csv[0] == '\0')
		return;

	parts = g_strsplit(csv, ",", -1);

	for (i = 0; parts[i] != NULL; i++)
	{
		g_strstrip(parts[i]);
		if (parts[i][0] == '\0')
			continue;

		g_ptr_array_add(args, g_strdup(flag));
		g_ptr_array_add(args, g_strdup(parts[i]));
	}
}

static void
emit_session_args(AiCursorClient *self, GPtrArray *args)
{
	if (self->skip_permissions)
		g_ptr_array_add(args, g_strdup("--force"));

	emit_repeated_flag(args, "--add-dir", self->additional_directories);
	emit_repeated_flag(args, "--plugin-dir", self->plugin_dirs);
	emit_repeated_flag(args, "--header", self->headers);
	emit_value_flag(args, "--endpoint", self->endpoint);
	emit_value_flag(args, "--workspace", self->workspace);

	if (self->mode != NULL && self->mode[0] != '\0')
	{
		if (str_in_list(self->mode, AI_CURSOR_MODES))
		{
			g_ptr_array_add(args, g_strdup("--mode"));
			g_ptr_array_add(args, g_strdup(self->mode));
		}
		else
		{
			g_message("cursor: unknown mode '%s'; omitting the flag. "
				  "Valid modes: plan, ask",
				  self->mode);
		}
	}

	if (self->sandbox != NULL && self->sandbox[0] != '\0')
	{
		if (str_in_list(self->sandbox, AI_CURSOR_SANDBOXES))
		{
			g_ptr_array_add(args, g_strdup("--sandbox"));
			g_ptr_array_add(args, g_strdup(self->sandbox));
		}
		else
		{
			g_message("cursor: unknown sandbox '%s'; omitting the "
				  "flag. Valid values: enabled, disabled",
				  self->sandbox);
		}
	}

	if (self->worktree != NULL && self->worktree[0] != '\0')
	{
		g_ptr_array_add(args, g_strdup("--worktree"));
		g_ptr_array_add(args, g_strdup(self->worktree));
	}
	else if (self->worktree_auto)
	{
		g_ptr_array_add(args, g_strdup("--worktree"));
	}

	if ((self->worktree != NULL && self->worktree[0] != '\0') || self->worktree_auto)
	{
		emit_value_flag(args, "--worktree-base", self->worktree_base);
		if (self->skip_worktree_setup)
			g_ptr_array_add(args, g_strdup("--skip-worktree-setup"));
	}
	if (self->auto_review)
		g_ptr_array_add(args, g_strdup("--auto-review"));
	if (self->approve_mcps)
		g_ptr_array_add(args, g_strdup("--approve-mcps"));
	if (self->trust)
		g_ptr_array_add(args, g_strdup("--trust"));
}

static gchar *
cursor_model_arg(AiCursorClient *self, const gchar *model)
{
	if (model == NULL || model[0] == '\0')
		model = AI_CURSOR_DEFAULT_MODEL;

	if (self->model_params != NULL && self->model_params[0] != '\0')
	{
		if (strchr(model, '[') != NULL)
			return g_strdup(model);

		return g_strdup_printf("%s[%s]", model, self->model_params);
	}

	return g_strdup(model);
}

static gchar *
ai_cursor_client_get_executable_path(AiCliClient *client)
{
	const gchar *env_path;

	(void)client;

	env_path = g_getenv("CURSOR_AGENT_PATH");
	if (env_path != NULL && env_path[0] != '\0')
		return g_strdup(env_path);

	env_path = g_getenv("CURSOR_PATH");
	if (env_path != NULL && env_path[0] != '\0')
		return g_strdup(env_path);

	return g_strdup("cursor-agent");
}

/*
 * Spawn so an api-key property becomes CURSOR_API_KEY on the child
 * rather than an argv word (which would show up in ps).
 */
static GSubprocess *
ai_cursor_client_spawn(
	AiCliClient           *client,
	const gchar *const    *argv,
	GSubprocessFlags       flags,
	GError               **error
){
	AiCursorClient *self = AI_CURSOR_CLIENT(client);
	g_autoptr(GSubprocessLauncher) launcher = NULL;

	launcher = ai_cli_client_create_launcher(client, flags);

	if (self->api_key != NULL && self->api_key[0] != '\0')
	{
		g_subprocess_launcher_setenv(launcher, "CURSOR_API_KEY",
					     self->api_key, TRUE);
	}

	return g_subprocess_launcher_spawnv(launcher, argv, error);
}

gchar **
ai_cursor_client_build_argv(
	AiCliClient *client,
	GList       *messages,
	const gchar *system_prompt,
	gint         max_tokens,
	gboolean     streaming
){
	AiCursorClient *self = AI_CURSOR_CLIENT(client);
	GPtrArray *args;
	g_autofree gchar *model_arg = NULL;
	const gchar *session_id;
	gboolean persist;

	(void)messages;
	(void)max_tokens;

	g_free(self->turn_system_prompt);
	self->turn_system_prompt = g_strdup(system_prompt);

	args = g_ptr_array_new();

	g_ptr_array_add(args, g_strdup("cursor-agent"));
	g_ptr_array_add(args, g_strdup("--print"));

	if (streaming)
	{
		g_ptr_array_add(args, g_strdup("--output-format"));
		g_ptr_array_add(args, g_strdup("stream-json"));
		g_ptr_array_add(args, g_strdup("--stream-partial-output"));
	}
	else
	{
		g_ptr_array_add(args, g_strdup("--output-format"));
		g_ptr_array_add(args, g_strdup("json"));
	}

	model_arg = cursor_model_arg(self, ai_cli_client_get_model(client));
	g_ptr_array_add(args, g_strdup("--model"));
	g_ptr_array_add(args, g_steal_pointer(&model_arg));

	emit_session_args(self, args);

	persist = ai_cli_client_get_session_persistence(client);
	session_id = ai_cli_client_get_session_id(client);
	if (persist && session_id != NULL && session_id[0] != '\0')
	{
		g_ptr_array_add(args, g_strdup("--resume"));
		g_ptr_array_add(args, g_strdup(session_id));
	}
	else if (persist && self->continue_session)
	{
		g_ptr_array_add(args, g_strdup("--continue"));
	}

	g_ptr_array_add(args, NULL);

	return (gchar **)g_ptr_array_free(args, FALSE);
}

static gboolean
cursor_should_send_system_prompt(AiCursorClient *self, AiCliClient *client)
{
	const gchar *session_id;

	if (!ai_cli_client_get_session_persistence(client))
		return TRUE;

	session_id = ai_cli_client_get_session_id(client);
	if (session_id != NULL && session_id[0] != '\0')
		return FALSE;

	if (self->continue_session)
		return FALSE;

	return TRUE;
}

static gchar *
ai_cursor_client_build_stdin(
	AiCliClient *client,
	GList       *messages
){
	AiCursorClient *self = AI_CURSOR_CLIENT(client);
	GString *prompt;
	const gchar *sys_prompt;
	GList *l;

	prompt = g_string_new("");

	if (cursor_should_send_system_prompt(self, client))
	{
		sys_prompt = self->turn_system_prompt;
		if (sys_prompt == NULL || sys_prompt[0] == '\0')
			sys_prompt = ai_cli_client_get_system_prompt(client);

		if (sys_prompt != NULL && sys_prompt[0] != '\0')
		{
			g_string_append(prompt, "<system>\n");
			g_string_append(prompt, sys_prompt);
			g_string_append(prompt, "\n</system>\n\n");
		}
	}

	for (l = messages; l != NULL; l = l->next)
	{
		AiMessage *msg = l->data;
		g_autofree gchar *text = ai_message_get_text(msg);
		AiRole role = ai_message_get_role(msg);

		if (text == NULL || text[0] == '\0')
			continue;

		if (prompt->len > 0 && role == AI_ROLE_USER)
			g_string_append(prompt, "\n\n");

		if (role == AI_ROLE_USER)
		{
			g_string_append(prompt, text);
		}
		else if (role == AI_ROLE_ASSISTANT)
		{
			if (prompt->len > 0)
				g_string_append(prompt, "\n\n");
			g_string_append_printf(prompt,
				"Previous assistant response: %s", text);
		}
	}

	g_string_append(prompt,
		"\n\nIMPORTANT: Always include a plain text response. "
		"Tool use is fine, but you MUST provide a text summary of "
		"your work when finished. Never end your turn on tool calls alone.");

	return g_string_free(prompt, FALSE);
}

static const gchar *
cursor_get_string(JsonObject *obj, const gchar *name)
{
	JsonNode *node;

	if (obj == NULL || name == NULL)
		return NULL;

	node = json_object_get_member(obj, name);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	if (json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return json_node_get_string(node);
}

static JsonObject *
cursor_get_object(JsonObject *obj, const gchar *name)
{
	JsonNode *node;

	if (obj == NULL || name == NULL)
		return NULL;

	node = json_object_get_member(obj, name);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	return json_node_get_object(node);
}

static JsonArray *
cursor_get_array(JsonObject *obj, const gchar *name)
{
	JsonNode *node;

	if (obj == NULL || name == NULL)
		return NULL;

	node = json_object_get_member(obj, name);
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return NULL;

	return json_node_get_array(node);
}

static gboolean
cursor_get_boolean(JsonObject *obj, const gchar *name, gboolean fallback)
{
	JsonNode *node;

	if (obj == NULL || name == NULL)
		return fallback;

	node = json_object_get_member(obj, name);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return fallback;

	if (json_node_get_value_type(node) != G_TYPE_BOOLEAN)
		return fallback;

	return json_node_get_boolean(node);
}

static gboolean
cursor_has_member(JsonObject *obj, const gchar *name)
{
	return obj != NULL && name != NULL && json_object_has_member(obj, name);
}

static JsonObject *
cursor_try_parse(JsonParser *parser, const gchar *candidate)
{
	JsonNode *root;
	JsonArray *array;
	guint i;

	if (candidate == NULL || candidate[0] == '\0')
		return NULL;

	if (!json_parser_load_from_data(parser, candidate, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);
	if (root == NULL)
		return NULL;

	if (JSON_NODE_HOLDS_OBJECT(root))
		return json_node_get_object(root);

	if (!JSON_NODE_HOLDS_ARRAY(root))
		return NULL;

	array = json_node_get_array(root);
	for (i = json_array_get_length(array); i > 0; i--)
	{
		JsonNode *element = json_array_get_element(array, i - 1);
		JsonObject *obj;

		if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element))
			continue;

		obj = json_node_get_object(element);
		if (g_strcmp0(cursor_get_string(obj, "type"), "result") == 0)
			return obj;
	}

	return NULL;
}

static JsonObject *
cursor_parse_output_object(
	JsonParser  *parser,
	const gchar *json,
	GError     **error
){
	g_autofree gchar *last_line = NULL;
	g_auto(GStrv) lines = NULL;
	const gchar *brace;
	JsonObject *obj;
	gint i;

	obj = cursor_try_parse(parser, json);
	if (obj != NULL)
		return obj;

	lines = g_strsplit(json, "\n", -1);
	for (i = 0; lines[i] != NULL; i++)
	{
		g_strstrip(lines[i]);
		if (lines[i][0] != '\0')
		{
			g_free(last_line);
			last_line = g_strdup(lines[i]);
		}
	}

	obj = cursor_try_parse(parser, last_line);
	if (obj != NULL)
		return obj;

	brace = strchr(json, '{');
	if (brace != NULL && brace != json)
	{
		obj = cursor_try_parse(parser, brace);
		if (obj != NULL)
			return obj;
	}

	g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
		    "Could not parse a JSON object from CLI output");

	return NULL;
}

static void
cursor_store_session_id(AiCliClient *client, JsonObject *obj)
{
	const gchar *session_id;

	session_id = cursor_get_string(obj, "session_id");
	if (session_id != NULL && session_id[0] != '\0' &&
	    ai_cli_client_get_session_persistence(client))
	{
		ai_cli_client_set_session_id(client, session_id);
	}
}

static void
cursor_set_error_from_object(JsonObject *obj, GError **error)
{
	const gchar *message;

	message = cursor_get_string(obj, "result");
	if (message == NULL || message[0] == '\0')
		message = cursor_get_string(obj, "subtype");
	if (message == NULL || message[0] == '\0')
		message = "Unknown error";

	g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		    "CLI error: %s", message);
}

static AiResponse *
ai_cursor_client_parse_json_output(
	AiCliClient *client,
	const gchar *json,
	GError     **error
){
	AiCursorClient *self = AI_CURSOR_CLIENT(client);
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(AiResponse) response = NULL;
	JsonObject *obj;
	const gchar *type;
	const gchar *text;
	const gchar *session_id;

	parser = json_parser_new();

	obj = cursor_parse_output_object(parser, json, error);
	if (obj == NULL)
		return NULL;

	type = cursor_get_string(obj, "type");
	if (g_strcmp0(type, "result") != 0 && type != NULL && type[0] != '\0')
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
			    "Unexpected response type: %s", type);
		return NULL;
	}

	if (cursor_get_boolean(obj, "is_error", FALSE))
	{
		cursor_set_error_from_object(obj, error);
		return NULL;
	}

	session_id = cursor_get_string(obj, "session_id");
	if (session_id == NULL)
		session_id = "";

	response = ai_response_new(session_id, ai_cli_client_get_model(client));
	ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);

	cursor_store_session_id(client, obj);

	text = cursor_get_string(obj, "result");
	if (text != NULL && text[0] != '\0')
	{
		g_autoptr(AiTextContent) content = ai_text_content_new(text);
		ai_response_add_content_block(response,
			(AiContentBlock *)g_steal_pointer(&content));
	}
	else
	{
		g_free(self->last_tool_summary);
		self->last_tool_summary = g_strdup(
			"(completed tool operations — no text summary was provided)");
	}

	return (AiResponse *)g_steal_pointer(&response);
}

static gchar *
cursor_assistant_text(JsonObject *message)
{
	JsonArray *content;
	GString *acc;
	guint n;
	guint i;

	if (message == NULL)
		return NULL;

	content = cursor_get_array(message, "content");
	if (content == NULL)
		return NULL;

	acc = g_string_new("");
	n = json_array_get_length(content);

	for (i = 0; i < n; i++)
	{
		JsonNode *el = json_array_get_element(content, i);
		JsonObject *block;
		const gchar *text;

		if (el == NULL || !JSON_NODE_HOLDS_OBJECT(el))
			continue;

		block = json_node_get_object(el);
		if (g_strcmp0(cursor_get_string(block, "type"), "text") != 0)
			continue;

		text = cursor_get_string(block, "text");
		if (text != NULL)
			g_string_append(acc, text);
	}

	if (acc->len == 0)
	{
		g_string_free(acc, TRUE);
		return NULL;
	}

	return g_string_free(acc, FALSE);
}

static void
cursor_emit_tool_started(JsonObject *obj, GPtrArray *out_events)
{
	JsonObject *tool_call;
	const gchar *id;
	const gchar *name = NULL;
	JsonNode *args = NULL;
	g_autoptr(AiToolUse) tool_use = NULL;
	g_autofree gchar *derived = NULL;

	id = cursor_get_string(obj, "call_id");
	tool_call = cursor_get_object(obj, "tool_call");
	if (tool_call == NULL)
		return;

	{
		JsonObjectIter iter;
		const gchar *key;
		JsonNode *value;

		json_object_iter_init(&iter, tool_call);
		while (json_object_iter_next(&iter, &key, &value))
		{
			JsonObject *call;

			if (g_strcmp0(key, "function") == 0 &&
			    value != NULL && JSON_NODE_HOLDS_OBJECT(value))
			{
				JsonObject *fn = json_node_get_object(value);

				name = cursor_get_string(fn, "name");
				if (json_object_has_member(fn, "arguments"))
					args = json_object_get_member(fn, "arguments");
				break;
			}

			if (value == NULL || !JSON_NODE_HOLDS_OBJECT(value))
				continue;

			call = json_node_get_object(value);
			if (g_str_has_suffix(key, "ToolCall"))
			{
				derived = g_strndup(key, strlen(key) - strlen("ToolCall"));
				name = derived;
			}
			else
			{
				name = key;
			}

			if (json_object_has_member(call, "args"))
				args = json_object_get_member(call, "args");
			break;
		}
	}

	if (name == NULL || name[0] == '\0')
		return;

	tool_use = ai_tool_use_new(id != NULL ? id : "", name, args);
	g_ptr_array_add(out_events, ai_event_new_tool_started(tool_use));
}

static void
cursor_emit_tool_finished(JsonObject *obj, GPtrArray *out_events)
{
	JsonObject *tool_call;
	const gchar *id;
	GString *text;
	gboolean is_error = FALSE;
	g_autoptr(AiToolResult) result = NULL;
	g_autofree gchar *json_dump = NULL;

	id = cursor_get_string(obj, "call_id");
	tool_call = cursor_get_object(obj, "tool_call");
	text = g_string_new("");

	if (tool_call != NULL)
	{
		JsonObjectIter iter;
		const gchar *key;
		JsonNode *value;

		json_object_iter_init(&iter, tool_call);
		while (json_object_iter_next(&iter, &key, &value))
		{
			JsonObject *call;
			JsonObject *res;
			JsonObject *success;
			JsonObject *err;
			const gchar *content;

			if (value == NULL || !JSON_NODE_HOLDS_OBJECT(value))
				continue;

			call = json_node_get_object(value);
			res = cursor_get_object(call, "result");
			if (res == NULL)
				continue;

			success = cursor_get_object(res, "success");
			err = cursor_get_object(res, "error");
			if (success != NULL)
			{
				content = cursor_get_string(success, "content");
				if (content != NULL)
					g_string_append(text, content);
				else
				{
					g_autoptr(JsonGenerator) gen = json_generator_new();
					g_autoptr(JsonNode) node =
						json_node_new(JSON_NODE_OBJECT);

					json_node_set_object(node, success);
					json_generator_set_root(gen, node);
					json_dump = json_generator_to_data(gen, NULL);
					if (json_dump != NULL)
						g_string_append(text, json_dump);
				}
			}
			else if (err != NULL)
			{
				is_error = TRUE;
				content = cursor_get_string(err, "message");
				if (content != NULL)
					g_string_append(text, content);
			}
			break;
		}
	}

	result = ai_tool_result_new(id != NULL ? id : "", text->str, is_error);
	g_string_free(text, TRUE);
	g_ptr_array_add(out_events, ai_event_new_tool_finished(NULL, result));
}

static gboolean
ai_cursor_client_parse_stream_line(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	gchar       **delta_text,
	GError      **error
);

static gboolean
ai_cursor_client_parse_stream_events(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	GPtrArray    *out_events,
	GError      **error
){
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;
	JsonObject *obj;
	const gchar *type;
	const gchar *subtype;

	if (line == NULL || line[0] == '\0')
		return TRUE;

	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, line, -1, error))
	{
		g_clear_error(error);
		return TRUE;
	}

	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return TRUE;

	obj = json_node_get_object(root);
	type = cursor_get_string(obj, "type");
	subtype = cursor_get_string(obj, "subtype");

	if (g_strcmp0(type, "system") == 0)
	{
		cursor_store_session_id(client, obj);
		return TRUE;
	}

	if (g_strcmp0(type, "user") == 0)
	{
		cursor_store_session_id(client, obj);
		return TRUE;
	}

	if (g_strcmp0(type, "assistant") == 0)
	{
		g_autofree gchar *text = NULL;

		cursor_store_session_id(client, obj);

		/*
		 * With --stream-partial-output, only events that carry
		 * timestamp_ms and no model_call_id are new text. The
		 * others are duplicate flushes.
		 */
		if (!cursor_has_member(obj, "timestamp_ms") ||
		    cursor_has_member(obj, "model_call_id"))
		{
			return TRUE;
		}

		text = cursor_assistant_text(cursor_get_object(obj, "message"));
		if (text != NULL && text[0] != '\0')
			g_ptr_array_add(out_events, ai_event_new_text_delta(text));

		return TRUE;
	}

	if (g_strcmp0(type, "tool_call") == 0)
	{
		cursor_store_session_id(client, obj);

		if (g_strcmp0(subtype, "started") == 0)
			cursor_emit_tool_started(obj, out_events);
		else if (g_strcmp0(subtype, "completed") == 0)
			cursor_emit_tool_finished(obj, out_events);

		return TRUE;
	}

	if (g_strcmp0(type, "result") == 0)
	{
		const gchar *result_text;
		AiUsage *usage;

		if (cursor_get_boolean(obj, "is_error", FALSE))
		{
			cursor_set_error_from_object(obj, error);
			ai_response_set_stop_reason(response, AI_STOP_REASON_ERROR);
			return FALSE;
		}

		cursor_store_session_id(client, obj);

		result_text = cursor_get_string(obj, "result");
		if (result_text != NULL && result_text[0] != '\0' &&
		    ai_response_get_content_blocks(response) == NULL)
		{
			g_autoptr(AiTextContent) content =
				ai_text_content_new(result_text);
			ai_response_add_content_block(response,
				(AiContentBlock *)g_steal_pointer(&content));
		}

		usage = ai_response_get_usage(response);
		g_ptr_array_add(out_events, ai_event_new_usage(usage, -1));
		ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
		return TRUE;
	}

	return TRUE;
}

static void
ai_cursor_client_finalize(GObject *object)
{
	AiCursorClient *self = AI_CURSOR_CLIENT(object);

	g_free(self->additional_directories);
	g_free(self->plugin_dirs);
	g_free(self->headers);
	g_free(self->mode);
	g_free(self->sandbox);
	g_free(self->workspace);
	g_free(self->endpoint);
	g_free(self->api_key);
	g_free(self->model_params);
	g_free(self->worktree);
	g_free(self->worktree_base);
	g_free(self->turn_system_prompt);
	g_free(self->last_tool_summary);

	G_OBJECT_CLASS(ai_cursor_client_parent_class)->finalize(object);
}

static void
ai_cursor_client_class_init(AiCursorClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

	object_class->finalize     = ai_cursor_client_finalize;
	object_class->get_property = ai_cursor_client_get_property;
	object_class->set_property = ai_cursor_client_set_property;

	cli_class->get_executable_path = ai_cursor_client_get_executable_path;
	cli_class->spawn               = ai_cursor_client_spawn;
	cli_class->build_argv          = ai_cursor_client_build_argv;
	cli_class->build_stdin         = ai_cursor_client_build_stdin;
	cli_class->parse_json_output   = ai_cursor_client_parse_json_output;
	cli_class->parse_stream_line   = ai_cursor_client_parse_stream_line;
	cli_class->parse_stream_events = ai_cursor_client_parse_stream_events;

	properties[PROP_SKIP_PERMISSIONS] =
		g_param_spec_boolean("skip-permissions",
				     "Skip Permissions",
				     "Whether to pass --force (alias --yolo)",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_ADDITIONAL_DIRECTORIES] =
		g_param_spec_string("additional-directories",
				    "Additional Directories",
				    "Comma-separated extra --add-dir paths",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_PLUGIN_DIRS] =
		g_param_spec_string("plugin-dirs",
				    "Plugin Dirs",
				    "Comma-separated --plugin-dir paths",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_HEADERS] =
		g_param_spec_string("headers",
				    "Headers",
				    "Comma-separated 'Name: Value' --header items",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_MODE] =
		g_param_spec_string("mode",
				    "Mode",
				    "Execution mode (plan, ask)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_SANDBOX] =
		g_param_spec_string("sandbox",
				    "Sandbox",
				    "Sandbox mode (enabled, disabled)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_WORKSPACE] =
		g_param_spec_string("workspace",
				    "Workspace",
				    "Workspace directory or saved name (--workspace)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_ENDPOINT] =
		g_param_spec_string("endpoint",
				    "Endpoint",
				    "API endpoint URL (--endpoint)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_API_KEY] =
		g_param_spec_string("api-key",
				    "API Key",
				    "Delivered as CURSOR_API_KEY, not as an argv word",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_MODEL_PARAMS] =
		g_param_spec_string("model-params",
				    "Model Params",
				    "Bracket overrides appended to --model, e.g. context=1m,effort=high",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_WORKTREE] =
		g_param_spec_string("worktree",
				    "Worktree",
				    "Named isolated git worktree (--worktree NAME)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_WORKTREE_BASE] =
		g_param_spec_string("worktree-base",
				    "Worktree Base",
				    "Branch or ref to base a new worktree on",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_WORKTREE_AUTO] =
		g_param_spec_boolean("worktree-auto",
				     "Worktree Auto",
				     "Pass --worktree with a generated name",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_SKIP_WORKTREE_SETUP] =
		g_param_spec_boolean("skip-worktree-setup",
				     "Skip Worktree Setup",
				     "Whether to pass --skip-worktree-setup",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_AUTO_REVIEW] =
		g_param_spec_boolean("auto-review",
				     "Auto Review",
				     "Whether to pass --auto-review",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_APPROVE_MCPS] =
		g_param_spec_boolean("approve-mcps",
				     "Approve MCPs",
				     "Whether to pass --approve-mcps",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_TRUST] =
		g_param_spec_boolean("trust",
				     "Trust",
				     "Whether to pass --trust",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_CONTINUE_SESSION] =
		g_param_spec_boolean("continue-session",
				     "Continue Session",
				     "Continue the most recent session (--continue)",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_cursor_client_init(AiCursorClient *self)
{
	self->skip_permissions = FALSE;
	self->worktree_auto = FALSE;
	self->skip_worktree_setup = FALSE;
	self->auto_review = FALSE;
	self->approve_mcps = FALSE;
	self->trust = FALSE;
	self->continue_session = FALSE;

	ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_CURSOR_DEFAULT_MODEL);
}

static gboolean
ai_cursor_client_parse_stream_line(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	gchar       **delta_text,
	GError      **error
){
	g_autoptr(GPtrArray) events =
		g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);

	*delta_text = NULL;

	if (!ai_cursor_client_parse_stream_events(client, line, response,
						  events, error))
	{
		return FALSE;
	}

	*delta_text = ai_cli_client_events_to_delta(events);
	return TRUE;
}

static AiProviderType
ai_cursor_client_get_provider_type(AiProvider *provider)
{
	(void)provider;
	return AI_PROVIDER_CURSOR;
}

static const gchar *
ai_cursor_client_get_name(AiProvider *provider)
{
	(void)provider;
	return "Cursor";
}

static const gchar *
ai_cursor_client_get_default_model(AiProvider *provider)
{
	(void)provider;
	return AI_CURSOR_DEFAULT_MODEL;
}

typedef struct
{
	AiCursorClient *client;
	GTask          *task;
	GSubprocess    *subprocess;
	gchar          *stdin_data;
} ChatAsyncData;

static void
chat_async_data_free(ChatAsyncData *data)
{
	g_clear_object(&data->task);
	g_clear_object(&data->client);
	g_clear_object(&data->subprocess);
	g_clear_pointer(&data->stdin_data, g_free);
	g_slice_free(ChatAsyncData, data);
}

typedef struct
{
	AiCursorClient *client;
	GTask          *task;
	GSubprocess    *subprocess;
	gchar          *tool_summary;
} RetryAsyncData;

static void
retry_async_data_free(RetryAsyncData *data)
{
	g_clear_object(&data->task);
	g_clear_object(&data->client);
	g_clear_object(&data->subprocess);
	g_free(data->tool_summary);
	g_slice_free(RetryAsyncData, data);
}

static void
on_retry_communicate_complete(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	RetryAsyncData *data = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stdout_data = NULL;
	g_autofree gchar *stderr_data = NULL;
	AiCliClientClass *klass;
	AiResponse *response = NULL;

	if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
						   &stdout_data, &stderr_data,
						   &error))
	{
		goto fallback;
	}

	if (stdout_data == NULL || stdout_data[0] == '\0')
		goto fallback;

	klass = AI_CLI_CLIENT_GET_CLASS(data->client);
	response = klass->parse_json_output(AI_CLI_CLIENT(data->client),
					    stdout_data, &error);

	if (response != NULL &&
	    ai_response_get_content_blocks(response) != NULL)
	{
		g_task_return_pointer(data->task, response, g_object_unref);
		retry_async_data_free(data);
		return;
	}

	g_clear_object(&response);

fallback:
	g_clear_error(&error);
	g_debug("cursor: re-prompt failed, using tool summary as fallback");

	response = ai_response_new("",
		ai_cli_client_get_model(AI_CLI_CLIENT(data->client)));
	{
		g_autoptr(AiTextContent) tc = ai_text_content_new(data->tool_summary);
		ai_response_add_content_block(response,
			(AiContentBlock *)g_steal_pointer(&tc));
	}
	ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
	g_task_return_pointer(data->task, response, g_object_unref);
	retry_async_data_free(data);
}

static gboolean
attempt_text_retry(
	AiCursorClient *client,
	GTask          *task,
	const gchar    *tool_summary
){
	g_autoptr(GError) err = NULL;
	g_autofree gchar *exe = NULL;
	g_autoptr(GPtrArray) rargs = NULL;
	GSubprocess *rproc;
	RetryAsyncData *retry;
	const gchar *sid;
	g_autofree gchar *model_arg = NULL;

	exe = ai_cli_client_resolve_executable(AI_CLI_CLIENT(client), &err);
	if (exe == NULL)
		return FALSE;

	sid = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));
	if (sid == NULL || sid[0] == '\0')
		return FALSE;

	model_arg = cursor_model_arg(client,
		ai_cli_client_get_model(AI_CLI_CLIENT(client)));

	rargs = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(rargs, g_strdup(exe));
	g_ptr_array_add(rargs, g_strdup("--print"));
	g_ptr_array_add(rargs, g_strdup("--output-format"));
	g_ptr_array_add(rargs, g_strdup("json"));
	g_ptr_array_add(rargs, g_strdup("--model"));
	g_ptr_array_add(rargs, g_steal_pointer(&model_arg));
	emit_session_args(client, rargs);
	g_ptr_array_add(rargs, g_strdup("--resume"));
	g_ptr_array_add(rargs, g_strdup(sid));
	g_ptr_array_add(rargs, NULL);

	rproc = ai_cli_client_spawn(
		AI_CLI_CLIENT(client),
		(const gchar *const *)rargs->pdata,
		G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE,
		&err);

	if (rproc == NULL)
		return FALSE;

	retry = g_slice_new0(RetryAsyncData);
	retry->client       = g_object_ref(client);
	retry->task         = task;
	retry->subprocess   = rproc;
	retry->tool_summary = g_strdup(tool_summary);

	g_debug("cursor: no text in response, re-prompting for summary "
		"(session=%s)", sid);

	g_subprocess_communicate_utf8_async(
		rproc,
		"Provide a concise plain-text summary of what you just did. "
		"Do NOT use any tools.",
		NULL, on_retry_communicate_complete, retry);

	return TRUE;
}

static void
on_chat_communicate_complete(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	ChatAsyncData *data = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stdout_data = NULL;
	g_autofree gchar *stderr_data = NULL;
	AiCliClientClass *klass;
	AiResponse *response;

	if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
						   &stdout_data, &stderr_data,
						   &error))
	{
		g_task_return_error(data->task, g_steal_pointer(&error));
		chat_async_data_free(data);
		return;
	}

	if (stdout_data != NULL && stdout_data[0] != '\0')
	{
		klass = AI_CLI_CLIENT_GET_CLASS(data->client);
		response = klass->parse_json_output(AI_CLI_CLIENT(data->client),
						    stdout_data, &error);

		if (response != NULL)
		{
			if (ai_response_get_content_blocks(response) == NULL &&
			    data->client->last_tool_summary != NULL)
			{
				if (attempt_text_retry(data->client, data->task,
						       data->client->last_tool_summary))
				{
					g_object_unref(response);
					data->task = NULL;
					chat_async_data_free(data);
					return;
				}

				g_debug("cursor: re-prompt could not start, using "
					"fallback text");
				{
					g_autoptr(AiTextContent) tc = ai_text_content_new(
						data->client->last_tool_summary);
					ai_response_add_content_block(response,
						(AiContentBlock *)g_steal_pointer(&tc));
				}
			}

			g_task_return_pointer(data->task, response, g_object_unref);
			chat_async_data_free(data);
			return;
		}

		g_task_return_error(data->task, g_steal_pointer(&error));
		chat_async_data_free(data);
		return;
	}

	if (!g_subprocess_get_successful(data->subprocess))
	{
		g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_CLI_EXECUTION,
					"CLI exited with status %d: %s",
					g_subprocess_get_exit_status(data->subprocess),
					(stderr_data != NULL && stderr_data[0] != '\0')
					? stderr_data : "Unknown error");
	}
	else
	{
		g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
					"CLI produced no output");
	}

	chat_async_data_free(data);
}

static void
ai_cursor_client_chat_async(
	AiProvider          *provider,
	GList               *messages,
	const gchar         *system_prompt,
	gint                 max_tokens,
	GList               *tools,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiCursorClient *self = AI_CURSOR_CLIENT(provider);
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(self);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *executable = NULL;
	g_auto(GStrv) argv = NULL;
	gchar *stdin_data = NULL;
	g_autoptr(GSubprocess) subprocess = NULL;
	ChatAsyncData *data;
	GTask *task;
	GSubprocessFlags flags;

	(void)tools;

	task = g_task_new(self, cancellable, callback, user_data);

	executable = ai_cli_client_resolve_executable(AI_CLI_CLIENT(self), &error);
	if (executable == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		g_object_unref(task);
		return;
	}

	argv = klass->build_argv(AI_CLI_CLIENT(self), messages, system_prompt,
				 max_tokens, FALSE);
	if (argv == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
					"Failed to build command line arguments");
		g_object_unref(task);
		return;
	}

	if (klass->build_stdin != NULL)
		stdin_data = klass->build_stdin(AI_CLI_CLIENT(self), messages);

	g_free(argv[0]);
	argv[0] = g_steal_pointer(&executable);

	flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
	if (stdin_data != NULL)
		flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;

	subprocess = ai_cli_client_spawn(AI_CLI_CLIENT(self),
					 (const gchar * const *)argv,
					 flags, &error);
	if (subprocess == NULL)
	{
		g_free(stdin_data);
		g_task_return_error(task, g_steal_pointer(&error));
		g_object_unref(task);
		return;
	}

	data = g_slice_new0(ChatAsyncData);
	data->client = g_object_ref(self);
	data->task = task;
	data->subprocess = g_object_ref(subprocess);
	data->stdin_data = stdin_data;

	g_subprocess_communicate_utf8_async(subprocess, data->stdin_data,
					    cancellable,
					    on_chat_communicate_complete, data);
}

static AiResponse *
ai_cursor_client_chat_finish(
	AiProvider    *provider,
	GAsyncResult  *result,
	GError       **error
){
	(void)provider;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_cursor_client_list_models_async(
	AiProvider          *provider,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	GList *models = NULL;
	gsize i;

	(void)cancellable;

	task = g_task_new(provider, NULL, callback, user_data);

	for (i = 0; CURSOR_MODELS[i] != NULL; i++)
		models = g_list_append(models, g_strdup(CURSOR_MODELS[i]));

	g_task_return_pointer(task, models, NULL);
	g_object_unref(task);
}

static GList *
ai_cursor_client_list_models_finish(
	AiProvider    *provider,
	GAsyncResult  *result,
	GError       **error
){
	(void)provider;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_cursor_client_provider_init(AiProviderInterface *iface)
{
	iface->get_provider_type = ai_cursor_client_get_provider_type;
	iface->get_name = ai_cursor_client_get_name;
	iface->get_default_model = ai_cursor_client_get_default_model;
	iface->chat_async = ai_cursor_client_chat_async;
	iface->chat_finish = ai_cursor_client_chat_finish;
	iface->list_models_async = ai_cursor_client_list_models_async;
	iface->list_models_finish = ai_cursor_client_list_models_finish;
}

static void
ai_cursor_client_chat_stream_async(
	AiStreamable        *streamable,
	GList               *messages,
	const gchar         *system_prompt,
	gint                 max_tokens,
	GList               *tools,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	(void)tools;

	ai_cli_client_stream_run_async(AI_CLI_CLIENT(streamable), messages,
				       system_prompt, max_tokens,
				       cancellable, callback, user_data);
}

static AiResponse *
ai_cursor_client_chat_stream_finish(
	AiStreamable  *streamable,
	GAsyncResult  *result,
	GError       **error
){
	return ai_cli_client_stream_run_finish(AI_CLI_CLIENT(streamable),
					       result, error);
}

static void
ai_cursor_client_streamable_init(AiStreamableInterface *iface)
{
	iface->chat_stream_async = ai_cursor_client_chat_stream_async;
	iface->chat_stream_finish = ai_cursor_client_chat_stream_finish;
}

/**
 * ai_cursor_client_new:
 *
 * Creates a new #AiCursorClient.
 * The `cursor-agent` CLI must be available in PATH or specified via
 * %CURSOR_AGENT_PATH (or %CURSOR_PATH).
 *
 * Returns: (transfer full): a new #AiCursorClient
 */
AiCursorClient *
ai_cursor_client_new(void)
{
	g_autoptr(AiCursorClient) self =
		g_object_new(AI_TYPE_CURSOR_CLIENT, NULL);

	return (AiCursorClient *)g_steal_pointer(&self);
}

/**
 * ai_cursor_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiCursorClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiCursorClient
 */
AiCursorClient *
ai_cursor_client_new_with_config(AiConfig *config)
{
	g_autoptr(AiCursorClient) self =
		g_object_new(AI_TYPE_CURSOR_CLIENT,
			     "config", config,
			     NULL);

	return (AiCursorClient *)g_steal_pointer(&self);
}

/**
 * ai_cursor_client_get_skip_permissions:
 * @self: an #AiCursorClient
 *
 * Gets whether the CLI runs with `--force`.
 *
 * Returns: %TRUE if skip permissions is enabled
 */
gboolean
ai_cursor_client_get_skip_permissions(AiCursorClient *self)
{
	g_return_val_if_fail(AI_IS_CURSOR_CLIENT(self), FALSE);

	return self->skip_permissions;
}

/**
 * ai_cursor_client_set_skip_permissions:
 * @self: an #AiCursorClient
 * @skip: whether to bypass tool-use approval
 *
 * Sets whether to run cursor-agent with `--force` (the `--yolo` alias).
 */
void
ai_cursor_client_set_skip_permissions(
	AiCursorClient *self,
	gboolean        skip
){
	g_return_if_fail(AI_IS_CURSOR_CLIENT(self));

	if (self->skip_permissions == skip)
		return;

	self->skip_permissions = skip;

	g_object_notify_by_pspec(G_OBJECT(self),
				 properties[PROP_SKIP_PERMISSIONS]);
}
