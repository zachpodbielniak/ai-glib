/*
 * ai-typesafe-client.h - TypeSafe System One client (Jev)
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

#include "core/ai-decider.h"

G_BEGIN_DECLS

/*
 * Model ids and aliases accepted by POST /v1/systemone.
 * jev-latest tracks the current stable release. The response names the
 * version that actually answered. Pin a version when a threshold was
 * tuned against one.
 */
#define AI_TYPESAFE_MODEL_JEV_LATEST  "jev-latest"
#define AI_TYPESAFE_MODEL_JEV_PREVIEW "jev-preview"
#define AI_TYPESAFE_MODEL_JEV_1_14    "jev-1.14.0"
#define AI_TYPESAFE_MODEL_JEV_1_13    "jev-1.13.0"

#define AI_TYPE_TYPESAFE_CLIENT (ai_typesafe_client_get_type())

G_DECLARE_FINAL_TYPE(AiTypesafeClient, ai_typesafe_client, AI, TYPESAFE_CLIENT, GObject)

/**
 * ai_typesafe_client_new:
 *
 * Creates a client for https://api.typesafe.ai. Reads `TYPESAFE_API_KEY`,
 * `TYPESAFE_BASE_URL` and `TYPESAFE_DEFAULT_MODEL` once, as the initial
 * property values. A later g_object_set() replaces them. No request is
 * sent here.
 *
 * Returns: (transfer full): a new TypeSafe client
 */
AiTypesafeClient *
ai_typesafe_client_new(
	void
);

G_END_DECLS
