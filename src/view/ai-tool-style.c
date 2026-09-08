/*
 * ai-tool-style.c - How a tool name reads in a transcript
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "view/ai-tool-style.h"

G_DEFINE_BOXED_TYPE(AiToolStyle, ai_tool_style,
                    ai_tool_style_copy, ai_tool_style_free)

/*
 * The built-in vocabulary.
 *
 * Three families are covered. The lowercase names are ai-glib's own built-in
 * tools, the ones AiToolExecutor runs. The capitalised ones are Claude Code's
 * (a transcript of a claude-code run sees "Edit", never "edit"). Grok Build
 * streams snake_case (`read_file`, `search_replace`, `run_terminal_command`)
 * and Codex streams `file_change` / `command_execution`. All of them belong
 * here because all of them end up in the same transcript.
 *
 * target_key is the input parameter that names what the call acted on. It
 * is what turns "Edited 1 file" into "Edited ai-style.c", which is most of
 * what makes a collapsed summary worth reading.
 */
static const AiToolStyle BUILTIN_STYLES[] = {
    /* ai-glib's own executor tools */
    { "bash",         "Ran",       "command", "commands", AI_TOOL_CATEGORY_COMMAND,    "command", FALSE },
    { "read",         "Read",      "file",    "files",    AI_TOOL_CATEGORY_FILE_READ,  "path",    FALSE },
    { "write",        "Created",   "file",    "files",    AI_TOOL_CATEGORY_FILE_WRITE, "path",    TRUE  },
    { "edit",         "Edited",    "file",    "files",    AI_TOOL_CATEGORY_FILE_WRITE, "path",    TRUE  },
    { "glob",         "Searched",  "pattern", "patterns", AI_TOOL_CATEGORY_SEARCH,     "pattern", FALSE },
    { "grep",         "Searched",  "pattern", "patterns", AI_TOOL_CATEGORY_SEARCH,     "pattern", FALSE },
    { "ls",           "Listed",    "path",    "paths",    AI_TOOL_CATEGORY_FILE_READ,  "path",    FALSE },
    { "web_fetch",    "Fetched",   "page",    "pages",    AI_TOOL_CATEGORY_NETWORK,    "url",     FALSE },
    { "web_search",   "Searched",  "query",   "queries",  AI_TOOL_CATEGORY_SEARCH,     "query",   FALSE },
    { "multi_edit",   "Edited",    "file",    "files",    AI_TOOL_CATEGORY_FILE_WRITE, "path",    TRUE  },
    { "todo_write",   "Updated",   "todo list", "todo lists", AI_TOOL_CATEGORY_TASK,   NULL,      FALSE },
    { "task",         "Ran",       "agent",   "agents",   AI_TOOL_CATEGORY_TASK,       "agent",   FALSE },
    { "skill",        "Loaded",    "skill",   "skills",   AI_TOOL_CATEGORY_TASK,       "name",    FALSE },
    { "agent_spawn",  "Started",   "agent",   "agents",   AI_TOOL_CATEGORY_TASK,       "description", FALSE },
    { "agent_status", "Checked",   "agent",   "agents",   AI_TOOL_CATEGORY_TASK,       "agent_id", FALSE },
    { "agent_result", "Collected", "agent",   "agents",   AI_TOOL_CATEGORY_TASK,       "agent_id", FALSE },
    { "agent_wait",   "Waited for","agent",   "agents",   AI_TOOL_CATEGORY_TASK,       "agent_id", FALSE },
    { "agent_cancel", "Stopped",   "agent",   "agents",   AI_TOOL_CATEGORY_TASK,       "agent_id", FALSE },

    /* What the wrapped CLIs call theirs */
    { "Bash",         "Ran",       "command", "commands", AI_TOOL_CATEGORY_COMMAND,    "command", FALSE },
    { "Read",         "Read",      "file",    "files",    AI_TOOL_CATEGORY_FILE_READ,  "file_path", FALSE },
    { "Write",        "Created",   "file",    "files",    AI_TOOL_CATEGORY_FILE_WRITE, "file_path", TRUE  },
    { "Edit",         "Edited",    "file",    "files",    AI_TOOL_CATEGORY_FILE_WRITE, "file_path", TRUE  },
    { "MultiEdit",    "Edited",    "file",    "files",    AI_TOOL_CATEGORY_FILE_WRITE, "file_path", TRUE  },
	{ "apply_patch", "Patched", "file", "files", AI_TOOL_CATEGORY_FILE_WRITE, NULL, FALSE },
	{ "file_change", "Changed", "file", "files", AI_TOOL_CATEGORY_FILE_WRITE, NULL, FALSE },
	{ "command_execution", "Ran", "command", "commands", AI_TOOL_CATEGORY_COMMAND, "command", FALSE },
    { "NotebookEdit", "Edited",    "notebook","notebooks",AI_TOOL_CATEGORY_FILE_WRITE, "notebook_path", TRUE },
    { "Glob",         "Searched",  "pattern", "patterns", AI_TOOL_CATEGORY_SEARCH,     "pattern", FALSE },
    { "Grep",         "Searched",  "pattern", "patterns", AI_TOOL_CATEGORY_SEARCH,     "pattern", FALSE },
    { "WebFetch",     "Fetched",   "page",    "pages",    AI_TOOL_CATEGORY_NETWORK,    "url",     FALSE },
    { "WebSearch",    "Searched",  "query",   "queries",  AI_TOOL_CATEGORY_SEARCH,     "query",   FALSE },
    { "Task",         "Delegated", "task",    "tasks",    AI_TOOL_CATEGORY_TASK,       "description", FALSE },
    { "TodoWrite",    "Updated",   "todo",    "todos",    AI_TOOL_CATEGORY_TASK,       NULL,      FALSE },
    { "Skill",        "Loaded",    "skill",   "skills",   AI_TOOL_CATEGORY_TASK,       "command", FALSE },
    { "Agent",        "Delegated", "task",    "tasks",    AI_TOOL_CATEGORY_TASK,       "description", FALSE },
    { "AskUserQuestion", "Asked",  "question","questions",AI_TOOL_CATEGORY_TASK,       NULL,      FALSE },
    { "ToolSearch",   "Searched",  "tool",    "tools",    AI_TOOL_CATEGORY_SEARCH,     "query",   FALSE },
    { "SendMessage",  "Sent",      "message", "messages", AI_TOOL_CATEGORY_TASK,       "to",      FALSE },
    { "TaskStop",     "Stopped",   "task",    "tasks",    AI_TOOL_CATEGORY_TASK,       "task_id", FALSE },

    /*
     * Grok Build's streaming-messages-json names. These are what actually
     * arrive on the wire; the Claude-shaped Write/Bash names in the table
     * above still match if a session happens to emit them.
     */
    { "read_file",              "Read",      "file",     "files",     AI_TOOL_CATEGORY_FILE_READ,  "target_file", FALSE },
    { "search_replace",         "Edited",    "file",     "files",     AI_TOOL_CATEGORY_FILE_WRITE, "file_path",   TRUE  },
    { "run_terminal_command",   "Ran",       "command",  "commands",  AI_TOOL_CATEGORY_COMMAND,    "command",     FALSE },
    { "list_dir",               "Listed",    "path",     "paths",     AI_TOOL_CATEGORY_FILE_READ,  "target_directory", FALSE },
    { "delete_file",            "Deleted",   "file",     "files",     AI_TOOL_CATEGORY_FILE_WRITE, "file_path",   FALSE },
    { "edit_notebook",          "Edited",    "notebook", "notebooks", AI_TOOL_CATEGORY_FILE_WRITE, "target_notebook", TRUE },
    { "get_task_output",        "Collected", "task",     "tasks",     AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "get_command_or_subagent_output", "Collected", "task", "tasks", AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "get_terminal_command_output", "Collected", "command", "commands", AI_TOOL_CATEGORY_COMMAND, NULL,          FALSE },
    { "kill_task",              "Stopped",   "task",     "tasks",     AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "kill_command_or_subagent","Stopped",  "task",     "tasks",     AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "kill_terminal_command",  "Stopped",   "command",  "commands",  AI_TOOL_CATEGORY_COMMAND,    NULL,          FALSE },
    { "wait_commands_or_subagents", "Waited for", "task", "tasks",    AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "spawn_subagent",         "Started",   "agent",    "agents",    AI_TOOL_CATEGORY_TASK,       "description", FALSE },
    { "send_subagent_message",  "Messaged",  "agent",    "agents",    AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "enter_plan_mode",        "Entered",   "plan",     "plans",     AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "exit_plan_mode",         "Left",      "plan",     "plans",     AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "ask_user_question",      "Asked",     "question", "questions", AI_TOOL_CATEGORY_TASK,       NULL,          FALSE },
    { "search_tool",            "Searched",  "tool",     "tools",     AI_TOOL_CATEGORY_SEARCH,     "query",       FALSE }
};

