/*
 * ai-updater.c - Check for, and install, a newer build of ai-glib
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Two jobs, one object:
 *
 *  - The check. A fetch of the tracked upstream branch followed by a
 *    handful of local git commands, run on a worker thread so it never
 *    blocks a front-end's main loop, bounded by a timeout, and quiet
 *    about failure: an offline laptop is normal operation, not news.
 *
 *  - The run. Fast-forward, submodules, clean build, optional tests,
 *    install. Every step is a GSubprocess with an argv, never a shell
 *    string, and the first failure stops the pipeline -- so a build that
 *    does not compile never reaches `make install`, and the running
 *    install is untouched.
 *
 * Load-bearing rules, each with a test in tests/test-ai-updater.c:
 *
 *  - Never guess the checkout. It must exist, be the top of a git
 *    repository, and contain the commit this binary was built from.
 *    Anything else is UNAVAILABLE with a sentence saying which.
 *  - The branch must track the upstream being checked. `git merge
 *    --ff-only` into some other branch would install that branch.
 *  - Git never prompts. Children run in their own session with stdin
 *    closed and every askpass disabled; a background check in ai-tui
 *    that opened /dev/tty for an ssh passphrase would draw over the
 *    screen. The one exception is the interactive install, whose sudo
 *    is *meant* to prompt.
 *  - The source is held by pointer and destroyed with
 *    g_source_destroy(); stop() makes every later completion silent.
 */

#include "config.h"

#include "core/ai-updater.h"
#include "core/ai-build-info.h"
#include "core/ai-error.h"
#include "core/ai-json-util.h"
#include "core/ai-subprocess-util.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define AI_UPDATE_STATE_FILE "update-status.json"
#define AI_UPDATE_LOG_FILE   "update.log"

/* Local git commands. They read the object store and nothing else, so a
 * wedged one means a wedged filesystem; do not wait on it forever. */
#define AI_UPDATE_LOCAL_TIMEOUT_MS (15 * 1000)

#define AI_UPDATE_DEFAULT_FETCH_TIMEOUT_S (60)

/* After SIGTERM to a step's process group, how long before SIGKILL. */
#define AI_UPDATE_KILL_GRACE_US (3 * G_USEC_PER_SEC)

struct _AiUpdater
{
	GObject          parent_instance;

	gchar           *source_dir;
	gchar           *build_commit;
	gchar           *build_version;
	gchar           *upstream;
	gchar           *state_dir;
	gchar           *make_program;
	gchar           *sudo_program;
	gchar           *prefix;
	gchar           *libdir;
	gchar           *includedir;
	gchar           *build_type;
	guint            interval;
	guint            fetch_timeout;
	gboolean         run_tests;

	AiUpdateStatus  *status;

	GMainContext    *context;   /* the monitor's; NULL until started */
	GSource         *tick;
	GCancellable    *cancel;    /* the monitor's in-flight check */
	guint            checking;
	gboolean         running;
	gboolean         stopped;
};

G_DEFINE_TYPE(AiUpdater, ai_updater, G_TYPE_OBJECT)

enum
{
	PROP_0,
	PROP_SOURCE_DIR,
	PROP_BUILD_COMMIT,
	PROP_BUILD_VERSION,
	PROP_UPSTREAM,
	PROP_STATE_DIR,
	PROP_MAKE_PROGRAM,
	PROP_SUDO_PROGRAM,
	PROP_PREFIX,
	PROP_LIBDIR,
	PROP_INCLUDEDIR,
	PROP_BUILD_TYPE,
	PROP_INTERVAL,
	PROP_FETCH_TIMEOUT,
	PROP_RUN_TESTS,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
	SIGNAL_STATUS_CHANGED,
	SIGNAL_STEP,
	SIGNAL_OUTPUT,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ================================================================
 * The plan: an immutable snapshot a worker thread may read
 * ================================================================ */

typedef struct
{
	gchar            *source_dir;
	gchar            *build_commit;
	gchar            *build_version;
	gchar            *upstream;
	gchar            *state_dir;
	gchar            *make_program;
	gchar            *sudo_program;
	gchar            *prefix;
	gchar            *libdir;
	gchar            *includedir;
	gchar            *build_type;
	guint             fetch_timeout;
	gboolean          run_tests;
	gboolean          fetch;
	AiUpdateRunFlags  flags;
	AiUpdateStatus   *previous;

	/* Where step and output signals go. A NULL context means the caller
	 * is on the thread that owns @owner and they are emitted directly. */
	AiUpdater        *owner;
	GMainContext     *context;
	GOutputStream    *log;
} Plan;

static void
plan_free(Plan *plan)
{
	g_free(plan->source_dir);
	g_free(plan->build_commit);
	g_free(plan->build_version);
	g_free(plan->upstream);
	g_free(plan->state_dir);
	g_free(plan->make_program);
	g_free(plan->sudo_program);
	g_free(plan->prefix);
	g_free(plan->libdir);
	g_free(plan->includedir);
	g_free(plan->build_type);
	ai_update_status_free(plan->previous);
	g_clear_object(&plan->owner);
	g_clear_pointer(&plan->context, g_main_context_unref);
	g_clear_object(&plan->log);
	g_free(plan);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Plan, plan_free)

static Plan *
plan_new(AiUpdater *self, gboolean fetch, AiUpdateRunFlags flags, gboolean threaded)
{
	Plan *plan = g_new0(Plan, 1);

	plan->source_dir = g_strdup(self->source_dir);
	plan->build_commit = g_strdup(self->build_commit);
	plan->build_version = g_strdup(self->build_version);
	plan->upstream = g_strdup(self->upstream);
	plan->state_dir = g_strdup(self->state_dir);
	plan->make_program = g_strdup(self->make_program);
	plan->sudo_program = g_strdup(self->sudo_program);
	plan->prefix = g_strdup(self->prefix);
	plan->libdir = g_strdup(self->libdir);
	plan->includedir = g_strdup(self->includedir);
	plan->build_type = g_strdup(self->build_type);
	plan->fetch_timeout = self->fetch_timeout;
	plan->run_tests = self->run_tests;
	plan->fetch = fetch;
	plan->flags = flags;
	plan->previous = ai_update_status_copy(self->status);
	plan->owner = g_object_ref(self);
	if (threaded)
		plan->context = g_main_context_ref_thread_default();
	return plan;
}

/* ================================================================
 * Signals, from whichever thread the pipeline is on
 * ================================================================ */

typedef struct
{
	AiUpdater *owner;
	guint      signal;
	gchar     *text;
} Emission;

static void
emission_free(gpointer data)
{
	Emission *emission = data;

	g_object_unref(emission->owner);
	g_free(emission->text);
	g_free(emission);
}

static gboolean
emission_dispatch(gpointer data)
{
	Emission *emission = data;

	if (!emission->owner->stopped)
		g_signal_emit(emission->owner, signals[emission->signal], 0, emission->text);
	return G_SOURCE_REMOVE;
}

static void
plan_log(Plan *plan, const gchar *text)
{
	if (plan->log == NULL)
		return;
	g_output_stream_write_all(plan->log, text, strlen(text), NULL, NULL, NULL);
	g_output_stream_write_all(plan->log, "\n", 1, NULL, NULL, NULL);
}

/* Emit without logging. */
static void
plan_signal(Plan *plan, guint signal, const gchar *text)
{
	if (plan->context == NULL)
	{
		if (!plan->owner->stopped)
			g_signal_emit(plan->owner, signals[signal], 0, text);
		return;
	}

	{
		Emission *emission = g_new0(Emission, 1);

		emission->owner = g_object_ref(plan->owner);
		emission->signal = signal;
		emission->text = g_strdup(text);
		g_main_context_invoke_full(plan->context, G_PRIORITY_DEFAULT,
		                           emission_dispatch, emission, emission_free);
	}
}

static void
plan_emit(Plan *plan, guint signal, const gchar *text)
{
	plan_log(plan, text);
	plan_signal(plan, signal, text);
}

static void G_GNUC_PRINTF(2, 3)
plan_say(Plan *plan, const gchar *format, ...)
{
	g_autofree gchar *text = NULL;
	va_list args;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	plan_emit(plan, SIGNAL_OUTPUT, text);
}

/* ================================================================
 * Children
 * ================================================================ */

/* A new session: no controlling terminal to prompt on, and a process
 * group of its own that a cancel can signal as a whole. */
static void
child_new_session(gpointer data)
{
	setsid();
}

/*
 * The environment every child gets. GIT_DIR and friends are dropped
 * because they win over `git -C`: run from inside a git hook, the check
 * would otherwise read whatever repository the hook belonged to. The
 * MAKE* variables go because `make test` passes a jobserver to its
 * children, and a stale one confuses the make this spawns.
 */
static GSubprocessLauncher *
launcher_new(Plan *plan, GSubprocessFlags flags, gboolean detach)
{
	static const gchar *const drop[] = {
		"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
		"GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
		"GIT_PREFIX", "GIT_NAMESPACE", "MAKEFLAGS", "MFLAGS", "MAKELEVEL",
		"MAKEOVERRIDES",
	};
	GSubprocessLauncher *launcher = g_subprocess_launcher_new(flags);
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(drop); i++)
		g_subprocess_launcher_unsetenv(launcher, drop[i]);
	g_subprocess_launcher_setenv(launcher, "GIT_TERMINAL_PROMPT", "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_ASKPASS", "false", TRUE);
	g_subprocess_launcher_setenv(launcher, "SSH_ASKPASS", "false", TRUE);
	g_subprocess_launcher_setenv(launcher, "SSH_ASKPASS_REQUIRE", "never", TRUE);
	if (plan->source_dir != NULL)
		g_subprocess_launcher_set_cwd(launcher, plan->source_dir);
	if (detach)
		g_subprocess_launcher_set_child_setup(launcher, child_new_session, NULL, NULL);
	return launcher;
}

