/*
 * ai-codex-cli-client.c - Codex provider: `codex exec --json` (the default
 * transport) or `codex app-server` JSON-RPC over stdio (opt-in, steerable)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "config.h"
#include <string.h>
#include <math.h>
#include <glib/gstdio.h>
#include "model/ai-image-content.h"
#include "providers/ai-codex-cli-client.h"
#include "core/ai-cli-client-private.h"
#include "core/ai-cli-limit-private.h"
#include "core/ai-cli-steer-private.h"
#include "core/ai-json-util.h"
#include "core/ai-steerable.h"
#include "core/ai-error.h"
#include "core/ai-http-error.h"
#include "core/ai-event.h"
#include "core/ai-subprocess-util.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-result.h"

struct _AiCodexCliClient
{
    AiCliClient parent_instance;
    gchar *sandbox;
    gchar *additional_directories;
    gchar *profile;
    gchar *turn_system_prompt;
    gchar **mcp_overrides;
    gchar *last_error;
	gchar *image_directory;
	GList *image_files;
    gboolean skip_permissions;
    gboolean continue_session;
    gboolean search;

	/* "exec" (default) or "app-server"; anything else fails closed. */
	gchar *transport;
	/* How long a turn/steer may wait for its JSON-RPC answer. */
	guint steer_timeout_ms;
	/* app-server: the plan of the run being started, until its channel
	 * takes it on the first line codex writes (see CodexRpc). */
	gpointer pending_rpc;
};

static void provider_init(AiProviderInterface *iface);
static void streamable_init(AiStreamableInterface *iface);
static void steerable_init(AiSteerableInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiCodexCliClient, ai_codex_cli_client, AI_TYPE_CLI_CLIENT,
    G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, provider_init)
    G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE, streamable_init)
    G_IMPLEMENT_INTERFACE(AI_TYPE_STEERABLE, steerable_init))

enum { PROP_0, PROP_SANDBOX, PROP_SKIP_PERMISSIONS, PROP_CONTINUE_SESSION,
       PROP_ADDITIONAL_DIRECTORIES, PROP_PROFILE, PROP_SEARCH, PROP_TRANSPORT,
       PROP_STEER_TIMEOUT_MS, N_PROPS };
static GParamSpec *properties[N_PROPS];

/* Default wait for app-server's answer to a turn/steer request. */
#define CODEX_STEER_TIMEOUT_MS_DEFAULT (10000)

static void codex_rpc_free(gpointer data);
static gboolean parse_app_server_line(AiCodexCliClient *self, const gchar *line,
                                      AiResponse *response, GPtrArray *events,
                                      GError **error);

/*
 * codex_uses_app_server:
 * @self: a Codex client
 *
 * Whether runs go through `codex app-server` rather than `codex exec`.
 * An unknown transport string is neither; build_argv() refuses it.
 *
 * Returns: %TRUE for the app-server transport
 */
static gboolean
codex_uses_app_server(AiCodexCliClient *self)
{
	return g_strcmp0(self->transport, AI_CODEX_CLI_TRANSPORT_APP_SERVER) == 0;
}

/*
 * codex_set_transport:
 * @self: a Codex client
 * @transport: (nullable): "exec", "app-server", or NULL for "exec"
 *
 * Stores the transport and tells the shared stream reader whether this
 * client's runs keep stdin open.  app-server always needs it open -- the
 * handshake answers arrive after the first write -- and that open pipe
 * is also what turn/steer is written into.
 */
static void
codex_set_transport(AiCodexCliClient *self, const gchar *transport)
{
	g_free(self->transport);
	self->transport = g_strdup(transport != NULL ? transport
	                                             : AI_CODEX_CLI_TRANSPORT_EXEC);
	ai_cli_steer_set_open_input(AI_CLI_CLIENT(self), codex_uses_app_server(self));
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(object);
    switch (id)
    {
        case PROP_SANDBOX: g_value_set_string(value, self->sandbox); break;
        case PROP_ADDITIONAL_DIRECTORIES: g_value_set_string(value, self->additional_directories); break;
        case PROP_PROFILE: g_value_set_string(value, self->profile); break;
        case PROP_SKIP_PERMISSIONS: g_value_set_boolean(value, self->skip_permissions); break;
        case PROP_CONTINUE_SESSION: g_value_set_boolean(value, self->continue_session); break;
        case PROP_SEARCH: g_value_set_boolean(value, self->search); break;
        case PROP_TRANSPORT: g_value_set_string(value, self->transport); break;
        case PROP_STEER_TIMEOUT_MS: g_value_set_uint(value, self->steer_timeout_ms); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    }
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(object);
    switch (id)
    {
        case PROP_SANDBOX:
            g_free(self->sandbox); self->sandbox = g_value_dup_string(value); break;
        case PROP_ADDITIONAL_DIRECTORIES:
            g_free(self->additional_directories); self->additional_directories = g_value_dup_string(value); break;
        case PROP_PROFILE:
            g_free(self->profile); self->profile = g_value_dup_string(value); break;
        case PROP_SKIP_PERMISSIONS: self->skip_permissions = g_value_get_boolean(value); break;
        case PROP_CONTINUE_SESSION: self->continue_session = g_value_get_boolean(value); break;
        case PROP_SEARCH: self->search = g_value_get_boolean(value); break;
        case PROP_TRANSPORT: codex_set_transport(self, g_value_get_string(value)); break;
        case PROP_STEER_TIMEOUT_MS: self->steer_timeout_ms = g_value_get_uint(value); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    }
}

static gchar *
get_executable(AiCliClient *client)
{
    const gchar *path = g_getenv("CODEX_PATH");
    (void)client;
    return g_strdup(path != NULL && path[0] != '\0' ? path : "codex");
}

static void
arg(GPtrArray *args, const gchar *value)
{
    g_ptr_array_add(args, g_strdup(value));
}

/* Codex accepts local paths, not inline bytes. Own private copies until the
 * next invocation or finalization, including failed and cancelled turns. */
static void
clear_image_files(AiCodexCliClient *self)
{
	GList *l;

	for (l = self->image_files; l != NULL; l = l->next)
		g_unlink(l->data);
	g_clear_list(&self->image_files, g_free);
	if (self->image_directory != NULL)
		g_rmdir(self->image_directory);
	g_clear_pointer(&self->image_directory, g_free);
}

/*
 * write_image_files:
 * @self: a Codex client
 * @messages: (element-type AiMessage): the conversation
 *
 * Writes every image of the turn to a private file, in order, and keeps
 * the paths in self->image_files.  exec passes them as --image; the
 * app-server transport as localImage inputs of turn/start.
 *
 * Returns: %FALSE when a file could not be written (none are left)
 */
static gboolean
write_image_files(AiCodexCliClient *self, GList *messages)
{
	GList *l;
	GList *b;
	guint index = 0;

	clear_image_files(self);
	messages = ai_cli_client_messages_for_images(AI_CLI_CLIENT(self), messages);
	if (!ai_cli_messages_have_images(messages)) return TRUE;
	self->image_directory = g_dir_make_tmp("ai-glib-codex-images-XXXXXX", NULL);
	if (self->image_directory == NULL) return FALSE;
	for (l = messages; l != NULL; l = l->next)
	{
		for (b = ai_message_get_content_blocks(l->data); b != NULL; b = b->next)
		{
			AiImage *image;
			GBytes *bytes;
			const gchar *mime;
			const gchar *suffix;
			g_autofree gchar *name = NULL;
			g_autofree gchar *path = NULL;
			gsize size;
			gconstpointer data;

			if (!AI_IS_IMAGE_CONTENT(b->data)) continue;
			image = ai_image_content_get_image(AI_IMAGE_CONTENT(b->data));
			bytes = ai_image_get_bytes(image);
			mime = ai_image_get_mime_type(image);
			suffix = g_strcmp0(mime, "image/jpeg") == 0 ? "jpg" :
			         g_strcmp0(mime, "image/webp") == 0 ? "webp" :
			         g_strcmp0(mime, "image/gif") == 0 ? "gif" : "png";
			name = g_strdup_printf("image-%u.%s", ++index, suffix);
			path = g_build_filename(self->image_directory, name, NULL);
			data = g_bytes_get_data(bytes, &size);
			if (!g_file_set_contents_full(path, data, (gssize)size,
			                             G_FILE_SET_CONTENTS_NONE, 0600, NULL))
			{
				clear_image_files(self);
				return FALSE;
			}
			self->image_files = g_list_append(self->image_files, g_steal_pointer(&path));
		}
	}
	return TRUE;
}

static gboolean
append_image_files(AiCodexCliClient *self, GList *messages, GPtrArray *args)
{
	GList *l;

	if (!write_image_files(self, messages)) return FALSE;
	for (l = self->image_files; l != NULL; l = l->next)
	{
		arg(args, "--image");
		arg(args, l->data);
	}
	return TRUE;
}

/*
 * codex_policy_valid:
 * @self: a Codex client
 *
 * The sandbox/bypass checks both transports make before launching.
 * Invalid policy must fail closed, including when set through GObject.
 *
 * Returns: whether the sandbox and skip-permissions settings are usable
 */
static gboolean
codex_policy_valid(AiCodexCliClient *self)
{
	if (self->sandbox != NULL &&
	    g_strcmp0(self->sandbox, "read-only") != 0 &&
	    g_strcmp0(self->sandbox, "workspace-write") != 0 &&
	    g_strcmp0(self->sandbox, "danger-full-access") != 0)
		return FALSE;
	if (self->skip_permissions && self->sandbox != NULL &&
	    g_strcmp0(self->sandbox, "danger-full-access") != 0)
		return FALSE;
	return TRUE;
}

/* Known values only: the effort becomes TOML (exec) or a protocol enum. */
static gboolean
codex_effort_valid(const gchar *effort)
{
	const gchar *valid[] = { "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", NULL };

	return effort == NULL || *effort == '\0' || g_strv_contains(valid, effort);
}

static gchar **build_app_server_argv(AiCodexCliClient *self, const gchar *system_prompt);

