/*
 * test-build-checks.c - GIR verification must propagate build failures
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gio/gio.h>
#include <glib/gstdio.h>

static gchar *repo_root;

static void
test_gir_check (gconstpointer data)
{
    gint mode = GPOINTER_TO_INT (data);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp ("ai-gir-check-XXXXXX", &error);
    g_autofree gchar *stub = NULL;
    g_autofree gchar *quoted = NULL;
    g_autofree gchar *make_arg = NULL;
    g_autofree gchar *gir_arg = NULL;
    g_autofree gchar *typelib_arg = NULL;
    g_autofree gchar *output = NULL;
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) process = NULL;
    const gchar *script;

    g_assert_no_error (error);
    stub = g_build_filename (dir, "build-stub.sh", NULL);
    if (mode == 0)
        script = "printf 'injected GIR build failure\\n'\nexit 42\n";
    else if (mode == 1)
        script = "printf 'generated introspection data\\n'\n";
    else
        script = "printf 'Warning: injected scanner warning\\n'\n";
    g_assert_true (g_file_set_contents (stub, script, -1, &error));
    g_assert_no_error (error);
    quoted = g_shell_quote (stub);
    make_arg = g_strconcat ("MAKE=sh ", quoted, NULL);
    gir_arg = g_strconcat ("GIR_FILE=", dir, "/Ai.gir", NULL);
    typelib_arg = g_strconcat ("TYPELIB_FILE=", dir, "/Ai.typelib", NULL);
    launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                          G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_cwd (launcher, repo_root);
    /* Do not inherit the outer make's jobserver or command-line overrides. */
    g_subprocess_launcher_unsetenv (launcher, "MAKEFLAGS");
    g_subprocess_launcher_unsetenv (launcher, "MFLAGS");
    g_subprocess_launcher_unsetenv (launcher, "MAKEOVERRIDES");
    process = g_subprocess_launcher_spawn (launcher, &error,
        "make", "--no-print-directory", "test-gir-clean", "GIR_SCANNER=sh",
        make_arg, gir_arg, typelib_arg, NULL);
    g_assert_no_error (error);
    g_assert_true (g_subprocess_communicate_utf8 (process, NULL, NULL,
                                                 &output, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_subprocess_get_successful (process), ==, mode == 1);
    if (mode == 0)
        g_assert_nonnull (strstr (output, "injected GIR build failure"));
    if (mode != 1)
        g_assert_null (strstr (output, "PASS: GIR scanner is clean"));
    g_unlink (stub);
    g_rmdir (dir);
}

int
main (int argc, char **argv)
{
    g_autofree gchar *binary = g_canonicalize_filename (argv[0], NULL);
    g_autofree gchar *binary_dir = g_path_get_dirname (binary);

    repo_root = g_canonicalize_filename ("../../..", binary_dir);
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/ai-glib/build/gir-build-failure", GINT_TO_POINTER (0), test_gir_check);
    g_test_add_data_func ("/ai-glib/build/gir-success", GINT_TO_POINTER (1), test_gir_check);
    g_test_add_data_func ("/ai-glib/build/gir-warning", GINT_TO_POINTER (2), test_gir_check);
    {
        gint result = g_test_run ();
        g_free (repo_root);
        return result;
    }
}
