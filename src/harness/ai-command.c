/*
 * ai-command.c - Slash commands, from disk and from the library
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "core/ai-error.h"
#include "harness/ai-command.h"

/* How long a `` !`cmd` `` substitution may run before it is killed. A
 * command body that hangs must not hang the frontend that invoked it. */
#define SHELL_TIMEOUT_MS (10 * 1000)

/* How much of a shell substitution's output is kept. */
#define SHELL_OUTPUT_MAX (64 * 1024)

/* ================================================================
 * The built-in commands
 * ================================================================ */

/*
 * Registered here rather than in the TUI so `ai`, `ai-tui` and an Emacs
 * frontend all offer the same set, and so completion has something to
 * complete before a single file has been read.
 *
 * A frontend switches on the name; nothing in this file knows what
 * "clear" means.
 */
typedef struct
{
    const gchar *name;
    const gchar *description;
    const gchar *argument_hint;
} BuiltinCommand;

static const BuiltinCommand BUILTIN_COMMANDS[] = {
    { "help",     "List every command, skill and agent",        NULL },
    { "clear",    "Empty the transcript and the history",       NULL },
    { "quit",     "Leave",                                      NULL },
    { "model",    "Show or change the model",                   "[model]" },
    { "provider", "Show or change the provider",                "[provider]" },
    { "tools",    "List the tools the model can call",          NULL },
    { "commands", "List commands, and what shadows what",       NULL },
    { "skills",   "List skills",                                NULL },
    { "agents",   "List agents",                                NULL },
    { "reload",   "Rescan the command, skill and agent paths",  NULL },
    { "cwd",      "Show or change the working directory",       "[path]" },
    { "todos",    "Show the current todo list",                 NULL },
    { "expand",   "Show what a line would send, without sending it",
      "<line>" },
    { "save",     "Write the transcript to a file",             "<path>" },
    { NULL, NULL, NULL }
};

/* ================================================================
 * AiCommand
 * ================================================================ */

struct _AiCommand
{
    GObject        parent_instance;

    gchar         *name;
    gchar         *description;
    gchar         *argument_hint;
    AiCommandKind  kind;
    AiResource    *resource;
};

G_DEFINE_TYPE(AiCommand, ai_command, G_TYPE_OBJECT)

static void
ai_command_finalize(GObject *object)
{
    AiCommand *self = AI_COMMAND(object);

    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->description, g_free);
    g_clear_pointer(&self->argument_hint, g_free);
    g_clear_object(&self->resource);

    G_OBJECT_CLASS(ai_command_parent_class)->finalize(object);
}

static void
ai_command_class_init(AiCommandClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_command_finalize;
}

static void
ai_command_init(AiCommand *self)
{
    self->kind = AI_COMMAND_BUILTIN;
}

/**
 * ai_command_new_builtin:
 * @name: what the user types after the slash
 * @description: (nullable): one line, for listings
 * @argument_hint: (nullable): e.g. "<path>"
 *
 * Creates a command the frontend handles itself.
 *
 * Returns: (transfer full): a new #AiCommand
 */
AiCommand *
ai_command_new_builtin(
    const gchar *name,
    const gchar *description,
    const gchar *argument_hint
){
    g_autoptr(AiCommand) self = g_object_new(AI_TYPE_COMMAND, NULL);

    g_return_val_if_fail(name != NULL, NULL);

    self->name = g_strdup(name);
    self->description = g_strdup(description);
    self->argument_hint = g_strdup(argument_hint);
    self->kind = AI_COMMAND_BUILTIN;

    return (AiCommand *)g_steal_pointer(&self);
}

/**
 * ai_command_new_for_resource:
 * @resource: (transfer none): a command, skill or agent
 *
 * Wraps a file so it can be invoked with a slash.
 *
 * A skill and a command behave identically here --- body first, then the
 * arguments --- which is what makes `/skill-gtest-scaffold src/foo.c`
 * work with no skill-specific code anywhere. An agent is marked so a
 * caller can dispatch it rather than send it.
 *
 * Returns: (transfer full): a new #AiCommand
 */
