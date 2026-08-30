/*
 * ai-resource-registry.c - Finding the other harnesses' files
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "harness/ai-resource-registry.h"

/*
 * How deep a resource directory is walked.
 *
 * One level below the search directory is where every real layout puts
 * things --- `<name>/SKILL.md`, and claude's `<namespace>/<name>.md`. The
 * limit is what makes a symlink loop terminate; without it a link back to
 * the parent walks forever.
 */
#define MAX_SCAN_DEPTH (2)

/* How long to wait after a file changes before rescanning. Editors write
 * a file as several operations, so a rescan per event would run four
 * times for one save. */
#define RESCAN_DEBOUNCE_MS (250)

/* ================================================================
 * The search path table
 * ================================================================ */

typedef enum
{
    USER_BASE_NONE = 0,
    USER_BASE_HOME,
    USER_BASE_XDG_CONFIG
} UserBase;

/**
 * AiResourceSource:
 * @origin: which harness owns the directory
 * @kind: what is found in it
 * @project_dir: relative to the working directory, or %NULL
 * @user_base: what @user_dir is relative to
 * @user_dir: relative to @user_base, or %NULL
 *
 * One place resources are looked for.
 *
 * This table *is* the registration --- the same pattern #AiToolStyle and
 * #AiImageModelInfo already use. Teaching ai-glib about another harness
 * is one struct literal, and nothing else in the library needs to know
 * the new name.
 *
 * Order matters twice over. Scope dominates: every project directory is
 * searched before any user directory, so a repository can override a
 * personal command. Within one scope the table order breaks ties, and
 * ai-glib's own directories come first so a file written specifically
 * for ai-glib wins over one it is merely borrowing.
 */
typedef struct
{
    const gchar    *origin;
    AiResourceKind  kind;
    const gchar    *project_dir;
    UserBase        user_base;
    const gchar    *user_dir;
} AiResourceSource;

