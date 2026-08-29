/*
 * ai-antigravity-client.c - Google Antigravity CLI client (agy)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Wraps Google's `agy` CLI (Antigravity) in headless print mode. The
 * prompt is piped on stdin as one `--input-format stream-json` user
 * event rather than as `--print`'s next argument: `--print` consumes
 * the following argv word, so putting flags after it would be taken
 * as the prompt, and a positional prompt would hit MAX_ARG_STRLEN
 * on a long conversation.
 */

#include "config.h"

#include <string.h>

#include "providers/ai-antigravity-client.h"
#include "providers/ai-antigravity-client-internal.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

/*
 * Private structure for AiAntigravityClient.
 */
struct _AiAntigravityClient
{
	AiCliClient parent_instance;

	gboolean skip_permissions;

	/*
	 * Everything below is emitted only when set (except
	 * disable-slash-commands, which defaults on so a library
	 * caller's prompt is text). An unconfigured client still
	 * emits the stream-json I/O flags, because those are how
	 * the prompt is delivered.
	 */
	gchar   *additional_directories;
	gchar   *agent;
	gchar   *json_schema;
	gchar   *log_file;
	gchar   *mode;
	gchar   *print_timeout;
	gchar   *project;
	gboolean new_project;
	gboolean sandbox;
	gboolean disable_slash_commands;
	gboolean continue_session;

	/* Stashed by build_argv so build_stdin can see this turn's
	 * system prompt: AiConversation passes it as an argument,
	 * not via the client's property. */
	gchar   *turn_system_prompt;

	/* Cached summary for the re-prompt fallback when the model
	 * produces no text (empty "response" with tool use only). */
	gchar   *last_tool_summary;
};

/*
 * Interface implementations forward declarations.
 */
static void ai_antigravity_client_provider_init(AiProviderInterface *iface);
static void ai_antigravity_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiAntigravityClient, ai_antigravity_client, AI_TYPE_CLI_CLIENT,
			G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
					      ai_antigravity_client_provider_init)
			G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
					      ai_antigravity_client_streamable_init))

/*
 * Property IDs.
 */
enum
{
	PROP_0,
	PROP_SKIP_PERMISSIONS,
	PROP_ADDITIONAL_DIRECTORIES,
	PROP_AGENT,
	PROP_JSON_SCHEMA,
	PROP_LOG_FILE,
	PROP_MODE,
	PROP_PRINT_TIMEOUT,
	PROP_PROJECT,
	PROP_NEW_PROJECT,
	PROP_SANDBOX,
	PROP_DISABLE_SLASH_COMMANDS,
	PROP_CONTINUE_SESSION,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_antigravity_client_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiAntigravityClient *self = AI_ANTIGRAVITY_CLIENT(object);

