/*
 * ai-gui-session.c - One conversation, plus everything the window shows about it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include <glib/gstdio.h>

#include "core/ai-json-util.h"

#include "ai-gui-content.h"
#include "ai-gui-session.h"
#include "ai-gui-work.h"

/*
 * Nothing in this file touches GTK, on purpose.
 *
 * Everything it does -- building a provider, queueing a follow-up,
 * persisting a session to disk -- is testable without a display, and
 * tests/test-ai-gui-session.c links it directly for exactly that reason.
 * A run that needed an X server would pass or fail by whose machine ran
 * it, which is the same trap the harness tests avoid by sandboxing HOME.
 */

struct _AiGuiSession
{
	GObject parent_instance;

	AiGuiOptions        *options;
	AiConversation      *conversation;
	AiResourceRegistry  *registry;
	AiCommandSet        *commands;
	AiCompletionContext *completion;
	AiPromptQueue       *queue;
	GCancellable        *cancellable;

	gchar   *id;
	gchar   *title;
	gchar   *provider_name;
	gchar   *provider_id;
	gchar   *model;
	gchar   *working_directory;
	gint64   created_at;
	gint64   updated_at;
	gboolean pinned;
	gboolean title_is_automatic;
	gboolean sending;
	gboolean approve_all;

	gulong busy_id;
	gulong activity_id;
	gulong approval_id;
	gulong agent_id;

	AiWorkSession *work;
	gchar         *project;
	gchar         *work_directory;
	gchar         *work_outcome;
	GSource       *heartbeat;
	GCancellable  *work_cancellable;
	gint           work_lock;
	gboolean       awaiting_approval;
};

G_DEFINE_FINAL_TYPE(AiGuiSession, ai_gui_session, G_TYPE_OBJECT)

enum
{
	PROP_0,
	PROP_TITLE,
	PROP_PROVIDER_NAME,
	PROP_MODEL,
	PROP_WORKING_DIRECTORY,
	PROP_PROJECT,
	PROP_BUSY,
	PROP_ACTIVITY,
	PROP_QUEUED,
	PROP_UPDATED_AT,
	PROP_PINNED,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
	SIGNAL_APPROVAL_REQUESTED,
	SIGNAL_TURN_FINISHED,
	SIGNAL_AGENT_FINISHED,
	SIGNAL_BUILTIN,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static void session_pump_queue(AiGuiSession *self);

/* ================================================================
 * Provider construction
 * ================================================================ */

/*
 * Apply every `--set` to @provider.
 *
 * A name the provider does not have is an error rather than a silent
 * no-op, for the reason `ai --set` gives: a misspelled knob that is
 * quietly ignored looks exactly like one that had no effect.
 */
static gboolean
session_apply_overrides(
	GObject      *provider,
	gchar       **sets,
	GError      **error
){
	gsize i;

	if (sets == NULL)
		return TRUE;

	for (i = 0; sets[i] != NULL; i++)
	{
		GValue value = G_VALUE_INIT;
		g_autofree gchar *name = NULL;
		const gchar *text;
		const gchar *eq;
		GParamSpec *pspec;

		eq = strchr(sets[i], '=');

		if (eq != NULL)
		{
			name = g_strndup(sets[i], (gsize)(eq - sets[i]));
			text = eq + 1;
		}
		else
		{
			/* A bare `--set NAME` is a boolean switch, as in `ai`. */
			name = g_strdup(sets[i]);
			text = "true";
		}

		g_strstrip(name);

		pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
		                                     name);

		if (pspec == NULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "no property '%s' on this provider", name);
			return FALSE;
		}

		if ((pspec->flags & G_PARAM_WRITABLE) == 0)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "property '%s' is read-only", name);
			return FALSE;
		}

		if (!ai_gui_value_from_string(&value, pspec, text))
		{
			g_value_unset(&value);
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "cannot parse '%s' for property '%s'", text, name);
			return FALSE;
		}

		g_object_set_property(provider, name, &value);
		g_value_unset(&value);
	}

	return TRUE;
}

/*
 * Build a provider the way ai-tui does.
 *
 * @initial says whether this is the session's first provider. Only then
 * do the saved application defaults, --continue and the system prompt
 * apply: a mid-session switch starts the new provider native, because
 * ai_conversation_set_provider() is what carries the context across and
 * a resumed native session on the far side would be a second, conflicting
 * history.
 */
static GObject *
session_build_provider(
	const AiGuiOptions  *options,
	const gchar         *provider_name,
	const gchar         *model,
	gboolean             initial,
	gchar              **out_model,
	GError             **error
){
	g_autoptr(AiConfig) config = ai_config_new();
	g_autofree gchar *resolved_model = NULL;
	AiProviderType type;
	GObject *provider;

	if (initial)
	{
		if (!ai_provider_factory_resolve_defaults(
			config, "ai-gui",
			provider_name != NULL ? provider_name : "default",
			model != NULL ? model : "default",
			&type, &resolved_model, error))
		{
			return NULL;
		}

		provider = ai_provider_factory_new(type, config, error);
	}
	else
	{
		provider = ai_provider_factory_new_from_string(provider_name, config,
		                                              error);
		resolved_model = g_strdup(model);
	}

	if (provider == NULL)
		return NULL;

	if (AI_IS_CLIENT(provider))
	{
		AiClient *client = AI_CLIENT(provider);

		if (resolved_model != NULL && *resolved_model != '\0')
			ai_client_set_model(client, resolved_model);

		if (initial && options->system != NULL)
			ai_client_set_system_prompt(client, options->system);

		ai_client_set_max_tokens(client, options->max_tokens);
	}
	else if (AI_IS_CLI_CLIENT(provider))
	{
		AiCliClient *client = AI_CLI_CLIENT(provider);

		/*
		 * A desktop window has no deadline of its own, so the library's
		 * 30-minute process bound is the wrong one to keep: a long
		 * agentic run that is still printing is not a hung child.
		 */
		ai_cli_client_set_process_timeout_ms(client, 0);

		if (resolved_model != NULL && *resolved_model != '\0')
			ai_cli_client_set_model(client, resolved_model);

		if (initial && options->system != NULL)
			ai_cli_client_set_system_prompt(client, options->system);

		if (options->effort != NULL)
			ai_cli_client_set_effort_level(client, options->effort);

		ai_cli_client_set_max_tokens(client, options->max_tokens);
	}

	if (initial && options->continue_session &&
	    g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
	                                 "continue-session") != NULL)
	{
		g_object_set(provider, "continue-session", TRUE, NULL);
	}

	if (options->skip_permissions &&
	    g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
	                                 "skip-permissions") != NULL)
	{
		g_object_set(provider, "skip-permissions", TRUE, NULL);
	}

	if (!session_apply_overrides(provider, options->sets, error))
	{
		g_object_unref(provider);
		return NULL;
	}

	if (out_model != NULL)
	{
		if (AI_IS_CLIENT(provider))
			*out_model = g_strdup(ai_client_get_model(AI_CLIENT(provider)));
		else if (AI_IS_CLI_CLIENT(provider))
			*out_model = g_strdup(ai_cli_client_get_model(AI_CLI_CLIENT(provider)));
		else
			*out_model = g_strdup(resolved_model);
	}

	return provider;
}