static const AiResourceSource RESOURCE_SOURCES[] = {
    /* ai-glib's own, first so it can override a borrowed definition. */
    { "ai-glib",  AI_RESOURCE_COMMAND, ".ai-glib/commands",
      USER_BASE_XDG_CONFIG, "ai-glib/commands" },
    { "ai-glib",  AI_RESOURCE_SKILL,   ".ai-glib/skills",
      USER_BASE_XDG_CONFIG, "ai-glib/skills" },
    { "ai-glib",  AI_RESOURCE_AGENT,   ".ai-glib/agents",
      USER_BASE_XDG_CONFIG, "ai-glib/agents" },

    /* claude-code. */
    { "claude",   AI_RESOURCE_COMMAND, ".claude/commands",
      USER_BASE_HOME, ".claude/commands" },
    { "claude",   AI_RESOURCE_SKILL,   ".claude/skills",
      USER_BASE_HOME, ".claude/skills" },
    { "claude",   AI_RESOURCE_AGENT,   ".claude/agents",
      USER_BASE_HOME, ".claude/agents" },

    /* opencode. Its configuration lives under XDG -- its own built-in
     * customize-opencode skill says "NOT ~/.opencode/" in as many
     * words. Every one of its three kinds is accepted both singular
     * and plural, in both scopes -- `agent` and `agents`, `command`
     * and `commands`, `skill` and `skills` -- so each needs two rows
     * rather than a guess at which spelling the user chose. It also
     * auto-loads two directories belonging to other harnesses, which is
     * why they appear here under its own origin as well as theirs. */
    { "opencode", AI_RESOURCE_COMMAND, ".opencode/command",
      USER_BASE_XDG_CONFIG, "opencode/command" },
    { "opencode", AI_RESOURCE_COMMAND, ".opencode/commands",
      USER_BASE_XDG_CONFIG, "opencode/commands" },
    { "opencode", AI_RESOURCE_SKILL,   ".opencode/skill",
      USER_BASE_XDG_CONFIG, "opencode/skill" },
    { "opencode", AI_RESOURCE_SKILL,   ".opencode/skills",
      USER_BASE_XDG_CONFIG, "opencode/skills" },
    { "opencode", AI_RESOURCE_SKILL,   NULL,
      USER_BASE_HOME, ".claude/skills" },
    { "opencode", AI_RESOURCE_SKILL,   NULL,
      USER_BASE_HOME, ".agents/skills" },
    { "opencode", AI_RESOURCE_AGENT,   ".opencode/agent",
      USER_BASE_XDG_CONFIG, "opencode/agent" },
    { "opencode", AI_RESOURCE_AGENT,   ".opencode/agents",
      USER_BASE_XDG_CONFIG, "opencode/agents" },

    /* grok. Its slash commands ARE its skills -- there is no commands
     * directory to look in -- and it reads ~/.claude/skills for
     * compatibility as well as its own. */
    { "grok",     AI_RESOURCE_SKILL,   ".grok/skills",
      USER_BASE_HOME, ".grok/skills" },
    { "grok",     AI_RESOURCE_SKILL,   NULL,
      USER_BASE_HOME, ".claude/skills" },
    { "grok",     AI_RESOURCE_AGENT,   ".grok/agents",
      USER_BASE_HOME, ".grok/agents" },

    /* antigravity / agy. Skills live under a customization root, and
     * there are four accepted spellings of it. `.agents` is the usual
     * one and comes first so it wins a tie, but a project using any of
     * the other three is one antigravity still reads, and a table
     * naming only the first tells a caller its skill is invisible when
     * it is not.
     *
     * Only the first row carries the user directory. The global root is
     * ~/.gemini/config however the project spells its own, so repeating
     * it would walk the same directory four times and file every skill
     * there as shadowing itself.
     *
     * antigravity has neither a commands nor an agents concept. Its
     * customization types are exactly rules, skills, plugins and hooks;
     * "subagent" appears nowhere in its vocabulary, and the
     * .agents/agents and ~/.gemini/config/agents this table used to
     * name have never been directories it looks in. */
    { "antigravity", AI_RESOURCE_SKILL,   ".agents/skills",
      USER_BASE_HOME, ".gemini/config/skills" },
    { "antigravity", AI_RESOURCE_SKILL,   ".agent/skills",
      USER_BASE_HOME, NULL },
    { "antigravity", AI_RESOURCE_SKILL,   "_agents/skills",
      USER_BASE_HOME, NULL },
    { "antigravity", AI_RESOURCE_SKILL,   "_agent/skills",
      USER_BASE_HOME, NULL },

    /* cursor / cursor-agent. It reads its own root, the .agents root,
     * and two other harnesses' directories for back-compatibility.
     * Those last are listed so a caller asking where cursor looks is
     * told the truth: a skill already linked for claude-code is one
     * cursor can already see. */
    { "cursor",      AI_RESOURCE_SKILL,   ".cursor/skills",
      USER_BASE_HOME, ".cursor/skills" },
    { "cursor",      AI_RESOURCE_SKILL,   ".agents/skills",
      USER_BASE_HOME, ".agents/skills" },
    { "cursor",      AI_RESOURCE_SKILL,   ".claude/skills",
      USER_BASE_HOME, ".claude/skills" },
    { "cursor",      AI_RESOURCE_SKILL,   ".codex/skills",
      USER_BASE_HOME, ".codex/skills" },
    { "cursor",      AI_RESOURCE_COMMAND, ".cursor/commands",
      USER_BASE_HOME, ".cursor/commands" },
    { "cursor",      AI_RESOURCE_AGENT,   ".cursor/agents",
      USER_BASE_HOME, ".cursor/agents" }
};

/* ================================================================
 * The object
 * ================================================================ */

struct _AiResourceRegistry
{
    GObject     parent_instance;

    gchar      *working_directory;

    /* "<kind>:<name>" -> AiResource*, the winner of any collision. */
    GHashTable *resources;

    /* Everything a winner displaced, kept so a listing can explain why a
     * file the user is looking at is not the one being used. */
    GPtrArray  *shadowed;

    /* Resources an embedder added by hand. Kept separately so a rescan
     * does not throw away something nothing on disk can restore. */
    GPtrArray  *pinned;

    gboolean    watching;
    GPtrArray  *monitors;
    guint       rescan_id;
};

enum
{
    PROP_0,
    PROP_WORKING_DIRECTORY,
    PROP_WATCHING,
    N_PROPS
};

