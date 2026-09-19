#pragma once
/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>

typedef struct {
	GObject parent;
	guint writes, flushes, non_silent_samples;
} TestTransport;
typedef GObjectClass TestTransportClass;
static void
transport_iface(AiAudioTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestTransport, test_transport, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_AUDIO_TRANSPORT, transport_iface))
static void
test_transport_class_init(TestTransportClass *c)
{
}
static void
test_transport_init(TestTransport *s)
{
}
static void
write_pcm(AiAudioTransport *transport, GBytes *pcm, GCancellable *cancel,
		  GAsyncReadyCallback cb, gpointer data)
{
	g_autoptr(GTask) task = g_task_new(transport, cancel, cb, data);
	{
		gsize size, i;
		const guint8 *raw = g_bytes_get_data(pcm, &size);
		for (i = 0; i + 1 < size; i += 2)
			if ((gint16)((guint16)raw[i] | ((guint16)raw[i + 1] << 8)) != 0)
				((TestTransport *)transport)->non_silent_samples++;
	}
	((TestTransport *)transport)->writes++;
	g_task_return_boolean(task, TRUE);
}
static gboolean
write_finish(AiAudioTransport *self, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean(G_TASK(result), error);
}
static void
flush(AiAudioTransport *self)
{
	((TestTransport *)self)->flushes++;
}
static void
transport_iface(AiAudioTransportInterface *iface)
{
	iface->write_async = write_pcm;
	iface->write_finish = write_finish;
	iface->flush = flush;
}

typedef struct {
	GObject parent;
	GHashTable *active;
	guint cancelled;
} TestRecognizer;
typedef GObjectClass TestRecognizerClass;
static void
recognizer_iface(AiSpeechRecognizerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestRecognizer, test_recognizer, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_SPEECH_RECOGNIZER,
											  recognizer_iface))
static void
recognizer_finalize(GObject *object)
{
	g_hash_table_unref(((TestRecognizer *)object)->active);
	G_OBJECT_CLASS(test_recognizer_parent_class)->finalize(object);
}
static void
test_recognizer_class_init(TestRecognizerClass *c)
{
	c->finalize = recognizer_finalize;
}
static void
test_recognizer_init(TestRecognizer *s)
{
	s->active = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
static gboolean
begin(AiSpeechRecognizer *self, const gchar *speaker, GError **error)
{
	g_hash_table_add(((TestRecognizer *)self)->active, g_strdup(speaker));
	return TRUE;
}
static gboolean
feed(AiSpeechRecognizer *self, const gchar *speaker, GBytes *pcm, GError **error)
{
	g_assert_true(g_hash_table_contains(((TestRecognizer *)self)->active, speaker));
	return TRUE;
}
static void
end(AiSpeechRecognizer *self, const gchar *speaker)
{
	g_hash_table_remove(((TestRecognizer *)self)->active, speaker);
	g_signal_emit_by_name(self, "transcript", speaker, "hello", TRUE);
}
static void
cancel_stt(AiSpeechRecognizer *self, const gchar *speaker)
{
	if (g_hash_table_remove(((TestRecognizer *)self)->active, speaker))
		((TestRecognizer *)self)->cancelled++;
}
static void
recognizer_iface(AiSpeechRecognizerInterface *iface)
{
	iface->begin = begin;
	iface->feed = feed;
	iface->end = end;
	iface->cancel = cancel_stt;
}

typedef struct {
	GObject parent;
	GPtrArray *texts;
	guint hold_after;
	GTask *held;
	GBytes *pcm;
} TestSynthesizer;
typedef GObjectClass TestSynthesizerClass;
static void
synthesizer_iface(AiSpeechSynthesizerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestSynthesizer, test_synthesizer, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_SPEECH_SYNTHESIZER,
											  synthesizer_iface))