/* ================================================================
 * Signal plumbing
 * ================================================================ */

/*
 * First handler with an opinion wins.
 *
 * Not g_signal_accumulator_first_wins, which halts after the first
 * handler whatever it returned -- a window that answered DEFAULT would
 * then veto every handler behind it. This is the same accumulator the
 * library's own approval signal uses, for the same reason.
 */
static gboolean
approval_accumulator(
	GSignalInvocationHint *hint,
	GValue                *accumulated,
	const GValue          *handler_return,
	gpointer               data
){
	gint answer = g_value_get_int(handler_return);

	if (answer == AI_TOOL_APPROVAL_DEFAULT)
		return TRUE;

	g_value_set_int(accumulated, answer);
	return FALSE;
}

static AiToolApproval
on_approval_requested(
	AiConversation *conversation,
	AiToolUse      *tool_use,
	gpointer        user_data
){
	AiGuiSession *self = user_data;
	gint answer = AI_TOOL_APPROVAL_DEFAULT;

	if (self->approve_all || self->options->skip_permissions)
		return AI_TOOL_APPROVAL_ALLOW;

	/*
	 * INPUT on the dashboard, for as long as somebody is being asked.
	 *
	 * Published before the handler runs because the handler blocks on a
	 * dialog: waiting until it returned would raise the flag only after
	 * the question had already been answered.
	 */
	self->awaiting_approval = TRUE;
	ai_gui_session_publish_work(self);

	g_signal_emit(self, signals[SIGNAL_APPROVAL_REQUESTED], 0, tool_use,
	              &answer);

	self->awaiting_approval = FALSE;
	ai_gui_session_publish_work(self);

	return (AiToolApproval)answer;
}

static void
on_agent_finished(
	AiConversation *conversation,
	const gchar    *agent_id,
	gint            state,
	gpointer        user_data
){
	g_signal_emit(user_data, signals[SIGNAL_AGENT_FINISHED], 0, agent_id,
	              state);
}

static void
on_busy_notify(
	GObject    *conversation,
	GParamSpec *pspec,
	gpointer    user_data
){
	g_object_notify_by_pspec(user_data, properties[PROP_BUSY]);
	ai_gui_session_publish_work(user_data);
}

static void
on_activity_notify(
	GObject    *conversation,
	GParamSpec *pspec,
	gpointer    user_data
){
	g_object_notify_by_pspec(user_data, properties[PROP_ACTIVITY]);
	ai_gui_session_publish_work(user_data);
}

static void
session_connect_conversation(AiGuiSession *self)
{
	self->approval_id = g_signal_connect(self->conversation,
		"approval-requested", G_CALLBACK(on_approval_requested), self);
	self->agent_id = g_signal_connect(self->conversation, "agent-finished",
		G_CALLBACK(on_agent_finished), self);
	self->busy_id = g_signal_connect(self->conversation, "notify::busy",
		G_CALLBACK(on_busy_notify), self);
	self->activity_id = g_signal_connect(self->conversation, "notify::activity",
		G_CALLBACK(on_activity_notify), self);
}

/* ================================================================
 * The dashboard record
 * ================================================================ */

/*
 * What this session looks like from the dashboard.
 *
 * Every field the registry keeps is derived here rather than cached as
 * it changes: the sources of truth are the conversation, the executor
 * and the brigade, and a second copy kept in step by hand is a second
 * copy that eventually is not.
 */
void
ai_gui_session_publish_work(AiGuiSession *self)
{
	GObject *provider;
	AiToolExecutor *executor;
	AiBrigade *brigade;
	g_autoptr(GList) agents = NULL;
	g_autofree gchar *native = NULL;
	g_autofree gchar *activity = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *label;
	GList *iter;
	guint active = 0;
	guint completed = 0;
	guint total;
	guint i;
	gboolean attention;

	g_return_if_fail(AI_GUI_IS_SESSION(self));

	if (self->work == NULL)
		return;

	provider = ai_conversation_get_provider(self->conversation);
	executor = ai_conversation_get_executor(self->conversation);
	brigade = ai_conversation_get_brigade(self->conversation);
	attention = self->awaiting_approval;

	if (brigade != NULL)
		agents = ai_brigade_list(brigade);

	for (iter = agents; iter != NULL; iter = iter->next)
	{
		AiAgentState state = ai_agent_get_state(iter->data);

		if (state >= AI_AGENT_STATE_QUEUED && state <= AI_AGENT_STATE_BLOCKED)
			active++;

		if (state == AI_AGENT_STATE_WAITING_INPUT)
			attention = TRUE;
	}

	total = ai_tool_executor_get_n_todos(executor);

	for (i = 0; i < total; i++)
	{
		AiTodoState state;

		ai_tool_executor_get_todo_fields(executor, i, NULL, &state);

		if (state == AI_TODO_COMPLETED)
			completed++;
	}

	if (provider != NULL &&
	    g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
	                                 "session-id") != NULL)
	{
		g_object_get(provider, "session-id", &native, NULL);
	}

	label = ai_conversation_get_activity(self->conversation);
	activity = g_strdup_printf("%s / %u background / %u/%u todos / %"
		G_GINT64_FORMAT "s",
		label != NULL && *label != '\0' ? label : "Ready", active,
		completed, total,
		ai_conversation_get_activity_elapsed(self->conversation)
			/ G_USEC_PER_SEC);

	g_object_set(self->work,
		"provider", self->provider_id != NULL ? self->provider_id : "",
		"model", self->model != NULL ? self->model : "",
		"provider-session", native != NULL ? native : "",
		"activity", activity,
		"title", self->title != NULL ? self->title : "",
		NULL);

	ai_work_session_update(self->work,
		self->sending || ai_conversation_get_busy(self->conversation) ||
			ai_prompt_queue_get_length(self->queue) != 0,
		attention, active, self->work_outcome);

	if (!ai_work_session_save(self->work, self->work_directory, TRUE, &error))
	{
		/*
		 * g_debug: a state directory that will not take a write is the
		 * machine, not a bug here, and the dashboard degrades to showing
		 * this session as disconnected rather than failing a turn.
		 */
		g_debug("ai-gui: cannot publish the dashboard record: %s",
		        error->message);
	}
}