	switch (prop_id)
	{
		case PROP_SKIP_PERMISSIONS:
			g_value_set_boolean(value, self->skip_permissions);
			break;
		case PROP_ADDITIONAL_DIRECTORIES:
			g_value_set_string(value, self->additional_directories);
			break;
		case PROP_AGENT:
			g_value_set_string(value, self->agent);
			break;
		case PROP_JSON_SCHEMA:
			g_value_set_string(value, self->json_schema);
			break;
		case PROP_LOG_FILE:
			g_value_set_string(value, self->log_file);
			break;
		case PROP_MODE:
			g_value_set_string(value, self->mode);
			break;
		case PROP_PRINT_TIMEOUT:
			g_value_set_string(value, self->print_timeout);
			break;
		case PROP_PROJECT:
			g_value_set_string(value, self->project);
			break;
		case PROP_NEW_PROJECT:
			g_value_set_boolean(value, self->new_project);
			break;
		case PROP_SANDBOX:
			g_value_set_boolean(value, self->sandbox);
			break;
		case PROP_DISABLE_SLASH_COMMANDS:
			g_value_set_boolean(value, self->disable_slash_commands);
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
ai_antigravity_client_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiAntigravityClient *self = AI_ANTIGRAVITY_CLIENT(object);

	switch (prop_id)
	{
		case PROP_SKIP_PERMISSIONS:
			self->skip_permissions = g_value_get_boolean(value);
			break;
		case PROP_ADDITIONAL_DIRECTORIES:
			g_free(self->additional_directories);
			self->additional_directories = g_value_dup_string(value);
			break;
		case PROP_AGENT:
			g_free(self->agent);
			self->agent = g_value_dup_string(value);
			break;
		case PROP_JSON_SCHEMA:
			g_free(self->json_schema);
			self->json_schema = g_value_dup_string(value);
			break;
		case PROP_LOG_FILE:
			g_free(self->log_file);
			self->log_file = g_value_dup_string(value);
			break;
		case PROP_MODE:
			g_free(self->mode);
			self->mode = g_value_dup_string(value);
			break;
		case PROP_PRINT_TIMEOUT:
			g_free(self->print_timeout);
			self->print_timeout = g_value_dup_string(value);
			break;
		case PROP_PROJECT:
			g_free(self->project);
			self->project = g_value_dup_string(value);
			break;
		case PROP_NEW_PROJECT:
			self->new_project = g_value_get_boolean(value);
			break;
		case PROP_SANDBOX:
			self->sandbox = g_value_get_boolean(value);
			break;
		case PROP_DISABLE_SLASH_COMMANDS:
			self->disable_slash_commands = g_value_get_boolean(value);
			break;
		case PROP_CONTINUE_SESSION:
			self->continue_session = g_value_get_boolean(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

/*
 * The execution modes `agy --mode` accepts.
 */
static const gchar *AI_ANTIGRAVITY_MODES[] = {
	"accept-edits", "plan",
	NULL
};

static gboolean
mode_is_valid(const gchar *mode)
{
	gsize i;

	for (i = 0; AI_ANTIGRAVITY_MODES[i] != NULL; i++)
	{
		if (g_strcmp0(mode, AI_ANTIGRAVITY_MODES[i]) == 0)
			return TRUE;
	}

	return FALSE;
}

/*
 * Map an #AiCliClient:effort-level string onto what agy accepts.
 *
 * agy takes exactly low/medium/high. #AiEffortLevel also has "xhigh"
 * and "max", which agy rejects, so both fold onto high.
 *
 * Returns: (nullable): a static string to pass to --effort, or %NULL
 *   to omit the flag entirely.
 */
static const gchar *
agy_effort_arg(const gchar *effort)
{
	if (effort == NULL || effort[0] == '\0')
		return NULL;

	if (g_strcmp0(effort, "low") == 0)
		return "low";
	if (g_strcmp0(effort, "medium") == 0)
		return "medium";
	if (g_strcmp0(effort, "high") == 0)
		return "high";

	/* AI_EFFORT_XHIGH and AI_EFFORT_MAX have no agy equivalent. */
	if (g_strcmp0(effort, "xhigh") == 0 ||
	    g_strcmp0(effort, "max") == 0)
		return "high";

	g_message("antigravity: unknown effort level '%s'; omitting "
		  "--effort. Valid levels: low, medium, high",
		  effort);

	return NULL;
}

static void
emit_value_flag(GPtrArray *args, const gchar *flag, const gchar *value)
{
	if (value == NULL || value[0] == '\0')
		return;

	g_ptr_array_add(args, g_strdup(flag));
	g_ptr_array_add(args, g_strdup(value));
}

/*
 * Append a comma-separated value as the flag repeated once per item,
 * which is the shape --add-dir takes (repeatable, not a list after one
 * flag).
 */
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

/*
 * agy's own --print-timeout, as distinct from the library's
 * process-timeout-ms (which kills the child). agy defaults to 5m, which
 * is shorter than the library's 30-minute default, so this is always
 * emitted: otherwise a 30-minute bound on our side would still die at
 * five minutes inside agy.
 */
static void
emit_print_timeout(AiAntigravityClient *self, GPtrArray *args,
		   AiCliClient *client)
{
	g_autofree gchar *formatted = NULL;
	const gchar *value;
	gint timeout_ms;

	if (self->print_timeout != NULL && self->print_timeout[0] != '\0')
	{
		value = self->print_timeout;
	}
	else
	{
		timeout_ms = ai_cli_client_get_process_timeout_ms(client);
		if (timeout_ms <= 0)
		{
			value = "168h";
		}
		else if (timeout_ms % 60000 == 0)
		{
			formatted = g_strdup_printf("%dm", timeout_ms / 60000);
			value = formatted;
		}
		else if (timeout_ms % 1000 == 0)
		{
			formatted = g_strdup_printf("%ds", timeout_ms / 1000);
			value = formatted;
		}
		else
		{
			formatted = g_strdup_printf("%dms", timeout_ms);
			value = formatted;
		}
	}

	g_ptr_array_add(args, g_strdup("--print-timeout"));
	g_ptr_array_add(args, g_strdup(value));
}

/*
 * Session-independent knobs shared by the one-shot and re-prompt argv
 * builders. A retry that dropped --sandbox or --agent would run under
 * different rules than the turn it is summarising.
 */
static void
emit_session_args(AiAntigravityClient *self, GPtrArray *args)
{
	if (self->skip_permissions)
		g_ptr_array_add(args, g_strdup("--dangerously-skip-permissions"));

	emit_repeated_flag(args, "--add-dir", self->additional_directories);
	emit_value_flag(args, "--agent", self->agent);
	emit_value_flag(args, "--json-schema", self->json_schema);
	emit_value_flag(args, "--log-file", self->log_file);
	emit_value_flag(args, "--project", self->project);

	if (self->mode != NULL && self->mode[0] != '\0')
	{
		if (mode_is_valid(self->mode))
		{
			g_ptr_array_add(args, g_strdup("--mode"));
			g_ptr_array_add(args, g_strdup(self->mode));
		}
		else
		{
			g_message("antigravity: unknown mode '%s'; omitting "
				  "the flag. Valid modes: accept-edits, plan",
				  self->mode);
		}
	}

	if (self->new_project)
		g_ptr_array_add(args, g_strdup("--new-project"));

	if (self->sandbox)
		g_ptr_array_add(args, g_strdup("--sandbox"));

	if (self->disable_slash_commands)
		g_ptr_array_add(args, g_strdup("--disable-slash-commands"));
}

static gchar *
ai_antigravity_client_get_executable_path(AiCliClient *client)
{
	const gchar *env_path;

	(void)client;

	env_path = g_getenv("AGY_PATH");
	if (env_path != NULL && env_path[0] != '\0')
		return g_strdup(env_path);

	return g_strdup("agy");
}

/*
 * Build command line arguments for the agy CLI.
 *
 * Always:
 *   agy --input-format stream-json --output-format stream-json ...
 *
 * The prompt is never in argv. --print is omitted on purpose: it
 * consumes the next argv word as the prompt, so `--print --model X`
 * would send "--model" as the prompt. stream-json input is the
 * documented stdin protocol and is what keeps long conversations
 * under MAX_ARG_STRLEN.
 *
 * Streaming and non-streaming share the same format because
 * --input-format stream-json requires --output-format stream-json.
 * parse_json_output extracts the terminal result event.
 */
gchar **
ai_antigravity_client_build_argv(
	AiCliClient *client,
	GList       *messages,
	const gchar *system_prompt,
	gint         max_tokens,
	gboolean     streaming
){
	AiAntigravityClient *self = AI_ANTIGRAVITY_CLIENT(client);
	GPtrArray *args;
	const gchar *model;
	const gchar *session_id;
	const gchar *effort;
	gboolean persist;

	(void)messages;
	(void)max_tokens;
	(void)streaming;

	g_free(self->turn_system_prompt);
	self->turn_system_prompt = g_strdup(system_prompt);

	args = g_ptr_array_new();

	/* Executable placeholder — the caller overwrites it with the
	 * resolved path before spawning. */
	g_ptr_array_add(args, g_strdup("agy"));

	g_ptr_array_add(args, g_strdup("--input-format"));
	g_ptr_array_add(args, g_strdup("stream-json"));
	g_ptr_array_add(args, g_strdup("--output-format"));
	g_ptr_array_add(args, g_strdup("stream-json"));

	emit_print_timeout(self, args, client);

	/* Model */
	model = ai_cli_client_get_model(client);
	if (model == NULL || model[0] == '\0')
		model = AI_ANTIGRAVITY_DEFAULT_MODEL;
	g_ptr_array_add(args, g_strdup("--model"));
	g_ptr_array_add(args, g_strdup(model));

	emit_session_args(self, args);

	/* Session management — an explicit id wins over --continue. */
	persist = ai_cli_client_get_session_persistence(client);
	session_id = ai_cli_client_get_session_id(client);
	if (persist && session_id != NULL && session_id[0] != '\0')
	{
		g_ptr_array_add(args, g_strdup("--conversation"));
		g_ptr_array_add(args, g_strdup(session_id));
	}
	else if (persist && self->continue_session)
	{
		g_ptr_array_add(args, g_strdup("--continue"));
	}

	effort = agy_effort_arg(ai_cli_client_get_effort_level(client));
	if (effort != NULL)
	{
		g_ptr_array_add(args, g_strdup("--effort"));
		g_ptr_array_add(args, g_strdup(effort));
	}

	g_ptr_array_add(args, NULL);

	return (gchar **)g_ptr_array_free(args, FALSE);
}

/*
 * Wrap @content as one stream-json user event, with a trailing newline
 * so the CLI sees a complete NDJSON line.
 */
static gchar *
agy_user_event_json(const gchar *content)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *data = NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "event");
	json_builder_add_string_value(builder, "user");
	json_builder_set_member_name(builder, "message");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "content");
	json_builder_add_string_value(builder,
				      content != NULL ? content : "");
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	json_generator_set_root(gen, root);
	data = json_generator_to_data(gen, NULL);

	return g_strconcat(data != NULL ? data : "{}", "\n", NULL);
}

static gboolean
agy_should_send_system_prompt(AiAntigravityClient *self, AiCliClient *client)
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

/*
 * Flatten the conversation into one user-event JSON line for stdin.
 *
 * agy has no --system-prompt flag, so a fresh session inlines the
 * system prompt the way opencode does. A resumed or --continue session
 * already carries it and must not see it again.
 */
static gchar *
ai_antigravity_client_build_stdin(
	AiCliClient *client,
	GList       *messages
){
	AiAntigravityClient *self = AI_ANTIGRAVITY_CLIENT(client);
	GString *prompt;
	const gchar *sys_prompt;
	GList *l;

	prompt = g_string_new("");

	if (agy_should_send_system_prompt(self, client))
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

	{
		g_autofree gchar *flat = g_string_free(prompt, FALSE);

		return agy_user_event_json(flat);
	}
}

/*
 * Type-safe JSON member accessors. json-glib's *_member_with_default()
 * emit a critical on a type mismatch, and subprocess stdout is untrusted.
 */

static const gchar *
agy_get_string(JsonObject *obj, const gchar *name)
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
agy_get_object(JsonObject *obj, const gchar *name)
{
	JsonNode *node;

	if (obj == NULL || name == NULL)
		return NULL;

	node = json_object_get_member(obj, name);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	return json_node_get_object(node);
}

static gint
agy_get_int(JsonObject *obj, const gchar *name, gint fallback)
{
	JsonNode *node;
	GType value_type;

	if (obj == NULL || name == NULL)
		return fallback;

	node = json_object_get_member(obj, name);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return fallback;

	value_type = json_node_get_value_type(node);
	if (value_type != G_TYPE_INT64 && value_type != G_TYPE_INT &&
	    value_type != G_TYPE_DOUBLE)
		return fallback;

	return (gint)json_node_get_int(node);
}

static JsonObject *
agy_unwrap_envelope(JsonObject *obj)
{
	const gchar *event;
	JsonObject *inner;

	if (obj == NULL)
		return NULL;

	event = agy_get_string(obj, "event");
	if (g_strcmp0(event, "result") == 0)
	{
		inner = agy_get_object(obj, "result");
		if (inner != NULL)
			return inner;
	}

	return obj;
}

static gboolean
agy_looks_like_result(JsonObject *obj)
{
	if (obj == NULL)
		return FALSE;

	if (agy_get_string(obj, "status") != NULL)
		return TRUE;
	if (agy_get_string(obj, "response") != NULL)
		return TRUE;
	if (agy_get_string(obj, "error") != NULL)
		return TRUE;
	if (agy_get_string(obj, "conversation_id") != NULL)
		return TRUE;

	return FALSE;
}

/*
 * Load @candidate and return the JSON object it carries, or NULL.
 *
 * json-glib folds a newline-separated sequence of objects into an
 * array, which is exactly stream-json stdout. Walk back to the last
 * result-shaped object.
 */
static JsonObject *
agy_try_parse(JsonParser *parser, const gchar *candidate)
{
	JsonNode *root;
	JsonArray *array;
	guint i;
	JsonObject *fallback = NULL;

	if (candidate == NULL || candidate[0] == '\0')
		return NULL;

	if (!json_parser_load_from_data(parser, candidate, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);
	if (root == NULL)
		return NULL;

	if (JSON_NODE_HOLDS_OBJECT(root))
		return agy_unwrap_envelope(json_node_get_object(root));

	if (!JSON_NODE_HOLDS_ARRAY(root))
		return NULL;

	array = json_node_get_array(root);
	for (i = json_array_get_length(array); i > 0; i--)
	{
		JsonNode *element = json_array_get_element(array, i - 1);
		JsonObject *unwrapped;

		if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element))
			continue;

		unwrapped = agy_unwrap_envelope(json_node_get_object(element));
		if (fallback == NULL)
			fallback = unwrapped;

		if (agy_looks_like_result(unwrapped))
			return unwrapped;
	}