static gchar **
build_argv(AiCliClient *client, GList *messages, const gchar *system_prompt,
           gint max_tokens, gboolean streaming)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(client);
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    const gchar *session = ai_cli_client_get_session_id(client);
    const gchar *effort = ai_cli_client_get_effort_level(client);
    gboolean persist = ai_cli_client_get_session_persistence(client);
    gboolean resume = persist && ((session != NULL && *session) || self->continue_session);
    (void)messages; (void)max_tokens; (void)streaming;

    /* Invalid policy must fail closed, including when set through GObject. */
    if (!codex_policy_valid(self))
        return NULL;
    if (codex_uses_app_server(self))
    {
        if (!write_image_files(self, messages)) return NULL;
        return build_app_server_argv(self, system_prompt);
    }
    if (g_strcmp0(self->transport, AI_CODEX_CLI_TRANSPORT_EXEC) != 0)
        return NULL;

    g_free(self->turn_system_prompt);
    self->turn_system_prompt = g_strdup(system_prompt);
    arg(args, "codex");
    /* Global flags precede exec; exec-only flags precede resume. */
    arg(args, "--ask-for-approval"); arg(args, "never");
    if (self->search) arg(args, "--search");
    if (self->profile != NULL && *self->profile)
    { arg(args, "--profile"); arg(args, self->profile); }
    arg(args, "exec");
    if (self->mcp_overrides != NULL)
    {
        guint i;
        /* Keep all -c overrides at exec scope. Codex's subcommand parser
         * replaces parent -c values when exec has its own effort override. */
        for (i = 0; self->mcp_overrides[i] != NULL; i++)
        { arg(args, "-c"); arg(args, self->mcp_overrides[i]); }
    }
    arg(args, "--json");
    arg(args, "--skip-git-repo-check");
    if (self->skip_permissions)
        arg(args, "--dangerously-bypass-approvals-and-sandbox");
    else
    {
        arg(args, "--sandbox");
        arg(args, self->sandbox != NULL ? self->sandbox : "read-only");
    }
    if (!persist) arg(args, "--ephemeral");
    arg(args, "--model"); arg(args, ai_cli_client_get_model(client));
    if (effort != NULL && *effort)
    {
        /* Known values only: this value is TOML, not a shell argument. */
        if (!codex_effort_valid(effort)) return NULL;
        arg(args, "-c");
        g_ptr_array_add(args, g_strdup_printf("model_reasoning_effort=\"%s\"", effort));
    }
    if (self->additional_directories != NULL)
    {
        g_auto(GStrv) dirs = g_strsplit(self->additional_directories, ",", -1);
        guint i;
        for (i = 0; dirs[i] != NULL; i++)
        {
            g_strstrip(dirs[i]);
            if (*dirs[i]) { arg(args, "--add-dir"); arg(args, dirs[i]); }
        }
    }
    if (resume)
    {
        arg(args, "resume");
        if (session != NULL && *session) arg(args, session);
        else arg(args, "--last");
    }
    if (!append_image_files(self, messages, args)) return NULL;
    arg(args, "-");
    g_ptr_array_add(args, NULL);
    return (gchar **)g_ptr_array_free(g_steal_pointer(&args), FALSE);
}

/*
 * codex_prompt_text:
 * @client: a Codex client
 * @messages: (element-type AiMessage): the conversation
 *
 * The turn's prompt, as both transports send it: the system prompt in a
 * <system> block when this is not a resumed native thread, then the
 * projected conversation.
 *
 * Returns: (transfer full): the prompt
 */
static gchar *
codex_prompt_text(AiCliClient *client, GList *messages)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(client);
    GString *prompt = g_string_new(NULL);
    const gchar *system = self->turn_system_prompt;
    const gchar *session = ai_cli_client_get_session_id(client);
    GList *l;
    if (system == NULL) system = ai_cli_client_get_system_prompt(client);
    if (system != NULL && *system &&
        (self->mcp_overrides != NULL || !ai_cli_client_get_session_persistence(client) ||
         ((session == NULL || !*session) && !self->continue_session)))
        g_string_append_printf(prompt, "<system>\n%s\n</system>\n\n", system);
    messages = ai_cli_client_messages_for_prompt(client, messages);
    for (l = messages; l != NULL; l = l->next)
    {
        g_autofree gchar *text = ai_cli_client_project_multimodal_message(l->data);
        if (text != NULL && *text)
        {
            if (prompt->len > 0) g_string_append(prompt, "\n\n");
            g_string_append(prompt, text);
        }
    }
    return g_string_free(prompt, FALSE);
}

static gchar *build_app_server_stdin(AiCodexCliClient *self, gchar *prompt);

static gchar *
build_stdin(AiCliClient *client, GList *messages)
{
    gchar *prompt = codex_prompt_text(client, messages);

    /* app-server: the first write is initialize; the prompt waits for turn/start. */
    if (codex_uses_app_server(AI_CODEX_CLI_CLIENT(client)))
        return build_app_server_stdin(AI_CODEX_CLI_CLIENT(client), prompt);
    return prompt;
}

/* Clamp before converting floating-point counters to an integer. */
static gint
usage_tokens(JsonObject *usage, const gchar *name)
{
    gdouble count = ai_json_get_double(usage, name, 0);
    if (isnan(count) || count <= 0) return 0;
    if (count >= G_MAXINT) return G_MAXINT;
    return (gint)count;
}

/*
 * Codex exec reports token counts and no USD. That table is the
 * registration: a model with no row stays unpriced (-1), never $0.
 * Rates are dollars per million tokens; one token at $N/Mtok is
 * exactly N micro-dollars, so the product is the cost with no float.
 */
typedef struct
{
    const gchar *model;
    gint64       input_per_mtok;
    gint64       output_per_mtok;
} CodexPrice;

static const CodexPrice CODEX_PRICES[] = {
    { AI_CODEX_CLI_MODEL_GPT_6_ASTRA, 10, 50 },
};

static gint64
codex_cost_micros(const gchar *model, gint input_tokens, gint output_tokens)
{
    guint i;

    if (model == NULL || *model == '\0')
        return -1;
    for (i = 0; i < G_N_ELEMENTS(CODEX_PRICES); i++)
    {
        if (g_ascii_strcasecmp(CODEX_PRICES[i].model, model) != 0)
            continue;
        return (gint64)input_tokens * CODEX_PRICES[i].input_per_mtok
             + (gint64)output_tokens * CODEX_PRICES[i].output_per_mtok;
    }
    return -1;
}

/* Capture a bounded diff once, at event receipt. Never run repository diff
 * drivers, textconv filters, or a shell, and never read an unbounded pipe. */
static gboolean
capture_diff(
	AiCliClient *client,
	const gchar *path,
	gboolean     added,
	gint64       deadline,
	gchar      **patch,
	gboolean    *truncated
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GString) output = g_string_new(NULL);
	GInputStream *stream;
	const gchar *cwd = ai_cli_client_get_working_directory(client);
	gboolean eof = FALSE;

	*patch = NULL;
	*truncated = FALSE;
	if (g_get_monotonic_time() >= deadline) return FALSE;
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                    G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	if (cwd != NULL) g_subprocess_launcher_set_cwd(launcher, cwd);
	g_subprocess_launcher_setenv(launcher, "GIT_OPTIONAL_LOCKS", "0", TRUE);
	process = g_subprocess_launcher_spawn(launcher, NULL,
		"git", "--no-pager", "--literal-pathspecs", "-c", "core.fsmonitor=false",
		"diff", "--no-ext-diff",
		"--no-textconv", "--no-color", "--no-renames", "--unified=3",
		added ? "--no-index" : "HEAD", "--", added ? "/dev/null" : path,
		added ? path : NULL, NULL);
	if (process == NULL) return FALSE;
	stream = g_subprocess_get_stdout_pipe(process);
	if (!G_IS_POLLABLE_INPUT_STREAM(stream) ||
	    !g_pollable_input_stream_can_poll(G_POLLABLE_INPUT_STREAM(stream)))
	{
		g_subprocess_force_exit(process);
		return FALSE;
	}

	/* Poll with a shared event deadline: eight changed files cannot cause
	 * eight independent long stalls. Stop after 64 KiB of patch data. */
	while (output->len < 65536 && g_get_monotonic_time() < deadline)
	{
		gchar buffer[4096];
		g_autoptr(GError) error = NULL;
		gssize size = g_pollable_input_stream_read_nonblocking(
			G_POLLABLE_INPUT_STREAM(stream), buffer,
			MIN(sizeof buffer, 65536 - output->len), NULL, &error);
		if (size > 0) g_string_append_len(output, buffer, size);
		else if (size == 0) { eof = TRUE; break; }
		else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
			g_usleep(1000);
		else break;
	}
	if (!eof)
	{
		g_subprocess_force_exit(process);
		*truncated = TRUE;
	}
	else
	{
		gint remaining = (gint)MAX(1, (deadline - g_get_monotonic_time()) / 1000);
		if (!ai_subprocess_communicate_utf8_bounded(process, NULL, remaining,
		                                          NULL, NULL, NULL, NULL) ||
		    !g_subprocess_get_if_exited(process) ||
		    g_subprocess_get_exit_status(process) > (added ? 1 : 0))
			return FALSE;
	}
	if (output->len == 0) return FALSE;
	*patch = g_string_free(g_steal_pointer(&output), FALSE);
	return TRUE;
}

/* Codex exec reports paths and kinds, not edit hunks. Supplement successful
 * events with explicitly labeled working-tree context, never pretend it is
 * the exact patch. Provider-supplied diffs always take precedence. */
static void
capture_file_changes(AiCliClient *client, JsonObject *item)
{
	JsonArray *changes = ai_json_get_array(item, "changes");
	gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
	guint i;

	if (changes == NULL ||
	    g_strcmp0(ai_json_get_string(item, "status", NULL), "completed") != 0)
		return;
	for (i = 0; i < json_array_get_length(changes) && i < 8; i++)
	{
		JsonObject *change = ai_json_array_get_object(changes, i);
		const gchar *path = ai_json_get_string(change, "path", NULL);
		g_autofree gchar *patch = NULL;
		gboolean truncated = FALSE;
		gboolean added = FALSE;

		if (path == NULL || *path == '\0' ||
		    ai_json_get_string(change, "diff", NULL) != NULL) continue;
		if (!capture_diff(client, path, FALSE, deadline, &patch, &truncated))
		{
			if (g_strcmp0(ai_json_get_string(change, "kind", NULL), "add") != 0 ||
			    !capture_diff(client, path, TRUE, deadline, &patch, &truncated)) continue;
			added = TRUE;
		}
		json_object_set_string_member(change, "diff", patch);
		json_object_set_string_member(change, "diff_source",
		                              added ? "current_file" : "working_tree");
		json_object_set_boolean_member(change, "diff_truncated", truncated);
	}
}

/*
 * What codex said went wrong, readable. Codex passes a failed API call's
 * whole response body through as the message -- {"type":"error",
 * "status":400,"error":{"message":"The 'gpt-5.5' model is not supported
 * when using Codex with a ChatGPT account.",...}} -- so the sentence a
 * person needs is two levels down. ai_http_error_extract_message() reads
 * that envelope for the HTTP providers already, and gives plain text back
 * bounded and unchanged.
 */
static gchar *
codex_error_text(const gchar *raw)
{
    if (raw == NULL || *raw == '\0')
        return NULL;
    return ai_http_error_extract_message(raw, strlen(raw));
}

