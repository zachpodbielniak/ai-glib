/*
 * harness-input.c - The input pipeline, without a terminal
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Takes a line of input, shows what a frontend would do with it, and
 * prints the completion candidates for a cursor position. No ncurses, no
 * network, no API key --- which is the point: the input pipeline is
 * library code, and this proves it can be driven from anywhere.
 *
 * Read this before writing the Emacs side.
 *
 *   make examples
 *   ./build/release/examples/harness-input 'explain @src/core/ai-event.c'
 *   ./build/release/examples/harness-input '/help'
 *   ./build/release/examples/harness-input --complete '@src/co'
 */

#include <ai-glib.h>

#include <stdlib.h>
#include <string.h>

/* Print every command, skill and agent that was found. */
static void
list_everything(AiCommandSet *commands)
{
    GList *iter;
    GList *found = ai_command_set_list(commands);

    g_print("Found %u commands:\n\n", g_list_length(found));

    for (iter = found; iter != NULL; iter = iter->next)
    {
        AiCommand   *command = iter->data;
        const gchar *hint = ai_command_get_argument_hint(command);
        const gchar *description = ai_command_get_description(command);

        g_print("  /%s%s%s\n", ai_command_get_name(command),
                hint != NULL ? " " : "", hint != NULL ? hint : "");

        if (description != NULL)
        {
            g_print("      %s  [%s]\n", description,
                    ai_command_get_origin(command));
        }
    }

    g_list_free_full(found, g_object_unref);
}

/*
 * The completion half.
 *
 * The range is the whole product: a frontend replaces buffer[start..end)
 * with a candidate's text and does nothing else. Printing it here makes
 * that concrete --- and makes an off-by-one visible, which it never is
 * once a menu is drawing over it.
 */
static void
show_completion(
    AiCommandSet *commands,
    const gchar  *buffer,
    const gchar  *cwd
){
    g_autoptr(AiCompletionContext) context =
        ai_completion_context_new(commands, cwd);
    g_autoptr(AiCompletionResult) result = NULL;
    g_autofree gchar             *prefix = NULL;
    guint                         n;
    guint                         i;

    /* At the end of the buffer, which is where a user's cursor is while
     * they are typing. */
    result = ai_completion_context_query(context, buffer, (guint)strlen(buffer));

    n = ai_completion_result_get_n_items(result);

    g_print("Completing %s\n", buffer);
    g_print("  replacing bytes [%u, %u) --- \"%.*s\"\n",
            ai_completion_result_get_start(result),
            ai_completion_result_get_end(result),
            (gint)(ai_completion_result_get_end(result) -
                   ai_completion_result_get_start(result)),
            buffer + ai_completion_result_get_start(result));

    if (n == 0)
    {
        g_print("  no candidates\n");
        return;
    }

    prefix = ai_completion_result_get_common_prefix(result);
    g_print("  common prefix: \"%s\"\n", prefix != NULL ? prefix : "");
    g_print("  %u candidate%s:\n", n, n == 1 ? "" : "s");

    for (i = 0; i < n; i++)
    {
        const gchar *text = NULL;
        const gchar *description = NULL;
        const gchar *origin = NULL;
        gboolean     is_directory = FALSE;

        /*
         * Out-parameters rather than the struct, because that is the
         * shape bindings get --- and this example exists to show the
         * path an Emacs frontend takes.
         */
        ai_completion_result_get_item_fields(result, i, &text, NULL,
                                             &description, &origin,
                                             &is_directory);

        g_print("    %-40s %s%s%s%s\n", text, is_directory ? "(dir) " : "",
                description != NULL ? description : "",
                origin != NULL ? "  " : "",
                origin != NULL ? origin : "");
    }
}

/* Resolve a line and print what would be sent. */
static void
show_resolution(
    AiCommandSet *commands,
    const gchar  *line,
    const gchar  *cwd
){
    g_autoptr(AiCommandResult) resolved = NULL;
    g_autoptr(GError)          error = NULL;
    g_autofree gchar          *expanded = NULL;
    GList                     *files = NULL;
    GList                     *iter;
    const gchar               *source = line;

    resolved = ai_command_set_resolve(commands, line, cwd, NULL, &error);

    if (resolved == NULL)
    {
        /* Only an unknown /name gets here. A line that is not a command
         * at all resolves successfully, which is what lets a caller run
         * every line through here unconditionally. */
        g_printerr("harness-input: %s\n", error->message);
        return;
    }

    switch (ai_command_result_get_outcome(resolved))
    {
        case AI_COMMAND_OUTCOME_NOT_A_COMMAND:
            g_print("An ordinary prompt.\n\n");
            break;

        case AI_COMMAND_OUTCOME_BUILTIN:
            /* Nothing is sent. The frontend acts on the name --- this
             * one has no transcript to clear, so it says so. */
            g_print("A built-in: /%s%s%s\n",
                    ai_command_result_get_name(resolved),
                    ai_command_result_get_arguments(resolved)[0] != '\0'
                        ? " " : "",
                    ai_command_result_get_arguments(resolved));
            g_print("Nothing would be sent; the frontend handles it.\n");
            return;

        case AI_COMMAND_OUTCOME_AGENT:
            g_print("Dispatches the agent /%s\n\n",
                    ai_command_result_get_name(resolved));
            source = ai_command_result_get_prompt(resolved);
            break;

        case AI_COMMAND_OUTCOME_PROMPT:
        default:
            g_print("Expands the command /%s\n\n",
                    ai_command_result_get_name(resolved));
            source = ai_command_result_get_prompt(resolved);
            break;
    }

    /*
     * Mentions are expanded after resolution, once. A @path in a command
     * body and one the user typed are treated identically, and neither
     * is expanded twice.
     */
    expanded = ai_mention_expand(source != NULL ? source : "", cwd, 0,
                                 &files);

    g_print("--- would send ---\n%s\n", expanded);

    if (files != NULL)
    {
        g_print("--- files pulled in ---\n");

        for (iter = files; iter != NULL; iter = iter->next)
        {
            g_print("  %s\n", (const gchar *)iter->data);
        }
    }

    g_list_free_full(files, g_free);
}

int
main(int argc, char *argv[])
{
    g_autoptr(AiResourceRegistry) registry = NULL;
    g_autoptr(AiCommandSet)       commands = NULL;
    g_autofree gchar             *cwd = g_get_current_dir();
    gboolean                      complete = FALSE;
    const gchar                  *line;

    if (argc < 2)
    {
        g_printerr("usage: %s [--complete] <line>\n"
                   "       %s --list\n", argv[0], argv[0]);
        return 2;
    }

    if (g_strcmp0(argv[1], "--complete") == 0)
    {
        complete = TRUE;

        if (argc < 3)
        {
            g_printerr("--complete needs a line\n");
            return 2;
        }
    }

    line = complete ? argv[2] : argv[1];

    /* Everything the harness layer needs: a registry, scanned, and a
     * command set over it. Three lines, and it is done. */
    registry = ai_resource_registry_new();
    ai_resource_registry_set_working_directory(registry, cwd);
    ai_resource_registry_scan(registry);

    commands = ai_command_set_new(registry);

    if (g_strcmp0(argv[1], "--list") == 0)
    {
        list_everything(commands);
        return 0;
    }

    if (complete)
    {
        show_completion(commands, line, cwd);
        return 0;
    }

    show_resolution(commands, line, cwd);

    return 0;
}
