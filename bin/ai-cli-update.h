/*
 * ai-cli-update.h - `ai --check-update` and `ai --update`
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Kept out of ai.c so the option parser only has to know two flags. The
 * wording comes from core/ai-update-status.h, the same sentences ai-tui
 * and ai-gui print; the pipeline is AiUpdater's.
 *
 * Exit status: 0 for a check, an install, or nothing to do; 1 for a
 * refusal or a failed step; 3 when everything but a privileged install
 * ran and the command to finish is printed. 2 stays with option errors.
 */

#pragma once

#include <stdio.h>
#include <unistd.h>

#include "ai-glib.h"
#include "core/ai-updater.h"

#define AI_CLI_UPDATE_EXIT_NEEDS_PRIVILEGE (3)

static void
ai_cli_update_on_output(AiUpdater *updater, const gchar *line, gpointer data)
{
	g_print("%s\n", line);
}

static gint
ai_cli_check_update(gboolean json)
{
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(AiUpdater) updater = ai_updater_new(config);
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autoptr(GError) error = NULL;

	/* An explicit check always fetches; the background interval is for
	 * checks nobody asked for. */
	status = ai_updater_check(updater, TRUE, NULL, &error);
	if (status == NULL)
	{
		g_printerr("ai: %s\n", error->message);
		return 1;
	}

	if (json)
	{
		g_autoptr(JsonNode) node = ai_update_status_to_json(status);
		g_autoptr(JsonGenerator) generator = json_generator_new();
		g_autofree gchar *text = NULL;
		JsonObject *object = json_node_get_object(node);

		json_object_set_string_member(object, "version", ai_build_info_get_version());
		if (ai_build_info_get_describe() != NULL)
			json_object_set_string_member(object, "describe", ai_build_info_get_describe());
		else
			json_object_set_null_member(object, "describe");
		json_object_set_boolean_member(object, "build_dirty", ai_build_info_get_dirty());
		json_object_set_string_member(object, "build_date", ai_build_info_get_date());
		json_generator_set_root(generator, node);
		json_generator_set_pretty(generator, TRUE);
		text = json_generator_to_data(generator, NULL);
		g_print("%s\n", text);
	}
	else
	{
		g_autofree gchar *summary = ai_update_status_dup_summary(status);

		g_print("%s\n", summary);
	}
	return 0;
}

static gint
ai_cli_run_update(void)
{
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(AiUpdater) updater = ai_updater_new(config);
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	/* sudo can only prompt on a terminal; without one the install is
	 * left for the user to run rather than hanging. */
	AiUpdateRunFlags flags = isatty(STDIN_FILENO) ? AI_UPDATE_RUN_INTERACTIVE
	                                              : AI_UPDATE_RUN_NONE;

	g_signal_connect(updater, "output", G_CALLBACK(ai_cli_update_on_output), NULL);
	result = ai_updater_run(updater, flags, NULL, &error);
	if (result == NULL)
	{
		const AiUpdateStatus *status = ai_updater_get_status(updater);

		if (status != NULL && status->state == AI_UPDATE_STATE_UP_TO_DATE &&
		    g_error_matches(error, AI_ERROR, AI_ERROR_INVALID_REQUEST))
		{
			g_print("%s\n", error->message);
			return 0;
		}
		g_printerr("ai: %s\n", error->message);
		return 1;
	}

	{
		g_autofree gchar *summary = ai_update_result_dup_summary(result);

		g_print("%s\n", summary);
	}
	return result->outcome == AI_UPDATE_OUTCOME_NEEDS_PRIVILEGE
		? AI_CLI_UPDATE_EXIT_NEEDS_PRIVILEGE : 0;
}