enum
{
    SIGNAL_CHANGED,
    N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint       signals[N_SIGNALS];

G_DEFINE_TYPE(AiResourceRegistry, ai_resource_registry, G_TYPE_OBJECT)

static void registry_detach_monitors(AiResourceRegistry *self);
static void registry_attach_monitors(AiResourceRegistry *self);

/* ================================================================
 * Path resolution
 * ================================================================ */

/* The absolute path of one source's project directory, or NULL. */
static gchar *
source_project_path(
    AiResourceRegistry     *self,
    const AiResourceSource *source
){
    if (source->project_dir == NULL || self->working_directory == NULL)
    {
        return NULL;
    }

    return g_build_filename(self->working_directory, source->project_dir,
                            NULL);
}

/* The absolute path of one source's user directory, or NULL. */
static gchar *
source_user_path(const AiResourceSource *source)
{
    const gchar *base;

    if (source->user_dir == NULL)
    {
        return NULL;
    }

    switch (source->user_base)
    {
        case USER_BASE_HOME:
            base = g_get_home_dir();
            break;

        case USER_BASE_XDG_CONFIG:
            base = g_get_user_config_dir();
            break;

        default:
            return NULL;
    }

    if (base == NULL)
    {
        return NULL;
    }

    return g_build_filename(base, source->user_dir, NULL);
}

/* ================================================================
 * Scanning
 * ================================================================ */

/*
 * Is this the marker file that names a directory-shaped resource?
 *
 * Matched case-insensitively because the convention is SKILL.md but
 * nothing enforces it, and a lowercase skill.md is plainly meant the
 * same way.
 */
static gboolean
is_marker_file(const gchar *basename)
{
    return g_ascii_strcasecmp(basename, "SKILL.md") == 0 ||
           g_ascii_strcasecmp(basename, "AGENT.md") == 0 ||
           g_ascii_strcasecmp(basename, "COMMAND.md") == 0;
}

/* The key a resource is stored under. Kind is part of it so a skill and
 * a command may share a name without displacing each other. */
static gchar *
resource_key(AiResourceKind kind, const gchar *name)
{
    return g_strdup_printf("%d:%s", (gint)kind, name != NULL ? name : "");
}

/*
 * File a resource, or record it as shadowed.
 *
 * First writer wins, which is what makes the caller's scan order the
 * precedence rule rather than something restated here.
 */
static void
registry_file_resource(
    AiResourceRegistry *self,
    AiResource         *resource
){
    g_autofree gchar *key = NULL;
    const gchar      *name = ai_resource_get_name(resource);

    if (name == NULL || name[0] == '\0')
    {
        g_debug("ai_resource_registry: skipping a resource with no name");
        return;
    }

    key = resource_key(ai_resource_get_kind(resource), name);

    if (g_hash_table_contains(self->resources, key))
    {
        AiResource *winner = g_hash_table_lookup(self->resources, key);

        /*
         * The same file reached through two search paths is not a
         * collision worth reporting. It happens whenever directories
         * overlap --- a project checked out in $HOME, say --- and
         * listing a file as shadowing itself is pure noise.
         */
        const gchar *winner_path = ai_resource_get_path(winner);
        const gchar *loser_path = ai_resource_get_path(resource);

        /* Both NULL means two resources built from data, which really
         * are two different things. Only equal *paths* are the same
         * file. */
        if (winner_path == NULL || loser_path == NULL ||
            g_strcmp0(winner_path, loser_path) != 0)
        {
            g_ptr_array_add(self->shadowed, g_object_ref(resource));
        }

        return;
    }

    g_hash_table_replace(self->resources, g_steal_pointer(&key),
                         g_object_ref(resource));
}

/*
 * Load one file and file it.
 *
 * A file that cannot be read or is not text is skipped with a g_debug.
 * That level is deliberate: these files are not ours, and one bad file
 * must not take the other sixteen with it --- nor abort a test suite
 * running with G_DEBUG=fatal-warnings.
 */
static void
registry_load_file(
    AiResourceRegistry *self,
    const gchar        *path,
    const gchar        *name,
    AiResourceKind      kind,
    const gchar        *origin,
    AiResourceScope     scope
){
    g_autoptr(AiResource) resource = NULL;
    g_autoptr(GError)     local_error = NULL;

    /*
     * The name comes from the walk, not from the path: a namespaced file
     * at `git/status.md` is `/git:status`, which the path alone does not
     * say. A frontmatter `name:` still overrides it.
     */
    resource = ai_resource_new_from_file(path, name, kind, origin, scope,
                                         &local_error);

    if (resource == NULL)
    {
        g_debug("ai_resource_registry: skipping '%s': %s", path,
                local_error != NULL ? local_error->message : "unknown");
        return;
    }

    registry_file_resource(self, resource);
}

static void
registry_scan_dir(
    AiResourceRegistry *self,
    const gchar        *dir_path,
    const gchar        *prefix,
    AiResourceKind      kind,
    const gchar        *origin,
    AiResourceScope     scope,
    guint               depth
){
    g_autoptr(GDir)   dir = NULL;
    g_autoptr(GError) local_error = NULL;
    const gchar      *entry;

    if (depth > MAX_SCAN_DEPTH)
    {
        return;
    }

    dir = g_dir_open(dir_path, 0, &local_error);

    if (dir == NULL)
    {
        /*
         * Missing is the normal case --- most of the table will not
         * exist on any given machine --- and unreadable is the user's
         * business, not a bug here. Neither is worth more than a debug
         * line.
         */
        g_debug("ai_resource_registry: %s", local_error->message);
        return;
    }

    while ((entry = g_dir_read_name(dir)) != NULL)
    {
        g_autofree gchar *path = g_build_filename(dir_path, entry, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_DIR))
        {
            g_autofree gchar *nested_prefix = NULL;
            const gchar      *markers[] = { "SKILL.md", "AGENT.md",
                                            "COMMAND.md", NULL };
            guint             i;

            /* <name>/SKILL.md names the resource after the directory. */
            for (i = 0; markers[i] != NULL; i++)
            {
                g_autofree gchar *marker =
                    g_build_filename(path, markers[i], NULL);

                if (g_file_test(marker, G_FILE_TEST_IS_REGULAR))
                {
                    g_autofree gchar *name =
                        g_strdup_printf("%s%s", prefix, entry);

                    registry_load_file(self, marker, name, kind, origin,
                                       scope);
                    break;
                }
            }

            /*
             * Whether or not it held a marker, the directory may also
             * hold namespaced files --- claude spells a command in
             * `git/status.md` as `/git:status`. Recursing covers both
             * without a second walk.
             */
            nested_prefix = g_strdup_printf("%s%s:", prefix, entry);
            registry_scan_dir(self, path, nested_prefix, kind, origin,
                              scope, depth + 1);
            continue;
        }

        if (!g_str_has_suffix(entry, ".md"))
        {
            continue;
        }

        if (is_marker_file(entry))
        {
            /* Already handled by the directory branch above; picking it
             * up again here would list it a second time under the name
             * "SKILL". */
            continue;
        }

        {
            g_autofree gchar *stem = g_strdup(entry);
            gchar            *dot = g_strrstr(stem, ".");
            g_autofree gchar *name = NULL;

            if (dot != NULL && dot != stem)
            {
                *dot = '\0';
            }

            name = g_strdup_printf("%s%s", prefix, stem);
            registry_load_file(self, path, name, kind, origin, scope);
        }
    }
}

