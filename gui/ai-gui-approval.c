/*
 * ai-gui-approval.c - Asking a person whether a tool may run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-gui-approval.h"

typedef struct
{
	GMainLoop      *loop;
	AiToolApproval  answer;
} AiGuiApprovalWait;

static void
on_response(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiApprovalWait *wait = user_data;
	const gchar *response;

	response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source),
	                                          result);

	if (g_strcmp0(response, "allow") == 0)
		wait->answer = AI_TOOL_APPROVAL_ALLOW;
	else if (g_strcmp0(response, "always") == 0)
		wait->answer = AI_TOOL_APPROVAL_ALLOW_ALWAYS;
	else if (g_strcmp0(response, "deny-all") == 0)
		wait->answer = AI_TOOL_APPROVAL_DENY_ALL;
	else
		wait->answer = AI_TOOL_APPROVAL_DENY;

	if (g_main_loop_is_running(wait->loop))
		g_main_loop_quit(wait->loop);
}

/* The arguments, as far as they fit. A model that has just been asked to
 * run `rm -rf` deserves more than its tool's name on screen. */
static gchar *
approval_detail(AiToolUse *tool_use)
{
	g_autoptr(AiToolCall) call = ai_tool_call_new(tool_use);
	g_autoptr(JsonGenerator) generator = json_generator_new();
	const gchar *target = ai_tool_call_get_target(call);
	JsonNode *input = ai_tool_use_get_input(tool_use);
	g_autofree gchar *arguments = NULL;

	if (input != NULL)
	{
		json_generator_set_pretty(generator, TRUE);
		json_generator_set_root(generator, input);
		arguments = json_generator_to_data(generator, NULL);
	}

	if (target != NULL && *target != '\0')
	{
		return g_strdup_printf("%s\n\n%s", target,
		                       arguments != NULL ? arguments : "");
	}

	return g_strdup(arguments != NULL ? arguments : "(no arguments)");
}

AiToolApproval
ai_gui_approval_ask(
	GtkWindow *parent,
	AiToolUse *tool_use
){
	AiGuiApprovalWait wait;
	AdwDialog *dialog;
	g_autofree gchar *heading = NULL;
	g_autofree gchar *detail = NULL;

	g_return_val_if_fail(AI_IS_TOOL_USE(tool_use), AI_TOOL_APPROVAL_DENY);

	heading = g_strdup_printf("Run %s?", ai_tool_use_get_name(tool_use));
	detail = approval_detail(tool_use);

	dialog = adw_alert_dialog_new(heading, NULL);
	adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), detail);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
		"deny", "Deny",
		"deny-all", "Deny and stop",
		"always", "Always allow",
		"allow", "Allow",
		NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
		"allow", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
		"deny-all", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "allow");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "deny");

	wait.answer = AI_TOOL_APPROVAL_DENY;
	wait.loop = g_main_loop_new(g_main_context_get_thread_default(), FALSE);

	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog),
	                        parent != NULL ? GTK_WIDGET(parent) : NULL,
	                        NULL, on_response, &wait);

	g_main_loop_run(wait.loop);
	g_main_loop_unref(wait.loop);

	return wait.answer;
}