/*
 * One bounded git command in the checkout. Returns FALSE for a git
 * failure (with @err_text holding its stderr) as well as for a spawn
 * failure; only cancellation sets @error, because everything else is
 * an answer the check turns into a state.
 */
static gboolean
git_run(
	Plan               *plan,
	const gchar *const *args,
	gint                timeout_ms,
	GCancellable       *cancellable,
	gchar             **out,
	gchar             **err_text,
	GError            **error
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GError) local = NULL;
	g_autofree gchar *stdout_text = NULL;
	g_autofree gchar *stderr_text = NULL;
	gsize i;

	g_ptr_array_add(argv, (gpointer)"git");
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);

	launcher = launcher_new(plan, G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                              G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                              G_SUBPROCESS_FLAGS_STDERR_PIPE, TRUE);
	proc = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &local);
	if (proc == NULL)
	{
		g_debug("updater: cannot run git: %s", local->message);
		if (err_text != NULL)
			*err_text = g_strdup(local->message);
		return FALSE;
	}

	if (!ai_subprocess_communicate_utf8_bounded(proc, NULL, timeout_ms, cancellable,
	                                            &stdout_text, &stderr_text, &local))
	{
		if (g_cancellable_is_cancelled(cancellable))
		{
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
			return FALSE;
		}
		g_debug("updater: git %s: %s", args[0], local->message);
		if (err_text != NULL)
			*err_text = g_strdup(local->message);
		return FALSE;
	}

	if (!g_subprocess_get_successful(proc))
	{
		g_debug("updater: git %s failed: %s", args[0], stderr_text != NULL ? stderr_text : "");
		if (err_text != NULL)
			*err_text = g_strdup(stderr_text != NULL ? g_strstrip(stderr_text) : "");
		return FALSE;
	}

	if (out != NULL)
		*out = g_strdup(stdout_text != NULL ? g_strstrip(stdout_text) : "");
	return TRUE;
}

#define GIT_ARGS(...) ((const gchar *const[]){ __VA_ARGS__, NULL })

/* The first line of a git answer, or NULL when git said no. */
static gchar *
git_line(Plan *plan, const gchar *const *args, GCancellable *cancellable, GError **error)
{
	gchar *out = NULL;

	if (!git_run(plan, args, AI_UPDATE_LOCAL_TIMEOUT_MS, cancellable, &out, NULL, error))
		return NULL;
	return out;
}

static guint
git_count(Plan *plan, const gchar *range, GCancellable *cancellable, GError **error)
{
	g_autofree gchar *out = git_line(plan, GIT_ARGS("rev-list", "--count", range),
	                                 cancellable, error);

	return out != NULL ? (guint)g_ascii_strtoull(out, NULL, 10) : 0;
}

/* ================================================================
 * State file
 * ================================================================ */

static gchar *
state_path(const gchar *state_dir)
{
	return g_build_filename(state_dir, AI_UPDATE_STATE_FILE, NULL);
}

/* The whole state file as an object, or a fresh one. Never fails: a
 * malformed file costs itself, the same rule as every other file here
 * that a person or another version of this program may have written. */
static JsonObject *
state_read(const gchar *state_dir)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;

	if (state_dir == NULL)
		return json_object_new();
	path = state_path(state_dir);
	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return json_object_new();
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, contents, -1, NULL))
	{
		g_debug("updater: ignoring malformed %s", path);
		return json_object_new();
	}
	root = ai_json_root_object(parser);
	if (root == NULL || ai_json_get_int(root, "version", 0) != 1)
		return json_object_new();
	return json_object_ref(root);
}

static void
state_write(const gchar *state_dir, JsonObject *root)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	if (state_dir == NULL)
		return;
	if (g_mkdir_with_parents(state_dir, 0700) != 0)
	{
		g_debug("updater: cannot create %s: %s", state_dir, g_strerror(errno));
		return;
	}
	json_object_set_int_member(root, "version", 1);
	json_node_set_object(node, root);
	json_generator_set_root(generator, node);
	json_generator_set_pretty(generator, TRUE);
	text = json_generator_to_data(generator, NULL);
	path = state_path(state_dir);
	if (!g_file_set_contents(path, text, -1, &error))
		g_debug("updater: cannot write %s: %s", path, error->message);
}

static void
state_save_status(const gchar *state_dir, const AiUpdateStatus *status)
{
	g_autoptr(JsonObject) root = state_read(state_dir);

	json_object_set_member(root, "status", ai_update_status_to_json(status));
	state_write(state_dir, root);
}

static void
json_set_string_or_null(JsonObject *object, const gchar *name, const gchar *value)
{
	if (value != NULL)
		json_object_set_string_member(object, name, value);
	else
		json_object_set_null_member(object, name);
}

/**
 * ai_update_status_to_json: (skip)
 * @status: a status
 *
 * The wire form `ai --check-update --json` prints and the state file
 * keeps, including the sentence, so a script and a person read the same
 * verdict.
 *
 * Returns: (transfer full): an object node
 */
JsonNode *
ai_update_status_to_json(const AiUpdateStatus *status)
{
	JsonObject *object = json_object_new();
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *summary = ai_update_status_dup_summary(status);

	json_object_set_string_member(object, "state", ai_update_state_to_string(status->state));
	json_object_set_string_member(object, "summary", summary);
	json_object_set_int_member(object, "behind", status->behind);
	json_object_set_int_member(object, "checkout_behind", status->checkout_behind);
	json_object_set_int_member(object, "ahead", status->ahead);
	json_object_set_boolean_member(object, "dirty", status->dirty);
	json_object_set_boolean_member(object, "pending_restart", status->pending_restart);
	json_object_set_boolean_member(object, "fetch_failed", status->fetch_failed);
	json_set_string_or_null(object, "upstream", status->upstream);
	json_set_string_or_null(object, "branch", status->branch);
	json_set_string_or_null(object, "source_dir", status->source_dir);
	json_set_string_or_null(object, "build_commit", status->build_commit);
	json_set_string_or_null(object, "head_commit", status->head_commit);
	json_set_string_or_null(object, "upstream_commit", status->upstream_commit);
	json_set_string_or_null(object, "detail", status->detail);
	json_object_set_int_member(object, "checked_at", status->checked_at);
	json_object_set_int_member(object, "fetched_at", status->fetched_at);
	json_object_set_int_member(object, "attempted_at", status->attempted_at);
	json_node_take_object(node, object);
	return node;
}

