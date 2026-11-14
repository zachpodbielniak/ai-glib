/*
 * ai-acp-client.c - Any Agent Client Protocol (ACP) agent as a CLI client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The Agent Client Protocol (https://agentclientprotocol.com, protocol
 * version 1) is JSON-RPC 2.0 over the agent's stdio, one object per
 * line.  Any program that speaks it -- an editor's agent server, a
 * vendor's `<cli> acp` mode, a wrapper of one's own -- becomes a provider
 * here by naming its command line.  Everything below follows the
 * published v1 protocol pages (overview, initialization, session-setup,
 * prompt-turn, content, tool-calls, file-system, schema); the prior art
 * this was checked against is OpenMausBot's server/drivers/acp/core.ts.
 *
 * One process per turn, as with codex's app-server transport:
 *
 *   -> initialize {protocolVersion, clientCapabilities, clientInfo}  id 1
 *   <- result {protocolVersion, agentCapabilities, authMethods}
 *   -> session/new {cwd, mcpServers}                                 id 2
 *      or session/load {sessionId, cwd, mcpServers}                  id 2
 *   <- result {sessionId}   (load: {}, after replaying history)
 *   -> session/prompt {sessionId, prompt: [ContentBlock]}            id 3
 *   <- session/update notifications: chunks, tool calls, plans, usage
 *   <- requests: session/request_permission, fs/read_text_file, ...
 *   <- result {stopReason}            -- the end of the turn
 *   -> EOF on stdin; the agent exits (killed after shutdown-timeout-ms)
 *
 * and a cancellation is
 *
 *   -> session/cancel {sessionId}     (a notification)
 *   <- result {stopReason: "cancelled"} to the pending session/prompt
 *
 * The pipe is the run's AiCliSteerChannel, opened as protocol input
 * (ai_cli_steer_set_protocol_input()): ACP has no request that puts words
 * into a running turn, so the class keeps AiCliClient's can_steer FALSE
 * and the channel carries this parser's lines only.
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>

#include "providers/ai-acp-client.h"
#include "core/ai-cli-client-private.h"
#include "core/ai-cli-limit-private.h"
#include "core/ai-cli-steer-private.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "core/ai-json-util.h"
#include "core/ai-mcp-config.h"
#include "model/ai-image-content.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-result.h"
#include "model/ai-tool-use.h"
#include "model/ai-usage.h"

/* Our request ids.  Fixed, so a scripted test agent can answer them. */
#define ACP_RPC_INITIALIZE   (1)
#define ACP_RPC_SESSION      (2)
#define ACP_RPC_PROMPT       (3)
#define ACP_RPC_SESSION_NEW  (4)   /* session/new after a refused session/load */

/* JSON-RPC and ACP error codes (ACP schema, ErrorCode). */
#define ACP_ERROR_METHOD_NOT_FOUND (-32601)
#define ACP_ERROR_INVALID_PARAMS   (-32602)
#define ACP_ERROR_AUTH_REQUIRED    (-32000)

/* Largest file the client side of fs/ will read or write. */
#define ACP_FILE_MAX (8 * 1024 * 1024)

/* Largest MCP configuration file an endpoint may name. */
#define ACP_MCP_CONFIG_MAX (1024 * 1024)

/* How long the agent has to exit after its answer or a session/cancel. */
#define ACP_SHUTDOWN_TIMEOUT_MS_DEFAULT (5000)

/* Overrides the agent command when the command property is unset. */
#define ACP_COMMAND_ENV "AI_ACP_COMMAND"

struct _AiAcpClient
{
	AiCliClient  parent_instance;

	gchar       *command;            /* the agent's command line */
	gboolean     skip_permissions;   /* allow what the agent asks */
	gboolean     client_fs;          /* serve fs/read_text_file (and write) */
	guint        shutdown_timeout_ms;

	JsonNode    *mcp_servers;        /* ACP McpServer array from the endpoint */
	GPtrArray   *mcp_skipped;        /* "NAME: reason" for servers left out */
	gchar       *turn_system_prompt;

	gpointer     pending_run;        /* AcpRun of the run being started */
};

static void provider_init(AiProviderInterface *iface);
static void streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiAcpClient, ai_acp_client, AI_TYPE_CLI_CLIENT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, provider_init)
	G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE, streamable_init))

enum
{
	PROP_0,
	PROP_COMMAND,
	PROP_SKIP_PERMISSIONS,
	PROP_CLIENT_FS,
	PROP_SHUTDOWN_TIMEOUT_MS,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* ================================================================
 * One run
 * ================================================================ */

/* What the run knows about one tool call, merged across its updates. */
typedef struct
{
	JsonObject *state;     /* every non-null field the agent sent */
	gboolean    started;
	gboolean    finished;
} AcpTool;

/*
 * AcpRun:
 *
 * One turn.  Planned by build_stdin() from the client's settings at the
 * moment the run starts, held in the client's pending_run until the
 * parser sees the first line, then owned (one reference) by the run's
 * channel.  The call that started it holds another, and so do the
 * cancellation handler and any armed timer, so it outlives whichever of
 * them finishes first.  Everything except the reference count is touched
 * only on @context.
 */
typedef struct
{
	gint               ref_count;   /* atomic */

	GMainContext      *context;     /* where the stream run was started */
	GCancellable      *kill;        /* handed to the stream run: kills the agent */
	GCancellable      *user;        /* the caller's, or NULL */
	gulong             cancel_id;
	GSource           *timer;       /* shutdown or cancel grace, held */
	AiCliSteerChannel *channel;     /* borrowed while attached */
	AiResponse        *response;    /* the stream run's, ref'd on attach */

	/* The plan */
	gchar             *cwd;         /* absolute */
	gchar             *root;        /* realpath(cwd): the fs/ confinement root */
	gchar             *resume_id;   /* session to load, or NULL */
	gboolean           persist;
	gboolean           skip_permissions;
	gboolean           client_fs;
	gboolean           fs_write;
	gchar             *prompt_full; /* system block + the whole conversation */
	gchar             *prompt_last; /* the newest message, for a loaded session */
	GPtrArray         *images;      /* JsonNode*: ACP image blocks */
	JsonNode          *mcp_servers; /* array, copied */
	GPtrArray         *mcp_skipped; /* strings, copied */
	guint              shutdown_timeout_ms;

	/* What the agent said about itself */
	gboolean           load_session;
	gboolean           image_ok;
	gboolean           mcp_http;
	gboolean           mcp_sse;

	/* Progress */
	gboolean           resumed;
	gchar             *session_id;
	gboolean           prompt_sent;
	gboolean           prompt_done;
	gboolean           cancel_sent;
	gboolean           user_cancelled;
	gboolean           done;
	GError            *outcome;     /* why a finished prompt failed, or NULL */

	GString           *text;        /* the text block being written */
	GHashTable        *tools;       /* toolCallId -> AcpTool */
	guint              unkeyed;     /* counter for tool calls without an id */
	gint64             input_tokens;
	gint64             output_tokens;
	gboolean           have_usage;
	gint64             cost_micros; /* -1 while unknown */
} AcpRun;

static void
acp_tool_free(gpointer data)
{
	AcpTool *tool = data;

	if (tool == NULL)
		return;
	if (tool->state != NULL)
		json_object_unref(tool->state);
	g_free(tool);
}

static void
acp_json_node_free(gpointer data)
{
	if (data != NULL)
		json_node_unref(data);
}

static AcpRun *
acp_run_new(void)
{
	AcpRun *run = g_new0(AcpRun, 1);

	run->ref_count = 1;
	run->context = g_main_context_ref_thread_default();
	run->images = g_ptr_array_new_with_free_func(acp_json_node_free);
	run->mcp_skipped = g_ptr_array_new_with_free_func(g_free);
	run->text = g_string_new(NULL);
	run->tools = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, acp_tool_free);
	run->cost_micros = -1;
	return run;
}

static AcpRun *
acp_run_ref(AcpRun *run)
{
	g_atomic_int_inc(&run->ref_count);
	return run;
}

static void
acp_run_unref(gpointer data)
{
	AcpRun *run = data;

	if (run == NULL || !g_atomic_int_dec_and_test(&run->ref_count))
		return;

	/* A timer holds a reference, so none can still be armed here. */
	g_clear_object(&run->kill);
	g_clear_object(&run->user);
	g_clear_object(&run->response);
	g_main_context_unref(run->context);
	g_free(run->cwd);
	g_free(run->root);
	g_free(run->resume_id);
	g_free(run->prompt_full);
	g_free(run->prompt_last);
	g_ptr_array_unref(run->images);
	g_clear_pointer(&run->mcp_servers, json_node_unref);
	g_ptr_array_unref(run->mcp_skipped);
	g_free(run->session_id);
	g_clear_error(&run->outcome);
	g_string_free(run->text, TRUE);
	g_hash_table_unref(run->tools);
	g_free(run);
}

/* The channel's reference: forget the channel, then drop it. */
static void
acp_run_detach(gpointer data)
{
	AcpRun *run = data;

	run->channel = NULL;
	acp_run_unref(run);
}

/* Stops the shutdown or cancel timer, if one is armed. */
static void
acp_run_disarm(AcpRun *run)
{
	if (run->timer == NULL)
		return;
	g_source_destroy(run->timer);
	g_clear_pointer(&run->timer, g_source_unref);
}

/* The grace is over: kill the agent.  The stream run then ends. */
static gboolean
on_acp_timer(gpointer user_data)
{
	AcpRun *run = user_data;

	/* The source is finishing; drop the held reference, not destroy it. */
	g_clear_pointer(&run->timer, g_source_unref);
	if (!run->done && run->kill != NULL)
		g_cancellable_cancel(run->kill);
	return G_SOURCE_REMOVE;
}

/*
 * acp_run_arm:
 * @run: the run
 * @timeout_ms: how long the agent has; 0 kills it at once
 *
 * Arms the one timer a run has, on the run's own context (a private one
 * for a synchronous call).  Held as a GSource, never by id: see
 * AGENTS.md, "Main contexts and source teardown".
 */
static void
acp_run_arm(AcpRun *run, guint timeout_ms)
{
	acp_run_disarm(run);
	if (run->done || run->kill == NULL)
		return;
	if (timeout_ms == 0)
	{
		g_cancellable_cancel(run->kill);
		return;
	}
	run->timer = g_timeout_source_new(timeout_ms);
	g_source_set_callback(run->timer, on_acp_timer, acp_run_ref(run), acp_run_unref);
	g_source_attach(run->timer, run->context);
}

/* ================================================================
 * JSON-RPC lines
 * ================================================================ */

