/*
 * test-event-source.c - Tests for the AiEventSource interface
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <ai-glib.h>

/*
 * A minimal implementer, so the interface can be exercised without dragging
 * in a provider and its transport.
 */
#define TEST_TYPE_SOURCE (test_source_get_type())
G_DECLARE_FINAL_TYPE(TestSource, test_source, TEST, SOURCE, GObject)

struct _TestSource
{
	GObject parent_instance;
};

G_DEFINE_TYPE_WITH_CODE(TestSource, test_source, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_EVENT_SOURCE, NULL))

static void test_source_class_init(TestSourceClass *klass) { (void)klass; }
static void test_source_init(TestSource *self) { (void)self; }

/* What a subscriber accumulates. */
typedef struct
{
	GPtrArray *events;   /* owned AiEvent* */
	guint      calls;
} Collector;

static Collector *
collector_new(void)
{
	Collector *c = g_new0(Collector, 1);

	c->events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);

	return c;
}

static void
collector_free(Collector *c)
{
	g_ptr_array_unref(c->events);
	g_free(c);
}

static void
on_event(AiEventSource *source, AiEvent *event, gpointer user_data)
{
	Collector *c = user_data;

	(void)source;

	c->calls++;
	g_ptr_array_add(c->events, ai_event_ref(event));
}

static void
test_source_implements_interface(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);

	g_assert_true(AI_IS_EVENT_SOURCE(source));
}

static void
test_emit_delivers_event(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(AiEvent) event = ai_event_new_text_delta("hello");
	Collector *c = collector_new();

	g_signal_connect(source, "event", G_CALLBACK(on_event), c);
	ai_event_source_emit(AI_EVENT_SOURCE(source), event);

	g_assert_cmpuint(c->calls, ==, 1);
	g_assert_cmpuint(c->events->len, ==, 1);
	g_assert_cmpstr(ai_event_get_text(g_ptr_array_index(c->events, 0)), ==, "hello");

	collector_free(c);
}

static void
test_emit_survives_handler(void)
{
	/*
	 * The handler takes its own reference; the event must outlive the
	 * emitter's g_autoptr going out of scope.
	 */
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	Collector *c = collector_new();

	g_signal_connect(source, "event", G_CALLBACK(on_event), c);

	{
		g_autoptr(AiEvent) event = ai_event_new_status("scoped");
		ai_event_source_emit(AI_EVENT_SOURCE(source), event);
	}

	g_assert_cmpstr(ai_event_get_text(g_ptr_array_index(c->events, 0)), ==, "scoped");

	collector_free(c);
}

static void
test_emit_stamps_source(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(AiEvent) event = ai_event_new_text_delta("x");

	g_assert_null(ai_event_get_source(event));

	ai_event_source_emit(AI_EVENT_SOURCE(source), event);
	g_assert_cmpstr(ai_event_get_source(event), ==, "TestSource");
}

static void
test_emit_keeps_explicit_source(void)
{
	/*
	 * An emitter that labelled the event itself -- the executor forwarding a
	 * provider's event, say -- must not have that label overwritten.
	 */
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(AiEvent) event = ai_event_new_text_delta("x");

	ai_event_set_source(event, "AiClaudeCodeClient");
	ai_event_source_emit(AI_EVENT_SOURCE(source), event);

	g_assert_cmpstr(ai_event_get_source(event), ==, "AiClaudeCodeClient");
}

static void
test_emit_multiple_subscribers(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(AiEvent) event = ai_event_new_text_delta("broadcast");
	Collector *a = collector_new();
	Collector *b = collector_new();

	g_signal_connect(source, "event", G_CALLBACK(on_event), a);
	g_signal_connect(source, "event", G_CALLBACK(on_event), b);

	ai_event_source_emit(AI_EVENT_SOURCE(source), event);

	g_assert_cmpuint(a->calls, ==, 1);
	g_assert_cmpuint(b->calls, ==, 1);

	/* One instance shared, not one copy each. */
	g_assert_true(g_ptr_array_index(a->events, 0) == g_ptr_array_index(b->events, 0));

	collector_free(a);
	collector_free(b);
}

static void
test_emit_after_disconnect(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(AiEvent) first = ai_event_new_text_delta("before");
	g_autoptr(AiEvent) second = ai_event_new_text_delta("after");
	Collector *c = collector_new();
	gulong id;

	id = g_signal_connect(source, "event", G_CALLBACK(on_event), c);
	ai_event_source_emit(AI_EVENT_SOURCE(source), first);

	g_signal_handler_disconnect(source, id);
	ai_event_source_emit(AI_EVENT_SOURCE(source), second);

	g_assert_cmpuint(c->calls, ==, 1);

	collector_free(c);
}

static void
test_emit_no_subscribers(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(AiEvent) event = ai_event_new_text_delta("into the void");

	/* Must not crash, and must still stamp the source. */
	ai_event_source_emit(AI_EVENT_SOURCE(source), event);
	g_assert_cmpstr(ai_event_get_source(event), ==, "TestSource");
}

static void
test_emit_null_event(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*event != NULL*");
	ai_event_source_emit(AI_EVENT_SOURCE(source), NULL);
	g_test_assert_expected_messages();
}

static void
test_emit_null_source(void)
{
	g_autoptr(AiEvent) event = ai_event_new_text_delta("x");

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
	                      "*AI_IS_EVENT_SOURCE*");
	ai_event_source_emit(NULL, event);
	g_test_assert_expected_messages();
}

