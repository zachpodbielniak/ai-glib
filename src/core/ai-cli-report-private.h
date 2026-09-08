/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include "core/ai-cli-report.h"
#include "core/ai-json-util.h"

/* Report construction and native-provider adapters, not installed API. */
AiCliReport *_ai_cli_report_new(AiCliClient *client, AiCliReportKind kind, const gchar *source);
JsonObject *_ai_cli_report_object(AiCliReport *report);
JsonObject *_ai_cli_report_add_entry(AiCliReport *report, const gchar *label, const gchar *unit);
AiCliReport *_ai_cli_report_query_native(AiCliClient *client, AiCliReportKind kind,
	guint limit, GCancellable *cancellable, GError **error);