AiCommand *
ai_command_new_for_resource(AiResource *resource)
{
    g_autoptr(AiCommand) self = g_object_new(AI_TYPE_COMMAND, NULL);

    g_return_val_if_fail(AI_IS_RESOURCE(resource), NULL);

    self->name = g_strdup(ai_resource_get_name(resource));
    self->description = g_strdup(ai_resource_get_description(resource));
    self->argument_hint =
        g_strdup(ai_resource_get_meta(resource, "argument-hint"));
    self->resource = g_object_ref(resource);
    self->kind = (ai_resource_get_kind(resource) == AI_RESOURCE_AGENT)
                     ? AI_COMMAND_AGENT
                     : AI_COMMAND_PROMPT;

    return (AiCommand *)g_steal_pointer(&self);
}

/**
 * ai_command_get_name:
 * @self: an #AiCommand
 *
 * Returns: (transfer none): what the user types after the slash
 */
const gchar *
ai_command_get_name(AiCommand *self)
{
    g_return_val_if_fail(AI_IS_COMMAND(self), NULL);

    return self->name;
}

/**
 * ai_command_get_description:
 * @self: an #AiCommand
 *
 * Returns: (transfer none) (nullable): one line, for listings
 */
const gchar *
ai_command_get_description(AiCommand *self)
{
    g_return_val_if_fail(AI_IS_COMMAND(self), NULL);

    return self->description;
}

/**
 * ai_command_get_argument_hint:
 * @self: an #AiCommand
 *
 * Returns: (transfer none) (nullable): what arguments it expects
 */
const gchar *
ai_command_get_argument_hint(AiCommand *self)
{
    g_return_val_if_fail(AI_IS_COMMAND(self), NULL);

    return self->argument_hint;
}

/**
 * ai_command_get_kind:
 * @self: an #AiCommand
 *
 * Returns: what invoking it does
 */
AiCommandKind
ai_command_get_kind(AiCommand *self)
{
    g_return_val_if_fail(AI_IS_COMMAND(self), AI_COMMAND_BUILTIN);

    return self->kind;
}

/**
 * ai_command_get_resource:
 * @self: an #AiCommand
 *
 * Returns: (transfer none) (nullable): the file behind it, if any
 */
AiResource *
ai_command_get_resource(AiCommand *self)
{
    g_return_val_if_fail(AI_IS_COMMAND(self), NULL);

    return self->resource;
}

/**
 * ai_command_get_origin:
 * @self: an #AiCommand
 *
 * Which harness's directory it came from, or "ai-glib" for a built-in.
 *
 * Returns: (transfer none): the origin, never %NULL
 */
const gchar *
ai_command_get_origin(AiCommand *self)
{
    g_return_val_if_fail(AI_IS_COMMAND(self), NULL);

    if (self->resource != NULL)
    {
        return ai_resource_get_origin(self->resource);
    }

    return "ai-glib";
}

/* ================================================================
 * AiCommandResult
 * ================================================================ */

struct _AiCommandResult
{
    GObject           parent_instance;

    AiCommandOutcome  outcome;
    AiCommand        *command;
    gchar            *name;
    gchar            *arguments;
    gchar            *prompt;
};

G_DEFINE_TYPE(AiCommandResult, ai_command_result, G_TYPE_OBJECT)

static void
ai_command_result_finalize(GObject *object)
{
    AiCommandResult *self = AI_COMMAND_RESULT(object);

    g_clear_object(&self->command);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->arguments, g_free);
    g_clear_pointer(&self->prompt, g_free);

    G_OBJECT_CLASS(ai_command_result_parent_class)->finalize(object);
}

static void
ai_command_result_class_init(AiCommandResultClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_command_result_finalize;
}

static void
ai_command_result_init(AiCommandResult *self)
{
    self->outcome = AI_COMMAND_OUTCOME_NOT_A_COMMAND;
}

/**
 * ai_command_result_get_outcome:
 * @self: an #AiCommandResult
 *
 * Returns: what the caller should do
 */
AiCommandOutcome
ai_command_result_get_outcome(AiCommandResult *self)
{
    g_return_val_if_fail(AI_IS_COMMAND_RESULT(self),
                         AI_COMMAND_OUTCOME_NOT_A_COMMAND);

    return self->outcome;
}

