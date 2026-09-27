/*
 * ai-build-info.h - What was built, from where, and when
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

#include <glib.h>

G_BEGIN_DECLS

const gchar *
ai_build_info_get_version(void);

const gchar *
ai_build_info_get_commit(void);

const gchar *
ai_build_info_get_describe(void);

gboolean
ai_build_info_get_dirty(void);

const gchar *
ai_build_info_get_date(void);

const gchar *
ai_build_info_get_source_dir(void);

const gchar *
ai_build_info_get_prefix(void);

gchar *
ai_build_info_dup_summary(void);

G_END_DECLS