static void
synth_finalize(GObject *object)
{
	g_ptr_array_unref(((TestSynthesizer *)object)->texts);
	g_clear_pointer(&((TestSynthesizer *)object)->pcm, g_bytes_unref);
	G_OBJECT_CLASS(test_synthesizer_parent_class)->finalize(object);
}
static void
test_synthesizer_class_init(TestSynthesizerClass *c)
{
	c->finalize = synth_finalize;
}
static void
test_synthesizer_init(TestSynthesizer *s)
{
	s->texts = g_ptr_array_new_with_free_func(g_free);
}
static void
synthesize(AiSpeechSynthesizer *self, const gchar *text, GCancellable *cancel,
		   GAsyncReadyCallback cb, gpointer data)
{
	TestSynthesizer *s = (TestSynthesizer *)self;
	g_autoptr(GTask) task = g_task_new(self, cancel, cb, data);
	guint8 samples[320] = {0};
	g_autoptr(GBytes) pcm =
		s->pcm != NULL ? g_bytes_ref(s->pcm) : g_bytes_new(samples, sizeof(samples));
	g_ptr_array_add(s->texts, g_strdup(text));
	g_signal_emit_by_name(self, "audio", pcm);
	if (s->hold_after != 0 && s->texts->len >= s->hold_after)
		s->held = g_steal_pointer(&task);
	else
		g_task_return_boolean(task, TRUE);
}
static gboolean
synth_finish(AiSpeechSynthesizer *self, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean(G_TASK(result), error);
}
static void
synthesizer_iface(AiSpeechSynthesizerInterface *iface)
{
	iface->synthesize_async = synthesize;
	iface->synthesize_finish = synth_finish;
}

typedef GObject TestActivity;
typedef GObjectClass TestActivityClass;
static void
activity_iface(AiVoiceActivityInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestActivity, test_activity, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_VOICE_ACTIVITY, activity_iface))
static void
test_activity_class_init(TestActivityClass *c)
{
}
static void
test_activity_init(TestActivity *s)
{
}
static gint
process(AiVoiceActivity *self, const gchar *speaker, GBytes *pcm, GError **error)
{
	const guint8 *p = g_bytes_get_data(pcm, NULL);
	return p[0];
}
static void
reset(AiVoiceActivity *self, const gchar *speaker)
{
}
static void
activity_iface(AiVoiceActivityInterface *iface)
{
	iface->process = process;
	iface->reset = reset;
}

typedef GObject TestAgentWorker;
typedef GObjectClass TestAgentWorkerClass;
static void
agent_worker_iface(AiAgentWorkerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestAgentWorker, test_agent_worker, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_AGENT_WORKER, agent_worker_iface))
static void
test_agent_worker_class_init(TestAgentWorkerClass *c)
{
}
static void
test_agent_worker_init(TestAgentWorker *s)
{
}
static gboolean
agent_complete(gpointer data)
{
	GTask *task = data;
	AiAgent *agent = g_task_get_task_data(task);
	ai_agent_set_result(agent, "The background fixture result");
	ai_agent_set_state(agent, AI_AGENT_STATE_DONE);
	g_task_return_boolean(task, TRUE);
	return G_SOURCE_REMOVE;
}
static void
agent_start(AiAgentWorker *self, AiAgent *agent, const gchar *prompt,
			GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
	GTask *task = g_task_new(self, cancel, cb, data);
	GSource *source = g_timeout_source_new(25);
	ai_agent_set_state(agent, AI_AGENT_STATE_RUNNING);
	g_task_set_task_data(task, g_object_ref(agent), g_object_unref);
	g_source_set_callback(source, agent_complete, task, g_object_unref);
	g_source_attach(source, g_main_context_get_thread_default());
	g_source_unref(source);
}
static gboolean
agent_finish(AiAgentWorker *self, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean(G_TASK(result), error);
}
static void
agent_worker_iface(AiAgentWorkerInterface *iface)
{
	iface->start_async = agent_start;
	iface->start_finish = agent_finish;
}