void
ai_gui_session_release_work(AiGuiSession *self)
{
	g_return_if_fail(AI_GUI_IS_SESSION(self));

	if (self->work == NULL)
		return;

	/* Metadata is kept, liveness is not: the row stays visible and
	 * resumable rather than vanishing when a window closes. */
	ai_work_session_save(self->work, self->work_directory, FALSE, NULL);
}

static gboolean
on_heartbeat(gpointer user_data)
{
	ai_gui_session_publish_work(user_data);
	return G_SOURCE_CONTINUE;
}

static void
session_start_heartbeat(AiGuiSession *self)
{
	if (self->heartbeat != NULL)
		return;

	/*
	 * Three seconds, against the registry's fifteen-second expiry.
	 *
	 * Held as a #GSource and destroyed with g_source_destroy(), never by
	 * id — see the main-context note in AGENTS.md for why an id from one
	 * context names a different source in another.
	 */
	self->heartbeat = g_timeout_source_new_seconds(3);
	g_source_set_callback(self->heartbeat, on_heartbeat, self, NULL);
	g_source_attach(self->heartbeat, g_main_context_get_thread_default());
}

/*
 * The project identity, with the notify the sidebar re-groups on.
 *
 * Every path that can change it goes through here so there is exactly
 * one place that decides what "never NULL, never empty" means: a session
 * whose group heading vanished would take its row out of the list.
 */
static void
session_set_project(
	AiGuiSession *self,
	const gchar  *project
){
	const gchar *value = project != NULL && *project != '\0'
		? project : self->working_directory;

	if (g_strcmp0(self->project, value) == 0)
		return;

	g_free(self->project);
	self->project = g_strdup(value != NULL ? value : "");

	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PROJECT]);
}

static void
on_work_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiSession *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiWorkSession) work = ai_gui_work_new_finish(result, &error);

	if (work == NULL)
	{
		g_debug("ai-gui: no dashboard record for this session: %s",
		        error != NULL ? error->message : "cancelled");
		g_object_unref(self);
		return;
	}

	if (self->work == NULL)
	{
		g_autoptr(GError) claim_error = NULL;

		self->work_lock = ai_gui_work_claim(self->work_directory,
			ai_work_session_get_id(work), &claim_error);
		self->work = g_steal_pointer(&work);
		g_object_set(self->conversation, "work-session", self->work, NULL);
		session_start_heartbeat(self);
	}
	else
	{
		/*
		 * A second answer for a session that already has a record: the
		 * directory changed under it, so the record moves rather than a
		 * second identity appearing beside it. The id, the links and the
		 * title somebody attached are all in the record being kept.
		 */
		g_object_set(self->work,
			"directory", ai_work_session_get_field(work, "directory"),
			"project", ai_work_session_get_field(work, "project"),
			"branch", ai_work_session_get_field(work, "branch"),
			NULL);
	}

	if (self->work_lock < 0)
	{
		self->work_lock = ai_gui_work_claim(self->work_directory,
			ai_work_session_get_id(self->work), NULL);
		session_start_heartbeat(self);
	}

	ai_gui_session_publish_work(self);
	session_set_project(self,
		ai_work_session_get_field(self->work, "project"));

	g_object_unref(self);
}

void
ai_gui_session_set_work_directory(
	AiGuiSession *self,
	const gchar  *directory
){
	g_return_if_fail(AI_GUI_IS_SESSION(self));

	g_free(self->work_directory);
	self->work_directory = directory != NULL ? g_strdup(directory)
		: ai_work_session_default_directory();
}

AiWorkSession *
ai_gui_session_get_work(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->work;
}

gboolean
ai_gui_session_adopt_work(
	AiGuiSession  *self,
	AiWorkSession *work,
	GError       **error
){
	gint lock;

	g_return_val_if_fail(AI_GUI_IS_SESSION(self), FALSE);
	g_return_val_if_fail(AI_IS_WORK_SESSION(work), FALSE);

	lock = ai_gui_work_claim(self->work_directory,
	                         ai_work_session_get_id(work), error);

	if (lock < 0)
		return FALSE;

	/*
	 * The adopted record replaces whatever registration was in flight.
	 * Cancelling it first is what stops the worker from installing a
	 * second identity a moment later and orphaning this one.
	 */
	g_cancellable_cancel(self->work_cancellable);

	if (self->work_lock >= 0)
		g_close(self->work_lock, NULL);

	self->work_lock = lock;
	g_set_object(&self->work, work);
	g_object_set(self->conversation, "work-session", self->work, NULL);

	/* A recovered record must never keep a previous terminal's target:
	 * this session is a window, not that pane. */
	g_object_set(self->work, "socket", "", "pane", "", NULL);

	session_start_heartbeat(self);
	ai_gui_session_publish_work(self);
	session_set_project(self,
		ai_work_session_get_field(self->work, "project"));

	return TRUE;
}

/* ================================================================
 * Construction
 * ================================================================ */

static void
session_touch(AiGuiSession *self)
{
	self->updated_at = g_get_real_time() / G_USEC_PER_SEC;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_UPDATED_AT]);
}

/*
 * The harness layer, built even when --no-expand was given.
 *
 * /help and the listings are how somebody works out why their file is not
 * being found, and that is exactly the moment they will have turned
 * expansion off. ai-tui makes the same call.
 */
static void
session_build_harness(AiGuiSession *self)
{
	self->registry = ai_resource_registry_new();
	ai_resource_registry_set_working_directory(self->registry,
	                                           self->working_directory);
	ai_resource_registry_scan(self->registry);
	ai_resource_registry_set_watching(self->registry, TRUE);

	self->commands = ai_command_set_new(self->registry);
	self->completion = ai_completion_context_new(self->commands,
	                                             self->working_directory);

	ai_conversation_set_command_set(self->conversation, self->commands);
	ai_conversation_set_working_directory(self->conversation,
	                                      self->working_directory);
}

/*
 * Ask for this directory's project identity, off the main thread.
 *
 * Run again whenever the working directory changes: ai_work_session_new()
 * shells out to git twice and its own documentation says to keep that off
 * a UI thread, which is the whole reason the sidebar cannot simply
 * compute the group it is drawing.
 *
 * The reference is held for the call. A session closed while git is still
 * answering must still have somewhere for the result to land.
 */
static void
session_register_work(AiGuiSession *self)
{
	ai_gui_work_new_async(self->working_directory, self->work_cancellable,
	                      on_work_ready, g_object_ref(self));
}