/*
 * Walk every search directory, project scope first.
 *
 * The two passes are what implement "project beats user"; doing it in
 * one pass with a comparison per collision would put the rule in two
 * places at once.
 */
static void
registry_rescan(AiResourceRegistry *self)
{
    gsize i;

    g_hash_table_remove_all(self->resources);
    g_ptr_array_set_size(self->shadowed, 0);

    /*
     * Pinned resources are filed first, so they win.
     *
     * They also have to be refiled at all: an embedder that registered a
     * command of its own would otherwise lose it the moment the user
     * changed directory, and nothing on disk could put it back.
     */
    for (i = 0; i < self->pinned->len; i++)
    {
        registry_file_resource(self, g_ptr_array_index(self->pinned, i));
    }

    for (i = 0; i < G_N_ELEMENTS(RESOURCE_SOURCES); i++)
    {
        g_autofree gchar *path =
            source_project_path(self, &RESOURCE_SOURCES[i]);

        if (path != NULL)
        {
            registry_scan_dir(self, path, "", RESOURCE_SOURCES[i].kind,
                              RESOURCE_SOURCES[i].origin,
                              AI_RESOURCE_SCOPE_PROJECT, 0);
        }
    }

    for (i = 0; i < G_N_ELEMENTS(RESOURCE_SOURCES); i++)
    {
        g_autofree gchar *path = source_user_path(&RESOURCE_SOURCES[i]);

        if (path != NULL)
        {
            registry_scan_dir(self, path, "", RESOURCE_SOURCES[i].kind,
                              RESOURCE_SOURCES[i].origin,
                              AI_RESOURCE_SCOPE_USER, 0);
        }
    }
}

/* ================================================================
 * Watching
 * ================================================================ */