static gboolean
parse_events(AiCliClient *client, const gchar *line, AiResponse *response,
             GPtrArray *events, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *obj, *item;
    const gchar *type, *kind;
    while (line != NULL && g_ascii_isspace(*line)) line++;
    if (line == NULL || !*line) return TRUE;
    if (codex_uses_app_server(AI_CODEX_CLI_CLIENT(client)))
        return parse_app_server_line(AI_CODEX_CLI_CLIENT(client), line, response,
                                     events, error);
    if (!json_parser_load_from_data(parser, line, -1, NULL)) goto malformed;
    obj = ai_json_root_object(parser);
    type = ai_json_get_string(obj, "type", NULL);
    if (type == NULL) goto malformed;
    if (g_str_equal(type, "thread.started"))
    {
        const gchar *id = ai_json_get_string(obj, "thread_id", NULL);
        if (id == NULL || !*id) goto malformed;
        if (ai_cli_client_get_session_persistence(client))
            ai_cli_client_set_session_id(client, id);
    }
    else if (g_str_equal(type, "turn.started"))
    {
        ai_response_set_stop_reason(response, AI_STOP_REASON_NONE);
        g_ptr_array_add(events, ai_event_new(AI_EVENT_STREAM_START));
    }
    else if (g_str_equal(type, "error"))
    {
        /*
         * Not the end of the turn. Codex writes this for a retry notice as
         * well as for the fault that will end it, and a fatal one is always
         * followed by turn.failed -- which is where the turn ends. Stopping
         * here also read the *first* such line as the answer. The message
         * is kept so that output ending with no turn.failed at all still
         * reports what codex said rather than a parse error.
         */
        AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(client);
        g_autofree gchar *text = codex_error_text(ai_json_get_string(obj, "message", NULL));

        if (text != NULL)
        {
            g_ptr_array_add(events, ai_event_new_status(text));
            g_free(self->last_error);
            self->last_error = g_steal_pointer(&text);
        }
    }
    else if (g_str_equal(type, "turn.failed"))
    {
        g_autofree gchar *text = codex_error_text(
            ai_json_get_string(ai_json_get_object(obj, "error"), "message",
                               ai_json_get_string(obj, "message", NULL)));

        g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "%s",
                    text != NULL ? text : "Codex turn failed");
        return FALSE;
    }
    else if (g_str_equal(type, "turn.completed"))
    {
        JsonObject *u = ai_json_get_object(obj, "usage");
        if (u != NULL)
        {
            gint input_tokens = usage_tokens(u, "input_tokens");
            gint output_tokens = usage_tokens(u, "output_tokens");
            gint64 cost = codex_cost_micros(ai_cli_client_get_model(client),
                                            input_tokens, output_tokens);
            g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

            ai_response_set_usage(response, usage);
            /* -1, not 0, when the model has no published rate: zero would
             * paint the turn as free. A priced model also lands on the
             * response so a non-streaming caller sees the same figure. */
            if (cost >= 0)
                ai_response_set_cost_micros(response, cost);
            g_ptr_array_add(events, ai_event_new_usage(usage, cost));
        }
        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }
    else if (g_str_has_prefix(type, "item."))
    {
        gboolean completed = g_str_equal(type, "item.completed");
        item = ai_json_get_object(obj, "item");
        kind = ai_json_get_string(item, "type", NULL);
        if (kind == NULL) goto malformed;
        if (g_str_equal(kind, "agent_message") || g_str_equal(kind, "reasoning"))
        {
            const gchar *text = ai_json_get_string(item, "text", NULL);
            if (!completed) return TRUE; /* updates are cumulative snapshots */
            if (text == NULL) goto malformed;
            if (g_str_equal(kind, "reasoning"))
                g_ptr_array_add(events, ai_event_new_thinking_delta(text));
            else
            {
                g_autoptr(AiTextContent) block = ai_text_content_new(text);
                ai_response_add_content_block(response, AI_CONTENT_BLOCK(g_steal_pointer(&block)));
                g_ptr_array_add(events, ai_event_new_text_delta(text));
            }
        }
        else if (g_str_equal(kind, "command_execution") || g_str_equal(kind, "file_change") ||
                 g_str_equal(kind, "mcp_tool_call") || g_str_equal(kind, "web_search"))
        {
            const gchar *id = ai_json_get_string(item, "id", NULL);
            g_autoptr(AiToolUse) tool = NULL;
            if (id == NULL) goto malformed;
            if (completed && g_str_equal(kind, "file_change"))
                capture_file_changes(client, item);
            tool = ai_tool_use_new(id, kind, ai_json_get_node(obj, "item"));
            if (g_str_equal(type, "item.started"))
                g_ptr_array_add(events, ai_event_new_tool_started(tool));
            if (completed)
            {
                g_autofree gchar *serialized = json_to_string(ai_json_get_node(obj, "item"), FALSE);
                const gchar *output = ai_json_get_string(item, "aggregated_output", serialized);
                gboolean failed = g_strcmp0(ai_json_get_string(item, "status", ""), "failed") == 0 ||
                                  ai_json_get_int(item, "exit_code", 0) != 0 ||
                                  ai_json_get_node(item, "error") != NULL;
                g_autoptr(AiToolResult) result = ai_tool_result_new_with_name(id, kind, output, failed);
                g_ptr_array_add(events, ai_event_new_tool_finished(tool, result));
            }
        }
        else if (g_str_equal(kind, "error"))
        {
            /*
             * An error *item* is codex's non-fatal notice -- "Model
             * metadata for `x` not found. Defaulting to fallback metadata"
             * is one, written before the turn even starts. Failing on it
             * reported that warning as the reason every turn on an
             * unlisted model failed, while the real refusal (a ChatGPT
             * login cannot use the model) arrived two lines later and was
             * never read. It is a status, and deliberately not kept as
             * last_error: naming a warning as the cause is the bug.
             */
            const gchar *text = ai_json_get_string(item, "message", NULL);

            if (completed && text != NULL && *text != '\0')
                g_ptr_array_add(events, ai_event_new_status(text));
        }
        /*
         * Unknown item kinds are dropped. Relaying the raw JSON as
         * STATUS would paint process chatter into the transcript the
         * way claude-code's thinking_tokens heartbeats did; a kind we
         * do not recognise is not something worth telling a human.
         */
    }
    return TRUE;
malformed:
    g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Malformed Codex JSONL event");
    return FALSE;
}

static gboolean
parse_line(AiCliClient *client, const gchar *line, AiResponse *response,
           gchar **delta, GError **error)
{
    g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
    *delta = NULL;
    if (!parse_events(client, line, response, events, error)) return FALSE;
    *delta = ai_cli_client_events_to_delta(events);
    return TRUE;
}

/*
 * A turn that stopped before turn.completed: say what codex last reported
 * as an error when it reported one, since that is why, and fall back to
 * the parse error only when it said nothing.
 */
static void
set_unfinished_error(AiCodexCliClient *self, GError **error)
{
    if (self->last_error != NULL)
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "%s", self->last_error);
    else
        g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                            "Codex output ended before turn.completed");
}

static AiResponse *
parse_output(AiCliClient *client, const gchar *json, GError **error)
{
    g_autoptr(AiResponse) response = ai_response_new("", ai_cli_client_get_model(client));
    g_auto(GStrv) lines = g_strsplit(json != NULL ? json : "", "\n", -1);
    guint i;
    g_clear_pointer(&AI_CODEX_CLI_CLIENT(client)->last_error, g_free);
    for (i = 0; lines[i] != NULL; i++)
    {
        g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
        if (!parse_events(client, lines[i], response, events, error)) return NULL;
    }
    if (ai_response_get_stop_reason(response) != AI_STOP_REASON_END_TURN)
    {
        set_unfinished_error(AI_CODEX_CLI_CLIENT(client), error);
        return NULL;
    }
    return g_steal_pointer(&response);
}

static void
finalize(GObject *object)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(object);
    clear_image_files(self);
    g_free(self->sandbox);
    g_free(self->additional_directories);
    g_free(self->profile);
    g_free(self->turn_system_prompt);
    g_strfreev(self->mcp_overrides);
    g_free(self->last_error);
    g_free(self->transport);
    g_clear_pointer(&self->pending_rpc, codex_rpc_free);
    G_OBJECT_CLASS(ai_codex_cli_client_parent_class)->finalize(object);
}

/*
 * endpoint_applied:
 * @client: the receiving CLI client
 * @endpoint: (nullable): the granted configuration file
 * @error: return location for a configuration error
 *
 * Accept only the small JSON/TOML intersection needed for stdio MCP
 * servers. This prevents a host fragment from changing sandbox or model
 * configuration, and rejects malformed values before launching Codex.
 * Keep an owned snapshot so deletion or replacement of the host's file
 * cannot change a grant after it has been applied.
 *
 * Returns: whether the grant was accepted
 */
static gboolean
endpoint_applied(
	AiCliClient           *client,
	const AiAgentEndpoint *endpoint,
	GError               **error
){
	AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(client);
	g_autofree gchar *contents = NULL;
	g_auto(GStrv) lines = NULL;
	g_autoptr(GPtrArray) overrides = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileInputStream) input = NULL;
	GHashTableIter iter;
	gpointer stored_key;
	gsize length;
	guint i;

	/* Failed replacement must not leave a revoked grant active. */
	g_clear_pointer(&self->mcp_overrides, g_strfreev);
	if (endpoint == NULL || g_strcmp0(endpoint->kind, AI_ENDPOINT_KIND_MCP_CONFIG_CODEX) != 0)
		return TRUE;
	if (endpoint->value == NULL || *endpoint->value == '\0')
		goto invalid;
	file = g_file_new_for_path(endpoint->value);
	input = g_file_read(file, NULL, error);
	if (input == NULL) return FALSE;
	contents = g_malloc0(65538);
	if (!g_input_stream_read_all(G_INPUT_STREAM(input), contents, 65537, &length, NULL, error))
		return FALSE;
	if (length > 65536 || strlen(contents) != length || !g_utf8_validate(contents, length, NULL))
		goto invalid;
	lines = g_strsplit(contents, "\n", -1);
	for (i = 0; lines[i] != NULL; i++)
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_auto(GStrv) path = NULL;
		gchar *line = g_strstrip(lines[i]);
		gchar *equals;
		gchar *key;
		gchar *value;
		JsonNode *node;
		guint j;

		if (*line == '\0' || *line == '#') continue;
		equals = strchr(line, '=');
		if (equals == NULL) goto invalid;
		*equals = '\0';
		key = g_strstrip(line);
		value = g_strstrip(equals + 1);
		path = g_strsplit(key, ".", -1);
		if (g_strv_length(path) != 3 || !g_str_equal(path[0], "mcp_servers") || !*path[1])
			goto invalid;
		for (j = 0; path[1][j] != '\0'; j++)
			if (!g_ascii_isalnum(path[1][j]) && path[1][j] != '_' && path[1][j] != '-')
				goto invalid;
		if (!g_hash_table_add(keys, g_strdup(key))) goto invalid;
		if (!json_parser_load_from_data(parser, value, -1, NULL)) goto invalid;
		node = json_parser_get_root(parser);
		if (g_str_equal(path[2], "command"))
		{
			if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING ||
			    !*json_node_get_string(node)) goto invalid;
		}
		else if (g_str_equal(path[2], "args"))
		{
			JsonArray *array;
			if (!JSON_NODE_HOLDS_ARRAY(node)) goto invalid;
			array = json_node_get_array(node);
			for (j = 0; j < json_array_get_length(array); j++)
			{
				JsonNode *element = json_array_get_element(array, j);
				if (!JSON_NODE_HOLDS_VALUE(element) || json_node_get_value_type(element) != G_TYPE_STRING)
					goto invalid;
			}
		}
		else if (g_str_equal(path[2], "required"))
		{
			if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
				goto invalid;
		}
		else if (g_str_equal(path[2], "default_tools_approval_mode"))
		{
			/* Host fragments may authorize their own tools, never global policy. */
			if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING ||
			    g_strcmp0(json_node_get_string(node), "approve") != 0)
				goto invalid;
		}
		else goto invalid;
		/* JSON permits \\/, while TOML does not; reject that ambiguous escape. */
		if (strstr(value, "\\/") != NULL) goto invalid;
		g_ptr_array_add(overrides, g_strdup_printf("%s=%s", key, value));
	}
	if (overrides->len == 0) goto invalid;
	/* Every server needs its own command; args or required alone must not
	 * inherit an unrelated user command. */
	g_hash_table_iter_init(&iter, keys);
	while (g_hash_table_iter_next(&iter, &stored_key, NULL))
	{
		const gchar *key = (const gchar *)stored_key;
		if (!g_str_has_suffix(key, ".command"))
		{
			g_autofree gchar *prefix = g_strndup(key, strrchr(key, '.') - key);
			g_autofree gchar *command_key = g_strconcat(prefix, ".command", NULL);
			if (!g_hash_table_contains(keys, command_key)) goto invalid;
		}
	}
	g_ptr_array_add(overrides, NULL);
	self->mcp_overrides = (gchar **)g_ptr_array_free(g_steal_pointer(&overrides), FALSE);
	return TRUE;

