/*
 * ai-resource.h - A command, skill or agent, as a file on disk
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

G_BEGIN_DECLS

/**
 * AiResourceKind:
 * @AI_RESOURCE_COMMAND: a `/name` the user invokes
 * @AI_RESOURCE_SKILL: instructions loaded into context on demand
 * @AI_RESOURCE_AGENT: a persona a subagent runs under
 *
 * What a resource file is for.
 *
 * The three differ in where they are found and what invoking one does,
 * not in how they are stored --- all three are markdown with YAML
 * frontmatter, which is why one type covers them.
 */
typedef enum
{
    AI_RESOURCE_COMMAND = 0,
    AI_RESOURCE_SKILL,
    AI_RESOURCE_AGENT
} AiResourceKind;

/**
 * AiResourceScope:
 * @AI_RESOURCE_SCOPE_BUILTIN: compiled into ai-glib
 * @AI_RESOURCE_SCOPE_USER: found under the user's home directory
 * @AI_RESOURCE_SCOPE_PROJECT: found under the working directory
 *
 * Where a resource came from, which decides what shadows what.
 *
 * Project beats user beats builtin, so a repository can override a
 * personal command without the user editing anything.
 */
typedef enum
{
    AI_RESOURCE_SCOPE_BUILTIN = 0,
    AI_RESOURCE_SCOPE_USER,
    AI_RESOURCE_SCOPE_PROJECT
} AiResourceScope;

#define AI_TYPE_RESOURCE (ai_resource_get_type())

G_DECLARE_FINAL_TYPE(AiResource, ai_resource, AI, RESOURCE, GObject)

AiResource *
ai_resource_new_from_file(
    const gchar     *path,
    const gchar     *name,
    AiResourceKind   kind,
    const gchar     *origin,
    AiResourceScope  scope,
    GError         **error
);

AiResource *
ai_resource_new_from_data(
    const gchar     *data,
    gssize           length,
    const gchar     *name,
    AiResourceKind   kind,
    const gchar     *origin,
    AiResourceScope  scope,
    GError         **error
);

AiResourceKind
ai_resource_get_kind(AiResource *self);

AiResourceScope
ai_resource_get_scope(AiResource *self);

const gchar *
ai_resource_get_name(AiResource *self);

const gchar *
ai_resource_get_description(AiResource *self);

const gchar *
ai_resource_get_body(AiResource *self);

const gchar *
ai_resource_get_path(AiResource *self);

const gchar *
ai_resource_get_origin(AiResource *self);

const gchar *
ai_resource_get_meta(
    AiResource  *self,
    const gchar *key
);

gchar **
ai_resource_get_meta_list(
    AiResource  *self,
    const gchar *key
);

gboolean
ai_resource_get_meta_boolean(
    AiResource  *self,
    const gchar *key,
    gboolean     fallback
);

gchar **
ai_resource_get_meta_keys(AiResource *self);

const gchar *
ai_resource_kind_to_string(AiResourceKind kind);

const gchar *
ai_resource_scope_to_string(AiResourceScope scope);

G_END_DECLS