static gboolean
on_rescan_timeout(gpointer user_data)
{
    AiResourceRegistry *self = user_data;

    self->rescan_id = 0;

    registry_rescan(self);

    /* Directories can appear and disappear between scans, so the monitor
     * set is rebuilt alongside the resource set. */
    if (self->watching)
    {
        registry_detach_monitors(self);
        registry_attach_monitors(self);
    }

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);

    return G_SOURCE_REMOVE;
}

/*
 * Coalesce a burst of file events into one rescan.
 *
 * The source is attached to the thread-default context rather than the
 * global default, for the same reason the image-generation timeouts are:
 * a caller driving a nested loop on a private context would never see a
 * global-default timer fire.
 */
static void
registry_schedule_rescan(AiResourceRegistry *self)
{
    GSource *source;

    if (self->rescan_id != 0)
    {
        return;
    }

    source = g_timeout_source_new(RESCAN_DEBOUNCE_MS);
    g_source_set_callback(source, on_rescan_timeout, self, NULL);
    self->rescan_id = g_source_attach(source,
                                      g_main_context_get_thread_default());
    g_source_unref(source);
}

static void
on_monitor_changed(
    GFileMonitor      *monitor,
    GFile             *file,
    GFile             *other,
    GFileMonitorEvent  event,
    gpointer           user_data
){
    AiResourceRegistry *self = user_data;

    (void)monitor;
    (void)file;
    (void)other;
    (void)event;

    registry_schedule_rescan(self);
}

static void
registry_watch_path(
    AiResourceRegistry *self,
    const gchar        *path
){
    g_autoptr(GFile)        dir = NULL;
    g_autoptr(GFileMonitor) monitor = NULL;
    g_autoptr(GError)       local_error = NULL;

    if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_DIR))
    {
        return;
    }

    dir = g_file_new_for_path(path);
    monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL,
                                       &local_error);

    if (monitor == NULL)
    {
        g_debug("ai_resource_registry: cannot watch '%s': %s", path,
                local_error->message);
        return;
    }

    g_signal_connect(monitor, "changed",
                     G_CALLBACK(on_monitor_changed), self);
    g_ptr_array_add(self->monitors, g_steal_pointer(&monitor));
}

static void
registry_attach_monitors(AiResourceRegistry *self)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(RESOURCE_SOURCES); i++)
    {
        g_autofree gchar *project =
            source_project_path(self, &RESOURCE_SOURCES[i]);
        g_autofree gchar *user = source_user_path(&RESOURCE_SOURCES[i]);

        registry_watch_path(self, project);
        registry_watch_path(self, user);
    }
}

static void
registry_detach_monitors(AiResourceRegistry *self)
{
    guint i;

    for (i = 0; i < self->monitors->len; i++)
    {
        GFileMonitor *monitor = g_ptr_array_index(self->monitors, i);

        g_signal_handlers_disconnect_by_data(monitor, self);
        g_file_monitor_cancel(monitor);
    }

    g_ptr_array_set_size(self->monitors, 0);
}

/* ================================================================
 * GObject plumbing
 * ================================================================ */

static void
ai_resource_registry_dispose(GObject *object)
{
    AiResourceRegistry *self = AI_RESOURCE_REGISTRY(object);

    /*
     * Monitors go first and the pending rescan with them. A timeout that
     * fired between dispose and finalize would run against a half-torn
     * object.
     */
    if (self->rescan_id != 0)
    {
        GSource *source = g_main_context_find_source_by_id(
            g_main_context_get_thread_default(), self->rescan_id);

        if (source != NULL)
        {
            g_source_destroy(source);
        }

        self->rescan_id = 0;
    }

    registry_detach_monitors(self);

    G_OBJECT_CLASS(ai_resource_registry_parent_class)->dispose(object);
}

static void
ai_resource_registry_finalize(GObject *object)
{
    AiResourceRegistry *self = AI_RESOURCE_REGISTRY(object);

    g_clear_pointer(&self->working_directory, g_free);
    g_clear_pointer(&self->resources, g_hash_table_unref);
    g_clear_pointer(&self->shadowed, g_ptr_array_unref);
    g_clear_pointer(&self->pinned, g_ptr_array_unref);
    g_clear_pointer(&self->monitors, g_ptr_array_unref);

    G_OBJECT_CLASS(ai_resource_registry_parent_class)->finalize(object);
}