/* Serializes a builder's root and terminates the line. */
static gchar *
acp_finish_line(JsonBuilder *builder)
{
	g_autoptr(JsonNode) root = json_builder_get_root(builder);
	g_autofree gchar *json = json_to_string(root, FALSE);

	return g_strconcat(json, "\n", NULL);
}

/*
 * acp_request_line:
 * @id: the request id, or -1 for a notification
 * @method: the method
 * @params: (transfer none): the params object
 *
 * Returns: (transfer full): one newline-terminated JSON-RPC 2.0 line
 */
static gchar *
acp_request_line(gint64 id, const gchar *method, JsonNode *params)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	if (id >= 0)
	{
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, id);
	}
	json_builder_set_member_name(builder, "method");
	json_builder_add_string_value(builder, method);
	json_builder_set_member_name(builder, "params");
	json_builder_add_value(builder, json_node_copy(params));
	json_builder_end_object(builder);
	return acp_finish_line(builder);
}

/* The answer to a request the agent made; takes @result. */
static gchar *
acp_result_line(JsonNode *id, JsonNode *result)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	json_builder_set_member_name(builder, "id");
	json_builder_add_value(builder, json_node_copy(id));
	json_builder_set_member_name(builder, "result");
	json_builder_add_value(builder, result);
	json_builder_end_object(builder);
	return acp_finish_line(builder);
}

/* An error answer to a request the agent made. */
static gchar *
acp_error_line(JsonNode *id, gint64 code, const gchar *message)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	json_builder_set_member_name(builder, "id");
	json_builder_add_value(builder, json_node_copy(id));
	json_builder_set_member_name(builder, "error");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "code");
	json_builder_add_int_value(builder, code);
	json_builder_set_member_name(builder, "message");
	json_builder_add_string_value(builder, message);
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	return acp_finish_line(builder);
}

/* {"sessionId": @session_id} -- session/cancel's params. */
static JsonNode *
acp_session_params(const gchar *session_id)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "sessionId");
	json_builder_add_string_value(builder, session_id);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* ================================================================
 * Failures
 * ================================================================ */

/*
 * What a generic ACP agent says when it is limited, throttled or signed
 * out.  There is no one CLI behind this class, so the table holds the
 * wording that is common to the agents ai-glib already wraps (the per-CLI
 * tables of codex, grok-build, opencode, cursor and antigravity, each
 * cited where it is defined) and to the ACP engines OpenMausBot drives
 * (its LIMIT_WORDS in server/drivers/acp/quiet-status.ts: "429", "rate
 * limit", "too many requests", "usage limit", "quota",
 * "insufficient_quota", "out of credits").  Ordered as
 * ai-cli-limit-private.h asks: the session limit before the rate-limit
 * words its text may also contain.  ACP's own code for a signed-out agent,
 * -32000 "Authentication required" (ACP schema, ErrorCode), is decided
 * from the code, not from words (acp_failure()).
 */
static const AiCliLimitPattern acp_limit_patterns[] = {
	{ AI_ERROR_SESSION_LIMIT, "usage limit" },
	{ AI_ERROR_SESSION_LIMIT, "usage_limit" },
	{ AI_ERROR_INSUFFICIENT_QUOTA, "insufficient_quota" },
	{ AI_ERROR_INSUFFICIENT_QUOTA, "quota exceeded" },
	{ AI_ERROR_INSUFFICIENT_QUOTA, "out of credits" },
	{ AI_ERROR_INSUFFICIENT_QUOTA, "credit balance is too low" },
	{ AI_ERROR_RATE_LIMITED, "too many requests" },
	{ AI_ERROR_RATE_LIMITED, "rate limit" },
	{ AI_ERROR_RATE_LIMITED, "rate_limit" },
	{ AI_ERROR_RATE_LIMITED, "429" },
	{ AI_ERROR_INVALID_API_KEY, "authentication required" },
	{ AI_ERROR_INVALID_API_KEY, "not authenticated" },
	{ AI_ERROR_INVALID_API_KEY, "not logged in" },
	{ AI_ERROR_INVALID_API_KEY, "invalid api key" },
	{ AI_ERROR_INVALID_API_KEY, "invalid_api_key" },
	{ AI_ERROR_INVALID_API_KEY, "401 unauthorized" },
	{ 0, NULL }
};

#define ACP_CLI_NAME "ACP agent"

/*
 * acp_error_text:
 * @failure: a JSON-RPC error object {code, message, data}
 *
 * The message, with the cause many agents put in `data` under a generic
 * message ("Internal error": "<what really happened>"): a string, or
 * data.message / data.details.
 *
 * Returns: (transfer full): the text
 */
static gchar *
acp_error_text(JsonObject *failure)
{
	const gchar *message = ai_json_get_string(failure, "message", "ACP request failed");
	JsonObject *data = ai_json_get_object(failure, "data");
	const gchar *detail = ai_json_get_string(failure, "data", NULL);

	if (detail == NULL)
		detail = ai_json_get_string(data, "message", NULL);
	if (detail == NULL)
		detail = ai_json_get_string(data, "details", NULL);
	if (detail != NULL && *detail != '\0' && g_strcmp0(detail, message) != 0)
		return g_strdup_printf("%s: %s", message, detail);
	return g_strdup(message);
}

/*
 * acp_failure:
 * @failure: (nullable): a JSON-RPC error object
 * @step: what was being asked ("session/prompt")
 * @error: return location
 *
 * One failed request, classified through the shared limit table so a
 * usage limit, a 429 and a lapsed login read the same here as on every
 * other CLI provider.  ACP's -32000 is a sign-in failure whatever its
 * words; anything else unmatched is AI_ERROR_CLI_EXECUTION, which the base
 * reclassifies once more on the way out (harmless).
 */
static void
acp_failure(JsonObject *failure, const gchar *step, GError **error)
{
	g_autofree gchar *text = acp_error_text(failure);
	g_autofree gchar *said = g_strdup_printf("ACP agent refused %s: %s", step, text);
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	if (ai_json_get_int(failure, "code", 0) == ACP_ERROR_AUTH_REQUIRED)
	{
		const AiCliLimitPattern signed_out[] = {
			{ AI_ERROR_INVALID_API_KEY, said }, { 0, NULL }
		};

		if (ai_cli_limit_classify(signed_out, ACP_CLI_NAME, said, now, error))
			return;
	}
	if (ai_cli_limit_classify(acp_limit_patterns, ACP_CLI_NAME, said, now, error))
		return;
	g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, said);
}

/* ================================================================
 * Configuration
 * ================================================================ */

/* The command line in effect: the property, else $AI_ACP_COMMAND. */
static const gchar *
acp_command(AiAcpClient *self)
{
	const gchar *env;

	if (self->command != NULL && *self->command != '\0')
		return self->command;
	env = g_getenv(ACP_COMMAND_ENV);
	return env != NULL && *env != '\0' ? env : NULL;
}

/*
 * acp_parse_command:
 * @self: an ACP client
 * @error: return location
 *
 * The command line split the way a shell would split it, with no shell
 * run: no expansion, no substitution, no redirection.
 *
 * Returns: (transfer full) (nullable): the argv
 */
static gchar **
acp_parse_command(AiAcpClient *self, GError **error)
{
	const gchar *command = acp_command(self);
	gchar **argv = NULL;
	gint argc = 0;

	if (command == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		            "no ACP agent command: set AiAcpClient:command or $%s",
		            ACP_COMMAND_ENV);
		return NULL;
	}
	if (!g_shell_parse_argv(command, &argc, &argv, error))
		return NULL;
	if (argc < 1 || argv[0][0] == '\0')
	{
		g_strfreev(argv);
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		                    "the ACP agent command is empty");
		return NULL;
	}
	return argv;
}

static gchar *
get_executable(AiCliClient *client)
{
	g_auto(GStrv) argv = acp_parse_command(AI_ACP_CLIENT(client), NULL);

	return argv != NULL ? g_strdup(argv[0]) : NULL;
}

/* The whole command; argv[0] is replaced by the resolved executable. */
static gchar **
build_argv(AiCliClient *client, GList *messages, const gchar *system_prompt,
           gint max_tokens, gboolean streaming)
{
	AiAcpClient *self = AI_ACP_CLIENT(client);
	g_autoptr(GError) error = NULL;
	gchar **argv;

	(void)messages; (void)max_tokens; (void)streaming;

	argv = acp_parse_command(self, &error);
	if (argv == NULL)
	{
		g_debug("acp: %s", error->message);
		return NULL;
	}
	g_free(self->turn_system_prompt);
	self->turn_system_prompt = g_strdup(system_prompt);
	return argv;
}

/* ================================================================
 * Planning the run
 * ================================================================ */

/*
 * acp_prompt_text:
 * @self: an ACP client
 * @messages: (element-type AiMessage): the conversation
 * @resumed: whether the agent already holds the history (session/load)
 *
 * The turn's prompt, as codex's transports build theirs: the system
 * prompt in a <system> block and the projected conversation, or -- for a
 * session the agent loads and so already remembers -- the newest message
 * alone.
 *
 * Returns: (transfer full): the text
 */
static gchar *
acp_prompt_text(AiAcpClient *self, GList *messages, gboolean resumed)
{
	AiCliClient *client = AI_CLI_CLIENT(self);
	GString *prompt = g_string_new(NULL);
	const gchar *system = self->turn_system_prompt;
	GList *l;

	if (system == NULL)
		system = ai_cli_client_get_system_prompt(client);
	if (!resumed && system != NULL && *system != '\0')
		g_string_append_printf(prompt, "<system>\n%s\n</system>\n\n", system);
	for (l = resumed ? g_list_last(messages) : messages; l != NULL; l = l->next)
	{
		g_autofree gchar *text = ai_cli_client_project_multimodal_message(l->data);

		if (text == NULL || *text == '\0')
			continue;
		if (prompt->len > 0 && !g_str_has_suffix(prompt->str, "\n\n"))
			g_string_append(prompt, "\n\n");
		g_string_append(prompt, text);
	}
	return g_string_free(prompt, FALSE);
}

/* The newest message's images as ACP image blocks {type, mimeType, data}. */
static void
acp_collect_images(AcpRun *run, GList *messages)
{
	GList *last = g_list_last(messages);
	GList *b;

	if (last == NULL)
		return;
	for (b = ai_message_get_content_blocks(last->data); b != NULL; b = b->next)
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autofree gchar *encoded = NULL;
		AiImage *image;
		GBytes *bytes;
		const gchar *mime;
		gsize length = 0;
		const guchar *data;

		if (!AI_IS_IMAGE_CONTENT(b->data))
			continue;
		image = ai_image_content_get_image(b->data);
		bytes = image != NULL ? ai_image_get_bytes(image) : NULL;
		if (bytes == NULL)
			continue;
		data = g_bytes_get_data(bytes, &length);
		encoded = g_base64_encode(data, length);
		mime = ai_image_get_mime_type(image);

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "image");
		json_builder_set_member_name(builder, "mimeType");
		json_builder_add_string_value(builder, mime != NULL ? mime : "image/png");
		json_builder_set_member_name(builder, "data");
		json_builder_add_string_value(builder, encoded);
		json_builder_end_object(builder);
		g_ptr_array_add(run->images, json_builder_get_root(builder));
	}
}