static void
session_configure(AiGuiSession *self)
{
	ai_conversation_set_stream(self->conversation, self->options->stream);
	ai_conversation_set_max_tokens(self->conversation,
	                               self->options->max_tokens);

	if (self->options->system != NULL)
		ai_conversation_set_system_prompt(self->conversation,
		                                  self->options->system);

	if (self->options->local_tools)
		ai_conversation_set_local_tools(self->conversation, TRUE);

	session_build_harness(self);

	if (self->options->agents)
		ai_conversation_enable_background_agents(self->conversation,
		                                         AI_GUI_AGENT_MAX);

	session_connect_conversation(self);

	session_register_work(self);
}

AiGuiSession *
ai_gui_session_new(
	const AiGuiOptions  *options,
	const gchar         *provider_name,
	const gchar         *model,
	GError             **error
){
	g_autoptr(AiGuiSession) self = NULL;
	g_autofree gchar *resolved_model = NULL;
	GObject *provider;

	g_return_val_if_fail(options != NULL, NULL);

	provider = session_build_provider(options, provider_name, model, TRUE,
	                                  &resolved_model, error);

	if (provider == NULL)
		return NULL;

	self = g_object_new(AI_GUI_TYPE_SESSION, NULL);
	self->options = ai_gui_options_copy(options);
	self->approve_all = options->approve_all;
	self->conversation = ai_conversation_new(provider);
	self->provider_name = g_strdup(ai_provider_get_name(AI_PROVIDER(provider)));
	self->provider_id = g_strdup(ai_provider_type_to_string(
		ai_provider_get_provider_type(AI_PROVIDER(provider))));
	self->model = g_steal_pointer(&resolved_model);
	self->working_directory = options->working_directory != NULL
		? g_strdup(options->working_directory)
		: g_get_current_dir();
	self->created_at = g_get_real_time() / G_USEC_PER_SEC;
	self->updated_at = self->created_at;

	g_object_unref(provider);

	session_configure(self);

	return g_steal_pointer(&self);
}

/* ================================================================
 * Persistence
 * ================================================================ */

/*
 * What a restored block is, and is not.
 *
 * A transcript reloaded from disk is a *record* of a session, not a
 * resumption of it: the portable AiMessage history lives inside
 * AiConversation and has no setter, deliberately. Restoring these blocks
 * therefore shows a person what happened without telling the model it
 * happened -- and where the provider can genuinely resume (a CLI with a
 * session id), it is the provider's own store that carries the context,
 * which is the mechanism AiNativeSession already documents.
 *
 * Tool groups come back as status lines rather than live tool blocks.
 * A restored group that pretended to be live would offer to expand
 * arguments that were never saved.
 */
static const gchar *
block_kind_name(AiViewBlockKind kind)
{
	switch (kind)
	{
		case AI_VIEW_BLOCK_TURN:     return "turn";
		case AI_VIEW_BLOCK_TEXT:     return "text";
		case AI_VIEW_BLOCK_THINKING: return "thinking";
		case AI_VIEW_BLOCK_TOOL:     return "tool";
		case AI_VIEW_BLOCK_TODO:     return "todo";
		case AI_VIEW_BLOCK_AGENT:    return "agent";
		case AI_VIEW_BLOCK_STATUS:   return "status";
		default:                     return "status";
	}
}

/*
 * The block's own text where it has one, its rendering where it does not.
 *
 * Rendering everything would be simpler and wrong for the three kinds
 * that carry their words directly: a turn renders as "> what you typed",
 * and restoring *that* through ai_view_turn_block_new() produces
 * "> > what you typed", one arrow deeper on every save-and-restore
 * cycle. A tool group has no source text -- its rendering is the only
 * thing there is -- so those still go through the renderer.
 */
static gchar *
block_source_text(AiViewBlock *block)
{
	if (AI_IS_VIEW_TURN_BLOCK(block))
		return g_strdup(ai_view_turn_block_get_text(AI_VIEW_TURN_BLOCK(block)));

	if (AI_IS_VIEW_TEXT_BLOCK(block))
		return g_strdup(ai_view_text_block_get_text(AI_VIEW_TEXT_BLOCK(block)));

	if (AI_IS_VIEW_THINKING_BLOCK(block))
	{
		return g_strdup(ai_view_thinking_block_get_text(
			AI_VIEW_THINKING_BLOCK(block)));
	}

	if (AI_IS_VIEW_STATUS_BLOCK(block))
	{
		return g_strdup(ai_view_status_block_get_text(
			AI_VIEW_STATUS_BLOCK(block)));
	}

	return ai_view_block_render_text(block, 0);
}

JsonNode *
ai_gui_session_to_json(AiGuiSession *self)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	AiTranscript *transcript;
	guint i;
	guint n_blocks;

	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);

	transcript = ai_conversation_get_transcript(self->conversation);
	n_blocks = ai_transcript_get_n_blocks(transcript);

	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, self->id);
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, self->title);
	/*
	 * The canonical name, never the display name.
	 *
	 * ai_provider_get_name() answers "Claude Code"; the factory wants
	 * "claude-code". Saving the display name is the same mistake the
	 * native-session reader table documents -- it works for the
	 * providers whose two names happen to coincide and silently stops
	 * matching for the ones that do not, with the symptom being a
	 * restored session that quietly comes back on the default provider.
	 */
	json_builder_set_member_name(builder, "provider");
	json_builder_add_string_value(builder, self->provider_id);
	json_builder_set_member_name(builder, "provider-display");
	json_builder_add_string_value(builder, self->provider_name);
	json_builder_set_member_name(builder, "model");
	json_builder_add_string_value(builder, self->model != NULL ? self->model : "");
	json_builder_set_member_name(builder, "working-directory");
	json_builder_add_string_value(builder, self->working_directory);
	/*
	 * Saved so a reopened window groups the sidebar correctly on the
	 * first frame. Deriving it instead would mean every restored session
	 * sat under its own directory until two git subprocesses per session
	 * had finished, which is exactly the moment somebody is looking for
	 * the project they were last in.
	 */
	json_builder_set_member_name(builder, "project");
	json_builder_add_string_value(builder,
	                              ai_gui_session_get_project(self));
	json_builder_set_member_name(builder, "created");
	json_builder_add_int_value(builder, self->created_at);
	json_builder_set_member_name(builder, "updated");
	json_builder_add_int_value(builder, self->updated_at);
	json_builder_set_member_name(builder, "pinned");
	json_builder_add_boolean_value(builder, self->pinned);
	json_builder_set_member_name(builder, "automatic-title");
	json_builder_add_boolean_value(builder, self->title_is_automatic);

	/*
	 * The provider's own session id, when it has one.
	 *
	 * This is the only part of a saved session that can actually be
	 * resumed: reopening sets it back, and the wrapped CLI brings its own
	 * history along. Without it a reopened session is a transcript to
	 * read, which is worth saying plainly rather than implying otherwise.
	 */
	json_builder_set_member_name(builder, "session-id");
	{
		GObject *provider = ai_conversation_get_provider(self->conversation);
		g_autofree gchar *session_id = NULL;

		if (provider != NULL &&
		    g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
		                                 "session-id") != NULL)
		{
			g_object_get(provider, "session-id", &session_id, NULL);
		}

		json_builder_add_string_value(builder,
		                              session_id != NULL ? session_id : "");
	}

	/* Persist associations separately from the transcript and fetched text. */
	if (self->work != NULL)
	{
		g_autofree gchar *manifest = ai_work_session_dup_link_manifest(self->work);
		g_autoptr(JsonParser) parser = json_parser_new();
		json_parser_load_from_data(parser, manifest, -1, NULL);
		json_builder_set_member_name(builder, "work-links");
		json_builder_add_value(builder, json_node_copy(json_parser_get_root(parser)));
	}

	json_builder_set_member_name(builder, "blocks");
	json_builder_begin_array(builder);

	for (i = 0; i < n_blocks; i++)
	{
		AiViewBlock *block = ai_transcript_get_block(transcript, i);
		g_autofree gchar *text = NULL;

		if (block == NULL)
			continue;

		text = block_source_text(block);

		if (text == NULL || *text == '\0')
			continue;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder,
			block_kind_name(ai_view_block_get_kind(block)));
		json_builder_set_member_name(builder, "text");
		json_builder_add_string_value(builder, text);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* A restored block, complete and never streamed into again. */