static void
ai_resource_registry_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiResourceRegistry *self = AI_RESOURCE_REGISTRY(object);

    switch (prop_id)
    {
        case PROP_WORKING_DIRECTORY:
            g_value_set_string(value, self->working_directory);
            break;

        case PROP_WATCHING:
            g_value_set_boolean(value, self->watching);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_resource_registry_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiResourceRegistry *self = AI_RESOURCE_REGISTRY(object);

    switch (prop_id)
    {
        case PROP_WORKING_DIRECTORY:
            ai_resource_registry_set_working_directory(
                self, g_value_get_string(value));
            break;

        case PROP_WATCHING:
            ai_resource_registry_set_watching(self,
                                              g_value_get_boolean(value));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_resource_registry_class_init(AiResourceRegistryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = ai_resource_registry_dispose;
    object_class->finalize = ai_resource_registry_finalize;
    object_class->get_property = ai_resource_registry_get_property;
    object_class->set_property = ai_resource_registry_set_property;

    /**
     * AiResourceRegistry:working-directory:
     *
     * What the project-scope search paths are relative to.
     *
     * Setting it rescans, because a frontend that changes directory has
     * changed which project's commands apply.
     */
    properties[PROP_WORKING_DIRECTORY] =
        g_param_spec_string("working-directory", NULL, NULL, NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiResourceRegistry:watching:
     *
     * Whether the search directories are monitored for changes.
     *
     * Off by default: a registry built to answer one question should not
     * leave inotify watches behind.
     */
    properties[PROP_WATCHING] =
        g_param_spec_boolean("watching", NULL, NULL, FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);

    /**
     * AiResourceRegistry::changed:
     * @self: the registry
     *
     * The set of known resources may differ from the last time you
     * looked.
     *
     * Emitted once per rescan, not once per file --- a frontend
     * rebuilding a completion list wants one notification for a `git
     * pull` that touched thirty files, not thirty.
     */
    signals[SIGNAL_CHANGED] =
        g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);
}

static void
ai_resource_registry_init(AiResourceRegistry *self)
{
    self->working_directory = g_get_current_dir();
    self->resources = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_object_unref);
    self->shadowed = g_ptr_array_new_with_free_func(g_object_unref);
    self->pinned = g_ptr_array_new_with_free_func(g_object_unref);
    self->monitors = g_ptr_array_new_with_free_func(g_object_unref);
}

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * ai_resource_registry_new:
 *
 * Creates an empty registry rooted at the current directory.
 *
 * Nothing is read until ai_resource_registry_scan() is called, so
 * constructing one is cheap and cannot fail.
 *
 * Returns: (transfer full): a new #AiResourceRegistry
 */
AiResourceRegistry *
ai_resource_registry_new(void)
{
    return g_object_new(AI_TYPE_RESOURCE_REGISTRY, NULL);
}

/**
 * ai_resource_registry_set_working_directory:
 * @self: an #AiResourceRegistry
 * @path: (nullable): the directory, or %NULL for the current one
 *
 * Sets what project-scope paths are resolved against, and rescans.
 */
void
ai_resource_registry_set_working_directory(
    AiResourceRegistry *self,
    const gchar        *path
){
    g_return_if_fail(AI_IS_RESOURCE_REGISTRY(self));

    if (g_strcmp0(self->working_directory, path) == 0)
    {
        return;
    }

    g_free(self->working_directory);
    self->working_directory = (path != NULL) ? g_strdup(path)
                                             : g_get_current_dir();

    g_object_notify_by_pspec(G_OBJECT(self),
                             properties[PROP_WORKING_DIRECTORY]);

    ai_resource_registry_scan(self);
}

/**
 * ai_resource_registry_get_working_directory:
 * @self: an #AiResourceRegistry
 *
 * Returns: (transfer none): the directory project paths resolve against
 */
const gchar *
ai_resource_registry_get_working_directory(AiResourceRegistry *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE_REGISTRY(self), NULL);

    return self->working_directory;
}

/**
 * ai_resource_registry_scan:
 * @self: an #AiResourceRegistry
 *
 * Rereads every search directory and emits #AiResourceRegistry::changed.
 *
 * Scanning twice in a row is idempotent: the previous result is dropped
 * first, so nothing accumulates.
 */