/*
 * Styles registered at runtime, name -> AiToolStyle. Consulted before the
 * built-in table so a host can override an entry as well as add one.
 */
static GHashTable *registered_styles = NULL;

/*
 * Generic wording, by category, for a tool nobody has described.
 *
 * An unknown tool renders under one of these rather than not at all: a
 * transcript that silently omitted a call would be lying about what ran,
 * which is worse than calling it "Used 1 tool".
 */
static const AiToolStyle CATEGORY_FALLBACKS[AI_TOOL_N_CATEGORIES] = {
    { NULL, "Used",      "tool",    "tools",    AI_TOOL_CATEGORY_OTHER,      NULL, FALSE },
    { NULL, "Read",      "file",    "files",    AI_TOOL_CATEGORY_FILE_READ,  NULL, FALSE },
    { NULL, "Changed",   "file",    "files",    AI_TOOL_CATEGORY_FILE_WRITE, NULL, TRUE  },
    { NULL, "Ran",       "command", "commands", AI_TOOL_CATEGORY_COMMAND,    NULL, FALSE },
    { NULL, "Searched",  "query",   "queries",  AI_TOOL_CATEGORY_SEARCH,     NULL, FALSE },
    { NULL, "Fetched",   "page",    "pages",    AI_TOOL_CATEGORY_NETWORK,    NULL, FALSE },
    { NULL, "Delegated", "task",    "tasks",    AI_TOOL_CATEGORY_TASK,       NULL, FALSE }
};