invalid:
	g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		"Invalid Codex MCP configuration: expected unique mcp_servers.NAME.command/args/required/default_tools_approval_mode JSON assignments (maximum 64 KiB)");
	return FALSE;
}

static const gchar * const endpoint_kinds[] = {
	AI_ENDPOINT_KIND_ENV,
	AI_ENDPOINT_KIND_MCP_CONFIG_CODEX,
	NULL
};

/*
 * What codex prints when it is limited, throttled or signed out, and what
 * each means.  Read out of codex-cli 0.163.0-alpha.10.2 (the binary's
 * strings; the Display impls of CodexErr in codex-rs core/src/error.rs and
 * the login/token-refresh messages in codex-rs login), and reaching us as
 * the message of a turn.failed or error event, or on stderr:
 *
 *   "You've hit your usage limit. ... Try again at 3:45 PM."
 *   "You've hit your usage limit. ... Try again at Sep 5th, 2025 3:45 PM."
 *   "You've hit your usage limit for <model>. Switch to another model now,
 *    or try again at ..."                  (UsageLimitReachedError)
 *   "Quota exceeded. Check your plan and billing details."  (QuotaExceeded)
 *   "To use Codex with your ChatGPT plan, upgrade to Plus: ..."
 *                                           (UsageNotIncluded)
 *   "exceeded retry limit, last status: 429 Too Many Requests, request id: ..."
 *                                           (RetryLimit)
 *   "Your access token could not be refreshed because your refresh token
 *    has expired. Please log out and sign in again."  (RefreshTokenFailed)
 *   "no Codex credentials were found" / "Run codex login or provide an API
 *    key through a supported auth env var."  / "Not logged in"
 *
 * The usage-limit text's reset ("Try again at 3:45 PM", local time, or a
 * dated "Sep 5th, 2025 3:45 PM") is read by ai_session_limit_parse_reset().
 * The snake_case codes are the API's error types, which codex passes
 * through when it forwards a response body whole.
 */
static const AiCliLimitPattern codex_limit_patterns[] = {
    { AI_ERROR_SESSION_LIMIT, "hit your usage limit" },
    { AI_ERROR_SESSION_LIMIT, "usage_limit_reached" },
    { AI_ERROR_INSUFFICIENT_QUOTA, "quota exceeded" },
    { AI_ERROR_INSUFFICIENT_QUOTA, "insufficient_quota" },
    { AI_ERROR_INSUFFICIENT_QUOTA, "to use codex with your chatgpt plan" },
    { AI_ERROR_RATE_LIMITED, "too many requests" },
    { AI_ERROR_RATE_LIMITED, "rate limit" },
    { AI_ERROR_RATE_LIMITED, "rate_limit_exceeded" },
    { AI_ERROR_INVALID_API_KEY, "sign in again" },
    { AI_ERROR_INVALID_API_KEY, "access token could not be refreshed" },
    { AI_ERROR_INVALID_API_KEY, "no codex credentials were found" },
    { AI_ERROR_INVALID_API_KEY, "codex login" },
    { AI_ERROR_INVALID_API_KEY, "not logged in" },
    { AI_ERROR_INVALID_API_KEY, "401 unauthorized" },
    { AI_ERROR_INVALID_API_KEY, "invalid_api_key" },
    { AI_ERROR_INVALID_API_KEY, "incorrect api key" },
    { 0, NULL }
};

/*
 * codex keeps auth.json, config.toml and its sessions under CODEX_HOME.
 */
static const gchar * const codex_config_dir_env[] = { "CODEX_HOME", NULL };

/* ================================================================
 * The app-server transport
 *
 * `codex app-server` speaks JSON-RPC over stdio: one JSON object per
 * line each way, without the "jsonrpc":"2.0" member (codex-rs
 * app-server-protocol/src/rpc.rs).  Everything below was written from
 * the published protocol of openai/codex rust-v0.169.0 (commit
 * d32063f96c2dff1dc96af839b3952abb6acfeda4): the JSON schemas in
 * codex-rs/app-server-protocol/schema/json, the v2 types in
 * app-server-protocol/src/protocol/v2, and turn/steer's handler in
 * app-server/src/request_processors/turn_processor.rs.  It has been run
 * against tests/test-codex-app-server.c's fake server only, never a
 * real codex; docs/steering.org says how to check it against one.
 *
 * One process per turn, as with exec:
 *
 *   -> initialize {clientInfo}                       id 1
 *   <- result
 *   -> initialized                                   (notification)
 *   -> thread/start {model, cwd, sandbox, approvalPolicy, ephemeral}
 *      or thread/resume {threadId, ...}              id 2
 *   <- result {thread: {id}}
 *   -> turn/start {threadId, input, effort}          id 3
 *   <- result {turn: {id}}, then turn/started, item/started, item/completed, ...
 *   <- turn/completed {turn: {status, error}}
 *   -> EOF on stdin; app-server shuts down
 *
 * and while the turn runs, a steer is
 *
 *   -> turn/steer {threadId, expectedTurnId, input}  id 4, 5, ...
 *   <- result {turnId}             DELIVERED
 *   <- error  (no active turn, expected turn mismatch, not steerable)
 *                                  REFUSED
 *   (no answer within steer-timeout-ms)
 *                                  INDETERMINATE
 *
 * The process's stdin is the run's AiCliSteerChannel, so the handshake
 * lines the parser writes and the steers a caller writes share one
 * ordered pipe, and the channel's tri-state bookkeeping applies to
 * turn/steer exactly as it does to claude-code's echoed user events.
 * ================================================================ */

/* Request ids of the handshake; steers count on from CODEX_RPC_FIRST_STEER. */
#define CODEX_RPC_INITIALIZE   (1)
#define CODEX_RPC_THREAD       (2)
#define CODEX_RPC_TURN         (3)
#define CODEX_RPC_FIRST_STEER  (4)

/* JSON-RPC error codes app-server uses (app-server/src/error_code.rs). */
#define CODEX_RPC_INTERNAL_ERROR (-32603)

/*
 * CodexRpc:
 *
 * One app-server run.  Built by build_stdin() from the client's settings
 * at the moment the run starts (kept in pending_rpc), then owned by the
 * run's steer channel from the first line codex writes, so it lives
 * exactly as long as the run and two runs never share one.
 */
typedef struct
{
	gint64      next_id;
	gchar      *thread_method;     /* "thread/start" or "thread/resume" */
	JsonNode   *thread_params;     /* complete */
	JsonNode   *turn_params;       /* threadId is added when known */
	gboolean    persist;           /* record the thread id as session id */
	gboolean    skip_permissions;  /* how approval requests are answered */

	gchar      *thread_id;         /* codex's, from thread/start|resume */
	gchar      *turn_id;           /* codex's, from turn/start or turn/started */
	gboolean    stream_started;
	gboolean    turn_done;

	GHashTable *streamed_items;    /* item ids that already sent deltas */
	gint64      input_tokens;
	gint64      output_tokens;
	gboolean    have_usage;
} CodexRpc;

static void
codex_rpc_free(gpointer data)
{
	CodexRpc *rpc = data;

	if (rpc == NULL) return;
	g_free(rpc->thread_method);
	g_clear_pointer(&rpc->thread_params, json_node_unref);
	g_clear_pointer(&rpc->turn_params, json_node_unref);
	g_free(rpc->thread_id);
	g_free(rpc->turn_id);
	g_clear_pointer(&rpc->streamed_items, g_hash_table_unref);
	g_free(rpc);
}

/* Serializes a builder's root and terminates the line. */
static gchar *
codex_rpc_finish_line(JsonBuilder *builder)
{
	g_autoptr(JsonNode) root = json_builder_get_root(builder);
	g_autofree gchar *json = json_to_string(root, FALSE);

	return g_strconcat(json, "\n", NULL);
}

/*
 * codex_rpc_request_line:
 * @id: the request id, or -1 for a notification
 * @method: the method name
 * @params: (nullable) (transfer none): the params object
 *
 * Returns: (transfer full): one newline-terminated JSON-RPC line
 */
static gchar *
codex_rpc_request_line(gint64 id, const gchar *method, JsonNode *params)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	if (id >= 0)
	{
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, id);
	}
	json_builder_set_member_name(builder, "method");
	json_builder_add_string_value(builder, method);
	if (params != NULL)
	{
		json_builder_set_member_name(builder, "params");
		json_builder_add_value(builder, json_node_copy(params));
	}
	json_builder_end_object(builder);
	return codex_rpc_finish_line(builder);
}

/*
 * codex_rpc_result_line:
 * @id: (transfer none): the request id as codex sent it (number or string)
 * @result: (transfer full): the result object
 *
 * Returns: (transfer full): the response line
 */
static gchar *
codex_rpc_result_line(JsonNode *id, JsonNode *result)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_value(builder, json_node_copy(id));
	json_builder_set_member_name(builder, "result");
	json_builder_add_value(builder, result);
	json_builder_end_object(builder);
	return codex_rpc_finish_line(builder);
}

/* An error response for a request ai-glib does not serve. */
static gchar *
codex_rpc_error_line(JsonNode *id, gint64 code, const gchar *message)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
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
	return codex_rpc_finish_line(builder);
}

/* {"decision": <decision>} -- the shape of every approval answer. */
static JsonNode *
codex_rpc_decision(const gchar *decision)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "decision");
	json_builder_add_string_value(builder, decision);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* [{"type":"text","text":@text}] plus one localImage per image file. */
static void
codex_rpc_add_input(JsonBuilder *builder, const gchar *text, GList *images)
{
	GList *l;

	json_builder_set_member_name(builder, "input");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "text");
	json_builder_set_member_name(builder, "text");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	for (l = images; l != NULL; l = l->next)
	{
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "localImage");
		json_builder_set_member_name(builder, "path");
		json_builder_add_string_value(builder, l->data);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
}

/*
 * codex_writable_root:
 * @self: a Codex client
 * @dir: one entry of additional-directories, stripped
 *
 * The directory as an absolute path for sandbox_workspace_write.
 * writable_roots, which takes absolute paths.  A relative entry is taken
 * relative to the working directory, as exec's --add-dir would.  Paths
 * that would need TOML escaping are refused rather than escaped: they go
 * into a -c override, and a quote there would end the string.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL when unusable
 */
static gchar *
codex_writable_root(AiCodexCliClient *self, const gchar *dir)
{
	const gchar *cwd = ai_cli_client_get_working_directory(AI_CLI_CLIENT(self));
	const gchar *p;
	g_autofree gchar *base = NULL;

	for (p = dir; *p != '\0'; p++)
		if (*p == '"' || *p == '\\' || g_ascii_iscntrl(*p))
			return NULL;
	if (g_path_is_absolute(dir))
		return g_strdup(dir);
	base = cwd != NULL ? g_strdup(cwd) : g_get_current_dir();
	return g_build_filename(base, dir, NULL);
}