static void
session_restore_block(
	AiGuiSession *self,
	JsonObject   *record
){
	const gchar *kind = ai_json_get_string(record, "kind", "status");
	const gchar *text = ai_json_get_string(record, "text", NULL);
	AiViewBlock *block = NULL;

	if (text == NULL || *text == '\0')
		return;

	if (g_strcmp0(kind, "turn") == 0)
	{
		block = ai_view_turn_block_new(text);
	}
	else if (g_strcmp0(kind, "text") == 0)
	{
		block = ai_view_text_block_new();
		ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block), text);
	}
	else if (g_strcmp0(kind, "thinking") == 0)
	{
		block = ai_view_thinking_block_new();
		ai_view_thinking_block_append(AI_VIEW_THINKING_BLOCK(block), text);
	}
	else
	{
		block = ai_view_status_block_new(AI_VIEW_STATUS_INFO, text);
	}

	ai_view_block_set_complete(block, TRUE);
	ai_transcript_append(ai_conversation_get_transcript(self->conversation),
	                     block);
	g_object_unref(block);
}

AiGuiSession *
ai_gui_session_new_from_json(
	JsonObject          *object,
	const AiGuiOptions  *options,
	GError             **error
){
	g_autoptr(AiGuiSession) self = NULL;
	g_autoptr(AiGuiOptions) merged = NULL;
	g_autofree gchar *resolved_model = NULL;
	const gchar *provider_name;
	const gchar *model;
	const gchar *session_id;
	const gchar *directory;
	GObject *provider;
	JsonArray *blocks;

	g_return_val_if_fail(object != NULL, NULL);
	g_return_val_if_fail(options != NULL, NULL);

	provider_name = ai_json_get_string(object, "provider", NULL);
	model = ai_json_get_string(object, "model", NULL);
	session_id = ai_json_get_string(object, "session-id", NULL);
	directory = ai_json_get_string(object, "working-directory", NULL);

	/*
	 * A saved session names its own provider, model and directory; the
	 * rest of the options are the ones this run was started with. Saving
	 * the whole option set instead would mean a session reopened months
	 * later quietly re-enabled --skip-permissions because it was on once.
	 */
	merged = ai_gui_options_copy(options);
	g_clear_pointer(&merged->provider, g_free);
	g_clear_pointer(&merged->model, g_free);
	merged->provider = g_strdup(provider_name);
	merged->model = g_strdup(model);

	if (directory != NULL && *directory != '\0')
	{
		g_clear_pointer(&merged->working_directory, g_free);
		merged->working_directory = g_strdup(directory);
	}

	/*
	 * A saved session id beats --continue: they are two answers to "which
	 * session", and the explicit one is the one somebody clicked on.
	 */
	if (session_id != NULL && *session_id != '\0')
		merged->continue_session = FALSE;

	provider = session_build_provider(merged, merged->provider, merged->model,
	                                  TRUE, &resolved_model, error);

	if (provider == NULL)
		return NULL;

	if (session_id != NULL && *session_id != '\0' &&
	    g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
	                                 "session-id") != NULL)
	{
		g_object_set(provider, "session-id", session_id, NULL);
	}

	self = g_object_new(AI_GUI_TYPE_SESSION, NULL);
	self->options = g_steal_pointer(&merged);
	self->approve_all = options->approve_all;
	self->conversation = ai_conversation_new(provider);
	self->provider_name = g_strdup(ai_provider_get_name(AI_PROVIDER(provider)));
	self->provider_id = g_strdup(ai_provider_type_to_string(
		ai_provider_get_provider_type(AI_PROVIDER(provider))));
	self->model = g_steal_pointer(&resolved_model);
	self->working_directory = self->options->working_directory != NULL
		? g_strdup(self->options->working_directory)
		: g_get_current_dir();

	g_object_unref(provider);

	g_free(self->id);
	self->id = g_strdup(ai_json_get_string(object, "id", NULL));

	if (self->id == NULL || *self->id == '\0')
	{
		g_free(self->id);
		self->id = g_uuid_string_random();
	}

	g_free(self->title);
	self->title = g_strdup(ai_json_get_string(object, "title", "Session"));
	self->created_at = ai_json_get_int(object, "created", 0);
	self->updated_at = ai_json_get_int(object, "updated", self->created_at);
	self->pinned = ai_json_get_boolean(object, "pinned", FALSE);
	self->title_is_automatic = ai_json_get_boolean(object, "automatic-title",
	                                               FALSE);
	/*
	 * The saved answer, until the registration this session is about to
	 * start replaces it. A file written before this key existed has none,
	 * and the getter's fallback to the working directory covers it.
	 */
	self->project = g_strdup(ai_json_get_string(object, "project", NULL));

	/* Install persisted references synchronously, before a resumed turn can
	 * start. Git/project registration remains asynchronous. */
	{
		JsonArray *links = ai_json_get_array(object, "work-links");
		guint i;
		if (links != NULL)
		{
			self->work = g_object_new(AI_TYPE_WORK_SESSION, "directory", self->working_directory,
				"project", ai_gui_session_get_project(self), NULL);
			for (i = 0; i < MIN(32, json_array_get_length(links)); i++)
			{
				JsonObject *link = ai_json_array_get_object(links, i);
				ai_work_session_add_link_full(self->work, ai_json_get_string(link, "url", NULL),
					ai_json_get_string(link, "relationship", "related"), NULL);
			}
			g_object_set(self->conversation, "work-session", self->work, NULL);
		}
	}

	session_configure(self);

	blocks = ai_json_get_array(object, "blocks");

	if (blocks != NULL)
	{
		guint i;
		guint n = json_array_get_length(blocks);

		for (i = 0; i < n; i++)
		{
			JsonObject *record = ai_json_array_get_object(blocks, i);

			if (record != NULL)
				session_restore_block(self, record);
		}

		if (n > 0)
		{
			g_autoptr(AiViewBlock) note = NULL;

			note = ai_view_status_block_new(AI_VIEW_STATUS_INFO,
				session_id != NULL && *session_id != '\0'
					? "Restored from disk; the provider's own session was resumed."
					: "Restored from disk for reading. The model has not been told about it.");
			ai_view_block_set_complete(note, TRUE);
			ai_transcript_append(
				ai_conversation_get_transcript(self->conversation), note);
		}
	}

	return g_steal_pointer(&self);
}

