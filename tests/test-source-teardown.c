/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026
 *
 * test-source-teardown.c - Removing a source by id, in the wrong context
 *
 * Two places attached a GSource to the *thread-default* main context --
 * deliberately, each with a comment saying a caller driving a nested
 * loop on a private context would otherwise never see the timer -- and
 * then tore it down through its integer id.
 *
 * A source id is meaningful only inside the context it was attached to.
 * Every context allocates from 1, so an id from a private context is
 * very likely to name a *different* source on the global default.
 * g_source_remove() searches the global default only, so the teardown
 * destroyed that unrelated source and left the real one armed -- on an
 * object that was being freed.  When the ids did not collide it
 * criticaled instead, which is fatal under G_DEBUG=fatal-warnings.
 *
 * The decoy below is the whole test: it holds the id the private
 * context is about to issue, so a teardown that reaches into the wrong
 * context destroys it and this notices.
 */

#include <ai-glib.h>

/*
 * Drive @context until the rescan debounce has certainly elapsed.
 *
 * RESCAN_DEBOUNCE_MS is 250, so a second of turns is ample and the loop
 * is bounded rather than timed: a test that can hang is worse than one
 * that fails.
 */
static void
pump(GMainContext *context, guint turns)
{
    guint i;

    for (i = 0; i < turns; i++) {
        g_main_context_iteration(context, FALSE);
        g_usleep(1000);
    }
}

/*
 * Build a working directory the registry will watch, and change it, so
 * a rescan really is pending when the registry is disposed.
 *
 * Without a pending rescan there is no source to tear down and the
 * teardown path this is about never runs -- which is what made the
 * first version of this test pass against the bug.
 */
static gchar *
scheduled_rescan_workdir(void)
{
    gchar *dir = g_dir_make_tmp("ai-teardown-XXXXXX", NULL);
    g_autofree gchar *skills = NULL;

    g_assert_nonnull(dir);

    skills = g_build_filename(dir, ".claude", "skills", NULL);
    g_assert_cmpint(g_mkdir_with_parents(skills, 0700), ==, 0);

    return dir;
}

static void
touch_a_skill(const gchar *dir)
{
    g_autofree gchar *path = g_build_filename(dir, ".claude", "skills",
                                              "probe.md", NULL);

    g_assert_true(g_file_set_contents(path, "---\nname: probe\n---\n", -1,
                                      NULL));
}

/*
 * A registry disposed after its context stopped being the thread-default
 * must still disarm the source it attached there.
 *
 * The teardown looked the source up by *id*, in whatever context happened
 * to be thread-default at dispose time.  Ids are per context and every
 * context allocates from 1, so that either found nothing -- leaving the
 * timer armed on an object about to be freed, which is a use-after-free
 * 250 ms later -- or found an unrelated source on the global default and
 * destroyed that instead.
 *
 * Run under `make ASAN=1` the surviving timer is reported as a
 * heap-use-after-free rather than as a failed assertion, which is the
 * honest way to catch it: the dispatch happens inside GLib.
 */
static void
test_a_pending_rescan_is_disarmed_after_the_context_is_popped(void)
{
    g_autoptr(GMainContext) private_context = g_main_context_new();
    g_autofree gchar *dir = scheduled_rescan_workdir();
    AiResourceRegistry *registry;

    g_main_context_push_thread_default(private_context);

    registry = ai_resource_registry_new();
    g_assert_nonnull(registry);

    ai_resource_registry_set_working_directory(registry, dir);
    ai_resource_registry_set_watching(registry, TRUE);

    /* The change the monitor reports, and the turns that report it. */
    touch_a_skill(dir);
    pump(private_context, 60);

    /*
     * Disposed with the private context no longer thread-default, which
     * is the ordinary case: an object is freed wherever its last
     * reference goes, not necessarily where it was built.
     */
    g_main_context_pop_thread_default(private_context);

    g_object_unref(registry);

    /*
     * Past the debounce.  With the source left armed this dispatches
     * on_rescan_timeout() into the freed registry.
     */
    pump(private_context, 600);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/source-teardown/pending-rescan-is-disarmed",
                    test_a_pending_rescan_is_disarmed_after_the_context_is_popped);

    return g_test_run();
}