/*
 * build_app_server_argv:
 * @self: a Codex client (policy already checked)
 * @system_prompt: (nullable): the turn's system prompt
 *
 * `codex [-c ...] app-server`.  Configuration that app-server has no
 * request field for goes in root -c overrides, which codex hands to the
 * app-server it starts (codex-rs/cli/src/main.rs): the granted MCP
 * servers, live web search (`web_search="live"`, what --search sets),
 * and the extra writable roots exec takes as --add-dir.  Everything
 * per-thread or per-turn -- model, sandbox, approval policy, working
 * directory, effort -- travels in thread/start and turn/start instead.
 *
 * `--profile` is refused: codex rejects it for app-server ("--profile
 * only applies to runtime commands", cli/src/main.rs), so passing it
 * would fail every turn after launch instead of before.
 *
 * Returns: (transfer full) (nullable): argv, or %NULL to fail closed
 */
static gchar **
build_app_server_argv(AiCodexCliClient *self, const gchar *system_prompt)
{
	g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
	const gchar *effort = ai_cli_client_get_effort_level(AI_CLI_CLIENT(self));

	if (self->profile != NULL && *self->profile != '\0')
		return NULL;
	if (!codex_effort_valid(effort))
		return NULL;

	g_free(self->turn_system_prompt);
	self->turn_system_prompt = g_strdup(system_prompt);

	arg(args, "codex");
	if (self->mcp_overrides != NULL)
	{
		guint i;

		for (i = 0; self->mcp_overrides[i] != NULL; i++)
		{
			arg(args, "-c");
			arg(args, self->mcp_overrides[i]);
		}
	}
	if (self->search)
	{
		arg(args, "-c");
		arg(args, "web_search=\"live\"");
	}
	if (self->additional_directories != NULL)
	{
		g_auto(GStrv) dirs = g_strsplit(self->additional_directories, ",", -1);
		g_autoptr(GString) roots = g_string_new("sandbox_workspace_write.writable_roots=[");
		guint count = 0;
		guint i;

		for (i = 0; dirs[i] != NULL; i++)
		{
			g_autofree gchar *root = NULL;

			g_strstrip(dirs[i]);
			if (*dirs[i] == '\0') continue;
			root = codex_writable_root(self, dirs[i]);
			if (root == NULL) return NULL;
			g_string_append_printf(roots, "%s\"%s\"", count++ > 0 ? "," : "", root);
		}
		g_string_append_c(roots, ']');
		if (count > 0)
		{
			arg(args, "-c");
			arg(args, roots->str);
		}
	}
	arg(args, "app-server");
	g_ptr_array_add(args, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&args), FALSE);
}

/*
 * build_app_server_stdin:
 * @self: a Codex client
 * @prompt: (transfer full): the turn's prompt text
 *
 * Plans the run -- the thread and turn requests, built now from the
 * client's settings so that changing a property mid-turn cannot change a
 * request already decided -- keeps the plan in pending_rpc, and returns
 * the first line to write: initialize.
 *
 * Thread: resumed by id when the client persists sessions and has one
 * (ai_cli_client_get_session_id()); otherwise a new thread, ephemeral
 * when persistence is off.  continue-session without a session id has
 * no app-server equivalent of exec's `resume --last` and starts a new
 * thread.
 *
 * Policy, matching exec's flags: approvalPolicy "never" (exec's
 * --ask-for-approval never) and the sandbox mode, "danger-full-access"
 * under skip-permissions (exec's --dangerously-bypass-approvals-and-
 * sandbox).
 *
 * Returns: (transfer full): the initialize request line
 */
static gchar *
build_app_server_stdin(AiCodexCliClient *self, gchar *prompt)
{
	AiCliClient *client = AI_CLI_CLIENT(self);
	g_autofree gchar *text = prompt;
	g_autofree gchar *version = g_strdup_printf("%d.%d.%d", AI_VERSION_MAJOR,
	                                            AI_VERSION_MINOR, AI_VERSION_MICRO);
	g_autoptr(JsonBuilder) init = json_builder_new();
	g_autoptr(JsonBuilder) thread = json_builder_new();
	g_autoptr(JsonBuilder) turn = json_builder_new();
	g_autoptr(JsonNode) init_params = NULL;
	const gchar *session = ai_cli_client_get_session_id(client);
	const gchar *cwd = ai_cli_client_get_working_directory(client);
	const gchar *effort = ai_cli_client_get_effort_level(client);
	gboolean persist = ai_cli_client_get_session_persistence(client);
	gboolean resume = persist && session != NULL && *session != '\0';
	CodexRpc *rpc = g_new0(CodexRpc, 1);

	rpc->next_id = CODEX_RPC_FIRST_STEER;
	rpc->persist = persist;
	rpc->skip_permissions = self->skip_permissions;
	rpc->streamed_items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	/* initialize: who we are.  No capabilities: nothing experimental is used. */
	json_builder_begin_object(init);
	json_builder_set_member_name(init, "clientInfo");
	json_builder_begin_object(init);
	json_builder_set_member_name(init, "name");
	json_builder_add_string_value(init, "ai-glib");
	json_builder_set_member_name(init, "title");
	json_builder_add_string_value(init, "ai-glib");
	json_builder_set_member_name(init, "version");
	json_builder_add_string_value(init, version);
	json_builder_end_object(init);
	json_builder_end_object(init);
	init_params = json_builder_get_root(init);

	/* thread/start or thread/resume: where, with what, under which policy. */
	rpc->thread_method = g_strdup(resume ? "thread/resume" : "thread/start");
	json_builder_begin_object(thread);
	if (resume)
	{
		json_builder_set_member_name(thread, "threadId");
		json_builder_add_string_value(thread, session);
	}
	json_builder_set_member_name(thread, "model");
	json_builder_add_string_value(thread, ai_cli_client_get_model(client));
	if (cwd != NULL && *cwd != '\0')
	{
		json_builder_set_member_name(thread, "cwd");
		json_builder_add_string_value(thread, cwd);
	}
	json_builder_set_member_name(thread, "approvalPolicy");
	json_builder_add_string_value(thread, "never");
	json_builder_set_member_name(thread, "sandbox");
	json_builder_add_string_value(thread, self->skip_permissions ? "danger-full-access"
	                              : self->sandbox != NULL ? self->sandbox : "read-only");
	if (!resume && !persist)
	{
		json_builder_set_member_name(thread, "ephemeral");
		json_builder_add_boolean_value(thread, TRUE);
	}
	json_builder_end_object(thread);
	rpc->thread_params = json_builder_get_root(thread);

	/* turn/start: the prompt, the images, the effort; threadId comes later. */
	json_builder_begin_object(turn);
	codex_rpc_add_input(turn, text, self->image_files);
	if (effort != NULL && *effort != '\0')
	{
		json_builder_set_member_name(turn, "effort");
		json_builder_add_string_value(turn, effort);
	}
	json_builder_end_object(turn);
	rpc->turn_params = json_builder_get_root(turn);

	g_clear_pointer(&self->pending_rpc, codex_rpc_free);
	self->pending_rpc = rpc;

	return codex_rpc_request_line(CODEX_RPC_INITIALIZE, "initialize", init_params);
}

/* The exec tool names, so the view layer styles both transports alike. */
static const gchar *
codex_tool_name(const gchar *item_type)
{
	if (g_strcmp0(item_type, "commandExecution") == 0) return "command_execution";
	if (g_strcmp0(item_type, "fileChange") == 0) return "file_change";
	if (g_strcmp0(item_type, "mcpToolCall") == 0) return "mcp_tool_call";
	if (g_strcmp0(item_type, "webSearch") == 0) return "web_search";
	if (g_strcmp0(item_type, "dynamicToolCall") == 0) return "dynamic_tool_call";
	return NULL;
}

/*
 * codex_item_event:
 * @client: the Codex client
 * @params: an item/started or item/completed notification's params
 * @completed: which of the two
 * @rpc: the run
 * @response: the response being built
 * @events: (element-type AiEvent): where events go
 *
 * One ThreadItem's start or end.  Agent messages become response text
 * (their deltas were already streamed, so only an item that sent none
 * is emitted as a delta here); reasoning becomes thinking the same way;
 * commands, file changes, MCP and web-search calls become tool events
 * named as exec names them.  Other item kinds are dropped, as on exec.
 *
 * Returns: %FALSE when the item has no type or id
 */
static gboolean
codex_item_event(AiCliClient *client, JsonObject *params, gboolean completed,
                 CodexRpc *rpc, AiResponse *response, GPtrArray *events)
{
	JsonObject *item = ai_json_get_object(params, "item");
	const gchar *type = ai_json_get_string(item, "type", NULL);
	const gchar *id = ai_json_get_string(item, "id", NULL);
	const gchar *name = codex_tool_name(type);
	gboolean streamed;

	if (type == NULL || id == NULL)
		return FALSE;
	streamed = g_hash_table_contains(rpc->streamed_items, id);

	if (g_str_equal(type, "agentMessage"))
	{
		const gchar *text = ai_json_get_string(item, "text", NULL);

		if (!completed || text == NULL) return TRUE;
		{
			g_autoptr(AiTextContent) block = ai_text_content_new(text);

			ai_response_add_content_block(response, AI_CONTENT_BLOCK(g_steal_pointer(&block)));
		}
		if (!streamed && *text != '\0')
			g_ptr_array_add(events, ai_event_new_text_delta(text));
	}
	else if (g_str_equal(type, "reasoning"))
	{
		JsonArray *parts;
		g_autoptr(GString) text = g_string_new(NULL);
		guint i;

		if (!completed || streamed) return TRUE;
		parts = ai_json_get_array(item, "summary");
		if (parts == NULL || json_array_get_length(parts) == 0)
			parts = ai_json_get_array(item, "content");
		for (i = 0; parts != NULL && i < json_array_get_length(parts); i++)
		{
			const gchar *part = ai_json_array_get_string(parts, i, NULL);

			if (part == NULL) continue;
			if (text->len > 0) g_string_append(text, "\n\n");
			g_string_append(text, part);
		}
		if (text->len > 0)
			g_ptr_array_add(events, ai_event_new_thinking_delta(text->str));
	}
	else if (name != NULL)
	{
		JsonNode *node = ai_json_get_node(params, "item");
		g_autoptr(AiToolUse) tool = NULL;

		if (completed && g_str_equal(name, "file_change"))
			capture_file_changes(client, item);
		tool = ai_tool_use_new(id, name, node);
		if (!completed)
		{
			g_ptr_array_add(events, ai_event_new_tool_started(tool));
		}
		else
		{
			g_autofree gchar *serialized = json_to_string(node, FALSE);
			const gchar *status = ai_json_get_string(item, "status", "");
			JsonObject *failure = ai_json_get_object(item, "error");
			const gchar *output = ai_json_get_string(item, "aggregatedOutput", NULL);
			gboolean failed = g_str_equal(status, "failed") ||
			                  g_str_equal(status, "declined") ||
			                  ai_json_get_int(item, "exitCode", 0) != 0 ||
			                  failure != NULL;
			g_autoptr(AiToolResult) result = NULL;

			if (output == NULL && failure != NULL)
				output = ai_json_get_string(failure, "message", NULL);
			result = ai_tool_result_new_with_name(id, name,
			                                      output != NULL ? output : serialized,
			                                      failed);
			g_ptr_array_add(events, ai_event_new_tool_finished(tool, result));
		}
	}
	return TRUE;
}