static gchar *
json_dup_string(JsonObject *object, const gchar *name)
{
	const gchar *value = ai_json_get_string(object, name, NULL);

	return value != NULL && g_utf8_validate(value, -1, NULL) ? g_strdup(value) : NULL;
}

/**
 * ai_update_status_from_json: (skip)
 * @node: what ai_update_status_to_json() produced, or anything else
 *
 * Returns: (transfer full) (nullable): the status, or %NULL when @node
 *   is not one
 */
AiUpdateStatus *
ai_update_status_from_json(JsonNode *node)
{
	JsonObject *object;
	AiUpdateStatus *status;
	AiUpdateState state;

	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;
	object = json_node_get_object(node);
	state = ai_update_state_from_string(ai_json_get_string(object, "state", NULL));
	if (state == AI_UPDATE_STATE_UNKNOWN)
		return NULL;

	status = g_new0(AiUpdateStatus, 1);
	status->state = state;
	status->behind = (guint)MAX(0, ai_json_get_int(object, "behind", 0));
	status->checkout_behind = (guint)MAX(0, ai_json_get_int(object, "checkout_behind", 0));
	status->ahead = (guint)MAX(0, ai_json_get_int(object, "ahead", 0));
	status->dirty = ai_json_get_boolean(object, "dirty", FALSE);
	status->pending_restart = ai_json_get_boolean(object, "pending_restart", FALSE);
	status->fetch_failed = ai_json_get_boolean(object, "fetch_failed", FALSE);
	status->upstream = json_dup_string(object, "upstream");
	status->branch = json_dup_string(object, "branch");
	status->source_dir = json_dup_string(object, "source_dir");
	status->build_commit = json_dup_string(object, "build_commit");
	status->head_commit = json_dup_string(object, "head_commit");
	status->upstream_commit = json_dup_string(object, "upstream_commit");
	status->detail = json_dup_string(object, "detail");
	status->checked_at = ai_json_get_int(object, "checked_at", 0);
	status->fetched_at = ai_json_get_int(object, "fetched_at", 0);
	status->attempted_at = ai_json_get_int(object, "attempted_at", 0);
	return status;
}

/* ================================================================
 * The check
 * ================================================================ */

