/*
 * ai-build-info.c - What was built, from where, and when
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The one translation unit that includes the generated ai-build-stamp.h.
 * Keeping it to one is what makes a commit cheap: the stamp changes, this
 * object recompiles, and everything else only relinks.
 */

#include "core/ai-build-info.h"
#include "core/ai-updater.h"

#include "ai-build-stamp.h"

/* An empty stamp field means "unknown"; the API answers NULL for it. */
static const gchar *
stamp_or_null(const gchar *value)
{
	return value[0] != '\0' ? value : NULL;
}

/**
 * ai_build_info_get_version:
 *
 * The release number from `config.mk`, e.g. `0.3.0`.
 *
 * This is the semantic version and nothing else; a build between two
 * releases still answers the last release here. The commit it was built
 * from is ai_build_info_get_describe().
 *
 * Returns: (transfer none): the release version
 */
const gchar *
ai_build_info_get_version(void)
{
	return AI_BUILD_STAMP_VERSION;
}

/**
 * ai_build_info_get_commit:
 *
 * The full object name of the commit the library was built from.
 *
 * Returns: (transfer none) (nullable): the commit, or %NULL when the
 *   build did not come from a git checkout
 */
const gchar *
ai_build_info_get_commit(void)
{
	return stamp_or_null(AI_BUILD_STAMP_COMMIT);
}

/**
 * ai_build_info_get_describe:
 *
 * `git describe --tags --always` for the build: the nearest tag and how
 * far past it, or an abbreviated commit when there is no tag. A dirty
 * tree is not reflected here; see ai_build_info_get_dirty().
 *
 * Returns: (transfer none) (nullable): the description, or %NULL when
 *   the build did not come from a git checkout
 */
const gchar *
ai_build_info_get_describe(void)
{
	return stamp_or_null(AI_BUILD_STAMP_DESCRIBE);
}

/**
 * ai_build_info_get_dirty:
 *
 * Whether tracked files differed from the commit when this was built.
 * Untracked files and submodule contents do not count.
 *
 * Returns: %TRUE for a build of uncommitted changes
 */
gboolean
ai_build_info_get_dirty(void)
{
	return AI_BUILD_STAMP_DIRTY != 0;
}

/**
 * ai_build_info_get_date:
 *
 * When the provenance last changed, in UTC ISO 8601. It moves when the
 * commit, the dirty flag or the install paths do, not on every rebuild,
 * and honours `SOURCE_DATE_EPOCH`.
 *
 * Returns: (transfer none): the build date
 */
const gchar *
ai_build_info_get_date(void)
{
	return AI_BUILD_STAMP_DATE;
}

/**
 * ai_build_info_get_source_dir:
 *
 * The absolute path of the checkout this was built in. The updater
 * starts from it; `AI_GLIB_SOURCE_DIR` and the `updates.source-dir`
 * config key override it there, not here.
 *
 * Returns: (transfer none) (nullable): the source directory
 */
const gchar *
ai_build_info_get_source_dir(void)
{
	return stamp_or_null(AI_BUILD_STAMP_SOURCE_DIR);
}

/**
 * ai_build_info_get_prefix:
 *
 * The `PREFIX` the build was configured with, which is where `make
 * install` put it.
 *
 * Returns: (transfer none): the install prefix
 */
const gchar *
ai_build_info_get_prefix(void)
{
	return AI_BUILD_STAMP_PREFIX;
}

/**
 * ai_build_info_dup_summary:
 *
 * One line for `--version` and an about box:
 * `0.3.0 (v0.3.0-4-g0123456789ab-dirty, built 2026-09-27T12:00:00Z)`,
 * or `0.3.0 (built ...)` for a build outside git.
 *
 * Returns: (transfer full): the summary
 */
gchar *
ai_build_info_dup_summary(void)
{
	const gchar *describe = ai_build_info_get_describe();

	if (describe == NULL)
		return g_strdup_printf("%s (built %s)", AI_BUILD_STAMP_VERSION,
		                       AI_BUILD_STAMP_DATE);

	return g_strdup_printf("%s (%s%s, built %s)", AI_BUILD_STAMP_VERSION,
	                       describe, AI_BUILD_STAMP_DIRTY ? "-dirty" : "",
	                       AI_BUILD_STAMP_DATE);
}

/* Private, declared in ai-updater.h: the updater installs where the
 * build was configured to. Plain comments rather than doc comments, so
 * g-ir-scanner does not go looking for them in a public header. */
const gchar *
ai_build_info_get_libdir(void)
{
	return AI_BUILD_STAMP_LIBDIR;
}

const gchar *
ai_build_info_get_includedir(void)
{
	return AI_BUILD_STAMP_INCLUDEDIR;
}

const gchar *
ai_build_info_get_build_type(void)
{
	return AI_BUILD_STAMP_TYPE;
}