	return fallback;
}

static JsonObject *
agy_parse_output_object(
	JsonParser  *parser,
	const gchar *json,
	GError     **error
){
	g_autofree gchar *last_line = NULL;
	g_auto(GStrv) lines = NULL;
	const gchar *brace;
	JsonObject *obj;
	gint i;

	obj = agy_try_parse(parser, json);
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

	obj = agy_try_parse(parser, last_line);
	if (obj != NULL)
		return obj;

	brace = strchr(json, '{');
	if (brace != NULL && brace != json)
	{
		obj = agy_try_parse(parser, brace);
		if (obj != NULL)
			return obj;
	}

	g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
		    "Could not parse a JSON object from CLI output");

	return NULL;
}

static void
agy_apply_usage(JsonObject *obj, AiResponse *response)
{
	JsonObject *usage_obj;
	g_autoptr(AiUsage) usage = NULL;

	usage_obj = agy_get_object(obj, "usage");
	if (usage_obj == NULL)
		return;

	usage = ai_usage_new(agy_get_int(usage_obj, "input_tokens", 0),
			     agy_get_int(usage_obj, "output_tokens", 0));

	ai_response_set_usage(response, usage);
}

static void
agy_set_error_from_object(JsonObject *obj, GError **error)
{
	const gchar *message;
	const gchar *status;

	message = agy_get_string(obj, "error");
	if (message == NULL || message[0] == '\0')
		message = agy_get_string(obj, "response");
	if (message == NULL || message[0] == '\0')
	{
		status = agy_get_string(obj, "status");
		message = (status != NULL && status[0] != '\0')
			? status : "Unknown error";
	}

	g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		    "CLI error: %s", message);
}