/**
 * ai_command_result_get_command:
 * @self: an #AiCommandResult
 *
 * Returns: (transfer none) (nullable): the command that matched
 */
AiCommand *
ai_command_result_get_command(AiCommandResult *self)
{
    g_return_val_if_fail(AI_IS_COMMAND_RESULT(self), NULL);

    return self->command;
}

/**
 * ai_command_result_get_name:
 * @self: an #AiCommandResult
 *
 * The command's name, which a frontend switches on for a built-in.
 *
 * Returns: (transfer none) (nullable): the name
 */
const gchar *
ai_command_result_get_name(AiCommandResult *self)
{
    g_return_val_if_fail(AI_IS_COMMAND_RESULT(self), NULL);

    return self->name;
}

/**
 * ai_command_result_get_arguments:
 * @self: an #AiCommandResult
 *
 * Everything after the command name, trimmed.
 *
 * Returns: (transfer none) (nullable): the arguments
 */
const gchar *
ai_command_result_get_arguments(AiCommandResult *self)
{
    g_return_val_if_fail(AI_IS_COMMAND_RESULT(self), NULL);

    return self->arguments;
}

/**
 * ai_command_result_get_prompt:
 * @self: an #AiCommandResult
 *
 * The expanded text, for %AI_COMMAND_OUTCOME_PROMPT and
 * %AI_COMMAND_OUTCOME_AGENT.
 *
 * Mentions are *not* expanded here. That happens once, further along the
 * pipeline, so a `@path` in a command body and one the user typed are
 * treated identically and neither is expanded twice.
 *
 * Returns: (transfer none) (nullable): the text to send
 */
const gchar *
ai_command_result_get_prompt(AiCommandResult *self)
{
    g_return_val_if_fail(AI_IS_COMMAND_RESULT(self), NULL);

    return self->prompt;
}

/* ================================================================
 * Argument parsing
 * ================================================================ */

/**
 * ai_command_split_arguments:
 * @arguments: (nullable): everything after the command name
 *
 * Splits an argument string the way a shell would.
 *
 * Single and double quotes group, a backslash escapes the next
 * character, and runs of whitespace separate. An unterminated quote is
 * *recovered* rather than rejected: the user is mid-typing, and refusing
 * the whole line because a quote is still open would be useless.
 *
 * Returns: (transfer full) (array zero-terminated=1): the arguments
 */
gchar **
ai_command_split_arguments(const gchar *arguments)
{
    g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GString)   current = g_string_new(NULL);
    gboolean             have_token = FALSE;
    gchar                quote = 0;
    gsize                i;

    if (arguments == NULL)
    {
        g_ptr_array_add(out, NULL);
        return (gchar **)g_ptr_array_free(g_steal_pointer(&out), FALSE);
    }

    for (i = 0; arguments[i] != '\0'; i++)
    {
        gchar c = arguments[i];

        if (quote != 0)
        {
            if (c == quote)
            {
                quote = 0;
            }
            else if (c == '\\' && quote == '"' && arguments[i + 1] != '\0')
            {
                i++;
                g_string_append_c(current, arguments[i]);
            }
            else
            {
                g_string_append_c(current, c);
            }

            continue;
        }

        if (c == '\'' || c == '"')
        {
            quote = c;
            have_token = TRUE;
            continue;
        }

        if (c == '\\' && arguments[i + 1] != '\0')
        {
            i++;
            g_string_append_c(current, arguments[i]);
            have_token = TRUE;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            if (have_token)
            {
                g_ptr_array_add(out, g_strdup(current->str));
                g_string_truncate(current, 0);
                have_token = FALSE;
            }

            continue;
        }

        g_string_append_c(current, c);
        have_token = TRUE;
    }

    if (have_token)
    {
        g_ptr_array_add(out, g_strdup(current->str));
    }

    g_ptr_array_add(out, NULL);

    return (gchar **)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