/*
 * acp_initialize_line:
 * @run: the planned run
 *
 * The first line: initialize, with the capabilities this run serves.
 * Terminals are never offered: running commands for the agent would be
 * a second, unapproved way to execute what its own tools already run
 * behind session/request_permission.  File reads are offered when
 * client-fs is on; writes only when skip-permissions is too, because a
 * write through fs/ is not preceded by a permission request the client
 * could have refused.  Both stay inside the session's cwd (acp_confine()).
 *
 * Returns: (transfer full): the line
 */
static gchar *
acp_initialize_line(AcpRun *run)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) params = NULL;
	g_autofree gchar *version = g_strdup_printf("%d.%d.%d", AI_VERSION_MAJOR,
	                                            AI_VERSION_MINOR, AI_VERSION_MICRO);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "protocolVersion");
	json_builder_add_int_value(builder, AI_ACP_PROTOCOL_VERSION);
	json_builder_set_member_name(builder, "clientCapabilities");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "fs");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "readTextFile");
	json_builder_add_boolean_value(builder, run->client_fs);
	json_builder_set_member_name(builder, "writeTextFile");
	json_builder_add_boolean_value(builder, run->fs_write);
	json_builder_end_object(builder);
	json_builder_set_member_name(builder, "terminal");
	json_builder_add_boolean_value(builder, FALSE);
	json_builder_end_object(builder);
	json_builder_set_member_name(builder, "clientInfo");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "ai-glib");
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, "ai-glib");
	json_builder_set_member_name(builder, "version");
	json_builder_add_string_value(builder, version);
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	params = json_builder_get_root(builder);

	return acp_request_line(ACP_RPC_INITIALIZE, "initialize", params);
}

/*
 * build_stdin:
 * @client: an ACP client
 * @messages: (element-type AiMessage): the conversation
 *
 * Plans the run now, from the client's settings at this moment, so a
 * property changed mid-turn cannot change a request already decided, and
 * returns the first line to write.  The plan goes on the run the calling
 * chat or stream started (pending_run), or on a fresh one when the base's
 * stream run was called directly.
 *
 * Returns: (transfer full): the initialize line
 */
static gchar *
build_stdin(AiCliClient *client, GList *messages)
{
	AiAcpClient *self = AI_ACP_CLIENT(client);
	const gchar *session = ai_cli_client_get_session_id(client);
	const gchar *dir = ai_cli_client_get_working_directory(client);
	g_autofree gchar *here = g_get_current_dir();
	AcpRun *run = self->pending_run;
	guint i;

	if (run == NULL)
	{
		run = acp_run_new();
		self->pending_run = run;
	}

	/* cwd must be absolute (session-setup); it is also the fs/ root. */
	if (dir == NULL || *dir == '\0')
		run->cwd = g_strdup(here);
	else
		run->cwd = g_canonicalize_filename(dir, here);
	run->root = realpath(run->cwd, NULL);

	run->persist = ai_cli_client_get_session_persistence(client);
	if (run->persist && session != NULL && *session != '\0')
		run->resume_id = g_strdup(session);
	run->skip_permissions = self->skip_permissions;
	run->client_fs = self->client_fs;
	run->fs_write = self->client_fs && self->skip_permissions;
	run->shutdown_timeout_ms = self->shutdown_timeout_ms;
	run->prompt_full = acp_prompt_text(self, messages, FALSE);
	run->prompt_last = acp_prompt_text(self, messages, TRUE);
	acp_collect_images(run, messages);
	if (self->mcp_servers != NULL)
		run->mcp_servers = json_node_copy(self->mcp_servers);
	for (i = 0; self->mcp_skipped != NULL && i < self->mcp_skipped->len; i++)
		g_ptr_array_add(run->mcp_skipped, g_strdup(g_ptr_array_index(self->mcp_skipped, i)));

	return acp_initialize_line(run);
}

/* ================================================================
 * The handshake
 * ================================================================ */

/*
 * acp_session_line:
 * @run: the run
 * @load: session/load (the agent replays the history) or session/new
 * @id: the request id
 * @events: (element-type AiEvent): where notices go
 *
 * MCP servers go over as the endpoint granted them, less the ones this
 * agent cannot reach: an http or sse server needs the matching
 * mcpCapabilities flag, and stdio is something every agent supports.
 *
 * Returns: (transfer full): the line
 */
static gchar *
acp_session_line(AcpRun *run, gboolean load, gint64 id, GPtrArray *events)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) params = NULL;
	JsonArray *servers = run->mcp_servers != NULL ? json_node_get_array(run->mcp_servers) : NULL;
	guint i;

	json_builder_begin_object(builder);
	if (load)
	{
		json_builder_set_member_name(builder, "sessionId");
		json_builder_add_string_value(builder, run->resume_id);
	}
	json_builder_set_member_name(builder, "cwd");
	json_builder_add_string_value(builder, run->cwd);
	json_builder_set_member_name(builder, "mcpServers");
	json_builder_begin_array(builder);
	for (i = 0; servers != NULL && i < json_array_get_length(servers); i++)
	{
		JsonObject *server = ai_json_array_get_object(servers, i);
		const gchar *type = ai_json_get_string(server, "type", NULL);
		const gchar *name = ai_json_get_string(server, "name", "?");

		if ((g_strcmp0(type, "http") == 0 && !run->mcp_http) ||
		    (g_strcmp0(type, "sse") == 0 && !run->mcp_sse))
		{
			g_autofree gchar *notice = g_strdup_printf(
				"MCP server %s left out: the ACP agent does not accept %s servers",
				name, type);

			g_ptr_array_add(events, ai_event_new_status(notice));
			continue;
		}
		json_builder_add_value(builder, json_node_copy(json_array_get_element(servers, i)));
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	params = json_builder_get_root(builder);

	return acp_request_line(id, load ? "session/load" : "session/new", params);
}

/*
 * acp_send_prompt:
 *
 * session/prompt with the text and, when the agent takes them
 * (promptCapabilities.image), the newest message's images.  Images an
 * agent did not advertise are left out and said so: the prompt content
 * must stay within what initialize agreed (prompt-turn).
 */
static void
acp_send_prompt(AcpRun *run, AiResponse *response, GPtrArray *events)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) params = NULL;
	g_autofree gchar *line = NULL;
	guint i;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "sessionId");
	json_builder_add_string_value(builder, run->session_id);
	json_builder_set_member_name(builder, "prompt");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "text");
	json_builder_set_member_name(builder, "text");
	json_builder_add_string_value(builder, run->resumed ? run->prompt_last : run->prompt_full);
	json_builder_end_object(builder);
	for (i = 0; run->image_ok && i < run->images->len; i++)
		json_builder_add_value(builder, json_node_copy(g_ptr_array_index(run->images, i)));
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	params = json_builder_get_root(builder);

	if (!run->image_ok && run->images->len > 0)
	{
		g_autofree gchar *notice = g_strdup_printf(
			"%u image(s) left out: the ACP agent does not accept image prompts",
			run->images->len);

		g_ptr_array_add(events, ai_event_new_status(notice));
	}

	line = acp_request_line(ACP_RPC_PROMPT, "session/prompt", params);
	ai_cli_steer_channel_write(run->channel, line);
	run->prompt_sent = TRUE;
	ai_response_set_stop_reason(response, AI_STOP_REASON_NONE);
	g_ptr_array_add(events, ai_event_new(AI_EVENT_STREAM_START));
}

/* initialize answered: check the version, note the capabilities, open a session. */
static gboolean
acp_initialized(AcpRun *run, JsonObject *result, GPtrArray *events, GError **error)
{
	JsonObject *caps = ai_json_get_object(result, "agentCapabilities");
	JsonObject *prompt_caps = ai_json_get_object(caps, "promptCapabilities");
	JsonObject *mcp_caps = ai_json_get_object(caps, "mcpCapabilities");
	gint64 version = ai_json_get_int(result, "protocolVersion", AI_ACP_PROTOCOL_VERSION);
	gboolean load;
	g_autofree gchar *line = NULL;
	guint i;

	/* Negotiation: an agent that cannot speak ours answers its own. */
	if (version != AI_ACP_PROTOCOL_VERSION)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		            "the ACP agent speaks protocol version %" G_GINT64_FORMAT
		            "; ai-glib speaks %d", version, AI_ACP_PROTOCOL_VERSION);
		return FALSE;
	}
	run->load_session = ai_json_get_boolean(caps, "loadSession", FALSE);
	run->image_ok = ai_json_get_boolean(prompt_caps, "image", FALSE);
	run->mcp_http = ai_json_get_boolean(mcp_caps, "http", FALSE);
	run->mcp_sse = ai_json_get_boolean(mcp_caps, "sse", FALSE);

	for (i = 0; i < run->mcp_skipped->len; i++)
	{
		g_autofree gchar *notice = g_strdup_printf("MCP server %s",
		                                           (gchar *)g_ptr_array_index(run->mcp_skipped, i));

		g_ptr_array_add(events, ai_event_new_status(notice));
	}

	load = run->resume_id != NULL && run->load_session;
	if (run->resume_id != NULL && !run->load_session)
		g_ptr_array_add(events, ai_event_new_status(
			"the ACP agent cannot load sessions; starting a new one with the whole conversation"));
	run->resumed = load;
	line = acp_session_line(run, load, ACP_RPC_SESSION, events);
	ai_cli_steer_channel_write(run->channel, line);
	return TRUE;
}

