/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <ai-glib.h>

typedef struct {
	GObject parent;
	gint cancelled;
	gboolean emit_text;
	const gchar *const *deltas; /* NULL-terminated; replaces the fixed sentence */
} TestStalledProvider;
typedef GObjectClass TestStalledProviderClass;
static void
stalled_stream_iface(AiStreamableInterface *iface);
static void
stalled_provider_iface(AiProviderInterface *iface)
{
}
G_DEFINE_TYPE_WITH_CODE(TestStalledProvider, test_stalled_provider, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, stalled_provider_iface)
							G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
												  stalled_stream_iface)
								G_IMPLEMENT_INTERFACE(AI_TYPE_EVENT_SOURCE, NULL))
static void
test_stalled_provider_class_init(TestStalledProviderClass *klass)
{
}
static void
test_stalled_provider_init(TestStalledProvider *self)
{
	self->emit_text = TRUE;
}
static void
stalled_cancelled(GCancellable *cancel, gpointer data)
{
	g_atomic_int_set(&((TestStalledProvider *)data)->cancelled, 1);
}
static gboolean
stalled_complete(GCancellable *cancel, gpointer data)
{
	g_task_return_new_error(data, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Turn cancelled");
	return G_SOURCE_REMOVE;
}
static void
stalled_stream(AiStreamable *stream, GList *messages, const gchar *system,
			   gint max_tokens, GList *tools, GCancellable *cancel,
			   GAsyncReadyCallback callback, gpointer data)
{
	TestStalledProvider *self = (TestStalledProvider *)stream;
	g_autoptr(GTask) task = g_task_new(stream, cancel, callback, data);
	g_autoptr(GSource) source = g_cancellable_source_new(cancel);
	g_signal_connect_object(cancel, "cancelled", G_CALLBACK(stalled_cancelled), self, 0);
	g_source_set_callback(source, G_SOURCE_FUNC(stalled_complete), g_object_ref(task),
						  g_object_unref);
	g_source_attach(source, g_main_context_get_thread_default());
	if (self->deltas != NULL) {
		const gchar *const *d;
		for (d = self->deltas; *d != NULL; d++) {
			g_autoptr(AiEvent) event = ai_event_new_text_delta(*d);
			ai_event_source_emit(AI_EVENT_SOURCE(self), event);
		}
	} else if (self->emit_text) {
		g_autoptr(AiEvent) event = ai_event_new_text_delta("A streamed sentence.");
		ai_event_source_emit(AI_EVENT_SOURCE(self), event);
	}
}
static AiResponse *
stalled_finish(AiStreamable *self, GAsyncResult *result, GError **error)
{
	return g_task_propagate_pointer(G_TASK(result), error);
}
static void
stalled_stream_iface(AiStreamableInterface *iface)
{
	iface->chat_stream_async = stalled_stream;
	iface->chat_stream_finish = stalled_finish;
}