void
ai_resource_registry_scan(AiResourceRegistry *self)
{
    g_return_if_fail(AI_IS_RESOURCE_REGISTRY(self));

    registry_rescan(self);

    if (self->watching)
    {
        registry_detach_monitors(self);
        registry_attach_monitors(self);
    }

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

/**
 * ai_resource_registry_lookup:
 * @self: an #AiResourceRegistry
 * @kind: what to look for
 * @name: its name
 *
 * Finds one resource.
 *
 * Lookup is keyed on kind *and* name, so a skill and a command called
 * the same thing coexist rather than one hiding the other.
 *
 * Returns: (transfer none) (nullable): the resource, or %NULL
 */
AiResource *
ai_resource_registry_lookup(
    AiResourceRegistry *self,
    AiResourceKind      kind,
    const gchar        *name
){
    g_autofree gchar *key = NULL;

    g_return_val_if_fail(AI_IS_RESOURCE_REGISTRY(self), NULL);
    g_return_val_if_fail(name != NULL, NULL);

    key = resource_key(kind, name);

    return g_hash_table_lookup(self->resources, key);
}

static gint
compare_by_name(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(ai_resource_get_name((AiResource *)a),
                     ai_resource_get_name((AiResource *)b));
}

/**
 * ai_resource_registry_list:
 * @self: an #AiResourceRegistry
 * @kind: what to list
 *
 * Every resource of one kind, sorted by name.
 *
 * Returns: (transfer container) (element-type AiResource): the
 *   resources. Free with g_list_free(); the resources stay owned by the
 *   registry.
 */
GList *
ai_resource_registry_list(
    AiResourceRegistry *self,
    AiResourceKind      kind
){
    GList          *out = NULL;
    GHashTableIter  iter;
    gpointer        value;

    g_return_val_if_fail(AI_IS_RESOURCE_REGISTRY(self), NULL);

    g_hash_table_iter_init(&iter, self->resources);

    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        AiResource *resource = value;

        if (ai_resource_get_kind(resource) == kind)
        {
            out = g_list_prepend(out, resource);
        }
    }

    return g_list_sort(out, compare_by_name);
}

/**
 * ai_resource_registry_list_shadowed:
 * @self: an #AiResourceRegistry
 *
 * Every resource that lost a name collision.
 *
 * These are kept rather than dropped so a listing can answer "why is my
 * command not the one running" with the path of the file that beat it,
 * instead of silently omitting both halves of the problem.
 *
 * Returns: (transfer container) (element-type AiResource): the losers
 */
GList *
ai_resource_registry_list_shadowed(AiResourceRegistry *self)
{
    GList *out = NULL;
    guint  i;

    g_return_val_if_fail(AI_IS_RESOURCE_REGISTRY(self), NULL);

    for (i = self->shadowed->len; i > 0; i--)
    {
        out = g_list_prepend(out, g_ptr_array_index(self->shadowed, i - 1));
    }

    return out;
}

/**
 * ai_resource_registry_add:
 * @self: an #AiResourceRegistry
 * @resource: (transfer none): the resource to add
 *
 * Files a resource that did not come from disk.
 *
 * For tests, and for embedders with storage of their own. Added
 * resources survive a rescan and take precedence over anything found on
 * disk: the embedder asked for this one by name, and losing it the
 * moment the user changed directory --- with nothing on disk able to put
 * it back --- would make the call useless.
 *
 * Adding two with the same name follows the same first-writer-wins rule
 * a scan does; the second is recorded as shadowed.
 */
void
ai_resource_registry_add(
    AiResourceRegistry *self,
    AiResource         *resource
){
    g_return_if_fail(AI_IS_RESOURCE_REGISTRY(self));
    g_return_if_fail(AI_IS_RESOURCE(resource));

    g_ptr_array_add(self->pinned, g_object_ref(resource));
    registry_file_resource(self, resource);
}

/**
 * ai_resource_registry_set_watching:
 * @self: an #AiResourceRegistry
 * @watching: whether to monitor the search directories
 *
 * Starts or stops reporting changes made outside this process.
 *
 * Events are coalesced: an editor writing a file as several operations,
 * or a `git pull` touching thirty of them, produces one
 * #AiResourceRegistry::changed a quarter-second later rather than
 * thirty immediately.
 */
void
ai_resource_registry_set_watching(
    AiResourceRegistry *self,
    gboolean            watching
){
    g_return_if_fail(AI_IS_RESOURCE_REGISTRY(self));

    watching = !!watching;

    if (self->watching == watching)
    {
        return;
    }

    self->watching = watching;

    if (watching)
    {
        registry_attach_monitors(self);
    }
    else
    {
        registry_detach_monitors(self);
    }

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WATCHING]);
}

