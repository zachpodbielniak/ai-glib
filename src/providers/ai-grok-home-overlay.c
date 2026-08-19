/*
 * ai-grok-home-overlay.c - A throwaway GROK_HOME with extra config
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "providers/ai-grok-home-overlay.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <unistd.h>

/* The one file the overlay owns rather than links. */
#define GROK_CONFIG_NAME "config.toml"

/* Stamped into every overlay so destroy can refuse a path that is not
 * one.  Removing a directory tree on the strength of a caller-supplied
 * string is worth one cheap check. */
#define GROK_OVERLAY_STAMP ".ai-glib-overlay"

static gchar *
grok_default_home(void)
{
    const gchar *env = g_getenv("GROK_HOME");

    if (env != NULL && env[0] != '\0')
    {
        return g_strdup(env);
    }

    return g_build_filename(g_get_home_dir(), ".grok", NULL);
}

/*
 * Writes the overlay's config.toml: the source config, then the
 * fragment.  A missing source config is not an error -- a fresh grok
 * install has none, and the fragment alone is a valid config.
 */
static gboolean
grok_overlay_write_config(
    const gchar  *overlay,
    const gchar  *source_home,
    const gchar  *fragment_path,
    GError      **error
){
    g_autofree gchar *source_config = NULL;
    g_autofree gchar *dest_config = NULL;
    g_autofree gchar *base = NULL;
    g_autofree gchar *fragment = NULL;
    g_autoptr(GString) out = NULL;

    source_config = g_build_filename(source_home, GROK_CONFIG_NAME, NULL);
    dest_config = g_build_filename(overlay, GROK_CONFIG_NAME, NULL);
    out = g_string_new(NULL);

    if (g_file_get_contents(source_config, &base, NULL, NULL))
    {
        g_string_append(out, base);
        if (out->len > 0 && out->str[out->len - 1] != '\n')
        {
            g_string_append_c(out, '\n');
        }
    }

    if (fragment_path != NULL && fragment_path[0] != '\0')
    {
        if (!g_file_get_contents(fragment_path, &fragment, NULL, error))
        {
            return FALSE;
        }

        g_string_append_c(out, '\n');
        g_string_append(out, fragment);
        if (out->len > 0 && out->str[out->len - 1] != '\n')
        {
            g_string_append_c(out, '\n');
        }
    }

    return g_file_set_contents(dest_config, out->str, out->len, error);
}

gchar *
ai_grok_home_overlay_create(
    const gchar  *source_home,
    const gchar  *config_fragment_path,
    GError      **error
){
    g_autofree gchar *home = NULL;
    g_autofree gchar *overlay = NULL;
    g_autofree gchar *stamp = NULL;
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    home = source_home != NULL && source_home[0] != '\0'
        ? g_strdup(source_home)
        : grok_default_home();

    overlay = g_dir_make_tmp("ai-glib-grok-XXXXXX", error);
    if (overlay == NULL)
    {
        return NULL;
    }

    stamp = g_build_filename(overlay, GROK_OVERLAY_STAMP, NULL);
    if (!g_file_set_contents(stamp, "", 0, error))
    {
        ai_grok_home_overlay_destroy(overlay);
        return NULL;
    }

    /*
     * Link every entry of the real home except the config, so auth,
     * sessions and plugins keep working and anything grok writes lands
     * in the real directory rather than a temporary one that is about
     * to be deleted.
     */
    dir = g_dir_open(home, 0, NULL);
    if (dir != NULL)
    {
        while ((name = g_dir_read_name(dir)) != NULL)
        {
            g_autofree gchar *target = NULL;
            g_autofree gchar *link = NULL;

            if (g_strcmp0(name, GROK_CONFIG_NAME) == 0)
            {
                continue;
            }

            target = g_build_filename(home, name, NULL);
            link = g_build_filename(overlay, name, NULL);

            /* Best effort: a home we cannot fully mirror is still more
             * useful than no overlay, and grok treats a missing
             * optional file as absent rather than as an error. */
            if (symlink(target, link) != 0)
            {
                g_debug("grok overlay: could not link %s: %s", name,
                        g_strerror(errno));
            }
        }
    }

    if (!grok_overlay_write_config(overlay, home, config_fragment_path,
                                   error))
    {
        ai_grok_home_overlay_destroy(overlay);
        return NULL;
    }

    return g_steal_pointer(&overlay);
}

void
ai_grok_home_overlay_destroy(const gchar *overlay_path)
{
    g_autofree gchar *stamp = NULL;
    g_autoptr(GDir) dir = NULL;
    const gchar *name;

    if (overlay_path == NULL || overlay_path[0] == '\0')
    {
        return;
    }

    /* Refuse anything this module did not create.  Without the stamp a
     * stale or mistaken path could delete a real grok home. */
    stamp = g_build_filename(overlay_path, GROK_OVERLAY_STAMP, NULL);
    if (!g_file_test(stamp, G_FILE_TEST_EXISTS))
    {
        g_debug("grok overlay: refusing to remove %s, not an overlay",
                overlay_path);
        return;
    }

    /*
     * Collect first, unlink after.  Removing entries while readdir is
     * walking the same directory is not defined to work, and the failure
     * would be a silently half-removed overlay rather than an error.
     */
    dir = g_dir_open(overlay_path, 0, NULL);
    if (dir != NULL)
    {
        g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
        guint i;

        while ((name = g_dir_read_name(dir)) != NULL)
        {
            g_ptr_array_add(names, g_strdup(name));
        }

        for (i = 0; i < names->len; i++)
        {
            g_autofree gchar *path =
                g_build_filename(overlay_path,
                                 g_ptr_array_index(names, i), NULL);

            /*
             * g_unlink removes the link itself, never what it points at,
             * which is the whole safety property here: every entry
             * except the config and the stamp is a symlink into the
             * user's real grok home.
             */
            if (g_unlink(path) != 0)
            {
                g_debug("grok overlay: could not remove %s: %s", path,
                        g_strerror(errno));
            }
        }
    }

    if (g_rmdir(overlay_path) != 0)
    {
        g_debug("grok overlay: could not remove %s: %s", overlay_path,
                g_strerror(errno));
    }
}