static gboolean
agy_status_is_error(const gchar *status)
{
	if (status == NULL || status[0] == '\0')
		return FALSE;

	if (g_strcmp0(status, "SUCCESS") == 0)
		return FALSE;

	/* Anything else in the documented set is a failed run. */
	return TRUE;
}

static void
agy_store_session_id(AiCliClient *client, JsonObject *obj)
{
	const gchar *session_id;

	session_id = agy_get_string(obj, "conversation_id");
	if (session_id != NULL && session_id[0] != '\0' &&
	    ai_cli_client_get_session_persistence(client))
	{
		ai_cli_client_set_session_id(client, session_id);
	}
}

/*
 * Parse JSON output from the agy CLI.
 *
 * --output-format json:
 *   { "conversation_id", "status", "response", "error", "usage": {...} }
 *
 * --output-format stream-json (what we actually request):
 *   NDJSON ending in { "event":"result", "result": { ...same envelope... } }
 */
static AiResponse *
ai_antigravity_client_parse_json_output(
	AiCliClient *client,
	const gchar *json,
	GError     **error
){
	AiAntigravityClient *self = AI_ANTIGRAVITY_CLIENT(client);
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(AiResponse) response = NULL;
	JsonObject *obj;
	const gchar *status;
	const gchar *text;
	const gchar *session_id;

	parser = json_parser_new();

	obj = agy_parse_output_object(parser, json, error);
	if (obj == NULL)
		return NULL;

	status = agy_get_string(obj, "status");
	if (agy_status_is_error(status))
	{
		agy_set_error_from_object(obj, error);
		return NULL;
	}

	session_id = agy_get_string(obj, "conversation_id");
	if (session_id == NULL)
		session_id = "";

	response = ai_response_new(session_id, ai_cli_client_get_model(client));
	ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);

	agy_store_session_id(client, obj);

	text = agy_get_string(obj, "response");
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

	agy_apply_usage(obj, response);

	return (AiResponse *)g_steal_pointer(&response);
}