static AiUpdateStatus *
status_unavailable(AiUpdateStatus *status, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static AiUpdateStatus *
status_unavailable(AiUpdateStatus *status, const gchar *format, ...)
{
	va_list args;

	va_start(args, format);
	g_free(status->detail);
	status->detail = g_strdup_vprintf(format, args);
	va_end(args);
	status->state = AI_UPDATE_STATE_UNAVAILABLE;
	return status;
}

/* "origin/master" -> "origin", "master". The remote is the part before
 * the first slash; a branch may contain slashes, a remote rarely does. */
static gboolean
split_upstream(const gchar *upstream, gchar **remote, gchar **branch)
{
	const gchar *slash = upstream != NULL ? strchr(upstream, '/') : NULL;

	if (slash == NULL || slash == upstream || slash[1] == '\0')
		return FALSE;
	*remote = g_strndup(upstream, slash - upstream);
	*branch = g_strdup(slash + 1);
	return TRUE;
}

/* Whether the last recorded install already carries @upstream_commit
 * into this prefix, so only this process is old. */
static gboolean
install_pending_restart(Plan *plan, const gchar *upstream_commit, GCancellable *cancellable)
{
	g_autoptr(JsonObject) root = state_read(plan->state_dir);
	JsonObject *last = ai_json_get_object(root, "last_update");
	const gchar *to_commit = ai_json_get_string(last, "to_commit", NULL);
	const gchar *prefix = ai_json_get_string(last, "prefix", NULL);

	if (to_commit == NULL || g_strcmp0(prefix, plan->prefix) != 0 ||
	    g_strcmp0(to_commit, plan->build_commit) == 0)
		return FALSE;
	return git_run(plan, GIT_ARGS("merge-base", "--is-ancestor", upstream_commit, to_commit),
	               AI_UPDATE_LOCAL_TIMEOUT_MS, cancellable, NULL, NULL, NULL);
}

/*
 * The check itself. Always answers with a status; the only error is
 * cancellation. Runs on whichever thread calls it and touches nothing
 * but @plan.
 */
static AiUpdateStatus *
classify(Plan *plan, GCancellable *cancellable, GError **error)
{
	g_autoptr(AiUpdateStatus) status = g_new0(AiUpdateStatus, 1);
	g_autofree gchar *real = NULL;
	g_autofree gchar *top = NULL;
	g_autofree gchar *real_top = NULL;
	g_autofree gchar *remote = NULL;
	g_autofree gchar *branch_name = NULL;
	g_autofree gchar *tracked_remote = NULL;
	g_autofree gchar *tracked_merge = NULL;
	g_autofree gchar *upstream_ref = NULL;
	g_autofree gchar *range = NULL;
	g_autofree gchar *porcelain = NULL;
	g_autofree gchar *commit_spec = NULL;
	GError *local = NULL;

	status->source_dir = g_strdup(plan->source_dir);
	status->build_commit = g_strdup(plan->build_commit);
	status->checked_at = g_get_real_time() / G_USEC_PER_SEC;
	if (plan->previous != NULL)
	{
		status->fetched_at = plan->previous->fetched_at;
		status->attempted_at = plan->previous->attempted_at;
	}

#define CANCELLED() (local != NULL ? (g_propagate_error(error, local), TRUE) : FALSE)
#define UNAVAILABLE(...) \
	do { status_unavailable(status, __VA_ARGS__); return g_steal_pointer(&status); } while (0)

	if (plan->source_dir == NULL || plan->source_dir[0] == '\0')
		UNAVAILABLE(
			"this build does not know which checkout it came from. "
			"Set AI_GLIB_SOURCE_DIR or updates.source-dir.");
	if (!g_file_test(plan->source_dir, G_FILE_TEST_IS_DIR))
		UNAVAILABLE(
			"the source checkout %s does not exist.", plan->source_dir);

	top = git_line(plan, GIT_ARGS("rev-parse", "--show-toplevel"), cancellable, &local);
	if (CANCELLED())
		return NULL;
	if (top == NULL)
		UNAVAILABLE(
			"%s is not a git checkout.", plan->source_dir);
	real = realpath(plan->source_dir, NULL);
	real_top = realpath(top, NULL);
	if (real == NULL || real_top == NULL || g_strcmp0(real, real_top) != 0)
		UNAVAILABLE(
			"%s is inside a git checkout but not the top of one.", plan->source_dir);

	if (plan->build_commit == NULL || plan->build_commit[0] == '\0')
		UNAVAILABLE(
			"this binary was not built from a git checkout.");
	commit_spec = g_strdup_printf("%s^{commit}", plan->build_commit);
	if (!git_run(plan, GIT_ARGS("cat-file", "-e", commit_spec), AI_UPDATE_LOCAL_TIMEOUT_MS,
	             cancellable, NULL, NULL, &local))
	{
		if (CANCELLED())
			return NULL;
		UNAVAILABLE(
			"%s does not contain commit %.12s, which this binary was built from.",
			plan->source_dir, plan->build_commit);
	}

	status->branch = git_line(plan, GIT_ARGS("symbolic-ref", "-q", "--short", "HEAD"),
	                          cancellable, &local);
	if (CANCELLED())
		return NULL;
	if (status->branch == NULL)
		UNAVAILABLE(
			"%s has a detached HEAD; check out the branch to update.", plan->source_dir);

	{
		g_autofree gchar *remote_key = g_strdup_printf("branch.%s.remote", status->branch);
		g_autofree gchar *merge_key = g_strdup_printf("branch.%s.merge", status->branch);

		tracked_remote = git_line(plan, GIT_ARGS("config", "--get", remote_key), cancellable, &local);
		if (CANCELLED())
			return NULL;
		tracked_merge = git_line(plan, GIT_ARGS("config", "--get", merge_key), cancellable, &local);
		if (CANCELLED())
			return NULL;
	}

	/* Which upstream: configured, else the branch's own, else origin/master. */
	if (plan->upstream != NULL && plan->upstream[0] != '\0')
	{
		if (!split_upstream(plan->upstream, &remote, &branch_name))
			UNAVAILABLE(
				"the configured upstream \"%s\" is not REMOTE/BRANCH.", plan->upstream);
	}
	else if (tracked_remote != NULL && tracked_merge != NULL &&
	         g_str_has_prefix(tracked_merge, "refs/heads/") && !g_str_equal(tracked_remote, "."))
	{
		remote = g_strdup(tracked_remote);
		branch_name = g_strdup(tracked_merge + strlen("refs/heads/"));
	}
	else
	{
		remote = g_strdup("origin");
		branch_name = g_strdup("master");
	}
	status->upstream = g_strdup_printf("%s/%s", remote, branch_name);

	/* The branch has to track it, or a fast-forward would move a branch
	 * that is not the one being checked. */
	if (tracked_remote != NULL && tracked_merge != NULL)
	{
		g_autofree gchar *expected_merge = g_strdup_printf("refs/heads/%s", branch_name);

		if (!g_str_equal(tracked_remote, remote) || !g_str_equal(tracked_merge, expected_merge))
		{
			const gchar *short_merge = g_str_has_prefix(tracked_merge, "refs/heads/")
				? tracked_merge + strlen("refs/heads/") : tracked_merge;

			UNAVAILABLE(
				"branch %s tracks %s/%s, not %s.", status->branch, tracked_remote,
				short_merge, status->upstream);
		}
	}
	else if (!g_str_equal(status->branch, branch_name))
	{
		UNAVAILABLE(
			"branch %s does not track %s.", status->branch, status->upstream);
	}

	upstream_ref = g_strdup_printf("refs/remotes/%s/%s", remote, branch_name);

	if (plan->fetch)
	{
		g_autofree gchar *refspec = g_strdup_printf("+refs/heads/%s:%s", branch_name, upstream_ref);
		g_autofree gchar *fetch_error = NULL;
		gint timeout_ms = (gint)MIN((guint)G_MAXINT / 1000, MAX(plan->fetch_timeout, 1u)) * 1000;

		status->attempted_at = g_get_real_time() / G_USEC_PER_SEC;
		if (git_run(plan, GIT_ARGS("fetch", "--quiet", "--no-tags", "--no-write-fetch-head",
		                           remote, refspec),
		            timeout_ms, cancellable, NULL, &fetch_error, &local))
		{
			status->fetched_at = status->attempted_at;
		}
		else
		{
			if (CANCELLED())
				return NULL;
			/* Offline, auth, a moved remote: all normal, all quiet. */
			status->fetch_failed = TRUE;
			status->detail = g_strdup_printf("could not fetch %s: %s", status->upstream,
			                                 fetch_error != NULL && fetch_error[0] != '\0'
			                                     ? fetch_error : "unknown error");
		}
	}

	{
		g_autofree gchar *spec = g_strdup_printf("%s^{commit}", upstream_ref);

		status->upstream_commit = git_line(plan, GIT_ARGS("rev-parse", "--verify", "-q", spec),
		                                   cancellable, &local);
		if (CANCELLED())
			return NULL;
		if (status->upstream_commit == NULL)
			UNAVAILABLE(
				"%s has never been fetched into %s.", status->upstream, plan->source_dir);
	}

	status->head_commit = git_line(plan, GIT_ARGS("rev-parse", "HEAD"), cancellable, &local);
	if (CANCELLED())
		return NULL;

	range = g_strdup_printf("%s..%s", plan->build_commit, status->upstream_commit);
	status->behind = git_count(plan, range, cancellable, &local);
	if (CANCELLED())
		return NULL;
	g_free(range);
	range = g_strdup_printf("%s..HEAD", status->upstream_commit);
	status->ahead = git_count(plan, range, cancellable, &local);
	if (CANCELLED())
		return NULL;
	g_free(range);
	range = g_strdup_printf("HEAD..%s", status->upstream_commit);
	status->checkout_behind = git_count(plan, range, cancellable, &local);
	if (CANCELLED())
		return NULL;

	/* Submodules are excluded: the update itself resets them, and a
	 * build leaves their trees full of output. */
	porcelain = git_line(plan, GIT_ARGS("status", "--porcelain", "--untracked-files=no",
	                                    "--ignore-submodules=all"), cancellable, &local);
	if (CANCELLED())
		return NULL;
	status->dirty = porcelain != NULL && porcelain[0] != '\0';

	if (status->ahead > 0 && status->checkout_behind > 0)
		status->state = AI_UPDATE_STATE_DIVERGED;
	else if (status->dirty)
		status->state = AI_UPDATE_STATE_LOCAL_CHANGES;
	else if (status->behind > 0)
		status->state = AI_UPDATE_STATE_BEHIND;
	else
		status->state = AI_UPDATE_STATE_UP_TO_DATE;

	if (status->state == AI_UPDATE_STATE_BEHIND)
		status->pending_restart = install_pending_restart(plan, status->upstream_commit, cancellable);

#undef CANCELLED
#undef UNAVAILABLE

	return g_steal_pointer(&status);
}

/* ================================================================
 * The pipeline
 * ================================================================ */

void
ai_update_result_free(AiUpdateResult *result)
{
	if (result == NULL)
		return;
	g_free(result->from_version);
	g_free(result->to_version);
	g_free(result->from_commit);
	g_free(result->to_commit);
	g_free(result->privileged_command);
	g_free(result->log_path);
	g_free(result);
}

/* config.mk's VERSION_MAJOR/MINOR/MICRO, which is the release number. */
static gchar *
read_checkout_version(const gchar *source_dir)
{
	g_autofree gchar *path = g_build_filename(source_dir, "config.mk", NULL);
	g_autofree gchar *contents = NULL;
	g_auto(GStrv) lines = NULL;
	const gchar *names[] = { "VERSION_MAJOR", "VERSION_MINOR", "VERSION_MICRO" };
	gint64 parts[3] = { -1, -1, -1 };
	gsize i;
	gsize j;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return NULL;
	lines = g_strsplit(contents, "\n", -1);
	for (i = 0; lines[i] != NULL; i++)
	{
		for (j = 0; j < G_N_ELEMENTS(names); j++)
		{
			const gchar *rest;

			if (!g_str_has_prefix(lines[i], names[j]))
				continue;
			rest = lines[i] + strlen(names[j]);
			while (*rest == ' ' || *rest == '\t')
				rest++;
			if (*rest != '=')
				continue;
			rest++;
			while (*rest == ' ' || *rest == '\t')
				rest++;
			if (g_ascii_isdigit(*rest))
				parts[j] = g_ascii_strtoll(rest, NULL, 10);
		}
	}
	if (parts[0] < 0 || parts[1] < 0 || parts[2] < 0)
		return NULL;
	return g_strdup_printf("%" G_GINT64_FORMAT ".%" G_GINT64_FORMAT ".%" G_GINT64_FORMAT,
	                       parts[0], parts[1], parts[2]);
}

/* PREFIX=... LIBDIR=... INCLUDEDIR=... [DEBUG=1], appended to @argv. */
static void
add_make_vars(Plan *plan, GPtrArray *argv)
{
	if (plan->prefix != NULL)
		g_ptr_array_add(argv, g_strdup_printf("PREFIX=%s", plan->prefix));
	if (plan->libdir != NULL)
		g_ptr_array_add(argv, g_strdup_printf("LIBDIR=%s", plan->libdir));
	if (plan->includedir != NULL)
		g_ptr_array_add(argv, g_strdup_printf("INCLUDEDIR=%s", plan->includedir));
	if (g_strcmp0(plan->build_type, "debug") == 0)
		g_ptr_array_add(argv, g_strdup("DEBUG=1"));
}

static GPtrArray *
make_argv(Plan *plan, const gchar *first, ...) G_GNUC_NULL_TERMINATED;

static GPtrArray *
make_argv(Plan *plan, const gchar *first, ...)
{
	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	const gchar *arg;
	va_list args;

	g_ptr_array_add(argv, g_strdup(plan->make_program != NULL ? plan->make_program : "make"));
	va_start(args, first);
	for (arg = first; arg != NULL; arg = va_arg(args, const gchar *))
		g_ptr_array_add(argv, g_strdup(arg));
	va_end(args);
	return argv;
}

/* Every directory `make install` writes, walked up to the nearest one
 * that exists: that is the one whose permissions decide. */
static gboolean
install_writable(Plan *plan)
{
	g_autofree gchar *bin = g_build_filename(plan->prefix, "bin", NULL);
	g_autofree gchar *share = g_build_filename(plan->prefix, "share", NULL);
	g_autofree gchar *lib = plan->libdir != NULL ? g_strdup(plan->libdir)
	                                             : g_build_filename(plan->prefix, "lib", NULL);
	g_autofree gchar *include = plan->includedir != NULL
		? g_strdup(plan->includedir) : g_build_filename(plan->prefix, "include", NULL);
	const gchar *dirs[4];
	gsize i;

	dirs[0] = bin;
	dirs[1] = share;
	dirs[2] = lib;
	dirs[3] = include;
	for (i = 0; i < G_N_ELEMENTS(dirs); i++)
	{
		g_autofree gchar *dir = g_strdup(dirs[i]);

		while (!g_file_test(dir, G_FILE_TEST_EXISTS))
		{
			gchar *parent = g_path_get_dirname(dir);

			if (g_str_equal(parent, dir))
			{
				g_free(parent);
				break;
			}
			g_free(dir);
			dir = parent;
		}
		if (g_access(dir, W_OK) != 0)
			return FALSE;
	}
	return TRUE;
}

static gchar *
argv_to_display(GPtrArray *argv)
{
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; i < argv->len; i++)
	{
		g_autofree gchar *quoted = g_shell_quote(g_ptr_array_index(argv, i));
		const gchar *word = g_ptr_array_index(argv, i);

		if (i > 0)
			g_string_append_c(text, ' ');
		/* Quote only what needs it: the command is for a person to read. */
		if (word[0] != '\0' && strpbrk(word, " \t\n'\"\\$`*?[]{}()<>|&;#~!") == NULL)
			g_string_append(text, word);
		else
			g_string_append(text, quoted);
	}
	return g_string_free(text, FALSE);
}

