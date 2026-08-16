/*
 * ai-glib.h - Main umbrella header for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Include this header to use all ai-glib functionality:
 *   #include <ai-glib.h>
 */

#pragma once

#define AI_GLIB_INSIDE

/* Version information */
#include "ai-version.h"

/* Forward type declarations */
#include "ai-types.h"

/* Core infrastructure */
#include "core/ai-error.h"
#include "core/ai-enums.h"
#include "core/ai-config.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-event.h"
#include "core/ai-event-source.h"
#include "core/ai-image-capabilities.h"
#include "core/ai-image-generator.h"
#include "core/ai-client.h"
#include "core/ai-cli-client.h"
#include "core/ai-prompt-scorer.h"

/* View layer: a UI-agnostic model of a conversation */
#include "view/ai-style.h"
#include "view/ai-tool-call.h"
#include "view/ai-tool-style.h"
#include "view/ai-view-block.h"
#include "view/ai-view-blocks.h"
#include "view/ai-view-tool-block.h"
#include "view/ai-transcript.h"
#include "view/ai-conversation.h"

/* Model classes */
#include "model/ai-usage.h"
#include "model/ai-content-block.h"
#include "model/ai-text-content.h"
#include "model/ai-tool.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-image.h"
#include "model/ai-image-content.h"
#include "model/ai-image-request.h"
#include "model/ai-generated-image.h"
#include "model/ai-image-response.h"

/* Provider implementations (HTTP API) */
#include "providers/ai-claude-client.h"
#include "providers/ai-openai-client.h"
#include "providers/ai-grok-client.h"
#include "providers/ai-gemini-client.h"
#include "providers/ai-ollama-client.h"

/* Provider implementations (CLI wrappers) */
#include "providers/ai-claude-code-client.h"
#include "providers/ai-claude-tmux-client.h"
#include "providers/ai-opencode-client.h"
#include "providers/ai-grok-build-client.h"

/* Convenience API */
#include "convenience/ai-simple.h"
#include "convenience/ai-search-result.h"
#include "convenience/ai-search-options.h"
#include "convenience/ai-search-provider.h"
#include "convenience/ai-bing-search.h"
#include "convenience/ai-brave-search.h"
#include "convenience/ai-duckduckgo-search.h"
#include "convenience/ai-tool-executor.h"
#include "convenience/ai-provider-factory.h"

/* Agent orchestration */
#include "agent/ai-agent-enums.h"
#include "agent/ai-budget.h"
#include "agent/ai-price-table.h"
#include "agent/ai-agent-worker.h"
#include "agent/ai-agent-host.h"
#include "agent/ai-agent-store.h"
#include "agent/ai-agent-isolation.h"
#include "agent/ai-agent.h"
#include "agent/ai-brigade.h"
#include "agent/ai-mock-provider.h"

#undef AI_GLIB_INSIDE