/* session/new or session/load answered: remember the session, send the prompt. */
static gboolean
acp_session_opened(AiAcpClient *self, AcpRun *run, gint64 id, JsonObject *obj,
                   AiResponse *response, GPtrArray *events, GError **error)
{
	JsonObject *failure = ai_json_get_object(obj, "error");
	JsonObject *result = ai_json_get_object(obj, "result");
	gboolean loading = id == ACP_RPC_SESSION && run->resumed;

	if (failure != NULL)
	{
		/*
		 * A session the agent no longer has is not a failed turn: start a
		 * new one with the whole conversation.  A sign-in failure is, and
		 * a failed session/new is the end of the run.
		 */
		if (loading && ai_json_get_int(failure, "code", 0) != ACP_ERROR_AUTH_REQUIRED)
		{
			g_autofree gchar *text = acp_error_text(failure);
			g_autofree gchar *notice = g_strdup_printf(
				"the ACP agent could not load session %s (%s); starting a new one",
				run->resume_id, text);
			g_autofree gchar *line = NULL;

			g_ptr_array_add(events, ai_event_new_status(notice));
			run->resumed = FALSE;
			line = acp_session_line(run, FALSE, ACP_RPC_SESSION_NEW, events);
			ai_cli_steer_channel_write(run->channel, line);
			return TRUE;
		}
		acp_failure(failure, loading ? "session/load" : "session/new", error);
		return FALSE;
	}

	if (loading)
	{
		run->session_id = g_strdup(run->resume_id);
	}
	else
	{
		const gchar *session = ai_json_get_string(result, "sessionId", NULL);

		if (session == NULL || *session == '\0')
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
			                    "the ACP agent answered session/new without a sessionId");
			return FALSE;
		}
		run->session_id = g_strdup(session);
		if (run->persist)
			ai_cli_client_set_session_id(AI_CLI_CLIENT(self), session);
	}

	/* A cancel that arrived while the session opened stops here. */
	if (run->user_cancelled)
	{
		acp_run_arm(run, 0);
		return TRUE;
	}
	acp_send_prompt(run, response, events);
	return TRUE;
}

/* ================================================================
 * The turn's output
 * ================================================================ */

/* Closes the text block being written, as the shared reader does at a tool. */
static void
acp_flush_text(AcpRun *run, AiResponse *response)
{
	g_autoptr(AiTextContent) block = NULL;

	if (run->text->len == 0)
		return;
	block = ai_text_content_new(run->text->str);
	ai_response_add_content_block(response, AI_CONTENT_BLOCK(g_steal_pointer(&block)));
	g_string_truncate(run->text, 0);
}

/*
 * acp_tool_input:
 *
 * The tool's input as the view layer sees it: the agent's title and kind
 * (the acp_* styles in ai-tool-style.c name their target by "title"),
 * plus whatever it sent as name, rawInput and locations.
 *
 * Returns: (transfer full): an object node
 */
static JsonNode *
acp_tool_input(AcpTool *tool)
{
	static const gchar *const keys[] = { "title", "kind", "name", "rawInput", "locations", NULL };
	JsonObject *input = json_object_new();
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	guint i;

	for (i = 0; keys[i] != NULL; i++)
	{
		JsonNode *value = ai_json_get_node(tool->state, keys[i]);

		if (value != NULL)
			json_object_set_member(input, keys[i], json_node_copy(value));
	}
	json_node_take_object(node, input);
	return node;
}

/* "acp_<kind>", the name the acp_* tool styles are registered under. */
static gchar *
acp_tool_name(AcpTool *tool)
{
	static const gchar *const kinds[] = {
		"read", "edit", "delete", "move", "search", "execute",
		"think", "fetch", "switch_mode", "other", NULL
	};
	const gchar *kind = ai_json_get_string(tool->state, "kind", "other");

	return g_strconcat("acp_", g_strv_contains(kinds, kind) ? kind : "other", NULL);
}

/*
 * acp_tool_output:
 *
 * What the call produced, as text: its text content blocks, a line per
 * diff and terminal, or rawOutput when it sent no content.
 *
 * Returns: (transfer full): the text
 */
static gchar *
acp_tool_output(AcpTool *tool)
{
	JsonArray *content = ai_json_get_array(tool->state, "content");
	JsonNode *raw = ai_json_get_node(tool->state, "rawOutput");
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; content != NULL && i < json_array_get_length(content); i++)
	{
		JsonObject *item = ai_json_array_get_object(content, i);
		const gchar *type = ai_json_get_string(item, "type", "");
		const gchar *piece = NULL;
		g_autofree gchar *owned = NULL;

		if (g_str_equal(type, "content"))
			piece = ai_json_get_string(ai_json_get_object(item, "content"), "text", NULL);
		else if (g_str_equal(type, "diff"))
			piece = owned = g_strdup_printf("diff: %s",
			                                ai_json_get_string(item, "path", "?"));
		else if (g_str_equal(type, "terminal"))
			piece = owned = g_strdup_printf("terminal: %s",
			                                ai_json_get_string(item, "terminalId", "?"));
		if (piece == NULL || *piece == '\0')
			continue;
		if (out->len > 0)
			g_string_append_c(out, '\n');
		g_string_append(out, piece);
	}
	if (out->len == 0 && raw != NULL)
	{
		g_autofree gchar *json = NULL;

		if (JSON_NODE_HOLDS_VALUE(raw) && json_node_get_value_type(raw) == G_TYPE_STRING)
			g_string_append(out, json_node_get_string(raw));
		else
		{
			json = json_to_string(raw, FALSE);
			g_string_append(out, json);
		}
	}
	return g_string_free(out, FALSE);
}

/*
 * acp_tool_update:
 * @run: the run
 * @update: a tool_call or tool_call_update
 * @initial: which of the two
 * @response: the response being built
 * @events: (element-type AiEvent): where events go
 *
 * Merges the update into what the run knows of the call (an update only
 * changes the fields it sends, tool-calls page) and narrates it: started
 * once it is known, started again when its input or title arrives late
 * (AGENTS.md: consumers key on the id), finished once, when it reaches
 * "completed" or "failed".  A tool_call may arrive already finished.
 */
static void
acp_tool_update(AcpRun *run, JsonObject *update, gboolean initial,
                AiResponse *response, GPtrArray *events)
{
	const gchar *id = ai_json_get_string(update, "toolCallId", NULL);
	g_autofree gchar *synthetic = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(JsonNode) input = NULL;
	g_autoptr(AiToolUse) use = NULL;
	AcpTool *tool;
	const gchar *status;
	gboolean restart;
	GList *members;
	GList *l;

	if (id == NULL || *id == '\0')
	{
		/* An update with no id names nothing; a call with none gets one. */
		if (!initial)
			return;
		id = synthetic = g_strdup_printf("acp-tool-%u", ++run->unkeyed);
	}
	tool = g_hash_table_lookup(run->tools, id);
	if (tool == NULL)
	{
		tool = g_new0(AcpTool, 1);
		tool->state = json_object_new();
		g_hash_table_insert(run->tools, g_strdup(id), tool);
	}
	if (tool->finished)
		return;

	restart = tool->started && (ai_json_get_node(update, "rawInput") != NULL ||
	                            ai_json_get_node(update, "title") != NULL);
	members = json_object_get_members(update);
	for (l = members; l != NULL; l = l->next)
	{
		JsonNode *value = ai_json_get_node(update, l->data);

		if (value != NULL && !g_str_equal(l->data, "sessionUpdate"))
			json_object_set_member(tool->state, l->data, json_node_copy(value));
	}
	g_list_free(members);

	name = acp_tool_name(tool);
	input = acp_tool_input(tool);
	use = ai_tool_use_new(id, name, input);
	status = ai_json_get_string(tool->state, "status", "pending");

	if (!tool->started || restart)
	{
		acp_flush_text(run, response);
		tool->started = TRUE;
		g_ptr_array_add(events, ai_event_new_tool_started(use));
	}
	if (g_str_equal(status, "completed") || g_str_equal(status, "failed"))
	{
		g_autofree gchar *output = acp_tool_output(tool);
		g_autoptr(AiToolResult) result = ai_tool_result_new_with_name(
			id, name, output, g_str_equal(status, "failed"));

		tool->finished = TRUE;
		g_ptr_array_add(events, ai_event_new_tool_finished(use, result));
	}
}

/* A plan, as a status line a human can read: [x] done, [>] running, [ ] to do. */
static void
acp_plan(JsonObject *update, GPtrArray *events)
{
	JsonArray *entries = ai_json_get_array(update, "entries");
	g_autoptr(GString) text = g_string_new("Plan:");
	guint i;

	for (i = 0; entries != NULL && i < json_array_get_length(entries); i++)
	{
		JsonObject *entry = ai_json_array_get_object(entries, i);
		const gchar *status = ai_json_get_string(entry, "status", "pending");
		const gchar *mark = g_str_equal(status, "completed") ? "[x]"
		                  : g_str_equal(status, "in_progress") ? "[>]" : "[ ]";

		g_string_append_printf(text, "\n%s %s", mark,
		                       ai_json_get_string(entry, "content", ""));
	}
	g_ptr_array_add(events, ai_event_new_status(text->str));
}

/*
 * acp_session_update:
 *
 * One session/update.  Only updates for this run's session and only once
 * the prompt is out: anything earlier is a session/load replay of turns
 * the caller already has (session-setup), and anything after the prompt's
 * answer belongs to no turn.
 */
static void
acp_session_update(AcpRun *run, JsonObject *params, AiResponse *response,
                   GPtrArray *events)
{
	const gchar *session = ai_json_get_string(params, "sessionId", NULL);
	JsonObject *update = ai_json_get_object(params, "update");
	const gchar *kind = ai_json_get_string(update, "sessionUpdate", "");

	if (!run->prompt_sent || run->prompt_done)
		return;
	if (session != NULL && g_strcmp0(session, run->session_id) != 0)
		return;

	if (g_str_equal(kind, "agent_message_chunk") || g_str_equal(kind, "agent_thought_chunk"))
	{
		JsonObject *content = ai_json_get_object(update, "content");
		const gchar *text = ai_json_get_string(content, "text", NULL);

		if (g_strcmp0(ai_json_get_string(content, "type", NULL), "text") != 0 ||
		    text == NULL || *text == '\0')
			return;
		if (g_str_equal(kind, "agent_message_chunk"))
		{
			g_string_append(run->text, text);
			g_ptr_array_add(events, ai_event_new_text_delta(text));
		}
		else
		{
			g_ptr_array_add(events, ai_event_new_thinking_delta(text));
		}
	}
	else if (g_str_equal(kind, "tool_call") || g_str_equal(kind, "tool_call_update"))
	{
		acp_tool_update(run, update, g_str_equal(kind, "tool_call"), response, events);
	}
	else if (g_str_equal(kind, "plan"))
	{
		acp_plan(update, events);
	}
	else if (g_str_equal(kind, "usage_update"))
	{
		/* {used, size, cost?: {amount, currency}}: only a dollar cost is kept. */
		JsonObject *cost = ai_json_get_object(update, "cost");

		if (cost != NULL && g_strcmp0(ai_json_get_string(cost, "currency", NULL), "USD") == 0)
			run->cost_micros = (gint64)(ai_json_get_double(cost, "amount", 0.0) * 1000000.0 + 0.5);
	}
	/* user_message_chunk, commands, modes, session info: not events. */
}

