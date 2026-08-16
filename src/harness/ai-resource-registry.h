/*
 * ai-resource-registry.h - Finding the other harnesses' files
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

#include "harness/ai-resource.h"

G_BEGIN_DECLS

#define AI_TYPE_RESOURCE_REGISTRY (ai_resource_registry_get_type())

G_DECLARE_FINAL_TYPE(AiResourceRegistry, ai_resource_registry,
                     AI, RESOURCE_REGISTRY, GObject)

AiResourceRegistry *
ai_resource_registry_new(void);

void
ai_resource_registry_set_working_directory(
    AiResourceRegistry *self,
    const gchar        *path
);

const gchar *
ai_resource_registry_get_working_directory(AiResourceRegistry *self);

void
ai_resource_registry_scan(AiResourceRegistry *self);

AiResource *
ai_resource_registry_lookup(
    AiResourceRegistry *self,
    AiResourceKind      kind,
    const gchar        *name
);

GList *
ai_resource_registry_list(
    AiResourceRegistry *self,
    AiResourceKind      kind
);

GList *
ai_resource_registry_list_shadowed(AiResourceRegistry *self);

void
ai_resource_registry_add(
    AiResourceRegistry *self,
    AiResource         *resource
);

void
ai_resource_registry_set_watching(
    AiResourceRegistry *self,
    gboolean            watching
);

gboolean
ai_resource_registry_get_watching(AiResourceRegistry *self);

gchar **
ai_resource_registry_get_search_paths(
    AiResourceRegistry *self,
    AiResourceKind      kind
);

G_END_DECLS