/**
 * ai_command_substitute:
 * @body: the command body
 * @arguments: (nullable): everything after the command name
 * @argv: (nullable) (array zero-terminated=1): @arguments already split
 *
 * Replaces the argument placeholders in @body.
 *
 * `$ARGUMENTS` becomes the whole argument string and `$1` to `$9` become
 * individual arguments; a placeholder with nothing to fill it becomes
 * empty rather than staying literal, so a command written for two
 * arguments and invoked with one does not send the model a stray `$2`.
 * `$$` is a literal dollar. Everything else, `$0` and `$FOO` included,
 * is left exactly as written --- command bodies contain shell snippets,
 * and rewriting `$HOME` inside one would be worse than useless.
 *
 * Returns: (transfer full): the substituted text
 */
gchar *
ai_command_substitute(
    const gchar        *body,
    const gchar        *arguments,
    const gchar *const *argv
){
    g_autoptr(GString) out = NULL;
    gsize              i;

    g_return_val_if_fail(body != NULL, NULL);

    out = g_string_new(NULL);

    for (i = 0; body[i] != '\0'; i++)
    {
        if (body[i] != '$')
        {
            g_string_append_c(out, body[i]);
            continue;
        }

        if (body[i + 1] == '$')
        {
            g_string_append_c(out, '$');
            i++;
            continue;
        }

        if (strncmp(body + i + 1, "ARGUMENTS", 9) == 0)
        {
            if (arguments != NULL)
            {
                g_string_append(out, arguments);
            }

            i += 9;
            continue;
        }

        if (body[i + 1] >= '1' && body[i + 1] <= '9')
        {
            guint index = (guint)(body[i + 1] - '1');

            if (argv != NULL && g_strv_length((gchar **)argv) > index)
            {
                g_string_append(out, argv[index]);
            }

            i++;
            continue;
        }

        g_string_append_c(out, '$');
    }

    return g_strdup(out->str);
}

/* ================================================================
 * Shell substitution
 * ================================================================ */

typedef struct
{
    GCancellable *cancellable;
    GSubprocess  *subprocess;
    gboolean      timed_out;
} ShellRun;

static gboolean
on_shell_timeout(gpointer user_data)
{
    ShellRun *run = user_data;

    run->timed_out = TRUE;

    if (run->subprocess != NULL)
    {
        g_subprocess_force_exit(run->subprocess);
    }

    g_cancellable_cancel(run->cancellable);

    return G_SOURCE_REMOVE;
}

/* Forward the caller's cancellation onto the one this run owns. */
static void
on_outer_cancelled(GCancellable *outer, gpointer user_data)
{
    (void)outer;

    g_cancellable_cancel(G_CANCELLABLE(user_data));
}

/*
 * Run one `` !`cmd` `` and return what it printed.
 *
 * Never fails the expansion. A command that exits nonzero has still
 * usually printed something useful, and a command body that embeds `git
 * status` in a repository with no commits should still produce a prompt.
 * Stderr is discarded rather than folded in: it is not output the author
 * asked for, and quietly pasting a warning into a prompt is worse than
 * dropping it.
 */
