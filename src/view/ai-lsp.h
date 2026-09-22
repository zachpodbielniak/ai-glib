/* Private language-server semantic tokens. SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#ifndef AI_GLIB_COMPILATION
#error "This is a private ai-glib header."
#endif

#include "view/ai-markup.h"

const gchar *
ai_lsp_command_for_language(const gchar *language);

GArray *
ai_lsp_semantic_tokens(const gchar *text, GError **error);