/**
 * ai_resource_registry_get_watching:
 * @self: an #AiResourceRegistry
 *
 * Returns: whether the search directories are monitored
 */
gboolean
ai_resource_registry_get_watching(AiResourceRegistry *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE_REGISTRY(self), FALSE);

    return self->watching;
}

/**
 * ai_resource_registry_get_search_paths:
 * @self: an #AiResourceRegistry
 * @kind: which kind's paths to report
 *
 * Every directory that would be searched for @kind, in precedence
 * order, whether or not it exists.
 *
 * This is what a `/commands` listing shows when the answer to "why is my
 * file not being found" is that it is in the wrong directory.
 *
 * Returns: (transfer full) (array zero-terminated=1): the paths
 */
gchar **
ai_resource_registry_get_search_paths(
    AiResourceRegistry *self,
    AiResourceKind      kind
){
    g_autoptr(GPtrArray) paths = NULL;
    gsize                i;

    g_return_val_if_fail(AI_IS_RESOURCE_REGISTRY(self), NULL);

    paths = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < G_N_ELEMENTS(RESOURCE_SOURCES); i++)
    {
        gchar *path;

        if (RESOURCE_SOURCES[i].kind != kind)
        {
            continue;
        }

        path = source_project_path(self, &RESOURCE_SOURCES[i]);

        if (path != NULL)
        {
            g_ptr_array_add(paths, path);
        }
    }

    for (i = 0; i < G_N_ELEMENTS(RESOURCE_SOURCES); i++)
    {
        gchar *path;

        if (RESOURCE_SOURCES[i].kind != kind)
        {
            continue;
        }

        path = source_user_path(&RESOURCE_SOURCES[i]);

        if (path != NULL)
        {
            g_ptr_array_add(paths, path);
        }
    }

    g_ptr_array_add(paths, NULL);

    return (gchar **)g_ptr_array_free(g_steal_pointer(&paths), FALSE);
}

/**
 * ai_resource_registry_get_search_paths_for_origin:
 * @self: an #AiResourceRegistry
 * @origin: the harness to ask about, as spelled in #AiResource:origin
 * @kind: which kind of resource
 *
 * Where one harness looks for one kind of resource, project directories
 * first and then user ones, in the order it searches them.
 *
 * ai_resource_registry_get_search_paths() answers for every harness at
 * once, which is the right question when the caller is going to read
 * the directories. It is the wrong question when the caller is going to
 * *write* one: a tool that installs a skill for an agent has to put it
 * where that agent's own CLI will look, and nowhere else.
 *
 * The distinction is not academic. `~/.claude/skills` is read by four of
 * the five harnesses and `~/.agents/skills` by three, but not the same
 * three -- so neither is a universal answer, and a caller that guessed
 * one would silently install nothing for the harnesses that do not read
 * it.
 *
 * Returns: (transfer full) (array zero-terminated=1): the paths, or an
 *   empty vector when @origin is not a harness this build knows
 */
gchar **
ai_resource_registry_get_search_paths_for_origin(
    AiResourceRegistry *self,
    const gchar        *origin,
    AiResourceKind      kind
){
    g_autoptr(GPtrArray) paths = NULL;
    gsize                i;

    g_return_val_if_fail(AI_IS_RESOURCE_REGISTRY(self), NULL);
    g_return_val_if_fail(origin != NULL, NULL);

    paths = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < G_N_ELEMENTS(RESOURCE_SOURCES); i++)
    {
        gchar *path;

        if (RESOURCE_SOURCES[i].kind != kind ||
            g_strcmp0(RESOURCE_SOURCES[i].origin, origin) != 0)
        {
            continue;
        }

        path = source_project_path(self, &RESOURCE_SOURCES[i]);

        if (path != NULL)
        {
            g_ptr_array_add(paths, path);
        }
    }

    for (i = 0; i < G_N_ELEMENTS(RESOURCE_SOURCES); i++)
    {
        gchar *path;

        if (RESOURCE_SOURCES[i].kind != kind ||
            g_strcmp0(RESOURCE_SOURCES[i].origin, origin) != 0)
        {
            continue;
        }

        path = source_user_path(&RESOURCE_SOURCES[i]);

        if (path != NULL)
        {
            g_ptr_array_add(paths, path);
        }
    }

    g_ptr_array_add(paths, NULL);

    return (gchar **)g_ptr_array_free(g_steal_pointer(&paths), FALSE);
}