/* ================================================================
 * Sending
 * ================================================================ */

static void
on_send_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiSession *self = user_data;
	g_autoptr(AiCommandResult) command = NULL;
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = ai_conversation_send_input_images_finish(AI_CONVERSATION(source),
	                                              result, &command, &error);

	/*
	 * :busy is the disjunction of this flag and the conversation's own,
	 * so it has to be notified from here too.
	 *
	 * The conversation clears its half first and notifies then -- while
	 * this flag is still set -- so a frontend watching only that
	 * notification sees "still working" and never hears otherwise. The
	 * symptom is a stop button that stays red after the answer arrives.
	 */
	self->sending = FALSE;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BUSY]);

	/*
	 * DONE means this turn finished, never that the task is finished.
	 * A cancelled turn is STOPPED rather than ERROR: the person who
	 * pressed stop did not hit a failure.
	 */
	if (command == NULL)
	{
		/*
		 * Only a real turn sets an outcome. A /help that reported DONE
		 * would tell the dashboard a model had just finished work.
		 */
		g_free(self->work_outcome);
		self->work_outcome = g_strdup(ok ? "DONE"
			: (error != NULL ? "ERROR" : "STOPPED"));
	}

	/*
	 * A cancelled turn is not a failure worth a dialog: the person who
	 * pressed stop already knows. Everything else is reported.
	 */
	if (!ok && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_clear_error(&error);

	session_touch(self);

	/*
	 * A built-in resolved instead of a turn.
	 *
	 * ai_conversation_send_input_images_finish() hands back a command
	 * and sends nothing, so a frontend that only looked at the boolean
	 * would swallow every /help in silence -- which is exactly what this
	 * one did before the signal existed.
	 */
	if (command != NULL)
		g_signal_emit(self, signals[SIGNAL_BUILTIN], 0, command);
	else
		g_signal_emit(self, signals[SIGNAL_TURN_FINISHED], 0, ok,
		              error != NULL ? error->message : NULL);

	/*
	 * Pump before dropping the turn's reference: the queue entry takes a
	 * reference of its own, so the object survives either way -- but
	 * unrefing first means a closed session with a queue can be finalised
	 * between the two lines.
	 */
	session_pump_queue(self);
	g_object_unref(self);
}

static void
session_dispatch(
	AiGuiSession *self,
	const gchar  *text,
	GList        *images
){
	AiTranscript *transcript = ai_conversation_get_transcript(self->conversation);
	guint before = ai_transcript_get_n_blocks(transcript);

	g_clear_object(&self->cancellable);
	self->cancellable = g_cancellable_new();
	self->sending = TRUE;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BUSY]);

	/*
	 * A `/command` never becomes the title.
	 *
	 * It is not what the conversation is about -- and the first thing
	 * somebody does in a new session is often /help, which would then
	 * name it for as long as it lives. ai-tui applies the same rule.
	 */
	if ((self->title_is_automatic || self->title == NULL) &&
	    text != NULL && text[0] != '/')
	{
		g_free(self->title);
		self->title = ai_gui_summarise_prompt(text);
		self->title_is_automatic = TRUE;
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TITLE]);
	}

	session_touch(self);

	/*
	 * A reference for the turn. The window can close a session while a
	 * turn is in flight, and the callback still has to run -- cancelling
	 * makes it run sooner, not never.
	 */
	g_object_ref(self);

	if (self->options->expand)
	{
		ai_conversation_send_input_images_async(self->conversation, text,
		                                        images, self->cancellable,
		                                        on_send_ready, self);
	}
	else
	{
		ai_conversation_send_images_async(self->conversation, text, text,
		                                  images, self->cancellable,
		                                  on_send_ready, self);
	}

	/*
	 * The attachments, kept beside the turn block the conversation just
	 * appended.
	 *
	 * The transcript deliberately records `[Images attached]` and not
	 * the bytes, so the window keeps them itself -- which is the only
	 * way a thumbnail can be drawn where the image was actually sent.
	 * The block is appended synchronously, before the first await, so
	 * it is there by the time this line runs; a built-in resolves
	 * without appending anything, which is what the count guards.
	 */
	if (images != NULL &&
	    ai_transcript_get_n_blocks(transcript) > before)
	{
		AiViewBlock *turn = ai_transcript_get_last(transcript);

		if (turn != NULL && AI_IS_VIEW_TURN_BLOCK(turn))
		{
			ai_gui_content_attach_images(G_OBJECT(turn), images);

			/*
			 * And say so. ai_transcript_append() has already told the
			 * list view about this block, so its row was bound and
			 * rendered a moment ago -- before the attachments existed.
			 * Without this the thumbnails appear only if something else
			 * happens to redraw the row.
			 */
			ai_view_block_changed(turn);
		}
	}
}

/*
 * A queue entry is an intention, not a commitment.
 *
 * The same rule the brigade follows: a queue cleared while a turn was in
 * flight must not have its entries dispatched when that turn ends, or a
 * cancel reads as a delay.
 */
static void
session_pump_queue(AiGuiSession *self)
{
	g_autofree gchar *text = NULL;
	GList *images = NULL;

	if (self->sending || ai_conversation_get_busy(self->conversation))
		return;

	if (ai_prompt_queue_get_length(self->queue) == 0)
		return;

	text = ai_prompt_queue_pop(self->queue, &images);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_QUEUED]);

	if (text != NULL)
		session_dispatch(self, text, images);

	g_list_free_full(images, g_object_unref);
}

