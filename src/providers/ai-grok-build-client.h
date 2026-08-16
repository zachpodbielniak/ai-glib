/*
 * ai-grok-build-client.h - Grok Build CLI client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "core/ai-cli-client.h"
#include "core/ai-config.h"

G_BEGIN_DECLS

#define AI_TYPE_GROK_BUILD_CLIENT (ai_grok_build_client_get_type())

G_DECLARE_FINAL_TYPE(AiGrokBuildClient, ai_grok_build_client, AI, GROK_BUILD_CLIENT, AiCliClient)

/**
 * AI_GROK_BUILD_DEFAULT_MODEL:
 *
 * The default model for Grok Build clients.
 */
#define AI_GROK_BUILD_DEFAULT_MODEL "grok-4.6"

/*
 * Model IDs accepted by the `grok` CLI's --model argument.
 *
 * These are the agentic build models, not the raw xAI API model IDs --
 * see AI_GROK_MODEL_* in ai-grok-client.h for those. Run `grok models`
 * to see what the installed CLI actually offers.
 */
#define AI_GROK_BUILD_MODEL_GROK_4_6    "grok-4.6"
#define AI_GROK_BUILD_MODEL_GROK_4_5    "grok-4.5"

/*
 * Floating alias for the newest build model.
 */
#define AI_GROK_BUILD_MODEL_LATEST      AI_GROK_BUILD_MODEL_GROK_4_6


AiGrokBuildClient *
ai_grok_build_client_new(void);

AiGrokBuildClient *
ai_grok_build_client_new_with_config(AiConfig *config);

gdouble
ai_grok_build_client_get_total_cost(AiGrokBuildClient *self);

gboolean
ai_grok_build_client_get_skip_permissions(AiGrokBuildClient *self);

void
ai_grok_build_client_set_skip_permissions(
    AiGrokBuildClient *self,
    gboolean           skip
);

const gchar *
ai_grok_build_client_get_permission_mode(AiGrokBuildClient *self);

void
ai_grok_build_client_set_permission_mode(
    AiGrokBuildClient *self,
    const gchar       *mode
);

const gchar *
ai_grok_build_client_get_allowed_tools(AiGrokBuildClient *self);

void
ai_grok_build_client_set_allowed_tools(
    AiGrokBuildClient *self,
    const gchar       *tools
);

const gchar *
ai_grok_build_client_get_disallowed_tools(AiGrokBuildClient *self);

void
ai_grok_build_client_set_disallowed_tools(
    AiGrokBuildClient *self,
    const gchar       *tools
);

const gchar *
ai_grok_build_client_get_sandbox(AiGrokBuildClient *self);

void
ai_grok_build_client_set_sandbox(
    AiGrokBuildClient *self,
    const gchar       *profile
);

gint
ai_grok_build_client_get_max_turns(AiGrokBuildClient *self);

void
ai_grok_build_client_set_max_turns(
    AiGrokBuildClient *self,
    gint               max_turns
);

const gchar *
ai_grok_build_client_get_agent(AiGrokBuildClient *self);

void
ai_grok_build_client_set_agent(
    AiGrokBuildClient *self,
    const gchar       *agent
);

const gchar *
ai_grok_build_client_get_rules(AiGrokBuildClient *self);

void
ai_grok_build_client_set_rules(
    AiGrokBuildClient *self,
    const gchar       *rules
);

gboolean
ai_grok_build_client_get_disable_web_search(AiGrokBuildClient *self);

void
ai_grok_build_client_set_disable_web_search(
    AiGrokBuildClient *self,
    gboolean           disable
);

gboolean
ai_grok_build_client_get_verbatim(AiGrokBuildClient *self);

void
ai_grok_build_client_set_verbatim(
    AiGrokBuildClient *self,
    gboolean           verbatim
);

G_END_DECLS
