/*
 * ai-mock-provider.h - A scriptable provider, for tests
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "core/ai-provider.h"
#include "model/ai-response.h"

G_BEGIN_DECLS

#define AI_TYPE_MOCK_PROVIDER (ai_mock_provider_get_type())

G_DECLARE_FINAL_TYPE (AiMockProvider, ai_mock_provider, AI, MOCK_PROVIDER,
                      GObject)

/**
 * AiMockProvider:
 *
 * An #AiProvider that returns a scripted sequence of replies instead of
 * calling anything.
 *
 * Shipped in the library rather than hidden in the test suite on
 * purpose.  Anything embedding ai-glib has the same problem the library
 * does -- exercising a tool loop without a network, an API key or a
 * bill -- and until this existed the multi-turn loop had no test at all,
 * because there was nothing to run it against.
 *
 * Replies are consumed in order; when the script runs out the provider
 * returns its fallback text, so a loop that runs longer than expected
 * terminates instead of hanging.
 */

AiMockProvider *ai_mock_provider_new (void);

/**
 * ai_mock_provider_push_text:
 * @self: an #AiMockProvider
 * @text: what to reply with
 *
 * Queues a plain text reply.
 */
void ai_mock_provider_push_text (AiMockProvider *self, const gchar *text);

/**
 * ai_mock_provider_push_tool_use:
 * @self: an #AiMockProvider
 * @tool_name: the tool to call
 * @input_json: its arguments
 *
 * Queues a reply asking for a tool call, which is how a test drives the
 * executor around its loop.
 */
void ai_mock_provider_push_tool_use (AiMockProvider *self,
                                     const gchar *tool_name,
                                     const gchar *input_json);

/**
 * ai_mock_provider_push_error:
 * @self: an #AiMockProvider
 * @message: the failure
 *
 * Queues a failure, so error paths are reachable without breaking
 * anything real.
 */
void ai_mock_provider_push_error (AiMockProvider *self, const gchar *message);

void ai_mock_provider_set_fallback (AiMockProvider *self, const gchar *text);

/**
 * ai_mock_provider_get_call_count:
 * @self: an #AiMockProvider
 *
 * Returns: how many requests have been made, so a test can assert a loop
 *   stopped when it should have rather than merely produced the right
 *   answer.
 */
guint ai_mock_provider_get_call_count (AiMockProvider *self);

guint ai_mock_provider_get_stream_call_count (AiMockProvider *self);

/**
 * ai_mock_provider_set_delay_ms:
 * @self: an #AiMockProvider
 * @ms: how long each reply takes
 *
 * Makes replies take time, so concurrency and cancellation are
 * observable.  With no delay every agent finishes before the next one
 * starts and nothing overlaps.
 */
void ai_mock_provider_set_delay_ms (AiMockProvider *self, guint ms);

G_END_DECLS
