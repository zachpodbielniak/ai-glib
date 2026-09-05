/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include "core/ai-json-util.h"
#include "core/ai-error.h"

/* Merge the native Bash sandbox switch into settings without discarding
 * hooks, permissions, network rules or filesystem rules. NULL inherits. */
static inline gchar *
ai_claude_sandbox_settings(const gchar *settings, const gchar *mode, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *contents = NULL;
    JsonObject *root, *sandbox;
    gboolean enabled;

    if (mode == NULL) return g_strdup(settings);
    if (g_strcmp0(mode, "enabled") != 0 && g_strcmp0(mode, "disabled") != 0)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                            "Claude sandbox must be enabled or disabled");
        return NULL;
    }
    enabled = g_str_equal(mode, "enabled");
    while (settings != NULL && g_ascii_isspace(*settings)) settings++;
    if (settings != NULL && *settings && *settings != '{')
    {
        if (!g_file_get_contents(settings, &contents, NULL, error)) return NULL;
        settings = contents;
    }
    if (!json_parser_load_from_data(parser, settings != NULL && *settings ? settings : "{}", -1, error))
        return NULL;
    root = ai_json_root_object(parser);
    if (root == NULL)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Claude settings must be a JSON object");
        return NULL;
    }
    sandbox = ai_json_get_object(root, "sandbox");
    if (sandbox == NULL && ai_json_get_node(root, "sandbox") != NULL)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                            "Claude sandbox settings must be a JSON object");
        return NULL;
    }
    if (sandbox == NULL)
    {
        sandbox = json_object_new();
        json_object_set_object_member(root, "sandbox", sandbox);
    }
    json_object_set_boolean_member(sandbox, "enabled", enabled);
    if (enabled)
    {
        json_object_set_boolean_member(sandbox, "allowUnsandboxedCommands", FALSE);
        json_object_set_boolean_member(sandbox, "failIfUnavailable", TRUE);
    }
    return json_to_string(json_parser_get_root(parser), FALSE);
}
