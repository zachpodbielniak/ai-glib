/*
 * ai-updater.h - Check for, and install, a newer build of ai-glib
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API: it is
 * what `ai`, `ai-tui` and `ai-gui` share to update themselves from the
 * checkout that built them, and a library consumer has no such
 * checkout. The introspectable half is ai-build-info.h.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-updater.h is an internal header"
#endif

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "core/ai-config.h"
#include "core/ai-update-status.h"

G_BEGIN_DECLS

#define AI_TYPE_UPDATER (ai_updater_get_type())

G_DECLARE_FINAL_TYPE(AiUpdater, ai_updater, AI, UPDATER, GObject)

/*
 * AiUpdateRunFlags:
 * @AI_UPDATE_RUN_NONE: no terminal; a privileged install is not
 *   attempted, and the result says which command to run instead
 * @AI_UPDATE_RUN_INTERACTIVE: the caller's terminal belongs to the
 *   update, so an install into a prefix this user cannot write runs
 *   under sudo with the terminal attached, where it can prompt
 */
typedef enum
{
	AI_UPDATE_RUN_NONE        = 0,
	AI_UPDATE_RUN_INTERACTIVE = 1 << 0
} AiUpdateRunFlags;

typedef enum
{
	AI_UPDATE_OUTCOME_INSTALLED,
	AI_UPDATE_OUTCOME_NEEDS_PRIVILEGE
} AiUpdateOutcome;

/*
 * AiUpdateResult:
 * @privileged_command: for %AI_UPDATE_OUTCOME_NEEDS_PRIVILEGE, the one
 *   command a person runs in a terminal to finish; everything before the
 *   install has already happened
 * @log_path: where the whole run was written
 */
typedef struct
{
	AiUpdateOutcome outcome;
	gchar          *from_version;
	gchar          *to_version;
	gchar          *from_commit;
	gchar          *to_commit;
	gchar          *privileged_command;
	gchar          *prefix;
	gchar          *log_path;
} AiUpdateResult;

void
ai_update_result_free(AiUpdateResult *result);

gchar *
ai_update_result_dup_summary(const AiUpdateResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiUpdateResult, ai_update_result_free)

/* Implemented in ai-build-info.c, the one translation unit that includes
 * the generated stamp, so a commit recompiles that file alone. */
const gchar *
ai_build_info_get_libdir(void);

const gchar *
ai_build_info_get_includedir(void);

const gchar *
ai_build_info_get_build_type(void);

gboolean
ai_updater_checks_enabled(AiConfig *config);

AiUpdater *
ai_updater_new(AiConfig *config);

const AiUpdateStatus *
ai_updater_get_status(AiUpdater *self);

gboolean
ai_updater_is_checking(AiUpdater *self);

gboolean
ai_updater_load_cache(AiUpdater *self);

AiUpdateStatus *
ai_updater_check(
	AiUpdater     *self,
	gboolean       fetch,
	GCancellable  *cancellable,
	GError       **error
);

void
ai_updater_check_async(
	AiUpdater           *self,
	gboolean             fetch,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

AiUpdateStatus *
ai_updater_check_finish(
	AiUpdater     *self,
	GAsyncResult  *result,
	GError       **error
);

void
ai_updater_start(AiUpdater *self);

void
ai_updater_stop(AiUpdater *self);

AiUpdateResult *
ai_updater_run(
	AiUpdater         *self,
	AiUpdateRunFlags   flags,
	GCancellable      *cancellable,
	GError           **error
);

void
ai_updater_run_async(
	AiUpdater           *self,
	AiUpdateRunFlags     flags,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

AiUpdateResult *
ai_updater_run_finish(
	AiUpdater     *self,
	GAsyncResult  *result,
	GError       **error
);

JsonNode *
ai_update_status_to_json(const AiUpdateStatus *status);

AiUpdateStatus *
ai_update_status_from_json(JsonNode *node);

G_END_DECLS