static gchar *
agy_tool_id(JsonObject *step)
{
	gint index = agy_get_int(step, "step_index", -1);

	if (index >= 0)
		return g_strdup_printf("step-%d", index);

	return g_strdup("step");
}

static void
agy_emit_tool_started(JsonObject *step, GPtrArray *out_events)
{
	JsonObject *info;
	const gchar *name;
	JsonNode *parameters = NULL;
	g_autofree gchar *id = NULL;
	g_autoptr(AiToolUse) tool_use = NULL;

	info = agy_get_object(step, "tool_info");
	name = agy_get_string(step, "tool_name");
	if ((name == NULL || name[0] == '\0') && info != NULL)
		name = agy_get_string(info, "name");

	if (name == NULL || name[0] == '\0')
		return;

	if (info != NULL && json_object_has_member(info, "parameters"))
		parameters = json_object_get_member(info, "parameters");

	id = agy_tool_id(step);
	tool_use = ai_tool_use_new(id, name, parameters);
	g_ptr_array_add(out_events, ai_event_new_tool_started(tool_use));
}

static void
agy_emit_tool_finished(JsonObject *step, GPtrArray *out_events)
{
	JsonObject *info;
	JsonObject *err_obj;
	const gchar *output;
	const gchar *err_msg;
	gboolean is_error = FALSE;
	g_autofree gchar *id = NULL;
	g_autoptr(AiToolResult) result = NULL;
	GString *text;

	info = agy_get_object(step, "tool_info");
	id = agy_tool_id(step);
	text = g_string_new("");

	output = (info != NULL) ? agy_get_string(info, "output") : NULL;
	if (output != NULL)
		g_string_append(text, output);

	err_obj = (info != NULL) ? agy_get_object(info, "error") : NULL;
	err_msg = (err_obj != NULL) ? agy_get_string(err_obj, "message") : NULL;
	if (err_msg != NULL && err_msg[0] != '\0')
	{
		is_error = TRUE;
		if (text->len > 0)
			g_string_append_c(text, '\n');
		g_string_append(text, err_msg);
	}

	result = ai_tool_result_new(id, text->str, is_error);
	g_string_free(text, TRUE);
	g_ptr_array_add(out_events, ai_event_new_tool_finished(NULL, result));
}

static gboolean
ai_antigravity_client_parse_stream_line(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	gchar       **delta_text,
	GError      **error
);