gboolean
ai_gui_session_send(
	AiGuiSession  *self,
	const gchar   *text,
	GList         *images,
	GError       **error
){
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), FALSE);
	g_return_val_if_fail(text != NULL, FALSE);

	if (self->sending || ai_conversation_get_busy(self->conversation))
	{
		if (!ai_prompt_queue_push(self->queue, text, images, FALSE, error))
			return FALSE;

		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_QUEUED]);
		return TRUE;
	}

	session_dispatch(self, text, images);
	return TRUE;
}

void
ai_gui_session_cancel(AiGuiSession *self)
{
	g_return_if_fail(AI_GUI_IS_SESSION(self));

	if (self->cancellable != NULL)
		g_cancellable_cancel(self->cancellable);

	ai_conversation_cancel(self->conversation);
}

void
ai_gui_session_clear_queue(AiGuiSession *self)
{
	g_return_if_fail(AI_GUI_IS_SESSION(self));

	ai_prompt_queue_clear(self->queue);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_QUEUED]);
}

void
ai_gui_session_clear(AiGuiSession *self)
{
	g_return_if_fail(AI_GUI_IS_SESSION(self));

	ai_gui_session_cancel(self);
	ai_gui_session_clear_queue(self);
	ai_conversation_clear(self->conversation);
	session_touch(self);
}

/* ================================================================
 * Provider switching
 * ================================================================ */

gboolean
ai_gui_session_switch_provider(
	AiGuiSession  *self,
	const gchar   *provider_name,
	const gchar   *model,
	GError       **error
){
	g_autofree gchar *resolved_model = NULL;
	GObject *provider;

	g_return_val_if_fail(AI_GUI_IS_SESSION(self), FALSE);
	g_return_val_if_fail(provider_name != NULL, FALSE);

	provider = session_build_provider(self->options, provider_name, model,
	                                  FALSE, &resolved_model, error);

	if (provider == NULL)
		return FALSE;

	if (!ai_conversation_set_provider(self->conversation, provider, error))
	{
		g_object_unref(provider);
		return FALSE;
	}

	g_free(self->provider_name);
	self->provider_name = g_strdup(ai_provider_get_name(AI_PROVIDER(provider)));
	g_free(self->provider_id);
	self->provider_id = g_strdup(ai_provider_type_to_string(
		ai_provider_get_provider_type(AI_PROVIDER(provider))));
	g_free(self->model);
	self->model = g_steal_pointer(&resolved_model);

	g_object_unref(provider);

	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PROVIDER_NAME]);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MODEL]);
	session_touch(self);

	return TRUE;
}

/* ================================================================
 * Accessors
 * ================================================================ */

const gchar *
ai_gui_session_get_id(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->id;
}

const gchar *
ai_gui_session_get_title(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->title;
}

void
ai_gui_session_set_title(
	AiGuiSession *self,
	const gchar  *title
){
	g_return_if_fail(AI_GUI_IS_SESSION(self));

	if (title == NULL || *title == '\0')
		return;

	if (g_strcmp0(self->title, title) == 0)
		return;

	g_free(self->title);
	self->title = g_strdup(title);
	/* A name somebody typed is not replaced by the next prompt. */
	self->title_is_automatic = FALSE;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TITLE]);
}

const gchar *
ai_gui_session_get_provider_name(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->provider_name;
}

const gchar *
ai_gui_session_get_provider_id(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->provider_id;
}

const gchar *
ai_gui_session_get_model(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->model;
}

const gchar *
ai_gui_session_get_working_directory(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->working_directory;
}

const gchar *
ai_gui_session_get_project(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);

	if (self->project != NULL && *self->project != '\0')
		return self->project;

	return self->working_directory;
}

void
ai_gui_session_set_working_directory(
	AiGuiSession *self,
	const gchar  *path
){
	g_return_if_fail(AI_GUI_IS_SESSION(self));
	g_return_if_fail(path != NULL);

	if (g_strcmp0(self->working_directory, path) == 0)
		return;

	g_free(self->working_directory);
	self->working_directory = g_strdup(path);

	ai_conversation_set_working_directory(self->conversation, path);
	ai_resource_registry_set_working_directory(self->registry, path);
	ai_resource_registry_scan(self->registry);
	ai_completion_context_set_working_directory(self->completion, path);

	g_object_notify_by_pspec(G_OBJECT(self),
	                         properties[PROP_WORKING_DIRECTORY]);

	/*
	 * The new directory is the project until git says otherwise, so the
	 * row moves to a plausible group immediately rather than sitting
	 * under the old project for as long as two subprocesses take.
	 */
	session_set_project(self, path);
	session_register_work(self);
}

gint64
ai_gui_session_get_updated_at(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), 0);
	return self->updated_at;
}

gboolean
ai_gui_session_get_pinned(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), FALSE);
	return self->pinned;
}

void
ai_gui_session_set_pinned(
	AiGuiSession *self,
	gboolean      pinned
){
	g_return_if_fail(AI_GUI_IS_SESSION(self));

	if (self->pinned == pinned)
		return;

	self->pinned = pinned;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PINNED]);
}

gboolean
ai_gui_session_get_busy(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), FALSE);
	return self->sending || ai_conversation_get_busy(self->conversation);
}

const gchar *
ai_gui_session_get_activity(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return ai_conversation_get_activity(self->conversation);
}

guint
ai_gui_session_get_queued(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), 0);
	return ai_prompt_queue_get_length(self->queue);
}

AiConversation *
ai_gui_session_get_conversation(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->conversation;
}

AiTranscript *
ai_gui_session_get_transcript(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return ai_conversation_get_transcript(self->conversation);
}

AiCompletionContext *
ai_gui_session_get_completion(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->completion;
}

AiCommandSet *
ai_gui_session_get_commands(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->commands;
}

GObject *
ai_gui_session_get_provider(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return ai_conversation_get_provider(self->conversation);
}

gboolean
ai_gui_session_get_approve_all(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), FALSE);
	return self->approve_all;
}

void
ai_gui_session_set_approve_all(
	AiGuiSession *self,
	gboolean      approve_all
){
	g_return_if_fail(AI_GUI_IS_SESSION(self));
	self->approve_all = approve_all;
}

AiGuiOptions *
ai_gui_session_get_options(AiGuiSession *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);
	return self->options;
}