/*
 * acp_prompt_answered:
 *
 * The session/prompt result is the end of the turn (ACP has no separate
 * completion).  stopReason becomes the response's stop reason:
 *
 *   end_turn           AI_STOP_REASON_END_TURN
 *   max_tokens         AI_STOP_REASON_MAX_TOKENS
 *   max_turn_requests  AI_STOP_REASON_MAX_TOKENS, with a status saying so
 *   refusal            AI_STOP_REASON_CONTENT_FILTER
 *   cancelled          the caller's G_IO_ERROR_CANCELLED when it asked;
 *                      AI_ERROR_CANCELLED when the agent stopped on its own
 *
 * The response is complete from here; the process need only exit.  stdin
 * closes once the channel has nothing left to write, and the agent gets
 * shutdown-timeout-ms to go before it is killed.
 */
static void
acp_prompt_answered(AcpRun *run, JsonObject *obj, AiResponse *response, GPtrArray *events)
{
	JsonObject *failure = ai_json_get_object(obj, "error");
	JsonObject *result = ai_json_get_object(obj, "result");
	JsonObject *usage = ai_json_get_object(result, "usage");
	const gchar *reason = ai_json_get_string(result, "stopReason", NULL);

	run->prompt_done = TRUE;
	acp_flush_text(run, response);

	/* result.usage {inputTokens, outputTokens} is not in v1; some agents send it. */
	if (usage != NULL)
	{
		run->have_usage = TRUE;
		run->input_tokens = MAX(0, ai_json_get_int(usage, "inputTokens", 0));
		run->output_tokens = MAX(0, ai_json_get_int(usage, "outputTokens", 0));
	}
	if (run->have_usage)
	{
		g_autoptr(AiUsage) counted = ai_usage_new((gint)CLAMP(run->input_tokens, 0, G_MAXINT),
		                                          (gint)CLAMP(run->output_tokens, 0, G_MAXINT));

		ai_response_set_usage(response, counted);
		g_ptr_array_add(events, ai_event_new_usage(counted, run->cost_micros));
	}
	if (run->cost_micros >= 0)
		ai_response_set_cost_micros(response, run->cost_micros);

	if (failure != NULL)
		acp_failure(failure, "session/prompt", &run->outcome);
	else if (g_strcmp0(reason, "end_turn") == 0)
		ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
	else if (g_strcmp0(reason, "max_tokens") == 0)
		ai_response_set_stop_reason(response, AI_STOP_REASON_MAX_TOKENS);
	else if (g_strcmp0(reason, "max_turn_requests") == 0)
	{
		ai_response_set_stop_reason(response, AI_STOP_REASON_MAX_TOKENS);
		g_ptr_array_add(events, ai_event_new_status(
			"the ACP agent stopped: it reached its limit of model requests for one turn"));
	}
	else if (g_strcmp0(reason, "refusal") == 0)
		ai_response_set_stop_reason(response, AI_STOP_REASON_CONTENT_FILTER);
	else if (g_strcmp0(reason, "cancelled") == 0)
		g_set_error_literal(&run->outcome, AI_ERROR, AI_ERROR_CANCELLED,
		                    "the ACP agent cancelled the turn");
	else if (reason == NULL)
		g_set_error_literal(&run->outcome, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
		                    "the ACP agent answered session/prompt without a stopReason");
	else
	{
		/* A reason newer than v1: the answer is still the answer. */
		g_autofree gchar *notice = g_strdup_printf("the ACP agent stopped: %s", reason);

		ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
		g_ptr_array_add(events, ai_event_new_status(notice));
	}

	ai_cli_steer_channel_settle(run->channel);
	acp_run_arm(run, run->shutdown_timeout_ms);
}

/* ================================================================
 * Requests from the agent
 * ================================================================ */

/*
 * acp_confine:
 * @run: the run
 * @path: (nullable): the path the agent named
 * @must_exist: a read (the file must exist) or a write (it may not yet)
 * @error: return location
 *
 * The path the client will touch, or a refusal.  ACP paths are absolute
 * (file-system); `.` and `..` are folded lexically, then the deepest part
 * that exists is resolved through its symlinks and the rest appended, so
 * neither `..` nor a link can lead outside the session's cwd (realpath'd
 * when the run was planned).  The answer is the resolved path, which is
 * what is then read or replaced.
 *
 * Returns: (transfer full) (nullable): the confined path
 */
static gchar *
acp_confine(AcpRun *run, const gchar *path, gboolean must_exist, GError **error)
{
	g_autofree gchar *lexical = NULL;
	g_autofree gchar *existing = NULL;
	g_autofree gchar *suffix = NULL;
	g_autofree gchar *real = NULL;
	g_autofree gchar *full = NULL;
	struct stat st;

	if (run->root == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_PERMISSION_DENIED,
		            "the session directory %s cannot be resolved", run->cwd);
		return NULL;
	}
	if (path == NULL || !g_path_is_absolute(path))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "ACP file paths must be absolute");
		return NULL;
	}

	lexical = g_canonicalize_filename(path, NULL);
	existing = g_strdup(lexical);
	suffix = g_strdup("");
	while (lstat(existing, &st) != 0)
	{
		gint saved = errno;
		g_autofree gchar *base = g_path_get_basename(existing);
		g_autofree gchar *parent = g_path_get_dirname(existing);
		gchar *joined;

		if (must_exist || saved != ENOENT || g_str_equal(parent, existing))
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "%s: %s", path, g_strerror(saved));
			return NULL;
		}
		joined = *suffix != '\0' ? g_build_filename(base, suffix, NULL) : g_strdup(base);
		g_free(suffix);
		suffix = joined;
		g_free(existing);
		existing = g_steal_pointer(&parent);
	}

	real = realpath(existing, NULL);
	if (real == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "%s: %s", path, g_strerror(errno));
		return NULL;
	}
	full = *suffix != '\0' ? g_build_filename(real, suffix, NULL) : g_strdup(real);

	if (!g_str_equal(full, run->root) &&
	    !(g_str_has_prefix(full, run->root) &&
	      (g_str_equal(run->root, "/") || full[strlen(run->root)] == G_DIR_SEPARATOR)))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_PERMISSION_DENIED,
		            "%s is outside the session directory %s", path, run->cwd);
		return NULL;
	}
	return (gchar *)g_steal_pointer(&full);
}

/* fs/read_text_file {path, line?, limit?} -> {content}; line is 1-based. */
static JsonNode *
acp_read_file(AcpRun *run, JsonObject *params, GError **error)
{
	g_autofree gchar *path = acp_confine(run, ai_json_get_string(params, "path", NULL),
	                                     TRUE, error);
	g_autofree gchar *contents = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) slice = NULL;
	gint64 line = ai_json_get_int(params, "line", 1);
	gint64 limit = ai_json_get_int(params, "limit", -1);
	GStatBuf st;
	gsize length = 0;
	const gchar *p;
	gint64 at = 1;
	gint64 taken = 0;

	if (path == NULL)
		return NULL;
	if (g_stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > ACP_FILE_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "%s is not a regular file of at most %d bytes", path, ACP_FILE_MAX);
		return NULL;
	}
	if (!g_file_get_contents(path, &contents, &length, error))
		return NULL;
	if (length > ACP_FILE_MAX || !g_utf8_validate(contents, (gssize)length, NULL) ||
	    strlen(contents) != length)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "%s is not a text file", path);
		return NULL;
	}

	/* Lines [line, line + limit), counted from 1, newlines kept. */
	slice = g_string_new(NULL);
	if (line < 1)
		line = 1;
	for (p = contents; *p != '\0' && (limit < 0 || taken < limit); at++)
	{
		const gchar *end = strchr(p, '\n');
		gsize span = end != NULL ? (gsize)(end - p) + 1 : strlen(p);

		if (at >= line)
		{
			g_string_append_len(slice, p, (gssize)span);
			taken++;
		}
		p += span;
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "content");
	json_builder_add_string_value(builder, slice->str);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* fs/write_text_file {path, content} -> {}; creates missing directories. */
static JsonNode *
acp_write_file(AcpRun *run, JsonObject *params, GError **error)
{
	const gchar *content = ai_json_get_string(params, "content", NULL);
	g_autofree gchar *path = NULL;
	g_autofree gchar *parent = NULL;
	JsonNode *empty;

	if (content == NULL || strlen(content) > ACP_FILE_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "fs/write_text_file needs text content of at most %d bytes", ACP_FILE_MAX);
		return NULL;
	}
	path = acp_confine(run, ai_json_get_string(params, "path", NULL), FALSE, error);
	if (path == NULL)
		return NULL;
	parent = g_path_get_dirname(path);
	if (g_mkdir_with_parents(parent, 0755) != 0)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "%s: %s", parent, g_strerror(errno));
		return NULL;
	}
	if (!g_file_set_contents(path, content, -1, error))
		return NULL;

	empty = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(empty, json_object_new());
	return empty;
}

/*
 * acp_permission:
 *
 * session/request_permission, answered at once and the way ai-glib's
 * other CLI wrappers decide without asking anybody: skip-permissions
 * picks the agent's allow option (allow_once before allow_always),
 * otherwise its reject option (reject_once before reject_always).  The
 * choice is made by `kind`, never by an option's position or name
 * (tool-calls: an agent must not read an unrecognised outcome as
 * approval, and option order is not a contract).  With no option of the
 * wanted kind, or once the turn is being cancelled (prompt-turn requires
 * it then), the answer is the "cancelled" outcome.
 *
 * Returns: (transfer full): the result object
 */
static JsonNode *
acp_permission(AcpRun *run, JsonObject *params, GPtrArray *events)
{
	JsonArray *options = ai_json_get_array(params, "options");
	JsonObject *call = ai_json_get_object(params, "toolCall");
	const gchar *what = ai_json_get_string(call, "title", NULL);
	const gchar *want = run->skip_permissions ? "allow" : "reject";
	g_autofree gchar *once = g_strconcat(want, "_once", NULL);
	g_autofree gchar *always = g_strconcat(want, "_always", NULL);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *notice = NULL;
	const gchar *chosen = NULL;
	const gchar *chosen_kind = NULL;
	guint pass;
	guint i;

	if (what == NULL)
		what = ai_json_get_string(call, "toolCallId", "a tool call");

	for (pass = 0; pass < 2 && chosen == NULL && !run->cancel_sent; pass++)
	{
		for (i = 0; options != NULL && i < json_array_get_length(options); i++)
		{
			JsonObject *option = ai_json_array_get_object(options, i);
			const gchar *kind = ai_json_get_string(option, "kind", NULL);
			const gchar *id = ai_json_get_string(option, "optionId", NULL);

			if (id != NULL && g_strcmp0(kind, pass == 0 ? once : always) == 0)
			{
				chosen = id;
				chosen_kind = kind;
				break;
			}
		}
	}

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "outcome");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "outcome");
	json_builder_add_string_value(builder, chosen != NULL ? "selected" : "cancelled");
	if (chosen != NULL)
	{
		json_builder_set_member_name(builder, "optionId");
		json_builder_add_string_value(builder, chosen);
	}
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	notice = g_strdup_printf("ACP agent asked permission for %s: %s", what,
	                         chosen_kind != NULL ? chosen_kind
	                         : run->cancel_sent ? "cancelled (turn cancelled)"
	                         : "cancelled (no matching option)");
	g_ptr_array_add(events, ai_event_new_status(notice));
	return json_builder_get_root(builder);
}