static gboolean
line_is_cancelled(GError *error)
{
	return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* SIGTERM the step's whole group -- make and every compiler under it --
 * then SIGKILL make if it has not gone in a few seconds. */
static void
terminate_step(GSubprocess *proc, gboolean detached)
{
	const gchar *identifier = g_subprocess_get_identifier(proc);
	gint64 deadline = g_get_monotonic_time() + AI_UPDATE_KILL_GRACE_US;

	if (identifier == NULL)
		return;
	if (detached)
		kill(-(pid_t)g_ascii_strtoll(identifier, NULL, 10), SIGTERM);
	else
		g_subprocess_send_signal(proc, SIGTERM);
	/* The identifier goes NULL once GLib has reaped the child. */
	while (g_subprocess_get_identifier(proc) != NULL && g_get_monotonic_time() < deadline)
		g_usleep(50 * 1000);
	g_subprocess_force_exit(proc);
	g_subprocess_wait(proc, NULL, NULL);
}

/*
 * One step. Output is read line by line, logged and emitted; @inherit
 * instead hands the child this process's terminal, which is what lets
 * sudo ask for a password. Only cancellation and failure return FALSE.
 */
static gboolean
run_step(
	Plan          *plan,
	const gchar   *description,
	GPtrArray     *argv,
	gboolean       inherit,
	GCancellable  *cancellable,
	GError       **error
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *display = argv_to_display(argv);
	g_autofree gchar *header = g_strdup_printf("==> %s: %s", description, display);
	gboolean detached = !(plan->flags & AI_UPDATE_RUN_INTERACTIVE);
	gint status;

	plan_log(plan, header);
	plan_signal(plan, SIGNAL_STEP, description);
	plan_signal(plan, SIGNAL_OUTPUT, header);

	if (g_cancellable_set_error_if_cancelled(cancellable, error))
		return FALSE;

	g_ptr_array_add(argv, NULL);
	launcher = launcher_new(plan, inherit ? G_SUBPROCESS_FLAGS_NONE
	                                      : G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                                        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                        G_SUBPROCESS_FLAGS_STDERR_MERGE,
	                        detached && !inherit);
	proc = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &local);
	g_ptr_array_remove_index(argv, argv->len - 1);
	if (proc == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "%s: cannot run %s: %s",
		            description, (const gchar *)g_ptr_array_index(argv, 0), local->message);
		return FALSE;
	}

	if (!inherit)
	{
		g_autoptr(GDataInputStream) lines = NULL;

		g_output_stream_close(g_subprocess_get_stdin_pipe(proc), NULL, NULL);
		lines = g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc));
		for (;;)
		{
			g_autofree gchar *line = g_data_input_stream_read_line(lines, NULL, cancellable, &local);

			if (line == NULL)
				break;
			if (!g_utf8_validate(line, -1, NULL))
			{
				gchar *valid = g_utf8_make_valid(line, -1);

				g_free(line);
				line = valid;
			}
			plan_emit(plan, SIGNAL_OUTPUT, line);
		}
		if (local != NULL && !line_is_cancelled(local))
			g_clear_error(&local);
	}

	if (local == NULL)
		g_subprocess_wait(proc, cancellable, &local);

	if (local != NULL)
	{
		if (line_is_cancelled(local))
		{
			terminate_step(proc, detached && !inherit);
			plan_log(plan, "Cancelled.");
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "%s: %s", description, local->message);
		return FALSE;
	}

	if (!g_subprocess_get_successful(proc))
	{
		status = g_subprocess_get_if_exited(proc) ? g_subprocess_get_exit_status(proc) : -1;
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		            "%s failed (%s %d)", description,
		            status >= 0 ? "exit status" : "signal",
		            status >= 0 ? status : g_subprocess_get_term_sig(proc));
		return FALSE;
	}
	return TRUE;
}

static GOutputStream *
log_open(Plan *plan, gchar **path_out)
{
	g_autofree gchar *path = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) error = NULL;
	GFileOutputStream *stream;

	if (plan->state_dir == NULL || g_mkdir_with_parents(plan->state_dir, 0700) != 0)
		return NULL;
	path = g_build_filename(plan->state_dir, AI_UPDATE_LOG_FILE, NULL);
	file = g_file_new_for_path(path);
	stream = g_file_append_to(file, G_FILE_CREATE_PRIVATE, NULL, &error);
	if (stream == NULL)
	{
		g_debug("updater: cannot open %s: %s", path, error->message);
		return NULL;
	}
	*path_out = g_steal_pointer(&path);
	return G_OUTPUT_STREAM(stream);
}

static gchar *
now_iso8601(void)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();

	return g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
}

/* What a run hands back to the thread that owns the updater. */
typedef struct
{
	AiUpdateResult *result;
	AiUpdateStatus *status;
	GError         *error;
} RunOutput;

static void
run_output_free(RunOutput *output)
{
	ai_update_result_free(output->result);
	ai_update_status_free(output->status);
	g_clear_error(&output->error);
	g_free(output);
}

static void
record_install(Plan *plan, AiUpdateResult *result)
{
	g_autoptr(JsonObject) root = state_read(plan->state_dir);
	JsonObject *last = json_object_new();
	g_autofree gchar *at = now_iso8601();

	json_set_string_or_null(last, "from_version", result->from_version);
	json_set_string_or_null(last, "to_version", result->to_version);
	json_set_string_or_null(last, "from_commit", result->from_commit);
	json_set_string_or_null(last, "to_commit", result->to_commit);
	json_set_string_or_null(last, "prefix", plan->prefix);
	json_object_set_string_member(last, "at", at);
	json_object_set_object_member(root, "last_update", last);
	state_write(plan->state_dir, root);
}

static gchar *
short_commit(const gchar *commit)
{
	return commit != NULL ? g_strndup(commit, 12) : g_strdup("unknown");
}