static gchar *
run_shell(
    const gchar  *command,
    const gchar  *cwd,
    GCancellable *cancellable
){
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess)         subprocess = NULL;
    g_autoptr(GCancellable)        local_cancel = NULL;
    g_autoptr(GError)              local_error = NULL;
    g_autofree gchar              *stdout_text = NULL;
    ShellRun                       run = { NULL, NULL, FALSE };
    GSource                       *timeout = NULL;
    gulong                         cancel_id = 0;

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE);

    if (cwd != NULL)
    {
        g_subprocess_launcher_set_cwd(launcher, cwd);
    }

    subprocess = g_subprocess_launcher_spawn(launcher, &local_error,
                                             "/bin/sh", "-c", command, NULL);

    if (subprocess == NULL)
    {
        g_debug("ai_command: cannot run '%s': %s", command,
                local_error->message);
        return NULL;
    }

    local_cancel = g_cancellable_new();

    if (cancellable != NULL)
    {
        cancel_id = g_cancellable_connect(cancellable,
                                          G_CALLBACK(on_outer_cancelled),
                                          local_cancel, NULL);
    }

    run.cancellable = local_cancel;
    run.subprocess = subprocess;

    /*
     * The timeout source goes on the thread-default context, not the
     * global default: a caller resolving a command from inside a nested
     * loop on a private context would never see a global-default timer.
     */
    timeout = g_timeout_source_new(SHELL_TIMEOUT_MS);
    g_source_set_callback(timeout, on_shell_timeout, &run, NULL);
    g_source_attach(timeout, g_main_context_get_thread_default());

    g_subprocess_communicate_utf8(subprocess, NULL, local_cancel,
                                  &stdout_text, NULL, &local_error);

    g_source_destroy(timeout);
    g_source_unref(timeout);

    /* Disconnect before local_cancel goes out of scope: a handler left on
     * the caller's cancellable would fire against a freed object. */
    if (cancel_id != 0)
    {
        g_cancellable_disconnect(cancellable, cancel_id);
    }

    if (run.timed_out)
    {
        g_message("ai_command: '%s' did not finish within %d seconds; "
                  "its output is omitted", command, SHELL_TIMEOUT_MS / 1000);
        return NULL;
    }

    if (stdout_text == NULL)
    {
        g_debug("ai_command: '%s' produced no output (%s)", command,
                local_error != NULL ? local_error->message : "no error");
        return NULL;
    }

    if (strlen(stdout_text) > SHELL_OUTPUT_MAX)
    {
        stdout_text[SHELL_OUTPUT_MAX] = '\0';
    }

    /* A trailing newline from a command is noise inside a sentence. */
    g_strchomp(stdout_text);

    return g_steal_pointer(&stdout_text);
}

/*
 * Replace every `` !`cmd` `` in @body with what the command printed.
 *
 * With @allowed FALSE the backticks are left exactly as written, which
 * is the default for any file that has not asked for this.
 */
static gchar *
substitute_shell(
    const gchar  *body,
    const gchar  *cwd,
    gboolean      allowed,
    GCancellable *cancellable
){
    g_autoptr(GString) out = NULL;
    gsize              i;

    if (!allowed || strstr(body, "!`") == NULL)
    {
        return g_strdup(body);
    }

    out = g_string_new(NULL);

    for (i = 0; body[i] != '\0'; i++)
    {
        gsize closing;

        if (body[i] != '!' || body[i + 1] != '`')
        {
            g_string_append_c(out, body[i]);
            continue;
        }

        closing = i + 2;

        while (body[closing] != '\0' && body[closing] != '`')
        {
            closing++;
        }

        if (body[closing] == '\0')
        {
            /* Unterminated: not a substitution, just text. */
            g_string_append_c(out, body[i]);
            continue;
        }

        {
            g_autofree gchar *command =
                g_strndup(body + i + 2, closing - i - 2);
            g_autofree gchar *output = run_shell(command, cwd, cancellable);

            if (output != NULL)
            {
                g_string_append(out, output);
            }
        }

        i = closing;
    }

    return g_strdup(out->str);
}

/* ================================================================
 * AiCommandSet
 * ================================================================ */

struct _AiCommandSet
{
    GObject               parent_instance;

    AiResourceRegistry   *registry;
    GHashTable           *builtins;   /* name -> AiCommand* */
    AiCommandShellPolicy  shell_policy;
};

enum
{
    PROP_0,
    PROP_REGISTRY,
    PROP_SHELL_POLICY,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_TYPE(AiCommandSet, ai_command_set, G_TYPE_OBJECT)

static void
ai_command_set_finalize(GObject *object)
{
    AiCommandSet *self = AI_COMMAND_SET(object);

    g_clear_object(&self->registry);
    g_clear_pointer(&self->builtins, g_hash_table_unref);

    G_OBJECT_CLASS(ai_command_set_parent_class)->finalize(object);
}

static void
ai_command_set_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiCommandSet *self = AI_COMMAND_SET(object);

