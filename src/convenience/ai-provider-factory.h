/*
 * ai-provider-factory.h - Construct a provider by type
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

#include "core/ai-config.h"
#include "core/ai-enums.h"

G_BEGIN_DECLS

gboolean
ai_provider_factory_resolve_defaults(
	AiConfig       *config,
	const gchar    *app,
	const gchar    *provider_name,
	const gchar    *model,
	AiProviderType *out_provider,
	gchar         **out_model,
	GError        **error
);

GObject *
ai_provider_factory_new(
    AiProviderType   type,
    AiConfig        *config,
    GError         **error
);

GObject *
ai_provider_factory_new_from_string(
    const gchar  *name,
    AiConfig     *config,
    GError      **error
);

G_END_DECLS
