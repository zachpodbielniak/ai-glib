/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#define AI_TYPE_CALL_CONFIG (ai_call_config_get_type())
G_DECLARE_FINAL_TYPE(AiCallConfig, ai_call_config, AI, CALL_CONFIG, GObject)
AiCallConfig *
ai_call_config_new(void);
gboolean
ai_call_config_load(AiCallConfig *self, const gchar *path, GError **error);
gboolean
ai_call_config_set_text(AiCallConfig *self, const gchar *name, const gchar *text,
						GError **error);
gboolean
ai_call_config_apply_environment(AiCallConfig *self, GError **error);
