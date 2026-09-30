/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voice-worker-private.h"
#include "model/ai-text-content.h"

/* The existing executor contains synchronous tools. Keep the entire
 * conversation and brigade on one persistent private context, never on the
 * audio context. No provider/executor implementation is changed. */
struct _AiVoiceWorker {
	gatomicrefcount refs;
	AiConversation *conversation;
	GMainContext *context, *delivery;
	GMainLoop *loop;
	GWeakRef owner;
	AiVoiceMailFunc callback;
	GCancellable *cancel;
	AiTextContent *spoken_block;
	gchar *spoken, *answer;
	guint64 generation;
	gint trim_after; /* atomic: set from the audio thread, read on this one */
	gboolean stopping, saw_text;
};
typedef struct {
	AiVoiceWorker *worker;
	AiVoiceMailKind kind;
	guint64 generation;
	AiEvent *event;
	gchar *text;
	GError *error;
} Mail;
typedef struct {
	AiVoiceWorker *worker;
	gchar *text;
	GCancellable *cancel;
	guint64 generation;
	guint operation;
} Command;

static AiVoiceWorker *
worker_ref(AiVoiceWorker *w)
{
	g_atomic_ref_count_inc(&w->refs);
	return w;
}
static void
worker_unref(AiVoiceWorker *w)
{
	if (!g_atomic_ref_count_dec(&w->refs))
		return;
	g_clear_object(&w->conversation);
	g_clear_object(&w->cancel);
	g_clear_object(&w->spoken_block);
	g_main_loop_unref(w->loop);
	g_main_context_unref(w->context);
	g_main_context_unref(w->delivery);
	g_weak_ref_clear(&w->owner);
	g_free(w->spoken);
	g_free(w->answer);
	g_free(w);
}
static gboolean
deliver(gpointer data)
{
	Mail *m = data;
	g_autoptr(GObject) owner = g_weak_ref_get(&m->worker->owner);
	if (owner != NULL)
		m->worker->callback(owner, m->kind, m->generation, m->event, m->text, m->error);
	return G_SOURCE_REMOVE;
}
static void
mail_free(gpointer data)
{
	Mail *m = data;
	g_clear_pointer(&m->event, ai_event_unref);
	g_clear_error(&m->error);
	g_free(m->text);
	worker_unref(m->worker);
	g_free(m);
}
static void
post(AiVoiceWorker *w, AiVoiceMailKind kind, AiEvent *event, const gchar *text,
	 const GError *error)
{
	Mail *m = g_new0(Mail, 1);
	GSource *source = g_idle_source_new();
	m->worker = worker_ref(w);
	m->kind = kind;
	m->generation = w->generation;
	m->event = event != NULL ? ai_event_ref(event) : NULL;
	m->text = g_strdup(text);
	m->error = error != NULL ? g_error_copy(error) : NULL;
	g_source_set_callback(source, deliver, m, mail_free);
	g_source_attach(source, w->delivery);
	g_source_unref(source);
}
static void
maybe_quit(AiVoiceWorker *w)
{
	AiBrigade *brigade = ai_conversation_get_brigade(w->conversation);
	if (w->stopping && !ai_conversation_get_busy(w->conversation) &&
		(brigade == NULL || ai_brigade_count_live(brigade) == 0))
		g_main_loop_quit(w->loop);
}
static void
on_event(AiConversation *conversation, AiEvent *event, gpointer data)
{
	AiVoiceWorker *w = data;
	if (ai_event_get_kind(event) == AI_EVENT_TEXT_DELTA)
		w->saw_text = TRUE;
	post(w, AI_VOICE_MAIL_EVENT, event, NULL, NULL);
}
static void
on_tool_event(AiToolExecutor *executor, AiEvent *event, gpointer data)
{
	AiEventKind kind = ai_event_get_kind(event);
	if ((kind == AI_EVENT_TOOL_STARTED || kind == AI_EVENT_TOOL_FINISHED) &&
		g_strcmp0(ai_event_get_source(event), "AiToolExecutor") == 0)
		post(data, AI_VOICE_MAIL_EVENT, event, NULL, NULL);
}
static void
on_agent(AiConversation *conversation, const gchar *id, gint state, gpointer data)
{
	AiVoiceWorker *w = data;
	g_autofree gchar *line =
		g_strdup_printf("Background agent %s %s. You can ask for its result.", id,
						state == AI_AGENT_STATE_DONE		? "finished"
						: state == AI_AGENT_STATE_CANCELLED ? "was cancelled"
															: "failed");
	post(w, AI_VOICE_MAIL_AGENT, NULL, line, NULL);
	maybe_quit(w);
}
/* Every turn resends every earlier tool result. On a long call that is most
 * of the prompt, and a single big one used to be enough to exceed the model's
 * context. Results older than the configured number of turns become a short
 * note; the tool_use/tool_result pair survives, since providers require it. */