    switch (prop_id)
    {
        case PROP_REGISTRY:
            g_value_set_object(value, self->registry);
            break;

        case PROP_SHELL_POLICY:
            g_value_set_int(value, (gint)self->shell_policy);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_command_set_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiCommandSet *self = AI_COMMAND_SET(object);

    switch (prop_id)
    {
        case PROP_REGISTRY:
            g_set_object(&self->registry, g_value_get_object(value));
            break;

        case PROP_SHELL_POLICY:
            self->shell_policy = (AiCommandShellPolicy)g_value_get_int(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_command_set_class_init(AiCommandSetClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_command_set_finalize;
    object_class->get_property = ai_command_set_get_property;
    object_class->set_property = ai_command_set_set_property;

    /**
     * AiCommandSet:registry:
     *
     * Where file-backed commands, skills and agents come from.
     *
     * %NULL is legal and leaves only the built-ins, which is what an
     * embedder that does not want to read the user's home directory
     * wants.
     */
    properties[PROP_REGISTRY] =
        g_param_spec_object("registry", NULL, NULL, AI_TYPE_RESOURCE_REGISTRY,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCommandSet:shell-policy:
     *
     * Whether a command body's `` !`cmd` `` may execute.
     */
    properties[PROP_SHELL_POLICY] =
        g_param_spec_int("shell-policy", NULL, NULL, 0, 2,
                         AI_COMMAND_SHELL_OPT_IN,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_command_set_init(AiCommandSet *self)
{
    const BuiltinCommand *entry;

    self->shell_policy = AI_COMMAND_SHELL_OPT_IN;
    self->builtins = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_object_unref);

    for (entry = BUILTIN_COMMANDS; entry->name != NULL; entry++)
    {
        AiCommand *command = ai_command_new_builtin(entry->name,
                                                    entry->description,
                                                    entry->argument_hint);

        g_hash_table_replace(self->builtins, g_strdup(entry->name), command);
    }
}

/**
 * ai_command_set_new:
 * @registry: (nullable) (transfer none): where files come from
 *
 * Creates a command set: the built-ins, plus whatever @registry found.
 *
 * Returns: (transfer full): a new #AiCommandSet
 */
AiCommandSet *
ai_command_set_new(AiResourceRegistry *registry)
{
    return g_object_new(AI_TYPE_COMMAND_SET, "registry", registry, NULL);
}

/**
 * ai_command_set_get_registry:
 * @self: an #AiCommandSet
 *
 * Returns: (transfer none) (nullable): the registry
 */
AiResourceRegistry *
ai_command_set_get_registry(AiCommandSet *self)
{
    g_return_val_if_fail(AI_IS_COMMAND_SET(self), NULL);

    return self->registry;
}

/**
 * ai_command_set_set_shell_policy:
 * @self: an #AiCommandSet
 * @policy: the new policy
 *
 * Decides whether a command body may run shell commands.
 *
 * %AI_COMMAND_SHELL_NEVER is the setting for an embedder that does not
 * trust the directories being scanned, and it overrides a file's own
 * `shell: true` --- the file does not get the final word.
 */
void
ai_command_set_set_shell_policy(
    AiCommandSet         *self,
    AiCommandShellPolicy  policy
){
    g_return_if_fail(AI_IS_COMMAND_SET(self));

    if (self->shell_policy == policy)
    {
        return;
    }

    self->shell_policy = policy;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SHELL_POLICY]);
}

/**
 * ai_command_set_get_shell_policy:
 * @self: an #AiCommandSet
 *
 * Returns: the current policy
 */
AiCommandShellPolicy
ai_command_set_get_shell_policy(AiCommandSet *self)
{
    g_return_val_if_fail(AI_IS_COMMAND_SET(self), AI_COMMAND_SHELL_NEVER);

    return self->shell_policy;
}

/*
 * The resource behind a name, looking at commands before skills.
 *
 * Agents are deliberately last: an agent is normally reached through the
 * `task` tool or `/agent`, and a command of the same name is the more
 * likely thing to have been meant.
 */
static AiResource *
lookup_resource(AiCommandSet *self, const gchar *name)
{
    AiResource *resource;

    if (self->registry == NULL)
    {
        return NULL;
    }

    resource = ai_resource_registry_lookup(self->registry,
                                           AI_RESOURCE_COMMAND, name);

    if (resource != NULL)
    {
        return resource;
    }

    resource = ai_resource_registry_lookup(self->registry,
                                           AI_RESOURCE_SKILL, name);

    if (resource != NULL)
    {
        return resource;
    }

    return ai_resource_registry_lookup(self->registry, AI_RESOURCE_AGENT,
                                       name);
}

/**
 * ai_command_set_lookup:
 * @self: an #AiCommandSet
 * @name: a command name, without the slash
 *
 * Finds one command.
 *
 * A built-in always wins over a file of the same name. That is not
 * politeness --- a stray `quit.md` in a scanned directory must not be
 * able to take away the way out of the program.
 *
 * Returns: (transfer full) (nullable): the command, or %NULL
 */
AiCommand *
ai_command_set_lookup(
    AiCommandSet *self,
    const gchar  *name
){
    AiCommand  *builtin;
    AiResource *resource;

    g_return_val_if_fail(AI_IS_COMMAND_SET(self), NULL);
    g_return_val_if_fail(name != NULL, NULL);

    builtin = g_hash_table_lookup(self->builtins, name);

    if (builtin != NULL)
    {
        return g_object_ref(builtin);
    }

    resource = lookup_resource(self, name);

    if (resource != NULL)
    {
        return ai_command_new_for_resource(resource);
    }

    return NULL;
}

static gint
compare_commands(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(ai_command_get_name((AiCommand *)a),
                     ai_command_get_name((AiCommand *)b));
}

/**
 * ai_command_set_list:
 * @self: an #AiCommandSet
 *
 * Every command, built-in and file-backed, sorted by name.
 *
 * A file shadowed by a built-in of the same name is omitted, because
 * this list answers "what can I type", and typing it would reach the
 * built-in.
 *
 * Returns: (transfer full) (element-type AiCommand): the commands
 */
GList *
ai_command_set_list(AiCommandSet *self)
{
    GList          *out = NULL;
    GHashTableIter  iter;
    gpointer        value;

    g_return_val_if_fail(AI_IS_COMMAND_SET(self), NULL);

    g_hash_table_iter_init(&iter, self->builtins);

    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        out = g_list_prepend(out, g_object_ref(value));
    }

    if (self->registry != NULL)
    {
        AiResourceKind kinds[] = { AI_RESOURCE_COMMAND, AI_RESOURCE_SKILL,
                                   AI_RESOURCE_AGENT };
        gsize          k;

        for (k = 0; k < G_N_ELEMENTS(kinds); k++)
        {
            GList *resources = ai_resource_registry_list(self->registry,
                                                         kinds[k]);
            GList *iter_r;

            for (iter_r = resources; iter_r != NULL; iter_r = iter_r->next)
            {
                const gchar *name = ai_resource_get_name(iter_r->data);

                if (name == NULL ||
                    g_hash_table_contains(self->builtins, name))
                {
                    continue;
                }

                out = g_list_prepend(out,
                                     ai_command_new_for_resource(iter_r->data));
            }

            g_list_free(resources);
        }
    }

    return g_list_sort(out, compare_commands);
}

/**
 * ai_command_set_is_command_line:
 * @line: (nullable): a line of input
 *
 * Whether @line looks like a slash command at all.
 *
 * A lone `/` is not one, and neither is a path: `/usr/bin/thing` is
 * something a user types about far more often than they type a command
 * called `usr`.
 *
 * Returns: %TRUE if @line begins a command
 */
gboolean
ai_command_set_is_command_line(const gchar *line)
{
    gsize i;

    if (line == NULL || line[0] != '/')
    {
        return FALSE;
    }

    if (line[1] == '\0' || line[1] == ' ' || line[1] == '/')
    {
        return FALSE;
    }

    for (i = 1; line[i] != '\0' && line[i] != ' ' && line[i] != '\t'; i++)
    {
        if (line[i] == '/')
        {
            return FALSE;
        }
    }

    return TRUE;
}

/* The closest known names to @name, for an error message worth reading. */
static gchar *
suggest_names(AiCommandSet *self, const gchar *name)
{
    g_autoptr(GPtrArray) near = g_ptr_array_new_with_free_func(g_free);
    GList               *commands = ai_command_set_list(self);
    GList               *iter;
    gchar               *joined;

    for (iter = commands; iter != NULL; iter = iter->next)
    {
        const gchar *candidate = ai_command_get_name(iter->data);

        if (candidate != NULL && near->len < 5 &&
            (g_str_has_prefix(candidate, name) ||
             strstr(candidate, name) != NULL))
        {
            g_ptr_array_add(near, g_strdup(candidate));
        }
    }

    g_list_free_full(commands, g_object_unref);

    if (near->len == 0)
    {
        return NULL;
    }

    g_ptr_array_add(near, NULL);
    joined = g_strjoinv(", ", (gchar **)near->pdata);

    return joined;
}

/**
 * ai_command_set_resolve:
 * @self: an #AiCommandSet
 * @line: a line of input
 * @cwd: (nullable): what a shell substitution runs in
 * @cancellable: (nullable): interrupts a shell substitution
 * @error: (nullable): return location for a #GError
 *
 * Decides what a line of input means.
 *
 * A line that is not a slash command resolves to
 * %AI_COMMAND_OUTCOME_NOT_A_COMMAND rather than failing, so a caller can
 * run every line through here unconditionally.
 *
 * An unknown `/name` *is* an error, and the message names the near
 * misses. A caller driving a wrapped CLI should treat that error as its
 * cue to pass the line through untouched: `/compact` means something to
 * claude and nothing here.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL on error
 */
AiCommandResult *
ai_command_set_resolve(
    AiCommandSet  *self,
    const gchar   *line,
    const gchar   *cwd,
    GCancellable  *cancellable,
    GError       **error
){
    g_autoptr(AiCommandResult) result = NULL;
    g_autoptr(AiCommand)       command = NULL;
    g_autofree gchar          *name = NULL;
    const gchar               *rest;
    gsize                      name_len = 0;

    g_return_val_if_fail(AI_IS_COMMAND_SET(self), NULL);
    g_return_val_if_fail(line != NULL, NULL);

    result = g_object_new(AI_TYPE_COMMAND_RESULT, NULL);

    if (!ai_command_set_is_command_line(line))
    {
        result->outcome = AI_COMMAND_OUTCOME_NOT_A_COMMAND;
        return (AiCommandResult *)g_steal_pointer(&result);
    }

    while (line[1 + name_len] != '\0' &&
           line[1 + name_len] != ' ' && line[1 + name_len] != '\t')
    {
        name_len++;
    }

    name = g_strndup(line + 1, name_len);
    rest = line + 1 + name_len;

    command = ai_command_set_lookup(self, name);

    if (command == NULL)
    {
        g_autofree gchar *near = suggest_names(self, name);

        if (near != NULL)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                        "unknown command '/%s'; did you mean: %s", name,
                        near);
        }
        else
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                        "unknown command '/%s'", name);
        }

