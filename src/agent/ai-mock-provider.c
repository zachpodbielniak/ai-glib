/*
 * ai-mock-provider.c - A scriptable provider, for tests
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-mock-provider.h"

#include "core/ai-error.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

typedef enum
{
    SCRIPTED_TEXT,
    SCRIPTED_TOOL_USE,
    SCRIPTED_ERROR
} ScriptedKind;

typedef struct
{
    ScriptedKind kind;
    gchar *a;          /* text, tool name, or error message */
    gchar *b;          /* tool input JSON */
} Scripted;

struct _AiMockProvider
{
    GObject parent_instance;

    GQueue *script;      /* Scripted* */
    gchar  *fallback;
    guint   call_count;
    guint   delay_ms;
};

static void ai_mock_provider_provider_init (AiProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (AiMockProvider, ai_mock_provider, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                               ai_mock_provider_provider_init))

static void
scripted_free (gpointer data)
{
    Scripted *s = data;

    if (s == NULL) return;
    g_free(s->a);
    g_free(s->b);
    g_free(s);
}

static void
ai_mock_provider_finalize (GObject *object)
{
    AiMockProvider *self = AI_MOCK_PROVIDER(object);

    g_queue_free_full(self->script, scripted_free);
    g_free(self->fallback);

    G_OBJECT_CLASS(ai_mock_provider_parent_class)->finalize(object);
}

static void
ai_mock_provider_class_init (AiMockProviderClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_mock_provider_finalize;
}

static void
ai_mock_provider_init (AiMockProvider *self)
{
    self->script   = g_queue_new();
    self->fallback = g_strdup("(mock: script exhausted)");
}

AiMockProvider *
ai_mock_provider_new (void)
{
    return g_object_new(AI_TYPE_MOCK_PROVIDER, NULL);
}

static void
push (AiMockProvider *self, ScriptedKind kind, const gchar *a, const gchar *b)
{
    Scripted *s = g_new0(Scripted, 1);

    s->kind = kind;
    s->a = g_strdup(a);
    s->b = g_strdup(b);
    g_queue_push_tail(self->script, s);
}

void
ai_mock_provider_push_text (AiMockProvider *self, const gchar *text)
{
    g_return_if_fail(AI_IS_MOCK_PROVIDER(self));
    push(self, SCRIPTED_TEXT, text, NULL);
}

void
ai_mock_provider_push_tool_use (AiMockProvider *self, const gchar *tool_name,
                                const gchar *input_json)
{
    g_return_if_fail(AI_IS_MOCK_PROVIDER(self));
    push(self, SCRIPTED_TOOL_USE, tool_name, input_json ? input_json : "{}");
}

void
ai_mock_provider_push_error (AiMockProvider *self, const gchar *message)
{
    g_return_if_fail(AI_IS_MOCK_PROVIDER(self));
    push(self, SCRIPTED_ERROR, message, NULL);
}

void
ai_mock_provider_set_fallback (AiMockProvider *self, const gchar *text)
{
    g_return_if_fail(AI_IS_MOCK_PROVIDER(self));
    g_free(self->fallback);
    self->fallback = g_strdup(text);
}

guint
ai_mock_provider_get_call_count (AiMockProvider *self)
{
    g_return_val_if_fail(AI_IS_MOCK_PROVIDER(self), 0);
    return self->call_count;
}

void
ai_mock_provider_set_delay_ms (AiMockProvider *self, guint ms)
{
    g_return_if_fail(AI_IS_MOCK_PROVIDER(self));
    self->delay_ms = ms;
}

/* ── AiProvider ──────────────────────────────────────────────────── */

static AiProviderType
mock_get_provider_type (AiProvider *self)
{
    (void)self;
    return AI_PROVIDER_OLLAMA;
}

static const gchar *
mock_get_name (AiProvider *self)
{
    (void)self;
    return "mock";
}

static const gchar *
mock_get_default_model (AiProvider *self)
{
    (void)self;
    return "mock-model";
}

static AiResponse *
build_response (AiMockProvider *self, Scripted *s, GError **error)
{
    g_autoptr(AiResponse) response = ai_response_new("mock-id", "mock-model");

    if (s == NULL)
    {
        g_autoptr(AiTextContent) text = ai_text_content_new(self->fallback);
        ai_response_add_content_block(response,
            (AiContentBlock *)g_steal_pointer(&text));
        return (AiResponse *)g_steal_pointer(&response);
    }

    switch (s->kind)
    {
        case SCRIPTED_ERROR:
            g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", s->a);
            return NULL;

        case SCRIPTED_TOOL_USE:
        {
            g_autoptr(AiToolUse) tu =
                ai_tool_use_new_from_json_string("mock-tool-1", s->a, s->b);
            ai_response_add_content_block(response,
                (AiContentBlock *)g_steal_pointer(&tu));
            break;
        }

        case SCRIPTED_TEXT:
        default:
        {
            g_autoptr(AiTextContent) text = ai_text_content_new(s->a);
            ai_response_add_content_block(response,
                (AiContentBlock *)g_steal_pointer(&text));
            break;
        }
    }

    return (AiResponse *)g_steal_pointer(&response);
}

typedef struct
{
    GTask          *task;
    AiMockProvider *self;
    Scripted       *scripted;
} DeferredReply;

static gboolean
deliver (gpointer user_data)
{
    DeferredReply *d = user_data;
    g_autoptr(GError) error = NULL;
    AiResponse *response;

    response = build_response(d->self, d->scripted, &error);
    if (response == NULL)
        g_task_return_error(d->task, g_steal_pointer(&error));
    else
        g_task_return_pointer(d->task, response, g_object_unref);

    /* g_task_return_* does NOT consume the reference from g_task_new,
     * which is the leak this library has already been bitten by once. */
    g_clear_object(&d->task);
    scripted_free(d->scripted);
    g_object_unref(d->self);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void
mock_chat_async (AiProvider *provider, GList *messages,
                 const gchar *system_prompt, gint max_tokens, GList *tools,
                 GCancellable *cancellable, GAsyncReadyCallback callback,
                 gpointer user_data)
{
    AiMockProvider *self = AI_MOCK_PROVIDER(provider);
    DeferredReply *d;

    (void)messages; (void)system_prompt; (void)max_tokens; (void)tools;

    self->call_count++;

    d = g_new0(DeferredReply, 1);
    d->task     = g_task_new(self, cancellable, callback, user_data);
    d->self     = g_object_ref(self);
    d->scripted = g_queue_pop_head(self->script);

    /* Always deferred, even with no delay: replying synchronously from
     * inside chat_async would let a caller's completion handler run
     * before its own call returned, which no real provider does and
     * which would let a test pass against a re-entrancy bug. */
    if (self->delay_ms > 0)
        g_timeout_add_full(G_PRIORITY_DEFAULT, self->delay_ms, deliver, d, NULL);
    else
        g_idle_add(deliver, d);
}

static AiResponse *
mock_chat_finish (AiProvider *provider, GAsyncResult *result, GError **error)
{
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_mock_provider_provider_init (AiProviderInterface *iface)
{
    iface->get_provider_type = mock_get_provider_type;
    iface->get_name          = mock_get_name;
    iface->get_default_model = mock_get_default_model;
    iface->chat_async        = mock_chat_async;
    iface->chat_finish       = mock_chat_finish;
}