static void
pipeline(Plan *plan, GCancellable *cancellable, RunOutput *output)
{
	g_autoptr(AiUpdateResult) result = g_new0(AiUpdateResult, 1);
	g_autoptr(GPtrArray) argv = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *jobs = g_strdup_printf("-j%u", MAX(1u, g_get_num_processors()));
	g_autofree gchar *at = now_iso8601();
	g_autofree gchar *from_short = NULL;
	g_autofree gchar *to_short = NULL;
	AiUpdateStatus *status;
	GError **error = &output->error;

	plan->fetch = TRUE;
	status = classify(plan, cancellable, error);
	if (status == NULL)
		return;
	output->status = status;
	state_save_status(plan->state_dir, status);

	if (status->fetch_failed)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_NETWORK_ERROR, "Not updating: %s", status->detail);
		return;
	}
	refusal = ai_update_status_dup_refusal(status, FALSE);
	if (refusal != NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, refusal);
		return;
	}

	plan->log = log_open(plan, &result->log_path);
	plan_say(plan, "=== %s: updating %s from %s", at, plan->source_dir, status->upstream);

	result->from_version = g_strdup(plan->build_version);
	result->from_commit = g_strdup(plan->build_commit);

	if (status->checkout_behind > 0)
	{
		argv = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(argv, g_strdup("git"));
		g_ptr_array_add(argv, g_strdup("merge"));
		g_ptr_array_add(argv, g_strdup("--ff-only"));
		g_ptr_array_add(argv, g_strdup(status->upstream_commit));
		if (!run_step(plan, "Fast-forwarding", argv, FALSE, cancellable, error))
			goto failed;
		g_clear_pointer(&argv, g_ptr_array_unref);
	}

	argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup("git"));
	g_ptr_array_add(argv, g_strdup("submodule"));
	g_ptr_array_add(argv, g_strdup("update"));
	g_ptr_array_add(argv, g_strdup("--init"));
	g_ptr_array_add(argv, g_strdup("--recursive"));
	if (!run_step(plan, "Updating submodules", argv, FALSE, cancellable, error))
		goto failed;
	g_clear_pointer(&argv, g_ptr_array_unref);

	argv = make_argv(plan, "clean", NULL);
	if (g_strcmp0(plan->build_type, "debug") == 0)
		g_ptr_array_add(argv, g_strdup("DEBUG=1"));
	if (!run_step(plan, "Cleaning", argv, FALSE, cancellable, error))
		goto failed;
	g_clear_pointer(&argv, g_ptr_array_unref);

	argv = make_argv(plan, jobs, "all", NULL);
	add_make_vars(plan, argv);
	if (!run_step(plan, "Building", argv, FALSE, cancellable, error))
		goto failed;
	g_clear_pointer(&argv, g_ptr_array_unref);

	if (plan->run_tests)
	{
		argv = make_argv(plan, jobs, "test", NULL);
		add_make_vars(plan, argv);
		if (!run_step(plan, "Testing", argv, FALSE, cancellable, error))
			goto failed;
		g_clear_pointer(&argv, g_ptr_array_unref);
	}

	{
		g_autofree gchar *head = NULL;
		GError *local = NULL;

		head = git_line(plan, GIT_ARGS("rev-parse", "HEAD"), cancellable, &local);
		if (local != NULL)
		{
			g_propagate_error(error, local);
			goto failed;
		}
		result->to_commit = g_steal_pointer(&head);
		result->to_version = read_checkout_version(plan->source_dir);
	}

	argv = make_argv(plan, "install", NULL);
	add_make_vars(plan, argv);
	if (install_writable(plan))
	{
		if (!run_step(plan, "Installing", argv, FALSE, cancellable, error))
			goto failed;
	}
	else if (plan->flags & AI_UPDATE_RUN_INTERACTIVE)
	{
		g_ptr_array_insert(argv, 0, g_strdup(plan->sudo_program != NULL ? plan->sudo_program : "sudo"));
		plan_say(plan, "%s is not writable; installing with %s.", plan->prefix,
		         (const gchar *)g_ptr_array_index(argv, 0));
		if (!run_step(plan, "Installing", argv, TRUE, cancellable, error))
			goto failed;
	}
	else
	{
		/* The same command, spelled for a person: -C rather than a cwd. */
		g_ptr_array_insert(argv, 1, g_strdup("-C"));
		g_ptr_array_insert(argv, 2, g_strdup(plan->source_dir));
		g_ptr_array_insert(argv, 0, g_strdup(plan->sudo_program != NULL ? plan->sudo_program : "sudo"));
		result->privileged_command = argv_to_display(argv);
		result->outcome = AI_UPDATE_OUTCOME_NEEDS_PRIVILEGE;
		plan_say(plan, "Built. Installing into %s needs privilege; run:", plan->prefix);
		plan_say(plan, "  %s", result->privileged_command);
		output->result = g_steal_pointer(&result);
		return;
	}

	result->outcome = AI_UPDATE_OUTCOME_INSTALLED;
	record_install(plan, result);
	from_short = short_commit(result->from_commit);
	to_short = short_commit(result->to_commit);
	plan_say(plan, "Installed %s (%s) -> %s (%s) into %s. Restart to use it.",
	         result->from_version != NULL ? result->from_version : "unknown", from_short,
	         result->to_version != NULL ? result->to_version : "unknown", to_short,
	         plan->prefix);

	/* This process is now the old build; say so rather than "behind". */
	{
		AiUpdateStatus *after;

		plan->fetch = FALSE;
		ai_update_status_free(plan->previous);
		plan->previous = ai_update_status_copy(status);
		after = classify(plan, NULL, NULL);
		if (after != NULL)
		{
			ai_update_status_free(output->status);
			output->status = after;
			state_save_status(plan->state_dir, after);
		}
	}
	output->result = g_steal_pointer(&result);
	return;

failed:
	if (!line_is_cancelled(*error))
	{
		GError *cause = *error;

		*error = g_error_new(cause->domain, cause->code,
		                     "Update stopped; nothing was installed. %s%s%s%s",
		                     cause->message,
		                     result->log_path != NULL ? " See " : "",
		                     result->log_path != NULL ? result->log_path : "",
		                     result->log_path != NULL ? "." : "");
		g_error_free(cause);
	}
	plan_log(plan, (*error)->message);
}

/* ================================================================
 * Publishing a status on the owner's thread
 * ================================================================ */

static void
publish(AiUpdater *self, AiUpdateStatus *status)
{
	if (status == NULL || self->stopped)
		return;
	ai_update_status_free(self->status);
	self->status = ai_update_status_copy(status);
	g_signal_emit(self, signals[SIGNAL_STATUS_CHANGED], 0);
}

/* ================================================================
 * GObject
 * ================================================================ */

static void
ai_updater_dispose(GObject *object)
{
	ai_updater_stop(AI_UPDATER(object));
	G_OBJECT_CLASS(ai_updater_parent_class)->dispose(object);
}

static void
ai_updater_finalize(GObject *object)
{
	AiUpdater *self = AI_UPDATER(object);

	g_free(self->source_dir);
	g_free(self->build_commit);
	g_free(self->build_version);
	g_free(self->upstream);
	g_free(self->state_dir);
	g_free(self->make_program);
	g_free(self->sudo_program);
	g_free(self->prefix);
	g_free(self->libdir);
	g_free(self->includedir);
	g_free(self->build_type);
	ai_update_status_free(self->status);
	g_clear_object(&self->cancel);
	g_clear_pointer(&self->context, g_main_context_unref);
	G_OBJECT_CLASS(ai_updater_parent_class)->finalize(object);
}

static gchar **
string_field(AiUpdater *self, guint prop_id)
{
	switch (prop_id)
	{
		case PROP_SOURCE_DIR:    return &self->source_dir;
		case PROP_BUILD_COMMIT:  return &self->build_commit;
		case PROP_BUILD_VERSION: return &self->build_version;
		case PROP_UPSTREAM:      return &self->upstream;
		case PROP_STATE_DIR:     return &self->state_dir;
		case PROP_MAKE_PROGRAM:  return &self->make_program;
		case PROP_SUDO_PROGRAM:  return &self->sudo_program;
		case PROP_PREFIX:        return &self->prefix;
		case PROP_LIBDIR:        return &self->libdir;
		case PROP_INCLUDEDIR:    return &self->includedir;
		case PROP_BUILD_TYPE:    return &self->build_type;
		default:                 return NULL;
	}
}

static void
ai_updater_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	AiUpdater *self = AI_UPDATER(object);
	gchar **field = string_field(self, prop_id);

	if (field != NULL)
	{
		g_value_set_string(value, *field);
		return;
	}
	switch (prop_id)
	{
		case PROP_INTERVAL:      g_value_set_uint(value, self->interval); break;
		case PROP_FETCH_TIMEOUT: g_value_set_uint(value, self->fetch_timeout); break;
		case PROP_RUN_TESTS:     g_value_set_boolean(value, self->run_tests); break;
		default:                 G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec); break;
	}
}