/*
 * acp_answer_request:
 *
 * Every request the agent makes is answered, or it waits for ever.
 * Permission requests and the fs/ methods this run advertised are
 * served; the terminal/ methods (never advertised) and anything else get a JSON-RPC
 * "method not found".  A failed file request answers "invalid params"
 * with the reason, which is the agent's to report.
 */
static void
acp_answer_request(AcpRun *run, JsonObject *obj, GPtrArray *events)
{
	JsonNode *id = ai_json_get_node(obj, "id");
	const gchar *method = ai_json_get_string(obj, "method", "");
	JsonObject *params = ai_json_get_object(obj, "params");
	const gchar *session = ai_json_get_string(params, "sessionId", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	JsonNode *result = NULL;
	gboolean served;

	served = g_str_equal(method, "session/request_permission") ||
	         (g_str_equal(method, "fs/read_text_file") && run->client_fs) ||
	         (g_str_equal(method, "fs/write_text_file") && run->fs_write);
	if (!served)
	{
		g_autofree gchar *why = g_strdup_printf("ai-glib does not serve %s", method);

		line = acp_error_line(id, ACP_ERROR_METHOD_NOT_FOUND, why);
	}
	else if (session != NULL && g_strcmp0(session, run->session_id) != 0)
	{
		line = acp_error_line(id, ACP_ERROR_INVALID_PARAMS, "unknown sessionId");
	}
	else
	{
		if (g_str_equal(method, "session/request_permission"))
			result = acp_permission(run, params, events);
		else if (g_str_equal(method, "fs/read_text_file"))
			result = acp_read_file(run, params, &error);
		else
			result = acp_write_file(run, params, &error);
		line = result != NULL ? acp_result_line(id, result)
		                      : acp_error_line(id, ACP_ERROR_INVALID_PARAMS,
		                                       error != NULL ? error->message : method);
	}
	ai_cli_steer_channel_write(run->channel, line);
}

/* ================================================================
 * The parser
 * ================================================================ */

/* A response to one of our requests. */
static gboolean
acp_handle_response(AiAcpClient *self, AcpRun *run, JsonObject *obj,
                    AiResponse *response, GPtrArray *events, GError **error)
{
	gint64 id = ai_json_get_int(obj, "id", -1);
	JsonObject *failure = ai_json_get_object(obj, "error");

	switch (id)
	{
		case ACP_RPC_INITIALIZE:
			if (failure != NULL)
			{
				acp_failure(failure, "initialize", error);
				return FALSE;
			}
			return acp_initialized(run, ai_json_get_object(obj, "result"), events, error);

		case ACP_RPC_SESSION:
		case ACP_RPC_SESSION_NEW:
			return acp_session_opened(self, run, id, obj, response, events, error);

		case ACP_RPC_PROMPT:
			acp_prompt_answered(run, obj, response, events);
			return TRUE;

		default:
			/* Nothing else was asked. */
			return TRUE;
	}
}

/*
 * parse_events:
 * @client: the ACP client
 * @line: one line the agent wrote
 * @response: the response being built
 * @events: (element-type AiEvent): where events go
 * @error: return location for the run's failure
 *
 * Lines are told apart the JSON-RPC way: method and id is a request to
 * answer, method alone a notification, id alone a response to one of
 * ours.  Untrusted input: every member through ai-json-util.h, and a line
 * that is not JSON fails the run (an ACP agent writes nothing else on
 * stdout; its logs go to stderr).
 *
 * Returns: %FALSE when the run has failed
 */
static gboolean
parse_events(AiCliClient *client, const gchar *line, AiResponse *response,
             GPtrArray *events, GError **error)
{
	AiAcpClient *self = AI_ACP_CLIENT(client);
	g_autoptr(JsonParser) parser = json_parser_new();
	AiCliSteerChannel *channel = ai_cli_steer_current();
	JsonObject *obj;
	AcpRun *run;
	gboolean has_id;
	const gchar *method;

	while (line != NULL && g_ascii_isspace(*line))
		line++;
	if (line == NULL || *line == '\0')
		return TRUE;

	/* Without the open stdin there is no way to answer: say so plainly. */
	if (channel == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		                    "an ACP agent needs a streamed run with stdin open");
		return FALSE;
	}
	run = ai_cli_steer_channel_get_data(channel);
	if (run == NULL)
	{
		run = g_steal_pointer(&self->pending_run);
		if (run == NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
			                    "ACP run has no plan");
			return FALSE;
		}
		run->channel = channel;
		run->response = g_object_ref(response);
		/* pending_run's reference becomes the channel's. */
		ai_cli_steer_channel_set_data(channel, run, acp_run_detach);
	}

	if (!json_parser_load_from_data(parser, line, -1, NULL) ||
	    (obj = ai_json_root_object(parser)) == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
		            "Malformed ACP message: %.200s", line);
		return FALSE;
	}
	has_id = ai_json_get_node(obj, "id") != NULL;
	method = ai_json_get_string(obj, "method", NULL);

	if (method != NULL && has_id)
	{
		acp_answer_request(run, obj, events);
		return TRUE;
	}
	if (method != NULL)
	{
		if (g_str_equal(method, "session/update"))
			acp_session_update(run, ai_json_get_object(obj, "params"), response, events);
		return TRUE;
	}
	if (has_id)
		return acp_handle_response(self, run, obj, response, events, error);

	g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
	                    "Malformed ACP message: neither a request, a notification nor a response");
	return FALSE;
}

/* The text-only projection of parse_events, for parse_stream_line callers. */
static gboolean
parse_line(AiCliClient *client, const gchar *line, AiResponse *response,
           gchar **delta, GError **error)
{
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);

	*delta = NULL;
	if (!parse_events(client, line, response, events, error))
		return FALSE;
	*delta = ai_cli_client_events_to_delta(events);
	return TRUE;
}

/* There is no one-shot output to parse: ACP is a conversation. */
static AiResponse *
parse_output(AiCliClient *client, const gchar *json, GError **error)
{
	(void)client; (void)json;
	g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
	                    "an ACP agent cannot be read from buffered output");
	return NULL;
}

/* ================================================================
 * Cancellation
 * ================================================================ */

/*
 * acp_run_cancel:
 *
 * On the run's context: the caller cancelled.  A prompt in flight gets
 * session/cancel and shutdown-timeout-ms to answer "cancelled" (the agent
 * must, prompt-turn) before it is killed; a run that has not yet sent its
 * prompt is killed at once, since there is no turn to cancel; one whose
 * prompt is answered is already on its way out.
 */
static gboolean
acp_run_cancel(gpointer user_data)
{
	AcpRun *run = user_data;
	g_autoptr(JsonNode) params = NULL;
	g_autofree gchar *line = NULL;

	if (run->done || run->user_cancelled)
		return G_SOURCE_REMOVE;
	run->user_cancelled = TRUE;
	if (run->prompt_done)
		return G_SOURCE_REMOVE;
	if (run->channel == NULL || run->session_id == NULL || !run->prompt_sent)
	{
		acp_run_arm(run, 0);
		return G_SOURCE_REMOVE;
	}

	params = acp_session_params(run->session_id);
	line = acp_request_line(-1, "session/cancel", params);
	ai_cli_steer_channel_write(run->channel, line);
	run->cancel_sent = TRUE;
	acp_run_arm(run, run->shutdown_timeout_ms);
	return G_SOURCE_REMOVE;
}

/*
 * The caller's cancellable fired, on whatever thread cancelled it.  The
 * work is deferred to the run's context through an idle: never run
 * inline, where it would hold the cancellable's lock while writing.
 */
static void
on_user_cancelled(GCancellable *cancellable, gpointer user_data)
{
	AcpRun *run = user_data;
	GSource *idle = g_idle_source_new();

	(void)cancellable;
	g_source_set_callback(idle, acp_run_cancel, acp_run_ref(run), acp_run_unref);
	g_source_attach(idle, run->context);
	g_source_unref(idle);
}

/* ================================================================
 * Running a turn
 * ================================================================ */

static void
acp_set_pending(AiAcpClient *self, AcpRun *run)
{
	g_clear_pointer(&self->pending_run, acp_run_unref);
	self->pending_run = run != NULL ? acp_run_ref(run) : NULL;
}

/*
 * on_stream_done:
 *
 * The stream run is over.  A turn whose prompt was answered has its
 * response, whatever the process did next (killed after the grace, say);
 * one that was not answered failed with the stream's error, or with ours
 * when the agent simply exited.  The caller's cancellation wins over
 * everything (GTask's own check).
 */
static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GTask) task = user_data;
	AiAcpClient *self = AI_ACP_CLIENT(source);
	AcpRun *run = g_task_get_task_data(task);
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;

	response = ai_cli_client_stream_run_finish(AI_CLI_CLIENT(self), result, &error);
	run->done = TRUE;
	acp_run_disarm(run);
	if (run->user != NULL && run->cancel_id != 0)
	{
		g_cancellable_disconnect(run->user, run->cancel_id);
		run->cancel_id = 0;
	}
	if (self->pending_run == run)
		acp_set_pending(self, NULL);

	if (g_task_return_error_if_cancelled(task))
		return;
	if (run->prompt_done)
	{
		if (run->outcome != NULL)
		{
			GError *outcome = g_error_copy(run->outcome);

			ai_cli_limit_reclassify(AI_CLI_CLIENT(self), &outcome);
			g_task_return_error(task, outcome);
			return;
		}
		g_task_return_pointer(task, g_object_ref(run->response), g_object_unref);
		return;
	}
	if (error != NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	g_task_return_new_error(task, AI_ERROR, AI_ERROR_CLI_EXECUTION,
	                        "the ACP agent exited before answering session/prompt");
}

/*
 * acp_call_async:
 *
 * One turn: plans a run, connects the caller's cancellable to it (so a
 * cancel becomes session/cancel rather than an immediate kill) and hands
 * the stream run a cancellable of the run's own, which only the run
 * cancels.
 */
