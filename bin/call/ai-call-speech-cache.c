/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define G_LOG_DOMAIN "ai-call"
#include "ai-call-speech-cache.h"
#include <ai-glib.h>

#define CACHE_LIMIT (4 * 1024 * 1024)
struct _AiCallSpeechCache {
	GObject parent_instance;
	AiSpeechSynthesizer *synth;
	GCancellable *cancel;
	GByteArray *pending;
	GBytes *pcm;
	GSource *deadline;
	gchar *text;
	guint rate;
	gboolean started, invalid;
};
G_DEFINE_TYPE(AiCallSpeechCache, ai_call_speech_cache, G_TYPE_OBJECT)
static guint ready_signal, completed_signal;

static void
clear_deadline(AiCallSpeechCache *self)
{
	if (self->deadline) {
		g_source_destroy(self->deadline);
		g_clear_pointer(&self->deadline, g_source_unref);
	}
}
void
ai_call_speech_cache_cancel(AiCallSpeechCache *self)
{
	g_return_if_fail(AI_IS_CALL_SPEECH_CACHE(self));
	clear_deadline(self);
	if (self->cancel)
		g_cancellable_cancel(self->cancel);
}
static gboolean
expired(gpointer data)
{
	AiCallSpeechCache *self = data;
	g_clear_pointer(&self->deadline, g_source_unref);
	g_info("Speech fallback prewarm timed out after 4000 ms");
	g_cancellable_cancel(self->cancel);
	return G_SOURCE_REMOVE;
}
static void
audio(AiSpeechSynthesizer *synth, GBytes *bytes, guint rate, gpointer data)
{
	AiCallSpeechCache *self = data;
	gsize size;
	const guint8 *pcm = g_bytes_get_data(bytes, &size);
	if (self->invalid || g_cancellable_is_cancelled(self->cancel))
		return;
	if ((self->rate && self->rate != rate) || !rate || size % 2 ||
		size > CACHE_LIMIT - self->pending->len) {
		self->invalid = TRUE;
		g_info("Speech fallback prewarm rejected inconsistent format or oversized PCM");
		g_cancellable_cancel(self->cancel);
		return;
	}
	self->rate = rate;
	g_byte_array_append(self->pending, pcm, size);
}
static void
finished(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr(AiCallSpeechCache) self = data;
	g_autoptr(GError) error = NULL;
	gboolean ok = ai_speech_synthesizer_synthesize_finish(AI_SPEECH_SYNTHESIZER(source),
														  result, &error);
	clear_deadline(self);
	g_signal_handlers_disconnect_by_data(source, self);
	ok = ok && !self->invalid && !g_cancellable_is_cancelled(self->cancel) &&
		 self->pending->len > 0;
	if (ok) {
		self->pcm = g_byte_array_free_to_bytes(g_steal_pointer(&self->pending));
		g_info("Speech fallback prewarm ready: %" G_GSIZE_FORMAT " bytes at %u Hz",
			   g_bytes_get_size(self->pcm), self->rate);
		g_signal_emit(self, ready_signal, 0);
	} else {
		g_info("Speech fallback prewarm unavailable: %s",
			   error ? error->message : "empty or invalid PCM");
		g_clear_pointer(&self->pending, g_byte_array_unref);
		self->rate = 0;
	}
	g_signal_emit(self, completed_signal, 0, ok);
}
void
ai_call_speech_cache_start(AiCallSpeechCache *self)
{
	g_return_if_fail(AI_IS_CALL_SPEECH_CACHE(self));
	if (self->started)
		return;
	self->started = TRUE;
	self->cancel = g_cancellable_new();
	self->deadline = g_timeout_source_new(4000);
	g_source_set_callback(self->deadline, expired, self, NULL);
	g_source_attach(self->deadline, g_main_context_get_thread_default());
	g_signal_connect(self->synth, "audio", G_CALLBACK(audio), self);
	ai_speech_synthesizer_synthesize_async(self->synth, self->text, self->cancel,
										   finished, g_object_ref(self));
}
GBytes *
ai_call_speech_cache_get_pcm(AiCallSpeechCache *self)
{
	g_return_val_if_fail(AI_IS_CALL_SPEECH_CACHE(self), NULL);
	return self->pcm;
}
guint
ai_call_speech_cache_get_sample_rate(AiCallSpeechCache *self)
{
	g_return_val_if_fail(AI_IS_CALL_SPEECH_CACHE(self), 0);
	return self->pcm ? self->rate : 0;
}
static void
dispose(GObject *object)
{
	AiCallSpeechCache *self = AI_CALL_SPEECH_CACHE(object);
	ai_call_speech_cache_cancel(self);
	g_clear_object(&self->synth);
	G_OBJECT_CLASS(ai_call_speech_cache_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiCallSpeechCache *self = AI_CALL_SPEECH_CACHE(object);
	g_clear_object(&self->cancel);
	g_clear_pointer(&self->pending, g_byte_array_unref);
	g_clear_pointer(&self->pcm, g_bytes_unref);
	g_free(self->text);
	G_OBJECT_CLASS(ai_call_speech_cache_parent_class)->finalize(object);
}
static void
ai_call_speech_cache_class_init(AiCallSpeechCacheClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = dispose;
	G_OBJECT_CLASS(klass)->finalize = finalize;
	ready_signal = g_signal_new("ready", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
								NULL, NULL, NULL, G_TYPE_NONE, 0);
	completed_signal =
		g_signal_new("completed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
					 NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}
static void
ai_call_speech_cache_init(AiCallSpeechCache *self)
{
	self->pending = g_byte_array_new();
}
AiCallSpeechCache *
ai_call_speech_cache_new(const gchar *url, const gchar *text)
{
	g_autoptr(AiCallSpeechCache) self = NULL;
	g_return_val_if_fail(url != NULL && text != NULL, NULL);
	self = g_object_new(AI_TYPE_CALL_SPEECH_CACHE, NULL);
	self->synth = AI_SPEECH_SYNTHESIZER(ai_http_synthesizer_new(url));
	self->text = g_strdup(text);
	return g_steal_pointer(&self);
}