#define TRIM_KEEP_BYTES 160
static void
trim_history(AiVoiceWorker *w)
{
	guint keep = (guint)g_atomic_int_get(&w->trim_after), turns = 0;
	GList *l;
	if (keep == 0)
		return;
	for (l = g_list_last(ai_conversation_get_messages(w->conversation)); l != NULL;
		 l = l->prev) {
		AiMessage *message = l->data;
		gboolean caller_text = FALSE;
		GList *b;
		for (b = ai_message_get_content_blocks(message); b != NULL; b = b->next) {
			if (AI_IS_TOOL_RESULT(b->data) && turns >= keep) {
				const gchar *content = ai_tool_result_get_content(b->data);
				gsize len = content != NULL ? strlen(content) : 0;
				if (len > TRIM_KEEP_BYTES && !g_str_has_prefix(content, "[Earlier tool output")) {
					g_autofree gchar *note = g_strdup_printf(
						"[Earlier tool output trimmed to keep the call's context small; "
						"it was %" G_GSIZE_FORMAT " bytes. Run the tool again if you need it.]",
						len);
					g_object_set(b->data, "content", note, NULL);
				}
			} else if (AI_IS_TEXT_CONTENT(b->data) &&
					   ai_message_get_role(message) == AI_ROLE_USER)
				caller_text = TRUE;
		}
		if (caller_text)
			turns++;
	}
}
static void
prepare(AiConversation *conversation, GPtrArray *batch, gpointer data)
{
	AiVoiceWorker *w = data;
	g_autoptr(GPtrArray) projected = g_ptr_array_new_with_free_func(g_object_unref);
	trim_history(w);
	g_autoptr(AiMessage) spoken_message = NULL;
	guint i;
	for (i = 0; i < batch->len; i++) {
		AiMessage *message = g_ptr_array_index(batch, i);
		if (ai_message_get_role(message) == AI_ROLE_ASSISTANT) {
			g_autoptr(AiMessage) kept = ai_message_new(AI_ROLE_ASSISTANT);
			g_autofree gchar *text = ai_message_get_text(message);
			GList *l;
			if (text != NULL && *text != '\0') {
				g_free(w->answer);
				w->answer = g_strdup(text);
			}
			for (l = ai_message_get_content_blocks(message); l != NULL; l = l->next)
				if (!AI_IS_TEXT_CONTENT(l->data))
					ai_message_add_content_block(kept, g_object_ref(l->data));
			if (ai_message_get_content_blocks(kept) != NULL)
				g_ptr_array_add(projected, g_steal_pointer(&kept));
		} else
			g_ptr_array_add(projected, g_object_ref(message));
	}
	g_ptr_array_set_size(batch, 0);
	for (i = 0; i < projected->len; i++)
		g_ptr_array_add(batch, g_object_ref(g_ptr_array_index(projected, i)));
	g_clear_object(&w->spoken_block);
	w->spoken_block = ai_text_content_new(w->spoken != NULL ? w->spoken : "");
	spoken_message = ai_message_new(AI_ROLE_ASSISTANT);
	ai_message_add_content_block(spoken_message,
								 AI_CONTENT_BLOCK(g_object_ref(w->spoken_block)));
	g_ptr_array_add(batch, g_steal_pointer(&spoken_message));
}
static void
turn_done(GObject *source, GAsyncResult *result, gpointer data)
{
	AiVoiceWorker *w = data;
	g_autoptr(GError) error = NULL;
	ai_conversation_send_finish(AI_CONVERSATION(source), result, &error);
	post(w, AI_VOICE_MAIL_DONE, NULL, w->saw_text ? NULL : w->answer, error);
	g_clear_object(&w->cancel);
	maybe_quit(w);
}
static gboolean
command_run(gpointer data)
{
	Command *c = data;
	AiVoiceWorker *w = c->worker;
	if (c->operation == 2) {
		AiBrigade *brigade = ai_conversation_get_brigade(w->conversation);
		w->stopping = TRUE;
		ai_conversation_cancel(w->conversation);
		if (brigade != NULL)
			ai_brigade_cancel_all(brigade);
		maybe_quit(w);
	} else if (c->operation == 1) {
		if (c->generation == w->generation) {
			g_free(w->spoken);
			w->spoken = g_strdup(c->text);
			if (w->spoken_block != NULL)
				ai_text_content_set_text(w->spoken_block, w->spoken);
		}
	} else if (!w->stopping) {
		w->generation = c->generation;
		w->saw_text = FALSE;
		g_clear_pointer(&w->spoken, g_free);
		g_clear_pointer(&w->answer, g_free);
		g_clear_object(&w->spoken_block);
		g_set_object(&w->cancel, c->cancel);
		ai_conversation_send_async(w->conversation, c->text, c->cancel, turn_done, w);
	}
	return G_SOURCE_REMOVE;
}
static void
command_free(gpointer data)
{
	Command *c = data;
	worker_unref(c->worker);
	g_clear_object(&c->cancel);
	g_free(c->text);
	g_free(c);
}
static void
command(AiVoiceWorker *w, guint operation, const gchar *text, GCancellable *cancel,
		guint64 generation)
{
	Command *c = g_new0(Command, 1);
	GSource *source = g_idle_source_new();
	c->worker = worker_ref(w);
	c->operation = operation;
	c->text = g_strdup(text);
	c->cancel = cancel != NULL ? g_object_ref(cancel) : NULL;
	c->generation = generation;
	g_source_set_callback(source, command_run, c, command_free);
	g_source_attach(source, w->context);
	g_source_unref(source);
}
static gpointer
worker_main(gpointer data)
{
	AiVoiceWorker *w = data;
	g_main_context_push_thread_default(w->context);
	g_signal_connect(ai_conversation_get_executor(w->conversation), "event",
					 G_CALLBACK(on_tool_event), w);
	g_signal_connect(w->conversation, "event", G_CALLBACK(on_event), w);
	g_signal_connect(w->conversation, "agent-finished", G_CALLBACK(on_agent), w);
	g_signal_connect(w->conversation, "prepare-messages", G_CALLBACK(prepare), w);
	g_main_loop_run(w->loop);
	g_signal_handlers_disconnect_by_data(w->conversation, w);
	g_signal_handlers_disconnect_by_data(ai_conversation_get_executor(w->conversation),
										 w);
	while (g_main_context_pending(w->context))
		g_main_context_iteration(w->context, FALSE);
	g_main_context_pop_thread_default(w->context);
	worker_unref(w);
	return NULL;
}
AiVoiceWorker *
ai_voice_worker_new(AiConversation *conversation, GObject *owner, GMainContext *context,
					AiVoiceMailFunc callback)
{
	AiVoiceWorker *w = g_new0(AiVoiceWorker, 1);
	GThread *thread;
	g_atomic_ref_count_init(&w->refs);
	w->conversation = g_object_ref(conversation);
	w->context = g_main_context_new();
	w->delivery = g_main_context_ref(context);
	w->loop = g_main_loop_new(w->context, FALSE);
	w->callback = callback;
	g_weak_ref_init(&w->owner, owner);
	thread = g_thread_new("ai-voice-conversation", worker_main, worker_ref(w));
	g_thread_unref(thread);
	return w;
}
void
ai_voice_worker_send(AiVoiceWorker *w, const gchar *text, GCancellable *cancel,
					 guint64 generation)
{
	command(w, 0, text, cancel, generation);
}
void
ai_voice_worker_spoken(AiVoiceWorker *w, const gchar *text, guint64 generation)
{
	command(w, 1, text, NULL, generation);
}
void
ai_voice_worker_set_trim_after(AiVoiceWorker *w, guint turns)
{
	g_atomic_int_set(&w->trim_after, (gint)MIN(turns, (guint)G_MAXINT));
}
void
ai_voice_worker_stop(AiVoiceWorker *w)
{
	command(w, 2, NULL, NULL, 0);
	worker_unref(w);
}