gchar *
ai_gui_session_export(
	AiGuiSession   *self,
	AiExportFormat  format
){
	g_return_val_if_fail(AI_GUI_IS_SESSION(self), NULL);

	return ai_transcript_export(ai_gui_session_get_transcript(self), format);
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

static void
ai_gui_session_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiGuiSession *self = AI_GUI_SESSION(object);

	switch (prop_id)
	{
		case PROP_TITLE:
			g_value_set_string(value, self->title);
			break;
		case PROP_PROVIDER_NAME:
			g_value_set_string(value, self->provider_name);
			break;
		case PROP_MODEL:
			g_value_set_string(value, self->model);
			break;
		case PROP_PROJECT:
			g_value_set_string(value, ai_gui_session_get_project(self));
			break;
		case PROP_WORKING_DIRECTORY:
			g_value_set_string(value, self->working_directory);
			break;
		case PROP_BUSY:
			g_value_set_boolean(value, ai_gui_session_get_busy(self));
			break;
		case PROP_ACTIVITY:
			g_value_set_string(value, ai_gui_session_get_activity(self));
			break;
		case PROP_QUEUED:
			g_value_set_uint(value, ai_gui_session_get_queued(self));
			break;
		case PROP_UPDATED_AT:
			g_value_set_int64(value, self->updated_at);
			break;
		case PROP_PINNED:
			g_value_set_boolean(value, self->pinned);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void
ai_gui_session_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiGuiSession *self = AI_GUI_SESSION(object);

	switch (prop_id)
	{
		case PROP_TITLE:
			ai_gui_session_set_title(self, g_value_get_string(value));
			break;
		case PROP_PINNED:
			ai_gui_session_set_pinned(self, g_value_get_boolean(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void
ai_gui_session_dispose(GObject *object)
{
	AiGuiSession *self = AI_GUI_SESSION(object);

	if (self->conversation != NULL)
	{
		g_clear_signal_handler(&self->approval_id, self->conversation);
		g_clear_signal_handler(&self->agent_id, self->conversation);
		g_clear_signal_handler(&self->busy_id, self->conversation);
		g_clear_signal_handler(&self->activity_id, self->conversation);
	}

	if (self->registry != NULL)
		ai_resource_registry_set_watching(self->registry, FALSE);

	if (self->heartbeat != NULL)
	{
		g_source_destroy(self->heartbeat);
		g_clear_pointer(&self->heartbeat, g_source_unref);
	}

	if (self->work_cancellable != NULL)
		g_cancellable_cancel(self->work_cancellable);

	if (self->work != NULL)
		ai_gui_session_release_work(self);

	if (self->work_lock >= 0)
	{
		g_close(self->work_lock, NULL);
		self->work_lock = -1;
	}

	g_clear_object(&self->work);
	g_clear_object(&self->work_cancellable);

	g_clear_object(&self->cancellable);
	g_clear_object(&self->completion);
	g_clear_object(&self->commands);
	g_clear_object(&self->registry);
	g_clear_object(&self->queue);
	g_clear_object(&self->conversation);

	G_OBJECT_CLASS(ai_gui_session_parent_class)->dispose(object);
}

static void
ai_gui_session_finalize(GObject *object)
{
	AiGuiSession *self = AI_GUI_SESSION(object);

	g_clear_pointer(&self->options, ai_gui_options_free);
	g_free(self->id);
	g_free(self->title);
	g_free(self->provider_name);
	g_free(self->provider_id);
	g_free(self->model);
	g_free(self->working_directory);
	g_free(self->project);
	g_free(self->work_directory);
	g_free(self->work_outcome);

	G_OBJECT_CLASS(ai_gui_session_parent_class)->finalize(object);
}

static void
ai_gui_session_class_init(AiGuiSessionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->get_property = ai_gui_session_get_property;
	object_class->set_property = ai_gui_session_set_property;
	object_class->dispose = ai_gui_session_dispose;
	object_class->finalize = ai_gui_session_finalize;

	properties[PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_PROVIDER_NAME] = g_param_spec_string("provider-name",
		NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_MODEL] = g_param_spec_string("model", NULL, NULL, NULL,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_WORKING_DIRECTORY] = g_param_spec_string(
		"working-directory", NULL, NULL, NULL,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_PROJECT] = g_param_spec_string("project", NULL, NULL,
		NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_BUSY] = g_param_spec_boolean("busy", NULL, NULL, FALSE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_ACTIVITY] = g_param_spec_string("activity", NULL, NULL,
		NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_QUEUED] = g_param_spec_uint("queued", NULL, NULL, 0,
		G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_UPDATED_AT] = g_param_spec_int64("updated-at", NULL, NULL,
		0, G_MAXINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	properties[PROP_PINNED] = g_param_spec_boolean("pinned", NULL, NULL,
		FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPS, properties);

	/**
	 * AiGuiSession::approval-requested:
	 * @self: the session
	 * @tool_use: the call awaiting an answer
	 *
	 * A local tool wants to run.
	 *
	 * Forwarded from the conversation so the window connects to the
	 * object it already has. A handler that asks a person must spin its
	 * nested loop on g_main_context_get_thread_default(), for the reason
	 * the library's own approval documentation gives.
	 *
	 * Returns: an #AiToolApproval
	 */
	signals[SIGNAL_APPROVAL_REQUESTED] =
		g_signal_new("approval-requested", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, approval_accumulator, NULL, NULL,
		             G_TYPE_INT, 1, AI_TYPE_TOOL_USE);

	/**
	 * AiGuiSession::turn-finished:
	 * @self: the session
	 * @success: whether the turn completed
	 * @message: (nullable): why it did not
	 */
	signals[SIGNAL_TURN_FINISHED] =
		g_signal_new("turn-finished", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_STRING);

	/**
	 * AiGuiSession::agent-finished:
	 * @self: the session
	 * @agent_id: which agent
	 * @state: the terminal #AiAgentState it reached
	 */
	signals[SIGNAL_AGENT_FINISHED] =
		g_signal_new("agent-finished", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

	/**
	 * AiGuiSession::builtin-command:
	 * @self: the session
	 * @command: the resolved built-in
	 *
	 * The line was a `/command` this frontend has to run itself.
	 * Nothing was sent to the provider.
	 */
	signals[SIGNAL_BUILTIN] =
		g_signal_new("builtin-command", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, AI_TYPE_COMMAND_RESULT);
}

static void
ai_gui_session_init(AiGuiSession *self)
{
	self->id = g_uuid_string_random();
	self->title = g_strdup("New session");
	self->title_is_automatic = TRUE;
	self->queue = ai_prompt_queue_new();
	self->work_lock = -1;
	self->work_cancellable = g_cancellable_new();
	self->work_directory = ai_work_session_default_directory();
}