static void
acp_call_async(AiAcpClient *self, GList *messages, const gchar *system, gint max_tokens,
               GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task = g_task_new(self, cancellable, callback, user_data);
	AcpRun *run;

	g_task_set_source_tag(task, acp_call_async);
	if (g_task_return_error_if_cancelled(task))
	{
		g_object_unref(task);
		return;
	}

	run = acp_run_new();
	run->kill = g_cancellable_new();
	g_task_set_task_data(task, acp_run_ref(run), acp_run_unref);
	acp_set_pending(self, run);
	if (cancellable != NULL)
	{
		run->user = g_object_ref(cancellable);
		run->cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(on_user_cancelled),
		                                       acp_run_ref(run), acp_run_unref);
	}
	ai_cli_client_stream_run_async(AI_CLI_CLIENT(self), messages, system, max_tokens,
	                               run->kill, on_stream_done, task);
	acp_run_unref(run);
}

static AiResponse *
acp_call_finish(AiAcpClient *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

typedef struct
{
	gboolean    done;
	AiResponse *response;
	GError     *error;
} AcpSyncRun;

static void
on_acp_sync_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	AcpSyncRun *sync = user_data;

	sync->response = acp_call_finish(AI_ACP_CLIENT(source), result, &sync->error);
	sync->done = TRUE;
}

/*
 * chat_sync:
 *
 * ACP cannot write its whole input up front -- session/prompt needs the
 * session id the agent answers with -- so a synchronous call runs the
 * streamed protocol on a private main context and waits for it, as
 * codex's app-server transport does.
 */
static AiResponse *
chat_sync(AiCliClient *client, GList *messages, GCancellable *cancellable, GError **error)
{
	GMainContext *context = g_main_context_new();
	AcpSyncRun sync = { 0 };

	g_main_context_push_thread_default(context);
	acp_call_async(AI_ACP_CLIENT(client), messages, ai_cli_client_get_system_prompt(client),
	               ai_cli_client_get_max_tokens(client), cancellable, on_acp_sync_done, &sync);
	while (!sync.done)
		g_main_context_iteration(context, TRUE);
	g_main_context_pop_thread_default(context);
	g_main_context_unref(context);

	if (sync.response == NULL)
	{
		g_propagate_error(error, sync.error);
		return NULL;
	}
	return sync.response;
}

/* ================================================================
 * MCP servers
 * ================================================================ */

/* Sorted names, so the same map always renders the same bytes. */
static gint
acp_name_compare(gconstpointer a, gconstpointer b)
{
	return g_strcmp0((const gchar *)a, (const gchar *)b);
}

/* A {name, value} list from a JSON object of strings (env, headers). */
static void
acp_add_pairs(JsonBuilder *builder, const gchar *member, JsonObject *pairs)
{
	GList *names = pairs != NULL ? json_object_get_members(pairs) : NULL;
	GList *l;

	names = g_list_sort(names, acp_name_compare);
	json_builder_set_member_name(builder, member);
	json_builder_begin_array(builder);
	for (l = names; l != NULL; l = l->next)
	{
		const gchar *value = ai_json_get_string(pairs, l->data, NULL);

		if (value == NULL)
			continue;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, l->data);
		json_builder_set_member_name(builder, "value");
		json_builder_add_string_value(builder, value);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	g_list_free(names);
}

/*
 * acp_translate_server:
 * @name: the server's name
 * @server: one entry of a Claude-schema `mcpServers` map
 * @skipped: where a reason goes when the entry cannot be carried
 *
 * One McpServer in ACP's shape (session-setup): stdio as {name, command,
 * args, env: [{name, value}]}, with command made absolute because ACP
 * requires it; a URL as {type: "http"|"sse", name, url, headers}.
 *
 * Returns: (transfer full) (nullable): the entry, or %NULL when skipped
 */
static JsonNode *
acp_translate_server(const gchar *name, JsonObject *server, GPtrArray *skipped)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	const gchar *command = ai_json_get_string(server, "command", NULL);
	const gchar *url = ai_json_get_string(server, "url", NULL);
	const gchar *type = ai_json_get_string(server, "type", NULL);
	JsonArray *args = ai_json_get_array(server, "args");
	guint i;

	json_builder_begin_object(builder);
	if (command != NULL && *command != '\0')
	{
		g_autofree gchar *absolute = g_path_is_absolute(command)
		                             ? g_strdup(command) : g_find_program_in_path(command);

		if (absolute == NULL)
		{
			g_ptr_array_add(skipped, g_strdup_printf(
				"%s left out: %s is not on PATH and ACP needs an absolute command",
				name, command));
			json_builder_end_object(builder);
			return NULL;
		}
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, name);
		json_builder_set_member_name(builder, "command");
		json_builder_add_string_value(builder, absolute);
		json_builder_set_member_name(builder, "args");
		json_builder_begin_array(builder);
		for (i = 0; args != NULL && i < json_array_get_length(args); i++)
		{
			const gchar *word = ai_json_array_get_string(args, i, NULL);

			if (word != NULL)
				json_builder_add_string_value(builder, word);
		}
		json_builder_end_array(builder);
		acp_add_pairs(builder, "env", ai_json_get_object(server, "env"));
	}
	else if (url != NULL && *url != '\0')
	{
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, g_strcmp0(type, "sse") == 0 ? "sse" : "http");
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, name);
		json_builder_set_member_name(builder, "url");
		json_builder_add_string_value(builder, url);
		acp_add_pairs(builder, "headers", ai_json_get_object(server, "headers"));
	}
	else
	{
		g_ptr_array_add(skipped, g_strdup_printf("%s left out: names neither a command nor a url",
		                                         name));
		json_builder_end_object(builder);
		return NULL;
	}
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/*
 * endpoint_applied:
 * @client: the ACP client
 * @endpoint: (nullable): the granted endpoint, or %NULL on revocation
 * @error: return location
 *
 * An MCP endpoint is a Claude-schema `.mcp.json` file
 * (%AI_ENDPOINT_KIND_MCP_CONFIG, what ai_mcp_config_endpoint_for()
 * writes); its servers go to the agent in session/new's mcpServers.  The
 * file is read now and kept as a snapshot, so replacing or deleting it
 * cannot change a grant already applied.  Revocation clears it.
 *
 * Returns: whether the grant was accepted
 */
static gboolean
endpoint_applied(AiCliClient *client, const AiAgentEndpoint *endpoint, GError **error)
{
	AiAcpClient *self = AI_ACP_CLIENT(client);
	const gchar *paths[2] = { NULL, NULL };
	g_autoptr(JsonObject) servers = NULL;
	g_autoptr(GPtrArray) skipped = NULL;
	JsonArray *array;
	GList *names;
	GList *l;
	GStatBuf st;

	/* A failed replacement must not leave a revoked grant in force. */
	g_clear_pointer(&self->mcp_servers, json_node_unref);
	g_clear_pointer(&self->mcp_skipped, g_ptr_array_unref);
	if (endpoint == NULL || g_strcmp0(endpoint->kind, AI_ENDPOINT_KIND_MCP_CONFIG) != 0)
		return TRUE;

	if (endpoint->value == NULL || g_stat(endpoint->value, &st) != 0 ||
	    !S_ISREG(st.st_mode) || st.st_size > ACP_MCP_CONFIG_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "ACP MCP configuration must be a regular file of at most %d bytes: %s",
		            ACP_MCP_CONFIG_MAX, endpoint->value != NULL ? endpoint->value : "(none)");
		return FALSE;
	}
	paths[0] = endpoint->value;
	servers = ai_mcp_config_load_servers(paths, error);
	if (servers == NULL)
		return FALSE;

	skipped = g_ptr_array_new_with_free_func(g_free);
	array = json_array_new();
	names = g_list_sort(json_object_get_members(servers), acp_name_compare);
	for (l = names; l != NULL; l = l->next)
	{
		JsonObject *server = ai_json_get_object(servers, l->data);
		JsonNode *entry;

		if (server == NULL)
		{
			g_ptr_array_add(skipped, g_strdup_printf("%s left out: not a JSON object",
			                                         (gchar *)l->data));
			continue;
		}
		entry = acp_translate_server(l->data, server, skipped);
		if (entry != NULL)
			json_array_add_element(array, entry);
	}
	g_list_free(names);

	self->mcp_servers = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(self->mcp_servers, array);
	self->mcp_skipped = g_steal_pointer(&skipped);
	return TRUE;
}

static const gchar * const acp_endpoint_kinds[] = {
	AI_ENDPOINT_KIND_ENV,
	AI_ENDPOINT_KIND_MCP_CONFIG,
	NULL
};