/*
 * codex_turn_error:
 * @turn_error: (nullable): a TurnError {message, codexErrorInfo, ...}
 * @fallback: the sentence to use when codex gave none
 * @error: return location
 *
 * Why a turn failed, classified the way exec's turn.failed is: the
 * message through codex's limit phrase table first, so a usage limit,
 * a 429 and a lapsed login read the same on both transports.  When the
 * words match no phrase but codex said what kind of failure it was --
 * codexErrorInfo usageLimitExceeded, rateLimitExceeded, unauthorized, or
 * a retry limit on HTTP 429 -- that structured code decides, and the
 * error still takes ai_cli_limit_classify()'s shape (a one-row table
 * whose needle is the message itself).  Anything else is
 * AI_ERROR_CLI_EXECUTION, which the base reclassifies once more on the
 * way out (harmless: it then leaves a classified error alone).
 */
static void
codex_turn_error(JsonObject *turn_error, const gchar *fallback, GError **error)
{
	const gchar *raw = ai_json_get_string(turn_error, "message", NULL);
	g_autofree gchar *text = codex_error_text(raw);
	const gchar *said = text != NULL ? text : fallback;
	JsonNode *info = ai_json_get_node(turn_error, "codexErrorInfo");
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	const gchar *kind = NULL;
	AiError code = 0;

	if (ai_cli_limit_classify(codex_limit_patterns, "codex", said, now, error))
		return;

	/* codexErrorInfo is a bare string or a one-member object. */
	if (info != NULL && JSON_NODE_HOLDS_VALUE(info) &&
	    json_node_get_value_type(info) == G_TYPE_STRING)
	{
		kind = json_node_get_string(info);
	}
	else if (info != NULL && JSON_NODE_HOLDS_OBJECT(info))
	{
		JsonObject *object = json_node_get_object(info);
		JsonObject *attempts = ai_json_get_object(object, "responseTooManyFailedAttempts");

		if (attempts != NULL && ai_json_get_int(attempts, "httpStatusCode", 0) == 429)
			code = AI_ERROR_RATE_LIMITED;
	}
	if (g_strcmp0(kind, "usageLimitExceeded") == 0) code = AI_ERROR_SESSION_LIMIT;
	else if (g_strcmp0(kind, "rateLimitExceeded") == 0) code = AI_ERROR_RATE_LIMITED;
	else if (g_strcmp0(kind, "unauthorized") == 0) code = AI_ERROR_INVALID_API_KEY;

	if (code != 0)
	{
		const AiCliLimitPattern structured[] = { { code, said }, { 0, NULL } };

		if (ai_cli_limit_classify(structured, "codex", said, now, error))
			return;
	}
	g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "%s", said);
}

/* A failed handshake step: the whole run fails, with codex's words. */
static gboolean
codex_rpc_step_failed(const gchar *step, JsonObject *failure, GError **error)
{
	const gchar *message = ai_json_get_string(failure, "message", "no message");

	g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
	            "codex app-server refused %s: %s", step, message);
	return FALSE;
}

/*
 * codex_resolve_steer:
 * @channel: the run's channel
 * @rpc: the run
 * @id: the response's id
 * @obj: the response
 *
 * The answer to a turn/steer.  A result is DELIVERED: app-server answers
 * success only after core took the input (SteerSubmission::Steered in
 * turn_processor.rs).  An error is REFUSED, because every error path
 * there returns before or instead of submitting: invalid_request (-32600)
 * for "no active turn to steer", "expected active turn id `x` but found
 * `y`", "cannot steer a review/compact turn", an empty input, a server
 * that is draining; -32601 from an app-server too old to know the
 * method.  The one exception is an internal error (-32603, "failed to
 * steer turn: ..."), raised by the core call itself, which cannot be
 * shown not to have taken the words: INDETERMINATE, never a replay.
 *
 * Ids the channel never sent as steers are ignored by it.
 */
static void
codex_resolve_steer(AiCliSteerChannel *channel, gint64 id, JsonObject *obj)
{
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, id);
	JsonObject *failure = ai_json_get_object(obj, "error");
	const gchar *message;
	gint64 code;
	AiSteerError reason = AI_STEER_ERROR_NOT_STEERABLE;

	if (failure == NULL)
	{
		ai_cli_steer_channel_resolve(channel, key, AI_STEER_DELIVERED, NULL);
		return;
	}

	code = ai_json_get_int(failure, "code", 0);
	message = ai_json_get_string(failure, "message", "turn/steer failed");
	if (code == CODEX_RPC_INTERNAL_ERROR)
	{
		ai_cli_steer_channel_resolve(channel, key, AI_STEER_INDETERMINATE, NULL);
		return;
	}
	if (strstr(message, "expected active turn id") != NULL)
		reason = AI_STEER_ERROR_TURN_MISMATCH;
	else if (strstr(message, "no active turn") != NULL)
		reason = AI_STEER_ERROR_NO_ACTIVE_TURN;
	ai_cli_steer_channel_resolve(channel, key, AI_STEER_REFUSED,
	                             g_error_new(AI_STEER_ERROR, reason,
	                                         "codex refused the steer: %s", message));
}

/*
 * codex_answer_request:
 * @channel: the run's channel
 * @rpc: the run
 * @obj: a request codex sent us
 * @events: (element-type AiEvent): where a notice goes
 *
 * Every server request is answered, or codex waits on it for ever.
 * Approvals are answered the way exec's flags would have decided them:
 * approvalPolicy "never" normally means codex never asks, and if it does
 * anyway the answer is the one exec gives without asking -- decline, or
 * accept under skip-permissions (exec's bypass).  Extra permissions are
 * never granted (an empty profile).  An MCP elicitation asks for input
 * nobody headless can give: declined.  Anything else -- user input, a
 * dynamic tool call (none are registered), token refresh, attestation --
 * is a JSON-RPC error, so codex fails that step instead of hanging.
 */
static void
codex_answer_request(AiCliSteerChannel *channel, CodexRpc *rpc, JsonObject *obj,
                     GPtrArray *events)
{
	JsonNode *id = json_object_get_member(obj, "id");
	const gchar *method = ai_json_get_string(obj, "method", "");
	g_autofree gchar *line = NULL;
	const gchar *verdict = NULL;

	if (g_str_equal(method, "item/commandExecution/requestApproval") ||
	    g_str_equal(method, "item/fileChange/requestApproval"))
	{
		verdict = rpc->skip_permissions ? "accept" : "decline";
		line = codex_rpc_result_line(id, codex_rpc_decision(verdict));
	}
	else if (g_str_equal(method, "execCommandApproval") ||
	         g_str_equal(method, "applyPatchApproval"))
	{
		verdict = rpc->skip_permissions ? "approved" : "denied";
		line = codex_rpc_result_line(id, codex_rpc_decision(verdict));
	}
	else if (g_str_equal(method, "item/permissions/requestApproval"))
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "permissions");
		json_builder_begin_object(builder);
		json_builder_end_object(builder);
		json_builder_set_member_name(builder, "scope");
		json_builder_add_string_value(builder, "turn");
		json_builder_end_object(builder);
		verdict = "granted nothing";
		line = codex_rpc_result_line(id, json_builder_get_root(builder));
	}
	else if (g_str_equal(method, "mcpServer/elicitation/request"))
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "action");
		json_builder_add_string_value(builder, "decline");
		json_builder_end_object(builder);
		verdict = "decline";
		line = codex_rpc_result_line(id, json_builder_get_root(builder));
	}
	else
	{
		g_autofree gchar *why = g_strdup_printf(
			"ai-glib runs codex headless and does not serve %s", method);

		line = codex_rpc_error_line(id, -32601, why);
	}

	if (verdict != NULL)
	{
		g_autofree gchar *notice = g_strdup_printf("codex asked for %s: %s", method, verdict);

		g_ptr_array_add(events, ai_event_new_status(notice));
	}
	ai_cli_steer_channel_write(channel, line);
}

/*
 * codex_handle_response:
 * @self: the Codex client
 * @channel: the run's channel
 * @rpc: the run
 * @obj: a response (result or error) to one of our requests
 * @error: return location for the run's failure
 *
 * Moves the handshake on: initialize answered -> initialized and the
 * thread request; thread answered -> turn/start; turn answered -> the
 * turn id a steer must name.  Any other id is a steer's answer.
 *
 * Returns: %FALSE when the run has failed
 */
static gboolean
codex_handle_response(AiCodexCliClient *self, AiCliSteerChannel *channel,
                      CodexRpc *rpc, JsonObject *obj, GError **error)
{
	gint64 id = ai_json_get_int(obj, "id", -1);
	JsonObject *failure = ai_json_get_object(obj, "error");
	JsonObject *result = ai_json_get_object(obj, "result");

	switch (id)
	{
		case CODEX_RPC_INITIALIZE:
		{
			g_autofree gchar *initialized = NULL;
			g_autofree gchar *thread = NULL;

			if (failure != NULL)
				return codex_rpc_step_failed("initialize", failure, error);
			initialized = codex_rpc_request_line(-1, "initialized", NULL);
			thread = codex_rpc_request_line(CODEX_RPC_THREAD, rpc->thread_method,
			                                rpc->thread_params);
			ai_cli_steer_channel_write(channel, initialized);
			ai_cli_steer_channel_write(channel, thread);
			return TRUE;
		}

		case CODEX_RPC_THREAD:
		{
			const gchar *thread_id;
			g_autofree gchar *turn = NULL;

			if (failure != NULL)
			{
				/* A refusal here is often the limit or login itself. */
				codex_turn_error(failure, "codex app-server refused the thread", error);
				return FALSE;
			}
			thread_id = ai_json_get_string(ai_json_get_object(result, "thread"), "id", NULL);
			if (thread_id == NULL || *thread_id == '\0')
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
				                    "codex app-server answered the thread request without a thread id");
				return FALSE;
			}
			g_free(rpc->thread_id);
			rpc->thread_id = g_strdup(thread_id);
			if (rpc->persist)
				ai_cli_client_set_session_id(AI_CLI_CLIENT(self), thread_id);
			json_object_set_string_member(json_node_get_object(rpc->turn_params),
			                              "threadId", thread_id);
			turn = codex_rpc_request_line(CODEX_RPC_TURN, "turn/start", rpc->turn_params);
			ai_cli_steer_channel_write(channel, turn);
			return TRUE;
		}

		case CODEX_RPC_TURN:
		{
			const gchar *turn_id;

			if (failure != NULL)
			{
				codex_turn_error(failure, "codex app-server refused the turn", error);
				return FALSE;
			}
			turn_id = ai_json_get_string(ai_json_get_object(result, "turn"), "id", NULL);
			if (turn_id != NULL && rpc->turn_id == NULL)
				rpc->turn_id = g_strdup(turn_id);
			return TRUE;
		}

		default:
			if (id >= CODEX_RPC_FIRST_STEER)
				codex_resolve_steer(channel, id, obj);
			return TRUE;
	}
}

/*
 * codex_turn_completed:
 * @self: the Codex client
 * @channel: the run's channel
 * @rpc: the run
 * @turn: the completed Turn
 * @response: the response being built
 * @events: (element-type AiEvent): where the usage event goes
 * @error: return location for a failed or interrupted turn
 *
 * The end of the turn.  Usage first (summed from the turn's
 * thread/tokenUsage/updated "last" figures, priced like exec's), then the
 * outcome.  Either way the channel settles: no more steers, and stdin
 * closes once nothing is in flight, which is what makes app-server exit.
 *
 * Returns: %FALSE when the turn failed
 */