        return NULL;
    }

    result->command = g_object_ref(command);
    result->name = g_strdup(ai_command_get_name(command));

    /* g_strstrip edits in place and returns its argument, so the copy is
     * what gets kept. */
    result->arguments = g_strdup(rest);
    g_strstrip(result->arguments);

    if (ai_command_get_kind(command) == AI_COMMAND_BUILTIN)
    {
        result->outcome = AI_COMMAND_OUTCOME_BUILTIN;
        return (AiCommandResult *)g_steal_pointer(&result);
    }

    {
        AiResource        *resource = ai_command_get_resource(command);
        g_auto(GStrv)      argv = ai_command_split_arguments(result->arguments);
        g_autofree gchar  *substituted = NULL;
        gboolean           shell_allowed;

        substituted = ai_command_substitute(ai_resource_get_body(resource),
                                            result->arguments,
                                            (const gchar *const *)argv);

        switch (self->shell_policy)
        {
            case AI_COMMAND_SHELL_ALWAYS:
                shell_allowed = TRUE;
                break;

            case AI_COMMAND_SHELL_NEVER:
                shell_allowed = FALSE;
                break;

            default:
                shell_allowed =
                    ai_resource_get_meta_boolean(resource, "shell", FALSE);
                break;
        }

        result->prompt = substitute_shell(substituted, cwd, shell_allowed,
                                          cancellable);
    }

    result->outcome =
        (ai_command_get_kind(command) == AI_COMMAND_AGENT)
            ? AI_COMMAND_OUTCOME_AGENT
            : AI_COMMAND_OUTCOME_PROMPT;

    return (AiCommandResult *)g_steal_pointer(&result);
}