/* ================================================================
 * GObject
 * ================================================================ */

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiAcpClient *self = AI_ACP_CLIENT(object);

	switch (id)
	{
		case PROP_COMMAND:
			g_value_set_string(value, self->command);
			break;
		case PROP_SKIP_PERMISSIONS:
			g_value_set_boolean(value, self->skip_permissions);
			break;
		case PROP_CLIENT_FS:
			g_value_set_boolean(value, self->client_fs);
			break;
		case PROP_SHUTDOWN_TIMEOUT_MS:
			g_value_set_uint(value, self->shutdown_timeout_ms);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiAcpClient *self = AI_ACP_CLIENT(object);

	switch (id)
	{
		case PROP_COMMAND:
			g_free(self->command);
			self->command = g_value_dup_string(value);
			break;
		case PROP_SKIP_PERMISSIONS:
			self->skip_permissions = g_value_get_boolean(value);
			break;
		case PROP_CLIENT_FS:
			self->client_fs = g_value_get_boolean(value);
			break;
		case PROP_SHUTDOWN_TIMEOUT_MS:
			self->shutdown_timeout_ms = g_value_get_uint(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}

static void
finalize(GObject *object)
{
	AiAcpClient *self = AI_ACP_CLIENT(object);

	g_free(self->command);
	g_free(self->turn_system_prompt);
	g_clear_pointer(&self->mcp_servers, json_node_unref);
	g_clear_pointer(&self->mcp_skipped, g_ptr_array_unref);
	g_clear_pointer(&self->pending_run, acp_run_unref);
	G_OBJECT_CLASS(ai_acp_client_parent_class)->finalize(object);
}

static void
ai_acp_client_class_init(AiAcpClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	AiCliClientClass *cli = AI_CLI_CLIENT_CLASS(klass);

	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;

	cli->get_executable_path = get_executable;
	cli->build_argv = build_argv;
	cli->build_stdin = build_stdin;
	cli->parse_json_output = parse_output;
	cli->parse_stream_line = parse_line;
	cli->parse_stream_events = parse_events;
	cli->chat_sync = chat_sync;
	cli->endpoint_applied = endpoint_applied;
	cli->endpoint_kinds = acp_endpoint_kinds;
	/* An agent that exits non-zero before answering says why on stderr. */
	cli->check_exit_status = TRUE;
	/* Unknown per agent; nothing in ACP v1 says whether it searches tools. */
	cli->searches_tools = FALSE;
	/* Where an agent keeps its login is the agent's business. */
	cli->config_dir_env = NULL;
	ai_cli_limit_register(G_TYPE_FROM_CLASS(klass), ACP_CLI_NAME, acp_limit_patterns);

	/**
	 * AiAcpClient:command:
	 *
	 * The agent's command line, split the way a shell would split it but
	 * never run through one ("npx -y some-agent --acp" works; `$VAR`
	 * and `|` do not).  When unset, `$AI_ACP_COMMAND` is used; with
	 * neither, every turn fails before anything is started.
	 */
	properties[PROP_COMMAND] = g_param_spec_string("command", "Command",
		"The ACP agent's command line (shell-style quoting, no shell)",
		NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAcpClient:skip-permissions:
	 *
	 * How session/request_permission is answered: %TRUE picks the
	 * agent's allow option, %FALSE (the default) its reject option.  It
	 * also decides whether fs/write_text_file is offered.
	 */
	properties[PROP_SKIP_PERMISSIONS] = g_param_spec_boolean("skip-permissions",
		"Skip Permissions",
		"Allow what the agent asks permission for (and offer file writes)",
		FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAcpClient:client-fs:
	 *
	 * Whether the client offers ACP's fs/read_text_file (and, under
	 * #AiAcpClient:skip-permissions, fs/write_text_file).  Paths are
	 * confined to the working directory, symlinks included; files are
	 * at most 8 MiB of UTF-8.
	 */
	properties[PROP_CLIENT_FS] = g_param_spec_boolean("client-fs", "Client File System",
		"Serve the agent's fs/ requests inside the working directory",
		TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * AiAcpClient:shutdown-timeout-ms:
	 *
	 * How long the agent has to exit once its answer is in (stdin is
	 * closed), and to answer a session/cancel, before it is killed.
	 * 0 kills it at once.
	 */
	properties[PROP_SHUTDOWN_TIMEOUT_MS] = g_param_spec_uint("shutdown-timeout-ms",
		"Shutdown Timeout",
		"Grace for the agent to exit or honour session/cancel, in milliseconds",
		0, G_MAXUINT, ACP_SHUTDOWN_TIMEOUT_MS_DEFAULT,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_acp_client_init(AiAcpClient *self)
{
	ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_ACP_MODEL_DEFAULT);
	self->client_fs = TRUE;
	self->shutdown_timeout_ms = ACP_SHUTDOWN_TIMEOUT_MS_DEFAULT;
	/* The protocol answers requests mid-turn: stdin stays open. */
	ai_cli_steer_set_protocol_input(AI_CLI_CLIENT(self), TRUE);
}

/* ================================================================
 * AiProvider / AiStreamable
 * ================================================================ */

static AiProviderType
get_provider_type(AiProvider *provider)
{
	(void)provider;
	return AI_PROVIDER_ACP;
}

static const gchar *
get_name(AiProvider *provider)
{
	(void)provider;
	return "ACP agent";
}

static const gchar *
get_default_model(AiProvider *provider)
{
	(void)provider;
	return AI_ACP_MODEL_DEFAULT;
}

static void
chat_async(AiProvider *provider, GList *messages, const gchar *system, gint max_tokens,
           GList *tools, GCancellable *cancellable, GAsyncReadyCallback callback,
           gpointer user_data)
{
	(void)tools;
	acp_call_async(AI_ACP_CLIENT(provider), messages, system, max_tokens, cancellable,
	               callback, user_data);
}

static AiResponse *
chat_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{
	return acp_call_finish(AI_ACP_CLIENT(provider), result, error);
}

static void
stream_async(AiStreamable *streamable, GList *messages, const gchar *system, gint max_tokens,
             GList *tools, GCancellable *cancellable, GAsyncReadyCallback callback,
             gpointer user_data)
{
	(void)tools;
	acp_call_async(AI_ACP_CLIENT(streamable), messages, system, max_tokens, cancellable,
	               callback, user_data);
}

static AiResponse *
stream_finish(AiStreamable *streamable, GAsyncResult *result, GError **error)
{
	return acp_call_finish(AI_ACP_CLIENT(streamable), result, error);
}

static void
free_models(gpointer data)
{
	g_list_free_full(data, g_free);
}

static void
list_models_async(AiProvider *provider, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(provider, cancellable, callback, user_data);

	g_task_return_pointer(task, g_list_append(NULL, g_strdup(AI_ACP_MODEL_DEFAULT)),
	                      free_models);
}

static GList *
list_models_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{
	(void)provider;
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
provider_init(AiProviderInterface *iface)
{
	iface->get_provider_type = get_provider_type;
	iface->get_name = get_name;
	iface->get_default_model = get_default_model;
	iface->chat_async = chat_async;
	iface->chat_finish = chat_finish;
	iface->list_models_async = list_models_async;
	iface->list_models_finish = list_models_finish;
}

static void
streamable_init(AiStreamableInterface *iface)
{
	iface->chat_stream_async = stream_async;
	iface->chat_stream_finish = stream_finish;
}

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * ai_acp_client_new:
 *
 * Creates a client for an Agent Client Protocol agent.  Set its command
 * with ai_acp_client_set_command() or ai_acp_client_set_argv() (or
 * `$AI_ACP_COMMAND`) before the first turn.
 *
 * Returns: (transfer full): a new #AiAcpClient
 */
AiAcpClient *
ai_acp_client_new(void)
{
	return g_object_new(AI_TYPE_ACP_CLIENT, NULL);
}

/**
 * ai_acp_client_new_with_config:
 * @config: an #AiConfig
 *
 * Returns: (transfer full): a new #AiAcpClient
 */
AiAcpClient *
ai_acp_client_new_with_config(AiConfig *config)
{
	return g_object_new(AI_TYPE_ACP_CLIENT, "config", config, NULL);
}

/**
 * ai_acp_client_new_for_command:
 * @argv: (array zero-terminated=1): the agent's argv, e.g.
 *   `{ "gemini", "--experimental-acp", NULL }`
 *
 * Returns: (transfer full): a new #AiAcpClient running @argv
 */
AiAcpClient *
ai_acp_client_new_for_command(const gchar * const *argv)
{
	AiAcpClient *self = ai_acp_client_new();

	ai_acp_client_set_argv(self, argv);
	return self;
}

/**
 * ai_acp_client_get_command:
 * @self: an #AiAcpClient
 *
 * Returns: (transfer none) (nullable): the command line, as set
 */
const gchar *
ai_acp_client_get_command(AiAcpClient *self)
{
	g_return_val_if_fail(AI_IS_ACP_CLIENT(self), NULL);
	return self->command;
}

/**
 * ai_acp_client_set_command:
 * @self: an #AiAcpClient
 * @command: (nullable): the agent's command line; see #AiAcpClient:command
 */
void
ai_acp_client_set_command(AiAcpClient *self, const gchar *command)
{
	g_return_if_fail(AI_IS_ACP_CLIENT(self));
	g_object_set(self, "command", command, NULL);
}

/**
 * ai_acp_client_dup_argv:
 * @self: an #AiAcpClient
 * @error: return location for a #GError
 *
 * The argv the next turn will start: #AiAcpClient:command (or
 * `$AI_ACP_COMMAND`) split.  argv[0] is not yet resolved on PATH.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the
 *   argv, or %NULL with @error set when there is no usable command
 */
gchar **
ai_acp_client_dup_argv(AiAcpClient *self, GError **error)
{
	g_return_val_if_fail(AI_IS_ACP_CLIENT(self), NULL);
	return acp_parse_command(self, error);
}

/**
 * ai_acp_client_set_argv:
 * @self: an #AiAcpClient
 * @argv: (array zero-terminated=1) (nullable): the agent's argv
 *
 * Sets #AiAcpClient:command to @argv, each word quoted so that it splits
 * back into exactly these words.
 */
void
ai_acp_client_set_argv(AiAcpClient *self, const gchar * const *argv)
{
	g_autoptr(GString) line = NULL;
	guint i;

	g_return_if_fail(AI_IS_ACP_CLIENT(self));

	if (argv == NULL || argv[0] == NULL)
	{
		ai_acp_client_set_command(self, NULL);
		return;
	}
	line = g_string_new(NULL);
	for (i = 0; argv[i] != NULL; i++)
	{
		g_autofree gchar *quoted = g_shell_quote(argv[i]);

		if (i > 0)
			g_string_append_c(line, ' ');
		g_string_append(line, quoted);
	}
	ai_acp_client_set_command(self, line->str);
}

/**
 * ai_acp_client_get_skip_permissions:
 * @self: an #AiAcpClient
 *
 * Returns: whether permission requests are allowed
 */
gboolean
ai_acp_client_get_skip_permissions(AiAcpClient *self)
{
	g_return_val_if_fail(AI_IS_ACP_CLIENT(self), FALSE);
	return self->skip_permissions;
}

/**
 * ai_acp_client_set_skip_permissions:
 * @self: an #AiAcpClient
 * @skip: allow (%TRUE) or reject (%FALSE) what the agent asks for
 */
void
ai_acp_client_set_skip_permissions(AiAcpClient *self, gboolean skip)
{
	g_return_if_fail(AI_IS_ACP_CLIENT(self));
	g_object_set(self, "skip-permissions", skip, NULL);
}

/**
 * ai_acp_client_get_client_fs:
 * @self: an #AiAcpClient
 *
 * Returns: whether the client serves fs/ requests
 */
gboolean
ai_acp_client_get_client_fs(AiAcpClient *self)
{
	g_return_val_if_fail(AI_IS_ACP_CLIENT(self), FALSE);
	return self->client_fs;
}

/**
 * ai_acp_client_set_client_fs:
 * @self: an #AiAcpClient
 * @client_fs: whether to serve fs/ requests; see #AiAcpClient:client-fs
 */
void
ai_acp_client_set_client_fs(AiAcpClient *self, gboolean client_fs)
{
	g_return_if_fail(AI_IS_ACP_CLIENT(self));
	g_object_set(self, "client-fs", client_fs, NULL);
}

/**
 * ai_acp_client_get_shutdown_timeout_ms:
 * @self: an #AiAcpClient
 *
 * Returns: the shutdown grace, in milliseconds
 */
guint
ai_acp_client_get_shutdown_timeout_ms(AiAcpClient *self)
{
	g_return_val_if_fail(AI_IS_ACP_CLIENT(self), 0);
	return self->shutdown_timeout_ms;
}

/**
 * ai_acp_client_set_shutdown_timeout_ms:
 * @self: an #AiAcpClient
 * @timeout_ms: the grace; see #AiAcpClient:shutdown-timeout-ms
 */
void
ai_acp_client_set_shutdown_timeout_ms(AiAcpClient *self, guint timeout_ms)
{
	g_return_if_fail(AI_IS_ACP_CLIENT(self));
	g_object_set(self, "shutdown-timeout-ms", timeout_ms, NULL);
}