/**
 * ai_tool_style_copy:
 * @self: (nullable): an #AiToolStyle
 *
 * Copies @self, including its strings.
 *
 * Returns: (transfer full) (nullable): a copy of @self
 */
AiToolStyle *
ai_tool_style_copy(const AiToolStyle *self)
{
    AiToolStyle *copy;

    if (self == NULL)
    {
        return NULL;
    }

    copy = g_new0(AiToolStyle, 1);
    copy->tool_name = g_strdup(self->tool_name);
    copy->verb = g_strdup(self->verb);
    copy->noun_singular = g_strdup(self->noun_singular);
    copy->noun_plural = g_strdup(self->noun_plural);
    copy->target_key = g_strdup(self->target_key);
    copy->category = self->category;
    copy->counts_diff = self->counts_diff;

    return copy;
}

/**
 * ai_tool_style_free:
 * @self: (nullable): an #AiToolStyle
 *
 * Frees a style made by ai_tool_style_copy().
 *
 * The built-in table holds string literals and is never passed here.
 */
void
ai_tool_style_free(AiToolStyle *self)
{
    if (self == NULL)
    {
        return;
    }

    g_free((gchar *)self->tool_name);
    g_free((gchar *)self->verb);
    g_free((gchar *)self->noun_singular);
    g_free((gchar *)self->noun_plural);
    g_free((gchar *)self->target_key);
    g_free(self);
}

/**
 * ai_tool_style_register:
 * @style: (transfer none): the entry to add
 *
 * Teaches the transcript how to describe a tool.
 *
 * Copied, so @style may be a stack literal. Registering a name that already
 * has an entry replaces it, which is how a host overrides the built-in
 * wording as well as how it adds its own.
 *
 * |[<!-- language="C" -->
 * const AiToolStyle style = {
 *     "deploy", "Deployed", "service", "services",
 *     AI_TOOL_CATEGORY_COMMAND, "target", FALSE
 * };
 *
 * ai_tool_style_register (&style);
 * ]|
 */
void
ai_tool_style_register(const AiToolStyle *style)
{
    g_return_if_fail(style != NULL);
    g_return_if_fail(style->tool_name != NULL);

    if (registered_styles == NULL)
    {
        registered_styles = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)ai_tool_style_free);
    }

    g_hash_table_insert(registered_styles,
                        g_strdup(style->tool_name),
                        ai_tool_style_copy(style));
}

/**
 * ai_tool_style_lookup:
 * @tool_name: (nullable): the name the model used
 *
 * Finds how @tool_name should read.
 *
 * Runtime registrations win over the built-in table. Matching is
 * case-sensitive on purpose: `edit` is ai-glib's own tool and `Edit` is
 * claude-code's, and although they read the same today they need not.
 *
 * A PascalCase miss is retried as snake_case so Grok's ACP variant
 * `ReadFile` uses the same wording as the streamed `read_file`.
 *
 * Returns: (transfer none) (nullable): the entry, or %NULL if there is none
 */