static gboolean
codex_turn_completed(AiCodexCliClient *self, AiCliSteerChannel *channel, CodexRpc *rpc,
                     JsonObject *turn, AiResponse *response, GPtrArray *events,
                     GError **error)
{
	const gchar *status = ai_json_get_string(turn, "status", "");

	rpc->turn_done = TRUE;
	ai_cli_steer_channel_settle(channel);

	if (rpc->have_usage)
	{
		gint input_tokens = (gint)CLAMP(rpc->input_tokens, 0, G_MAXINT);
		gint output_tokens = (gint)CLAMP(rpc->output_tokens, 0, G_MAXINT);
		gint64 cost = codex_cost_micros(ai_cli_client_get_model(AI_CLI_CLIENT(self)),
		                                input_tokens, output_tokens);
		g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

		ai_response_set_usage(response, usage);
		if (cost >= 0)
			ai_response_set_cost_micros(response, cost);
		g_ptr_array_add(events, ai_event_new_usage(usage, cost));
	}

	if (g_str_equal(status, "completed"))
	{
		ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
		return TRUE;
	}
	if (g_str_equal(status, "interrupted"))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		                    "codex turn was interrupted");
		return FALSE;
	}
	codex_turn_error(ai_json_get_object(turn, "error"),
	                 self->last_error != NULL ? self->last_error : "Codex turn failed",
	                 error);
	return FALSE;
}

/*
 * codex_handle_notification:
 *
 * Translates one server notification into AiEvent values.  Text arrives
 * as real deltas (item/agentMessage/delta), unlike exec's whole
 * messages.  Notifications for another turn id (none are expected: one
 * process runs one turn) are ignored.
 *
 * Returns: %FALSE when the turn failed
 */
static gboolean
codex_handle_notification(AiCodexCliClient *self, AiCliSteerChannel *channel,
                          CodexRpc *rpc, JsonObject *obj, AiResponse *response,
                          GPtrArray *events, GError **error)
{
	const gchar *method = ai_json_get_string(obj, "method", "");
	JsonObject *params = ai_json_get_object(obj, "params");
	const gchar *turn_id = ai_json_get_string(params, "turnId", NULL);

	if (turn_id != NULL && rpc->turn_id != NULL && g_strcmp0(turn_id, rpc->turn_id) != 0)
		return TRUE;

	if (g_str_equal(method, "turn/started"))
	{
		const gchar *id = ai_json_get_string(ai_json_get_object(params, "turn"), "id", NULL);

		if (id != NULL && rpc->turn_id == NULL)
			rpc->turn_id = g_strdup(id);
		if (!rpc->stream_started)
		{
			rpc->stream_started = TRUE;
			ai_response_set_stop_reason(response, AI_STOP_REASON_NONE);
			g_ptr_array_add(events, ai_event_new(AI_EVENT_STREAM_START));
		}
	}
	else if (g_str_equal(method, "item/agentMessage/delta") ||
	         g_str_equal(method, "item/reasoning/summaryTextDelta") ||
	         g_str_equal(method, "item/reasoning/textDelta"))
	{
		const gchar *delta = ai_json_get_string(params, "delta", NULL);
		const gchar *item_id = ai_json_get_string(params, "itemId", NULL);

		if (delta == NULL || *delta == '\0') return TRUE;
		if (item_id != NULL)
			g_hash_table_add(rpc->streamed_items, g_strdup(item_id));
		if (g_str_equal(method, "item/agentMessage/delta"))
			g_ptr_array_add(events, ai_event_new_text_delta(delta));
		else
			g_ptr_array_add(events, ai_event_new_thinking_delta(delta));
	}
	else if (g_str_equal(method, "item/started") || g_str_equal(method, "item/completed"))
	{
		if (!codex_item_event(AI_CLI_CLIENT(self), params,
		                      g_str_equal(method, "item/completed"),
		                      rpc, response, events))
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
			                    "Malformed codex app-server item");
			return FALSE;
		}
	}
	else if (g_str_equal(method, "thread/tokenUsage/updated"))
	{
		JsonObject *last = ai_json_get_object(ai_json_get_object(params, "tokenUsage"), "last");

		if (last != NULL)
		{
			rpc->have_usage = TRUE;
			rpc->input_tokens += MAX(0, ai_json_get_int(last, "inputTokens", 0));
			rpc->output_tokens += MAX(0, ai_json_get_int(last, "outputTokens", 0));
		}
	}
	else if (g_str_equal(method, "error"))
	{
		/*
		 * As exec's top-level error: a retry notice or the fault that
		 * the turn's failure will repeat.  Kept so a run that ends with
		 * no turn/completed still says why.
		 */
		g_autofree gchar *text = codex_error_text(
			ai_json_get_string(ai_json_get_object(params, "error"), "message", NULL));

		if (text != NULL)
		{
			g_ptr_array_add(events, ai_event_new_status(text));
			if (!ai_json_get_boolean(params, "willRetry", FALSE))
			{
				g_free(self->last_error);
				self->last_error = g_steal_pointer(&text);
			}
		}
	}
	else if (g_str_equal(method, "warning"))
	{
		const gchar *text = ai_json_get_string(params, "message", NULL);

		if (text != NULL && *text != '\0')
			g_ptr_array_add(events, ai_event_new_status(text));
	}
	else if (g_str_equal(method, "turn/completed"))
	{
		JsonObject *turn = ai_json_get_object(params, "turn");
		const gchar *id = ai_json_get_string(turn, "id", NULL);

		if (rpc->turn_id != NULL && id != NULL && g_strcmp0(id, rpc->turn_id) != 0)
			return TRUE;
		return codex_turn_completed(self, channel, rpc, turn, response, events, error);
	}
	/* Everything else (plans, diffs, hooks, MCP startup, ...) is not an event. */
	return TRUE;
}

/*
 * parse_app_server_line:
 * @self: the Codex client
 * @line: one line codex wrote, leading space stripped
 * @response: the response being built
 * @events: (element-type AiEvent): where events go
 * @error: return location for the run's failure
 *
 * The parse_stream_events of the app-server transport.  Lines are told
 * apart the JSON-RPC way: method and id is a request to answer, method
 * alone a notification, id alone a response to one of ours.
 *
 * Returns: %FALSE when the run has failed
 */
static gboolean
parse_app_server_line(AiCodexCliClient *self, const gchar *line,
                      AiResponse *response, GPtrArray *events, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	AiCliSteerChannel *channel = ai_cli_steer_current();
	CodexRpc *rpc;
	JsonObject *obj;
	gboolean has_id;
	gboolean has_method;

	/* Without the open stdin there is no way to answer: say so plainly. */
	if (channel == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		                    "codex app-server needs a streamed run with stdin open");
		return FALSE;
	}
	rpc = ai_cli_steer_channel_get_data(channel);
	if (rpc == NULL)
	{
		rpc = g_steal_pointer(&self->pending_rpc);
		if (rpc == NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
			                    "codex app-server run has no plan");
			return FALSE;
		}
		ai_cli_steer_channel_set_data(channel, rpc, codex_rpc_free);
	}

	if (!json_parser_load_from_data(parser, line, -1, NULL) ||
	    (obj = ai_json_root_object(parser)) == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
		                    "Malformed codex app-server message");
		return FALSE;
	}
	has_id = ai_json_get_node(obj, "id") != NULL;
	has_method = ai_json_get_string(obj, "method", NULL) != NULL;

	if (has_method && has_id)
	{
		codex_answer_request(channel, rpc, obj, events);
		return TRUE;
	}
	if (has_method)
		return codex_handle_notification(self, channel, rpc, obj, response, events, error);
	if (has_id)
		return codex_handle_response(self, channel, rpc, obj, error);

	g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
	                    "Malformed codex app-server message");
	return FALSE;
}

/*
 * AiSteerable: a steer is a turn/steer request on the open stdin, naming
 * the turn codex is running.  Refused here, before anything is written,
 * while codex has not yet said which turn that is.
 */
static gchar *
codex_build_steer(
	AiCliClient        *client,
	AiCliSteerChannel  *channel,
	const gchar        *text,
	gchar             **ack_key,
	GError            **error
){
	CodexRpc *rpc = ai_cli_steer_channel_get_data(channel);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) params = NULL;
	gint64 id;

	(void)client;

	if (rpc == NULL || rpc->thread_id == NULL || rpc->turn_id == NULL)
	{
		g_set_error_literal(error, AI_STEER_ERROR, AI_STEER_ERROR_NOT_STEERABLE,
		                    "codex has not started the turn yet");
		return NULL;
	}
	if (rpc->turn_done)
	{
		g_set_error_literal(error, AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN,
		                    "the codex turn has ended");
		return NULL;
	}

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "threadId");
	json_builder_add_string_value(builder, rpc->thread_id);
	json_builder_set_member_name(builder, "expectedTurnId");
	json_builder_add_string_value(builder, rpc->turn_id);
	codex_rpc_add_input(builder, text, NULL);
	json_builder_end_object(builder);
	params = json_builder_get_root(builder);

	id = rpc->next_id++;
	*ack_key = g_strdup_printf("%" G_GINT64_FORMAT, id);
	return codex_rpc_request_line(id, "turn/steer", params);
}

static gboolean
codex_is_steerable(AiSteerable *steerable)
{
	return codex_uses_app_server(AI_CODEX_CLI_CLIENT(steerable));
}

static gchar *
codex_dup_turn_id(AiSteerable *steerable)
{
	return ai_cli_steer_dup_turn_id(AI_CLI_CLIENT(steerable));
}

static void
codex_steer_async(
	AiSteerable         *steerable,
	const gchar         *turn_id,
	const gchar         *text,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(steerable);

	/* app-server answers turn/steer at once, unlike claude's echo, so a
	 * bounded wait is right: silence past it is INDETERMINATE. */
	ai_cli_steer_async(AI_CLI_CLIENT(self), turn_id, text, codex_build_steer,
	                   self->steer_timeout_ms, cancellable, callback, user_data);
}

static AiSteerResult
codex_steer_finish(AiSteerable *steerable, GAsyncResult *result, GError **error)
{
	return ai_cli_steer_finish(AI_CLI_CLIENT(steerable), result, error);
}

/*
 * The class can steer; an instance can only on the app-server transport
 * (is_steerable).  ai_steerable_steer_async() refuses an exec client with
 * AI_STEER_ERROR_NOT_SUPPORTED before any of this runs.
 */
static void
steerable_init(AiSteerableInterface *iface)
{
	iface->can_steer    = TRUE;
	iface->is_steerable = codex_is_steerable;
	iface->dup_turn_id  = codex_dup_turn_id;
	iface->steer_async  = codex_steer_async;
	iface->steer_finish = codex_steer_finish;
}

typedef struct
{
	gboolean    done;
	AiResponse *response;
	GError     *error;
} CodexSyncRun;

static void
on_codex_sync_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	CodexSyncRun *run = user_data;

	run->response = ai_cli_client_stream_run_finish(AI_CLI_CLIENT(source), result,
	                                                &run->error);
	run->done = TRUE;
}

