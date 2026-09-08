/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "core/ai-cli-client.h"

G_BEGIN_DECLS

/**
 * AiCliReportKind:
 * @AI_CLI_REPORT_USAGE: Current quota windows or usage totals.
 * @AI_CLI_REPORT_HISTORY: Historical usage periods reported by the provider.
 *
 * Read-only account reporting, independent of model turns.
 */
typedef enum {
	AI_CLI_REPORT_USAGE,
	AI_CLI_REPORT_HISTORY
} AiCliReportKind;

#define AI_TYPE_CLI_REPORT (ai_cli_report_get_type())
G_DECLARE_FINAL_TYPE(AiCliReport, ai_cli_report, AI, CLI_REPORT, GObject)

const gchar *ai_cli_report_get_provider(AiCliReport *self);
AiCliReportKind ai_cli_report_get_kind(AiCliReport *self);
JsonNode *ai_cli_report_dup_data(AiCliReport *self);
gchar *ai_cli_report_to_json(AiCliReport *self);
gchar *ai_cli_report_to_text(AiCliReport *self);

AiCliReport *ai_cli_client_query_report(AiCliClient *self, AiCliReportKind kind,
	guint limit, GCancellable *cancellable, GError **error);
void ai_cli_client_query_report_async(AiCliClient *self, AiCliReportKind kind,
	guint limit, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
AiCliReport *ai_cli_client_query_report_finish(AiCliClient *self, GAsyncResult *result, GError **error);

G_END_DECLS