static gboolean
ai_antigravity_client_parse_stream_events(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	GPtrArray    *out_events,
	GError      **error
){
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;
	JsonObject *obj;
	const gchar *event;

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
	event = agy_get_string(obj, "event");

	if (g_strcmp0(event, "init") == 0)
	{
		agy_store_session_id(client, obj);
		return TRUE;
	}

	if (g_strcmp0(event, "step_update") == 0)
	{
		JsonObject *step = agy_get_object(obj, "step_update");
		const gchar *step_type;
		const gchar *state;
		const gchar *text_delta;
		const gchar *thinking;

		if (step == NULL)
			return TRUE;

		agy_store_session_id(client, step);

		step_type = agy_get_string(step, "step_type");
		state = agy_get_string(step, "state");
		text_delta = agy_get_string(step, "text_delta");
		thinking = agy_get_string(step, "thinking_delta");

		if (thinking != NULL && thinking[0] != '\0')
			g_ptr_array_add(out_events,
					ai_event_new_thinking_delta(thinking));

		if (text_delta != NULL && text_delta[0] != '\0')
			g_ptr_array_add(out_events,
					ai_event_new_text_delta(text_delta));

		if (g_strcmp0(step_type, "tool") == 0)
		{
			if (g_strcmp0(state, "ACTIVE") == 0)
			{
				agy_emit_tool_started(step, out_events);
			}
			else if (g_strcmp0(state, "DONE") == 0)
			{
				agy_emit_tool_started(step, out_events);
				agy_emit_tool_finished(step, out_events);
			}
		}

		return TRUE;
	}

	if (g_strcmp0(event, "result") == 0)
	{
		JsonObject *result = agy_get_object(obj, "result");
		const gchar *status;
		const gchar *result_text;
		AiUsage *usage;

		if (result == NULL)
			result = obj;

		status = agy_get_string(result, "status");
		if (agy_status_is_error(status))
		{
			agy_set_error_from_object(result, error);
			ai_response_set_stop_reason(response, AI_STOP_REASON_ERROR);
			return FALSE;
		}

		agy_store_session_id(client, result);

		result_text = agy_get_string(result, "response");
		if (result_text != NULL && result_text[0] != '\0' &&
		    ai_response_get_content_blocks(response) == NULL)
		{
			g_autoptr(AiTextContent) content =
				ai_text_content_new(result_text);
			ai_response_add_content_block(response,
				(AiContentBlock *)g_steal_pointer(&content));
		}

		agy_apply_usage(result, response);

		usage = ai_response_get_usage(response);
		g_ptr_array_add(out_events, ai_event_new_usage(usage, -1));

		ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
		return TRUE;
	}

	/* Unrecognised events are skipped, matching agy's own stdin
	 * contract for unknown event names. */
	return TRUE;
}

static void
ai_antigravity_client_finalize(GObject *object)
{
	AiAntigravityClient *self = AI_ANTIGRAVITY_CLIENT(object);

	g_free(self->additional_directories);
	g_free(self->agent);
	g_free(self->json_schema);
	g_free(self->log_file);
	g_free(self->mode);
	g_free(self->print_timeout);
	g_free(self->project);
	g_free(self->turn_system_prompt);
	g_free(self->last_tool_summary);

	G_OBJECT_CLASS(ai_antigravity_client_parent_class)->finalize(object);
}