/* Exact match against runtime registrations, then the built-in table. */
static const AiToolStyle *
lookup_exact(const gchar *tool_name)
{
    gsize i;

    if (registered_styles != NULL)
    {
        const AiToolStyle *found = g_hash_table_lookup(registered_styles,
                                                       tool_name);

        if (found != NULL)
        {
            return found;
        }
    }

    for (i = 0; i < G_N_ELEMENTS(BUILTIN_STYLES); i++)
    {
        if (g_strcmp0(BUILTIN_STYLES[i].tool_name, tool_name) == 0)
        {
            return &BUILTIN_STYLES[i];
        }
    }

    return NULL;
}

/*
 * Fold ACP/PascalCase names onto the snake_case entries they belong to.
 *
 * Grok's native history writes variant "ReadFile" while streaming-messages-json
 * writes name "read_file". Both must hit the same wording; converting here
 * keeps the table from listing every capitalisation.
 */
static gchar *
snake_from_pascal(const gchar *name)
{
    GString *out;
    const gchar *p;
    gboolean saw_upper = FALSE;

    for (p = name; *p != '\0'; p++)
    {
        if (g_ascii_isupper(*p))
        {
            saw_upper = TRUE;
            break;
        }
    }

    if (!saw_upper)
    {
        return NULL;
    }

    out = g_string_new(NULL);

    for (p = name; *p != '\0'; p++)
    {
        if (g_ascii_isupper(*p) && out->len > 0)
        {
            gchar prev = out->str[out->len - 1];
            gchar next = p[1];

            if (g_ascii_islower(prev) ||
                (g_ascii_isupper(prev) && g_ascii_islower(next)))
            {
                g_string_append_c(out, '_');
            }
        }

        g_string_append_c(out, g_ascii_tolower(*p));
    }

    return g_string_free(out, FALSE);
}

const AiToolStyle *
ai_tool_style_lookup(const gchar *tool_name)
{
    const AiToolStyle *found;
    g_autofree gchar *snake = NULL;

    if (tool_name == NULL || tool_name[0] == '\0')
    {
        return NULL;
    }

    found = lookup_exact(tool_name);
    if (found != NULL)
    {
        return found;
    }

    snake = snake_from_pascal(tool_name);
    if (snake != NULL && g_strcmp0(snake, tool_name) != 0)
    {
        found = lookup_exact(snake);
    }

    return found;
}

/**
 * ai_tool_category_verb:
 * @category: an #AiToolCategory
 *
 * The generic past-tense verb for @category, for tools with no entry.
 *
 * Returns: (transfer none): the verb, never %NULL
 */
const gchar *
ai_tool_category_verb(AiToolCategory category)
{
    if ((guint)category >= AI_TOOL_N_CATEGORIES)
    {
        category = AI_TOOL_CATEGORY_OTHER;
    }

    return CATEGORY_FALLBACKS[category].verb;
}

/**
 * ai_tool_category_noun:
 * @category: an #AiToolCategory
 * @plural: %TRUE for the plural form
 *
 * The generic noun for @category.
 *
 * Returns: (transfer none): the noun, never %NULL
 */
const gchar *
ai_tool_category_noun(
    AiToolCategory category,
    gboolean       plural
){
    if ((guint)category >= AI_TOOL_N_CATEGORIES)
    {
        category = AI_TOOL_CATEGORY_OTHER;
    }

    return plural
        ? CATEGORY_FALLBACKS[category].noun_plural
        : CATEGORY_FALLBACKS[category].noun_singular;
}

/**
 * ai_tool_category_gerund:
 * @category: an #AiToolCategory
 *
 * What a call in @category reads as while it is still running.
 *
 * The verbs in #AiToolStyle are past tense, because a transcript
 * summarises work that is finished. A progress indicator is the other
 * case --- "Edited" is wrong for something still happening --- and the
 * present participle belongs next to the past tense rather than in
 * whichever frontend needed it first.
 *
 * Returns: (transfer none): the verb, never %NULL
 */
const gchar *
ai_tool_category_gerund(AiToolCategory category)
{
    switch (category)
    {
        case AI_TOOL_CATEGORY_FILE_READ:
            return "Reading";

        case AI_TOOL_CATEGORY_FILE_WRITE:
            return "Editing";

        case AI_TOOL_CATEGORY_COMMAND:
            return "Running";

        case AI_TOOL_CATEGORY_SEARCH:
            return "Searching";

        case AI_TOOL_CATEGORY_NETWORK:
            return "Fetching";

        case AI_TOOL_CATEGORY_TASK:
            return "Working on";

        case AI_TOOL_CATEGORY_OTHER:
        default:
            return "Using";
    }
}
