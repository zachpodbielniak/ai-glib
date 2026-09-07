/* Private tool preview renderer. SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#ifndef AI_GLIB_COMPILATION
#error "This is a private ai-glib header."
#endif

#include "view/ai-tool-call.h"
#include "view/ai-style.h"

void _ai_tool_preview_append(AiToolCall *call, AiRenderedText *out, gboolean expanded);