static void
ai_updater_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	AiUpdater *self = AI_UPDATER(object);
	gchar **field = string_field(self, prop_id);

	if (field != NULL)
	{
		g_free(*field);
		*field = g_value_dup_string(value);
		return;
	}
	switch (prop_id)
	{
		case PROP_INTERVAL:      self->interval = g_value_get_uint(value); break;
		case PROP_FETCH_TIMEOUT: self->fetch_timeout = g_value_get_uint(value); break;
		case PROP_RUN_TESTS:     self->run_tests = g_value_get_boolean(value); break;
		default:                 G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec); break;
	}
}

static GParamSpec *
string_pspec(const gchar *name, const gchar *blurb, const gchar *fallback)
{
	return g_param_spec_string(name, NULL, blurb, fallback,
	                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
}

static void
ai_updater_class_init(AiUpdaterClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ai_updater_dispose;
	object_class->finalize = ai_updater_finalize;
	object_class->get_property = ai_updater_get_property;
	object_class->set_property = ai_updater_set_property;

	properties[PROP_SOURCE_DIR] = string_pspec("source-dir",
		"The checkout to update from", NULL);
	properties[PROP_BUILD_COMMIT] = string_pspec("build-commit",
		"The commit the running binary was built from", NULL);
	properties[PROP_BUILD_VERSION] = string_pspec("build-version",
		"The release number of the running binary", NULL);
	properties[PROP_UPSTREAM] = string_pspec("upstream",
		"REMOTE/BRANCH to track; NULL for the branch's own upstream, else origin/master", NULL);
	/* The default is resolved once per process, like every other XDG
	 * path GLib caches, which is why tests set XDG_STATE_HOME first. */
	properties[PROP_STATE_DIR] = g_param_spec_string("state-dir", NULL,
		"Where the status cache and the update log live", NULL,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	properties[PROP_MAKE_PROGRAM] = string_pspec("make-program",
		"The make to build and install with", "make");
	properties[PROP_SUDO_PROGRAM] = string_pspec("sudo-program",
		"The privilege helper for a prefix this user cannot write", "sudo");
	properties[PROP_PREFIX] = string_pspec("prefix", "PREFIX to install into", NULL);
	properties[PROP_LIBDIR] = string_pspec("libdir", "LIBDIR to install into", NULL);
	properties[PROP_INCLUDEDIR] = string_pspec("includedir", "INCLUDEDIR to install into", NULL);
	properties[PROP_BUILD_TYPE] = string_pspec("build-type", "release or debug", NULL);
	properties[PROP_INTERVAL] = g_param_spec_uint("interval", NULL,
		"Seconds between fetches", 0, G_MAXUINT, AI_UPDATE_DEFAULT_INTERVAL_S,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	properties[PROP_FETCH_TIMEOUT] = g_param_spec_uint("fetch-timeout", NULL,
		"Seconds before a fetch is abandoned", 1, 3600, AI_UPDATE_DEFAULT_FETCH_TIMEOUT_S,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	properties[PROP_RUN_TESTS] = g_param_spec_boolean("run-tests", NULL,
		"Run make test before installing", FALSE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, properties);

	signals[SIGNAL_STATUS_CHANGED] = g_signal_new("status-changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	signals[SIGNAL_STEP] = g_signal_new("step", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
	signals[SIGNAL_OUTPUT] = g_signal_new("output", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
ai_updater_init(AiUpdater *self)
{
	self->state_dir = g_build_filename(g_get_user_state_dir(), "ai-glib", NULL);
}

/* ================================================================
 * Public (private-to-the-project) API
 * ================================================================ */

/*
 * Whether the background check may run at all: AI_GLIB_NO_UPDATE_CHECK
 * (any value but "" or "0") and `updates.check: false` both say no.
 * An explicit `ai --check-update` or /update ignores this -- the user
 * asked.
 */
gboolean
ai_updater_checks_enabled(AiConfig *config)
{
	const gchar *env = g_getenv("AI_GLIB_NO_UPDATE_CHECK");
	gboolean enabled = TRUE;

	if (env != NULL && env[0] != '\0' && !g_str_equal(env, "0"))
		return FALSE;
	if (config != NULL)
		g_object_get(config, "update-check", &enabled, NULL);
	return enabled;
}

/*
 * An updater for the running build: the checkout from AI_GLIB_SOURCE_DIR,
 * else updates.source-dir, else the directory it was built in; the
 * install paths it was configured with; the tracked upstream, interval
 * and test switch from @config; and AI_GLIB_UPDATE_MAKE for `make`.
 */
AiUpdater *
ai_updater_new(AiConfig *config)
{
	const gchar *env_dir = g_getenv("AI_GLIB_SOURCE_DIR");
	const gchar *env_make = g_getenv("AI_GLIB_UPDATE_MAKE");
	g_autofree gchar *config_dir = NULL;
	g_autofree gchar *upstream = NULL;
	guint interval = AI_UPDATE_DEFAULT_INTERVAL_S;
	gboolean run_tests = FALSE;
	const gchar *source_dir;

	if (config != NULL)
		g_object_get(config,
		             "update-source-dir", &config_dir,
		             "update-upstream", &upstream,
		             "update-interval", &interval,
		             "update-run-tests", &run_tests,
		             NULL);

	source_dir = env_dir != NULL && env_dir[0] != '\0' ? env_dir
	           : config_dir != NULL && config_dir[0] != '\0' ? config_dir
	           : ai_build_info_get_source_dir();

	return g_object_new(AI_TYPE_UPDATER,
	                    "source-dir", source_dir,
	                    "build-commit", ai_build_info_get_commit(),
	                    "build-version", ai_build_info_get_version(),
	                    "upstream", upstream,
	                    "prefix", ai_build_info_get_prefix(),
	                    "libdir", ai_build_info_get_libdir(),
	                    "includedir", ai_build_info_get_includedir(),
	                    "build-type", ai_build_info_get_build_type(),
	                    "make-program", env_make != NULL && env_make[0] != '\0' ? env_make : "make",
	                    "interval", interval,
	                    "run-tests", run_tests,
	                    NULL);
}

/* The last status, borrowed; NULL before any check or cache load. */
const AiUpdateStatus *
ai_updater_get_status(AiUpdater *self)
{
	g_return_val_if_fail(AI_IS_UPDATER(self), NULL);
	return self->status;
}

gboolean
ai_updater_is_checking(AiUpdater *self)
{
	g_return_val_if_fail(AI_IS_UPDATER(self), FALSE);
	return self->checking > 0;
}

/*
 * Adopt the cached status, if it belongs to this checkout. Returns FALSE
 * for a missing, malformed or foreign cache. A cache written by a
 * different build keeps only its fetch times: the verdict was about
 * that build, the throttle is about the remote.
 */
gboolean
ai_updater_load_cache(AiUpdater *self)
{
	g_autoptr(JsonObject) root = NULL;
	g_autoptr(AiUpdateStatus) cached = NULL;
	JsonNode *node;

	g_return_val_if_fail(AI_IS_UPDATER(self), FALSE);

	root = state_read(self->state_dir);
	node = ai_json_get_node(root, "status");
	cached = ai_update_status_from_json(node);
	if (cached == NULL || g_strcmp0(cached->source_dir, self->source_dir) != 0)
		return FALSE;
	if (g_strcmp0(cached->build_commit, self->build_commit) != 0)
	{
		cached->state = AI_UPDATE_STATE_UNKNOWN;
		cached->behind = 0;
		cached->pending_restart = FALSE;
	}
	ai_update_status_free(self->status);
	self->status = g_steal_pointer(&cached);
	return TRUE;
}

/* Synchronous check, on the calling thread. Publishes and caches. */
AiUpdateStatus *
ai_updater_check(AiUpdater *self, gboolean fetch, GCancellable *cancellable, GError **error)
{
	g_autoptr(Plan) plan = NULL;
	AiUpdateStatus *status;

	g_return_val_if_fail(AI_IS_UPDATER(self), NULL);

	plan = plan_new(self, fetch, AI_UPDATE_RUN_NONE, FALSE);
	status = classify(plan, cancellable, error);
	if (status == NULL)
		return NULL;
	state_save_status(plan->state_dir, status);
	publish(self, status);
	return status;
}

static void
check_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
	Plan *plan = data;
	GError *error = NULL;
	AiUpdateStatus *status = classify(plan, cancellable, &error);

	if (status == NULL)
	{
		g_task_return_error(task, error);
		return;
	}
	state_save_status(plan->state_dir, status);
	g_task_return_pointer(task, status, (GDestroyNotify)ai_update_status_free);
}

static void
check_done(GObject *source, GAsyncResult *res, gpointer data)
{
	AiUpdater *self = AI_UPDATER(source);
	g_autoptr(GTask) outer = data;
	GError *error = NULL;
	AiUpdateStatus *status = g_task_propagate_pointer(G_TASK(res), &error);

	self->checking--;
	if (status == NULL)
	{
		g_task_return_error(outer, error);
		return;
	}
	publish(self, status);
	g_task_return_pointer(outer, status, (GDestroyNotify)ai_update_status_free);
}

/* The check on a worker thread; completes on the calling context. */
void
ai_updater_check_async(
	AiUpdater           *self,
	gboolean             fetch,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) inner = NULL;
	GTask *outer;

	g_return_if_fail(AI_IS_UPDATER(self));

	outer = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(outer, ai_updater_check_async);
	inner = g_task_new(self, cancellable, check_done, outer);
	g_task_set_task_data(inner, plan_new(self, fetch, AI_UPDATE_RUN_NONE, FALSE),
	                     (GDestroyNotify)plan_free);
	self->checking++;
	g_task_run_in_thread(inner, check_thread);
}

AiUpdateStatus *
ai_updater_check_finish(AiUpdater *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------- */

static void
monitor_checked(GObject *source, GAsyncResult *res, gpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autoptr(GError) error = NULL;

	status = ai_updater_check_finish(AI_UPDATER(source), res, &error);
	if (status == NULL && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_debug("updater: check failed: %s", error->message);
}

/*
 * One monitor pass. The fetch times are re-read from the cache first,
 * so a second ai-tui that fetched a minute ago stops this one fetching
 * again; the local state is always re-read, which is how a restart
 * right after an update stops claiming one is available.
 */
static void
monitor_check(AiUpdater *self)
{
	g_autoptr(JsonObject) root = NULL;
	g_autoptr(AiUpdateStatus) cached = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	JsonNode *node;

	if (self->stopped || self->checking > 0 || self->running)
		return;

	root = state_read(self->state_dir);
	node = ai_json_get_node(root, "status");
	cached = ai_update_status_from_json(node);
	if (cached != NULL && self->status != NULL &&
	    g_strcmp0(cached->source_dir, self->source_dir) == 0)
	{
		self->status->fetched_at = MAX(self->status->fetched_at, cached->fetched_at);
		self->status->attempted_at = MAX(self->status->attempted_at, cached->attempted_at);
	}

	ai_updater_check_async(self, ai_update_fetch_due(self->status, now, self->interval),
	                       self->cancel, monitor_checked, NULL);
}

static gboolean
on_tick(gpointer data)
{
	monitor_check(AI_UPDATER(data));
	return G_SOURCE_CONTINUE;
}

/*
 * Check now, then every few minutes, fetching only when the interval
 * has passed. Attached to the thread-default context, held by pointer.
 * Call ai_updater_stop() before dropping the last reference you care
 * about; dispose does it too.
 */
void
ai_updater_start(AiUpdater *self)
{
	g_return_if_fail(AI_IS_UPDATER(self));

	if (self->tick != NULL)
		return;
	self->stopped = FALSE;
	g_clear_object(&self->cancel);
	self->cancel = g_cancellable_new();
	g_clear_pointer(&self->context, g_main_context_unref);
	self->context = g_main_context_ref_thread_default();
	if (self->status == NULL)
		ai_updater_load_cache(self);

	monitor_check(self);

	self->tick = g_timeout_source_new_seconds(AI_UPDATE_TICK_S);
	g_source_set_callback(self->tick, on_tick, self, NULL);
	g_source_attach(self->tick, self->context);
}

/*
 * Stop the monitor, cancel whatever it has in flight, and make every
 * later completion -- check or run -- silent. Safe to call twice.
 */
void
ai_updater_stop(AiUpdater *self)
{
	g_return_if_fail(AI_IS_UPDATER(self));

	self->stopped = TRUE;
	if (self->cancel != NULL)
		g_cancellable_cancel(self->cancel);
	if (self->tick != NULL)
	{
		g_source_destroy(self->tick);
		g_source_unref(self->tick);
		self->tick = NULL;
	}
}

/* ---------------------------------------------------------------- */

static AiUpdateResult *
run_finish_output(AiUpdater *self, RunOutput *output, GError **error)
{
	self->running = FALSE;
	publish(self, output->status);
	if (output->error != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&output->error));
		return NULL;
	}
	return g_steal_pointer(&output->result);
}

/*
 * Run the pipeline on the calling thread. With
 * %AI_UPDATE_RUN_INTERACTIVE the caller's terminal is the update's: an
 * install needing privilege runs under sudo attached to it.
 *
 * Returns NULL with an error for a refusal (%AI_ERROR_INVALID_REQUEST,
 * the message from ai_update_status_dup_refusal()), an unreachable
 * remote (%AI_ERROR_NETWORK_ERROR), a failed step
 * (%AI_ERROR_CLI_EXECUTION, nothing installed) or a cancel.
 */
AiUpdateResult *
ai_updater_run(AiUpdater *self, AiUpdateRunFlags flags, GCancellable *cancellable, GError **error)
{
	g_autoptr(Plan) plan = NULL;
	RunOutput *output;
	AiUpdateResult *result;

	g_return_val_if_fail(AI_IS_UPDATER(self), NULL);

	if (self->running)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "An update is already running.");
		return NULL;
	}
	self->running = TRUE;
	plan = plan_new(self, TRUE, flags, FALSE);
	output = g_new0(RunOutput, 1);
	pipeline(plan, cancellable, output);
	result = run_finish_output(self, output, error);
	run_output_free(output);
	return result;
}

static void
run_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
	RunOutput *output = g_new0(RunOutput, 1);

	pipeline(data, cancellable, output);
	g_task_return_pointer(task, output, (GDestroyNotify)run_output_free);
}

static void
run_done(GObject *source, GAsyncResult *res, gpointer data)
{
	AiUpdater *self = AI_UPDATER(source);
	g_autoptr(GTask) outer = data;
	RunOutput *output = g_task_propagate_pointer(G_TASK(res), NULL);
	GError *error = NULL;
	AiUpdateResult *result = run_finish_output(self, output, &error);

	run_output_free(output);
	if (result == NULL)
		g_task_return_error(outer, error);
	else
		g_task_return_pointer(outer, result, (GDestroyNotify)ai_update_result_free);
}

/*
 * The pipeline on a worker thread, for a front-end with no terminal.
 * ::step and ::output arrive on the calling context; the flags should
 * not include %AI_UPDATE_RUN_INTERACTIVE, since there is nothing for
 * sudo to prompt on.
 */
void
ai_updater_run_async(
	AiUpdater           *self,
	AiUpdateRunFlags     flags,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) inner = NULL;
	GTask *outer;

	g_return_if_fail(AI_IS_UPDATER(self));

	outer = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(outer, ai_updater_run_async);
	if (self->running)
	{
		g_task_return_new_error(outer, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "An update is already running.");
		g_object_unref(outer);
		return;
	}
	self->running = TRUE;
	inner = g_task_new(self, cancellable, run_done, outer);
	/* The pipeline reports its own cancellation; GTask must not race it
	 * with an early return that would leave the steps running. */
	g_task_set_return_on_cancel(inner, FALSE);
	g_task_set_check_cancellable(inner, FALSE);
	g_task_set_task_data(inner, plan_new(self, TRUE, flags, TRUE), (GDestroyNotify)plan_free);
	g_task_run_in_thread(inner, run_thread);
}

AiUpdateResult *
ai_updater_run_finish(AiUpdater *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