static void
ai_antigravity_client_class_init(AiAntigravityClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

	object_class->finalize     = ai_antigravity_client_finalize;
	object_class->get_property = ai_antigravity_client_get_property;
	object_class->set_property = ai_antigravity_client_set_property;

	cli_class->get_executable_path = ai_antigravity_client_get_executable_path;
	cli_class->build_argv          = ai_antigravity_client_build_argv;
	cli_class->build_stdin         = ai_antigravity_client_build_stdin;
	cli_class->parse_json_output   = ai_antigravity_client_parse_json_output;
	cli_class->parse_stream_line   = ai_antigravity_client_parse_stream_line;
	cli_class->parse_stream_events = ai_antigravity_client_parse_stream_events;

	/**
	 * AiAntigravityClient:skip-permissions:
	 *
	 * Whether to run the CLI with `--dangerously-skip-permissions`.
	 * When enabled the CLI never prompts for tool-use approval.
	 */
	properties[PROP_SKIP_PERMISSIONS] =
		g_param_spec_boolean("skip-permissions",
				     "Skip Permissions",
				     "Whether to pass --dangerously-skip-permissions",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:additional-directories:
	 *
	 * Comma-separated extra workspace directories. Each item becomes
	 * its own `--add-dir <path>` pair.
	 */
	properties[PROP_ADDITIONAL_DIRECTORIES] =
		g_param_spec_string("additional-directories",
				    "Additional Directories",
				    "Comma-separated extra --add-dir paths",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:agent:
	 *
	 * An agent name, passed as --agent. List them with `agy agents`.
	 */
	properties[PROP_AGENT] =
		g_param_spec_string("agent",
				    "Agent",
				    "Agent name passed as --agent",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:json-schema:
	 *
	 * A JSON schema string or file path, passed as --json-schema, to
	 * constrain structured output.
	 */
	properties[PROP_JSON_SCHEMA] =
		g_param_spec_string("json-schema",
				    "JSON Schema",
				    "Schema string or path for --json-schema",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:log-file:
	 *
	 * Override the CLI log file path, passed as --log-file.
	 */
	properties[PROP_LOG_FILE] =
		g_param_spec_string("log-file",
				    "Log File",
				    "Override CLI log file path (--log-file)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:mode:
	 *
	 * Agent execution mode, passed as --mode. Valid values are
	 * "accept-edits" and "plan".
	 */
	properties[PROP_MODE] =
		g_param_spec_string("mode",
				    "Mode",
				    "Agent execution mode (accept-edits, plan)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:print-timeout:
	 *
	 * Override `--print-timeout` with a Go duration (for example
	 * "15m"). When unset, the library formats
	 * #AiCliClient:process-timeout-ms instead, so agy's 5-minute
	 * default does not cut a run the library would still wait for.
	 */
	properties[PROP_PRINT_TIMEOUT] =
		g_param_spec_string("print-timeout",
				    "Print Timeout",
				    "Override --print-timeout (Go duration)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:project:
	 *
	 * Project ID or name, passed as --project.
	 */
	properties[PROP_PROJECT] =
		g_param_spec_string("project",
				    "Project",
				    "Project ID or name (--project)",
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:new-project:
	 *
	 * Whether to pass --new-project, creating a new project for this
	 * session.
	 */
	properties[PROP_NEW_PROJECT] =
		g_param_spec_boolean("new-project",
				     "New Project",
				     "Whether to pass --new-project",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:sandbox:
	 *
	 * Whether to pass --sandbox, enabling terminal restrictions.
	 */
	properties[PROP_SANDBOX] =
		g_param_spec_boolean("sandbox",
				     "Sandbox",
				     "Whether to pass --sandbox",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:disable-slash-commands:
	 *
	 * Whether to pass --disable-slash-commands. Defaults to %TRUE:
	 * a library caller's prompt is text, and text that happens to
	 * begin with "/" must not be expanded as a slash command.
	 */
	properties[PROP_DISABLE_SLASH_COMMANDS] =
		g_param_spec_boolean("disable-slash-commands",
				     "Disable Slash Commands",
				     "Whether to pass --disable-slash-commands",
				     TRUE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAntigravityClient:continue-session:
	 *
	 * Whether to pass `--continue`, resuming the most recent
	 * conversation when no #AiCliClient:session-id is known.
	 *
	 * An explicit session id wins. Ignored when
	 * #AiCliClient:session-persistence is off.
	 */
	properties[PROP_CONTINUE_SESSION] =
		g_param_spec_boolean("continue-session",
				     "Continue Session",
				     "Continue the most recent conversation (--continue)",
				     FALSE,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_antigravity_client_init(AiAntigravityClient *self)
{
	self->skip_permissions = FALSE;
	self->new_project = FALSE;
	self->sandbox = FALSE;
	self->disable_slash_commands = TRUE;
	self->continue_session = FALSE;

	ai_cli_client_set_model(AI_CLI_CLIENT(self),
				AI_ANTIGRAVITY_DEFAULT_MODEL);
}

static gboolean
ai_antigravity_client_parse_stream_line(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	gchar       **delta_text,
	GError      **error
){
	g_autoptr(GPtrArray) events =
		g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);

	*delta_text = NULL;

	if (!ai_antigravity_client_parse_stream_events(client, line, response,
						       events, error))
	{
		return FALSE;
	}

	*delta_text = ai_cli_client_events_to_delta(events);
	return TRUE;
}

static AiProviderType
ai_antigravity_client_get_provider_type(AiProvider *provider)
{
	(void)provider;
	return AI_PROVIDER_ANTIGRAVITY;
}

static const gchar *
ai_antigravity_client_get_name(AiProvider *provider)
{
	(void)provider;
	return "Antigravity";
}

static const gchar *
ai_antigravity_client_get_default_model(AiProvider *provider)
{
	(void)provider;
	return AI_ANTIGRAVITY_DEFAULT_MODEL;
}

typedef struct
{
	AiAntigravityClient *client;
	GTask               *task;
	GSubprocess         *subprocess;
	gchar               *stdin_data;
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
	AiAntigravityClient *client;
	GTask               *task;
	GSubprocess         *subprocess;
	gchar               *tool_summary;
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
	g_debug("antigravity: re-prompt failed, using tool summary as fallback");

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
	AiAntigravityClient *client,
	GTask               *task,
	const gchar         *tool_summary
){
	g_autoptr(GError) err = NULL;
	g_autofree gchar *exe = NULL;
	g_autoptr(GPtrArray) rargs = NULL;
	GSubprocess *rproc;
	RetryAsyncData *retry;
	const gchar *model;
	const gchar *sid;
	g_autofree gchar *retry_stdin = NULL;

	exe = ai_cli_client_resolve_executable(AI_CLI_CLIENT(client), &err);
	if (exe == NULL)
		return FALSE;

	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	sid   = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));

	if (sid == NULL || sid[0] == '\0')
		return FALSE;

	rargs = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(rargs, g_strdup(exe));
	g_ptr_array_add(rargs, g_strdup("--input-format"));
	g_ptr_array_add(rargs, g_strdup("stream-json"));
	g_ptr_array_add(rargs, g_strdup("--output-format"));
	g_ptr_array_add(rargs, g_strdup("stream-json"));
	emit_print_timeout(client, rargs, AI_CLI_CLIENT(client));
	g_ptr_array_add(rargs, g_strdup("--model"));
	g_ptr_array_add(rargs,
		g_strdup((model != NULL && model[0] != '\0')
			 ? model : AI_ANTIGRAVITY_DEFAULT_MODEL));
	emit_session_args(client, rargs);
	g_ptr_array_add(rargs, g_strdup("--conversation"));
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

	retry_stdin = agy_user_event_json(
		"Provide a concise plain-text summary of what you just did. "
		"Do NOT use any tools.");

	retry = g_slice_new0(RetryAsyncData);
	retry->client       = g_object_ref(client);
	retry->task         = task;
	retry->subprocess   = rproc;
	retry->tool_summary = g_strdup(tool_summary);

	g_debug("antigravity: no text in response, re-prompting for summary "
		"(conversation=%s)", sid);

	g_subprocess_communicate_utf8_async(
		rproc, retry_stdin, NULL, on_retry_communicate_complete, retry);

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

				g_debug("antigravity: re-prompt could not start, "
					"using fallback text");
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
ai_antigravity_client_chat_async(
	AiProvider          *provider,
	GList               *messages,
	const gchar         *system_prompt,
	gint                 max_tokens,
	GList               *tools,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiAntigravityClient *self = AI_ANTIGRAVITY_CLIENT(provider);
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
ai_antigravity_client_chat_finish(
	AiProvider    *provider,
	GAsyncResult  *result,
	GError       **error
){
	(void)provider;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_antigravity_client_list_models_async(
	AiProvider          *provider,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	GList *models = NULL;

	(void)cancellable;

	task = g_task_new(provider, NULL, callback, user_data);

	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_HIGH));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_MEDIUM));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_LOW));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_6_FLASH_HIGH));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_6_FLASH_MEDIUM));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_6_FLASH_LOW));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_5_FLASH_HIGH));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_5_FLASH_MEDIUM));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_5_FLASH_LOW));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_1_PRO_HIGH));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GEMINI_3_1_PRO_LOW));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_CLAUDE_SONNET_4_6));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_CLAUDE_OPUS_4_6_THINKING));
	models = g_list_append(models, g_strdup(AI_ANTIGRAVITY_MODEL_GPT_OSS_120B_MEDIUM));

	g_task_return_pointer(task, models, NULL);
	g_object_unref(task);
}