static void
test_emit_all(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(GPtrArray) events =
		g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	Collector *c = collector_new();

	g_ptr_array_add(events, ai_event_new_text_delta("one"));
	g_ptr_array_add(events, ai_event_new_text_delta("two"));
	g_ptr_array_add(events, ai_event_new_text_delta("three"));

	g_signal_connect(source, "event", G_CALLBACK(on_event), c);
	ai_event_source_emit_all(AI_EVENT_SOURCE(source), events);

	/* Order is the contract: a line's events replay as they occurred. */
	g_assert_cmpuint(c->calls, ==, 3);
	g_assert_cmpstr(ai_event_get_text(g_ptr_array_index(c->events, 0)), ==, "one");
	g_assert_cmpstr(ai_event_get_text(g_ptr_array_index(c->events, 1)), ==, "two");
	g_assert_cmpstr(ai_event_get_text(g_ptr_array_index(c->events, 2)), ==, "three");

	collector_free(c);
}

static void
test_emit_all_empty(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	g_autoptr(GPtrArray) events = g_ptr_array_new();
	Collector *c = collector_new();

	g_signal_connect(source, "event", G_CALLBACK(on_event), c);

	/* Most NDJSON lines produce nothing; that is not an error. */
	ai_event_source_emit_all(AI_EVENT_SOURCE(source), events);
	g_assert_cmpuint(c->calls, ==, 0);

	collector_free(c);
}

static void
test_emit_all_null(void)
{
	g_autoptr(TestSource) source = g_object_new(TEST_TYPE_SOURCE, NULL);
	Collector *c = collector_new();

	g_signal_connect(source, "event", G_CALLBACK(on_event), c);

	ai_event_source_emit_all(AI_EVENT_SOURCE(source), NULL);
	g_assert_cmpuint(c->calls, ==, 0);

	collector_free(c);
}

static void
test_providers_are_event_sources(void)
{
	/* Both base classes, so every provider is observable the same way. */
	g_autoptr(AiClaudeClient) http = ai_claude_client_new();
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();

	g_assert_true(AI_IS_EVENT_SOURCE(http));
	g_assert_true(AI_IS_EVENT_SOURCE(cli));
}

static void
test_signal_id_is_unambiguous(void)
{
	/*
	 * The whole reason ::event is declared only on the interface.
	 *
	 * "delta" is declared twice -- on AiStreamable and again on AiCliClient
	 * -- so the id you get depends on which type you ask, and a handler
	 * connected on a CLI client is invisible to the interface's id.  This
	 * asserts that hazard exists for "delta" and does NOT exist for "event",
	 * which is what lets a frontend subscribe to any provider uniformly.
	 */
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	guint iface_delta;
	guint class_delta;
	guint iface_event;
	guint instance_event;

	iface_delta = g_signal_lookup("delta", AI_TYPE_STREAMABLE);
	class_delta = g_signal_lookup("delta", AI_TYPE_CLI_CLIENT);
	g_assert_cmpuint(iface_delta, !=, 0);
	g_assert_cmpuint(class_delta, !=, 0);
	g_assert_cmpuint(iface_delta, !=, class_delta);

	iface_event = g_signal_lookup("event", AI_TYPE_EVENT_SOURCE);
	instance_event = g_signal_lookup("event", G_OBJECT_TYPE(cli));
	g_assert_cmpuint(iface_event, !=, 0);
	g_assert_cmpuint(iface_event, ==, instance_event);
}

static void
test_emit_by_interface_signal_id(void)
{
	/*
	 * Emitting through the id looked up on the interface must reach a
	 * handler connected by name on a concrete provider.  If ::event were
	 * ever redeclared on a class, this is the test that would fail.
	 */
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	g_autoptr(AiEvent) event = ai_event_new_status("via interface id");
	Collector *c = collector_new();

	g_signal_connect(cli, "event", G_CALLBACK(on_event), c);
	g_signal_emit(cli, g_signal_lookup("event", AI_TYPE_EVENT_SOURCE), 0, event);

	g_assert_cmpuint(c->calls, ==, 1);

	collector_free(c);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/event-source/implements",
	                test_source_implements_interface);
	g_test_add_func("/ai-glib/event-source/emit", test_emit_delivers_event);
	g_test_add_func("/ai-glib/event-source/emit-survives-handler",
	                test_emit_survives_handler);
	g_test_add_func("/ai-glib/event-source/stamps-source", test_emit_stamps_source);
	g_test_add_func("/ai-glib/event-source/keeps-explicit-source",
	                test_emit_keeps_explicit_source);
	g_test_add_func("/ai-glib/event-source/multiple-subscribers",
	                test_emit_multiple_subscribers);
	g_test_add_func("/ai-glib/event-source/after-disconnect",
	                test_emit_after_disconnect);
	g_test_add_func("/ai-glib/event-source/no-subscribers", test_emit_no_subscribers);
	g_test_add_func("/ai-glib/event-source/null-event", test_emit_null_event);
	g_test_add_func("/ai-glib/event-source/null-source", test_emit_null_source);
	g_test_add_func("/ai-glib/event-source/emit-all", test_emit_all);
	g_test_add_func("/ai-glib/event-source/emit-all-empty", test_emit_all_empty);
	g_test_add_func("/ai-glib/event-source/emit-all-null", test_emit_all_null);
	g_test_add_func("/ai-glib/event-source/providers-implement",
	                test_providers_are_event_sources);
	g_test_add_func("/ai-glib/event-source/signal-id-unambiguous",
	                test_signal_id_is_unambiguous);
	g_test_add_func("/ai-glib/event-source/emit-by-interface-id",
	                test_emit_by_interface_signal_id);

	return g_test_run();
}