/*
 * codex_chat_sync:
 *
 * exec keeps the base's one-shot pipeline.  app-server cannot write its
 * whole input up front -- turn/start needs the thread id codex answers
 * with -- so the synchronous call runs the streamed protocol on a
 * private main context and waits for it.  A turn running this way is
 * steerable too: a steer from another thread reaches it through that
 * context.
 */
static AiResponse *
codex_chat_sync(AiCliClient *client, GList *messages, GCancellable *cancellable,
                GError **error)
{
	AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(client);
	GMainContext *context;
	CodexSyncRun run = { 0 };

	if (!codex_uses_app_server(self))
		return ai_cli_client_chat_sync_pipeline(client, messages, cancellable, error);

	g_clear_pointer(&self->last_error, g_free);
	context = g_main_context_new();
	g_main_context_push_thread_default(context);
	ai_cli_client_stream_run_async(client, messages, ai_cli_client_get_system_prompt(client),
	                               ai_cli_client_get_max_tokens(client), cancellable,
	                               on_codex_sync_done, &run);
	while (!run.done)
		g_main_context_iteration(context, TRUE);
	g_main_context_pop_thread_default(context);
	g_main_context_unref(context);

	if (run.response == NULL)
	{
		g_propagate_error(error, run.error);
		return NULL;
	}
	if (ai_response_get_stop_reason(run.response) != AI_STOP_REASON_END_TURN)
	{
		g_object_unref(run.response);
		set_unfinished_error(self, error);
		return NULL;
	}
	return run.response;
}

static void
ai_codex_cli_client_class_init(AiCodexCliClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiCliClientClass *cli = AI_CLI_CLIENT_CLASS(klass);
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->finalize = finalize;
    cli->check_exit_status = TRUE;
    cli->endpoint_applied = endpoint_applied;
    cli->endpoint_kinds = endpoint_kinds;
    cli->config_dir_env = codex_config_dir_env;
    ai_cli_limit_register(G_TYPE_FROM_CLASS(klass), "codex", codex_limit_patterns);
    cli->get_executable_path = get_executable;
    cli->build_argv = build_argv;
    cli->build_stdin = build_stdin;
    cli->parse_json_output = parse_output;
    cli->parse_stream_line = parse_line;
    cli->parse_stream_events = parse_events;
    cli->chat_sync = codex_chat_sync;
    properties[PROP_SANDBOX] = g_param_spec_string("sandbox", "Sandbox",
        "read-only, workspace-write, or danger-full-access; NULL uses read-only",
        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_SKIP_PERMISSIONS] = g_param_spec_boolean("skip-permissions", "Skip Permissions",
        "Bypass approvals and sandbox; conflicts with a restrictive explicit sandbox",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_CONTINUE_SESSION] = g_param_spec_boolean("continue-session", "Continue Session",
        "Resume the latest session when no session-id is set", FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_ADDITIONAL_DIRECTORIES] = g_param_spec_string("additional-directories", "Additional Directories",
        "Comma-separated additional writable directories (--add-dir)", NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_PROFILE] = g_param_spec_string("profile", "Profile",
        "Codex configuration profile", NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_SEARCH] = g_param_spec_boolean("search", "Search",
        "Enable live web search", FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    /**
     * AiCodexCliClient:transport:
     *
     * How codex is run: "exec" (the default) drives `codex exec --json`
     * and cannot be steered; "app-server" drives `codex app-server`
     * (JSON-RPC over stdio, one process per turn) and makes
     * ai_steerable_can_steer() %TRUE, steering a running turn with
     * `turn/steer`.  Any other value fails every turn with
     * %AI_ERROR_INVALID_REQUEST before codex is started.
     *
     * The app-server transport was written from codex's published
     * protocol (openai/codex rust-v0.169.0) and tested against a fake
     * server; see docs/steering.org for how to check it against a real
     * codex.
     */
    properties[PROP_TRANSPORT] = g_param_spec_string("transport", "Transport",
        "exec (default) or app-server; app-server is steerable",
        AI_CODEX_CLI_TRANSPORT_EXEC, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    /**
     * AiCodexCliClient:steer-timeout-ms:
     *
     * How long a steer waits for app-server to answer its `turn/steer`
     * request once the request is written.  Silence past it answers
     * %AI_STEER_INDETERMINATE: the request is out and may have been
     * taken.  0 waits until the run ends.
     */
    properties[PROP_STEER_TIMEOUT_MS] = g_param_spec_uint("steer-timeout-ms", "Steer Timeout",
        "Wait for app-server's answer to turn/steer, in milliseconds (0 = until the run ends)",
        0, G_MAXUINT, CODEX_STEER_TIMEOUT_MS_DEFAULT,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_codex_cli_client_init(AiCodexCliClient *self)
{
    ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_CODEX_CLI_DEFAULT_MODEL);
    self->steer_timeout_ms = CODEX_STEER_TIMEOUT_MS_DEFAULT;
    codex_set_transport(self, AI_CODEX_CLI_TRANSPORT_EXEC);
}

static AiProviderType get_provider_type(AiProvider *p) { (void)p; return AI_PROVIDER_CODEX_CLI; }
static const gchar *get_name(AiProvider *p) { (void)p; return "Codex CLI"; }
static const gchar *get_default_model(AiProvider *p) { (void)p; return AI_CODEX_CLI_DEFAULT_MODEL; }

/* Both asynchronous entry points share the base's cancellable, timeout-aware
 * subprocess pipeline. Codex always returns JSONL, even for buffered chat. */
static void
chat_async(AiProvider *p, GList *messages, const gchar *system, gint max_tokens,
           GList *tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    (void)tools;
    g_clear_pointer(&AI_CODEX_CLI_CLIENT(p)->last_error, g_free);
    ai_cli_client_stream_run_async(AI_CLI_CLIENT(p), messages, system, max_tokens, cancellable, callback, data);
}
static AiResponse *
chat_finish(AiProvider *p, GAsyncResult *result, GError **error)
{
    g_autoptr(AiResponse) response = ai_cli_client_stream_run_finish(AI_CLI_CLIENT(p), result, error);
    if (response != NULL && ai_response_get_stop_reason(response) != AI_STOP_REASON_END_TURN)
    {
        GError *local_error = NULL;

        set_unfinished_error(AI_CODEX_CLI_CLIENT(p), &local_error);
        ai_cli_limit_reclassify(AI_CLI_CLIENT(p), &local_error);
        g_propagate_error(error, local_error);
        return NULL;
    }
    return g_steal_pointer(&response);
}
static void
stream_async(AiStreamable *p, GList *messages, const gchar *system, gint max_tokens,
             GList *tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    chat_async(AI_PROVIDER(p), messages, system, max_tokens, tools, cancellable, callback, data);
}
static AiResponse *
stream_finish(AiStreamable *p, GAsyncResult *result, GError **error)
{
    return chat_finish(AI_PROVIDER(p), result, error);
}
static void free_models(gpointer data) { g_list_free_full(data, g_free); }
static void
list_models_async(AiProvider *provider, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    const gchar *models[] = { AI_CODEX_CLI_MODEL_GPT_6_ASTRA, AI_CODEX_CLI_MODEL_GPT_6_SOL,
        AI_CODEX_CLI_MODEL_GPT_6_LUNA, AI_CODEX_CLI_MODEL_GPT_5_6_SOL,
        AI_CODEX_CLI_MODEL_GPT_5_6_TERRA, AI_CODEX_CLI_MODEL_GPT_5_6_LUNA, AI_CODEX_CLI_MODEL_GPT_5_5,
        AI_CODEX_CLI_MODEL_GPT_5_4_MINI, AI_CODEX_CLI_MODEL_GPT_5_3_CODEX_SPARK };
    g_autoptr(GTask) task = g_task_new(provider, cancellable, callback, data);
    GList *list = NULL;
    guint i;
    for (i = 0; i < G_N_ELEMENTS(models); i++) list = g_list_append(list, g_strdup(models[i]));
    g_task_return_pointer(task, list, free_models);
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

/**
 * ai_codex_cli_client_new:
 *
 * Creates a Codex exec client using saved CLI authentication.
 * CODEX_PATH may override the executable found on PATH.
 *
 * Returns: (transfer full): a new #AiCodexCliClient
 */
AiCodexCliClient *ai_codex_cli_client_new(void)
{ return g_object_new(AI_TYPE_CODEX_CLI_CLIENT, NULL); }
/**
 * ai_codex_cli_client_new_with_config:
 * @config: an #AiConfig
 *
 * Returns: (transfer full): a new #AiCodexCliClient
 */
AiCodexCliClient *ai_codex_cli_client_new_with_config(AiConfig *config)
{ return g_object_new(AI_TYPE_CODEX_CLI_CLIENT, "config", config, NULL); }
/**
 * ai_codex_cli_client_get_sandbox:
 * @self: a Codex client
 * Returns: (nullable): the explicit sandbox policy, or NULL for read-only
 */
const gchar *ai_codex_cli_client_get_sandbox(AiCodexCliClient *self)
{ g_return_val_if_fail(AI_IS_CODEX_CLI_CLIENT(self), NULL); return self->sandbox; }
/**
 * ai_codex_cli_client_set_sandbox:
 * @self: a Codex client
 * @sandbox: (nullable): read-only, workspace-write, or danger-full-access
 *
 * Invalid policies or conflicts with skip-permissions fail before spawning.
 */
void ai_codex_cli_client_set_sandbox(AiCodexCliClient *self, const gchar *sandbox)
{ g_return_if_fail(AI_IS_CODEX_CLI_CLIENT(self)); g_object_set(self, "sandbox", sandbox, NULL); }
/**
 * ai_codex_cli_client_get_skip_permissions:
 * @self: a Codex client
 * Returns: whether approvals and sandbox are bypassed
 */
gboolean ai_codex_cli_client_get_skip_permissions(AiCodexCliClient *self)
{ g_return_val_if_fail(AI_IS_CODEX_CLI_CLIENT(self), FALSE); return self->skip_permissions; }
/**
 * ai_codex_cli_client_set_skip_permissions:
 * @self: a Codex client
 * @skip: whether to bypass approvals and sandbox
 */
void ai_codex_cli_client_set_skip_permissions(AiCodexCliClient *self, gboolean skip)
{ g_return_if_fail(AI_IS_CODEX_CLI_CLIENT(self)); g_object_set(self, "skip-permissions", skip, NULL); }
/**
 * ai_codex_cli_client_get_transport:
 * @self: a Codex client
 *
 * Returns: (transfer none): the transport, "exec" or "app-server"
 */
const gchar *
ai_codex_cli_client_get_transport(AiCodexCliClient *self)
{
	g_return_val_if_fail(AI_IS_CODEX_CLI_CLIENT(self), NULL);
	return self->transport;
}
/**
 * ai_codex_cli_client_set_transport:
 * @self: a Codex client
 * @transport: (nullable): %AI_CODEX_CLI_TRANSPORT_EXEC (the default, also
 *   chosen by %NULL) or %AI_CODEX_CLI_TRANSPORT_APP_SERVER
 *
 * Selects how codex is run; see #AiCodexCliClient:transport.  Set it
 * before a turn starts.  An unknown value fails the next turn with
 * %AI_ERROR_INVALID_REQUEST before codex is started.
 */
void
ai_codex_cli_client_set_transport(AiCodexCliClient *self, const gchar *transport)
{
	g_return_if_fail(AI_IS_CODEX_CLI_CLIENT(self));
	g_object_set(self, "transport", transport, NULL);
}