static GList *
ai_antigravity_client_list_models_finish(
	AiProvider    *provider,
	GAsyncResult  *result,
	GError       **error
){
	(void)provider;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_antigravity_client_provider_init(AiProviderInterface *iface)
{
	iface->get_provider_type = ai_antigravity_client_get_provider_type;
	iface->get_name = ai_antigravity_client_get_name;
	iface->get_default_model = ai_antigravity_client_get_default_model;
	iface->chat_async = ai_antigravity_client_chat_async;
	iface->chat_finish = ai_antigravity_client_chat_finish;
	iface->list_models_async = ai_antigravity_client_list_models_async;
	iface->list_models_finish = ai_antigravity_client_list_models_finish;
}

static void
ai_antigravity_client_chat_stream_async(
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
ai_antigravity_client_chat_stream_finish(
	AiStreamable  *streamable,
	GAsyncResult  *result,
	GError       **error
){
	return ai_cli_client_stream_run_finish(AI_CLI_CLIENT(streamable),
					       result, error);
}

static void
ai_antigravity_client_streamable_init(AiStreamableInterface *iface)
{
	iface->chat_stream_async = ai_antigravity_client_chat_stream_async;
	iface->chat_stream_finish = ai_antigravity_client_chat_stream_finish;
}

/**
 * ai_antigravity_client_new:
 *
 * Creates a new #AiAntigravityClient.
 * The `agy` CLI must be available in PATH or specified via the
 * %AGY_PATH environment variable.
 *
 * Returns: (transfer full): a new #AiAntigravityClient
 */
AiAntigravityClient *
ai_antigravity_client_new(void)
{
	g_autoptr(AiAntigravityClient) self =
		g_object_new(AI_TYPE_ANTIGRAVITY_CLIENT, NULL);

	return (AiAntigravityClient *)g_steal_pointer(&self);
}

/**
 * ai_antigravity_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiAntigravityClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiAntigravityClient
 */
AiAntigravityClient *
ai_antigravity_client_new_with_config(AiConfig *config)
{
	g_autoptr(AiAntigravityClient) self =
		g_object_new(AI_TYPE_ANTIGRAVITY_CLIENT,
			     "config", config,
			     NULL);

	return (AiAntigravityClient *)g_steal_pointer(&self);
}

/**
 * ai_antigravity_client_get_skip_permissions:
 * @self: an #AiAntigravityClient
 *
 * Gets whether the CLI runs with `--dangerously-skip-permissions`.
 *
 * Returns: %TRUE if skip permissions is enabled
 */
gboolean
ai_antigravity_client_get_skip_permissions(AiAntigravityClient *self)
{
	g_return_val_if_fail(AI_IS_ANTIGRAVITY_CLIENT(self), FALSE);

	return self->skip_permissions;
}

/**
 * ai_antigravity_client_set_skip_permissions:
 * @self: an #AiAntigravityClient
 * @skip: whether to bypass tool-use approval
 *
 * Sets whether to run the agy CLI with `--dangerously-skip-permissions`.
 */
void
ai_antigravity_client_set_skip_permissions(
	AiAntigravityClient *self,
	gboolean             skip
){
	g_return_if_fail(AI_IS_ANTIGRAVITY_CLIENT(self));

	if (self->skip_permissions == skip)
		return;

	self->skip_permissions = skip;

	g_object_notify_by_pspec(G_OBJECT(self),
				 properties[PROP_SKIP_PERMISSIONS]);
}
