/*
 * ai-benchmark.c - Measure and compare providers, models and features
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The runner. It turns targets x cases x iterations into a flat list of
 * jobs, runs them one at a time on the thread-default main context, and
 * records one AiBenchmarkSample per measured job.
 *
 * Execution rules, each with a test in tests/test-benchmark.c:
 *
 *  - Sequential. One call in flight at a time. Concurrency would measure
 *    contention on the caller's network or GPU, not the model.
 *
 *  - Round-robin. For each case and each iteration, every target of that
 *    kind runs once before the next iteration starts. Drift in a shared
 *    resource (a local GPU warming up, a network getting busy) then lands
 *    on every target equally instead of on whichever target ran last.
 *
 *  - Warmup first, unrecorded. Each target runs `warmup` calls of its
 *    first case before anything is measured. The first call to a local
 *    Ollama model loads its weights; counting that would compare a cold
 *    load against a hosted API.
 *
 *  - One clock. Latency is g_get_monotonic_time() around the async call,
 *    taken here, never a provider's own elapsed figure, so every kind and
 *    provider is measured the same way. Time to first token is the first
 *    text delta of a streamed chat.
 *
 *  - Timeouts are cancellations. A held GSource on the run's context
 *    cancels the call; the sample is an error reading "timed out after N
 *    ms". The runner still waits for the provider's callback, so nothing
 *    outlives the run.
 *
 *  - A target that fails to build is skipped and its error kept
 *    (ai_benchmark_get_target_error()); the others still run.
 *
 *  - Cancelling the run's cancellable stops it: the call in flight is
 *    cancelled, its sample is not recorded, the samples already taken
 *    are kept, and the run finishes with G_IO_ERROR_CANCELLED.
 *
 * Remote failures are g_debug: they are data here, not bugs.
 */

#include "bench/ai-benchmark.h"

#include <string.h>

#include "core/ai-decider.h"
#include "core/ai-embedder.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "core/ai-event-source.h"
#include "core/ai-image-generator.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "model/ai-image-request.h"
#include "model/ai-image-response.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-usage.h"

enum
{
	PROP_0,
	PROP_NAME,
	PROP_ITERATIONS,
	PROP_WARMUP,
	PROP_TIMEOUT_MS,
	PROP_CONFIG,
	N_PROPS
};

enum
{
	SIGNAL_SAMPLE,
	N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

typedef struct
{
	guint    target;     /* index into self->targets */
	guint    bench_case; /* index into self->cases */
	guint    iteration;
	gboolean warmup;
} Job;

/* Everything one run_async() owns, freed when its task completes. */
typedef struct
{
	AiBenchmark  *self;          /* borrowed; the task holds a reference */
	GMainContext *context;
	GArray       *jobs;
	guint         next;
	GCancellable *user;          /* nullable */

	/* the job in flight */
	GCancellable *call;
	gulong        call_link;
	GSource      *timeout;
	gboolean      timed_out;
	gint64        started_us;
	gint64        first_text_us;
	GObject      *signal_object;
	gulong        signal_id;
	GList        *messages;
	AiImageRequest *image_request;
} RunState;

struct _AiBenchmark
{
	GObject     parent_instance;
	gchar      *name;
	guint       iterations;
	guint       warmup;
	guint       timeout_ms;
	AiConfig   *config;

	GPtrArray  *targets;      /* AiBenchmarkTarget */
	GPtrArray  *cases;        /* AiBenchmarkCase */
	GPtrArray  *built;        /* GObject or NULL, parallel to targets */
	GHashTable *build_errors; /* label -> message */
	gboolean    prepared;

	GPtrArray  *samples;      /* AiBenchmarkSample */
	gboolean    running;
};

G_DEFINE_TYPE(AiBenchmark, ai_benchmark, G_TYPE_OBJECT)

static void run_next(GTask *task);

/* The built-client array holds NULL for a target that failed to build. */
static void
unref_nullable(
	gpointer object
){
	if (object != NULL)
		g_object_unref(object);
}

static void
ai_benchmark_finalize(
	GObject *object
){
	AiBenchmark *self;

	self = AI_BENCHMARK(object);
	g_free(self->name);
	g_clear_object(&self->config);
	g_ptr_array_unref(self->targets);
	g_ptr_array_unref(self->cases);
	g_ptr_array_unref(self->built);
	g_hash_table_unref(self->build_errors);
	g_ptr_array_unref(self->samples);
	G_OBJECT_CLASS(ai_benchmark_parent_class)->finalize(object);
}

static void
ai_benchmark_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiBenchmark *self;

	self = AI_BENCHMARK(object);
	switch (prop_id)
	{
		case PROP_NAME:
			g_value_set_string(value, self->name);
			break;
		case PROP_ITERATIONS:
			g_value_set_uint(value, self->iterations);
			break;
		case PROP_WARMUP:
			g_value_set_uint(value, self->warmup);
			break;
		case PROP_TIMEOUT_MS:
			g_value_set_uint(value, self->timeout_ms);
			break;
		case PROP_CONFIG:
			g_value_set_object(value, self->config);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void
ai_benchmark_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiBenchmark *self;

	self = AI_BENCHMARK(object);
	switch (prop_id)
	{
		case PROP_NAME:
			ai_benchmark_set_name(self, g_value_get_string(value));
			break;
		case PROP_ITERATIONS:
			ai_benchmark_set_iterations(self, g_value_get_uint(value));
			break;
		case PROP_WARMUP:
			ai_benchmark_set_warmup(self, g_value_get_uint(value));
			break;
		case PROP_TIMEOUT_MS:
			ai_benchmark_set_timeout_ms(self, g_value_get_uint(value));
			break;
		case PROP_CONFIG:
			ai_benchmark_set_config(self, (AiConfig *)g_value_get_object(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void
ai_benchmark_class_init(
	AiBenchmarkClass *klass
){
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = ai_benchmark_finalize;
	object_class->get_property = ai_benchmark_get_property;
	object_class->set_property = ai_benchmark_set_property;

	/**
	 * AiBenchmark:name:
	 *
	 * A title for the run, shown in every output format.
	 */
	properties[PROP_NAME] = g_param_spec_string("name", "Name", "Title of the benchmark",
		NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiBenchmark:iterations:
	 *
	 * Measured runs of each case on each target. At least 1.
	 */
	properties[PROP_ITERATIONS] = g_param_spec_uint("iterations", "Iterations",
		"Measured runs of each case on each target", 1, AI_BENCHMARK_MAX_ITERATIONS, 3,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiBenchmark:warmup:
	 *
	 * Unrecorded calls each target makes before measuring starts.
	 */
	properties[PROP_WARMUP] = g_param_spec_uint("warmup", "Warmup",
		"Unrecorded calls per target before measuring", 0, AI_BENCHMARK_MAX_WARMUP, 1,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiBenchmark:timeout-ms:
	 *
	 * Per-call deadline in milliseconds; 0 leaves it to the provider.
	 */
	properties[PROP_TIMEOUT_MS] = g_param_spec_uint("timeout-ms", "Timeout",
		"Per-call deadline in milliseconds, 0 for the provider's own", 0, G_MAXUINT, 0,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiBenchmark:config:
	 *
	 * Configuration handed to the provider factories when targets are built.
	 */
	properties[PROP_CONFIG] = g_param_spec_object("config", "Config",
		"Configuration for building targets", AI_TYPE_CONFIG,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties(object_class, N_PROPS, properties);

	/**
	 * AiBenchmark::sample:
	 * @self: the benchmark
	 * @sample: the measured run that just finished
	 *
	 * Emitted once per measured run, in run order, on the context the run
	 * was started from. Warmup calls emit nothing.
	 */
	signals[SIGNAL_SAMPLE] = g_signal_new("sample", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, AI_TYPE_BENCHMARK_SAMPLE);
}

static void
ai_benchmark_init(
	AiBenchmark *self
){
	self->iterations = 3;
	self->warmup = 1;
	self->targets = g_ptr_array_new_with_free_func(g_object_unref);
	self->cases = g_ptr_array_new_with_free_func(g_object_unref);
	self->built = g_ptr_array_new_with_free_func(unref_nullable);
	self->build_errors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->samples = g_ptr_array_new_with_free_func(g_object_unref);
}

/**
 * ai_benchmark_new:
 *
 * Creates an empty benchmark: 3 iterations, 1 warmup call per target,
 * no timeout of its own. Add targets and cases, then run it.
 *
 * Returns: (transfer full): a new benchmark
 */
AiBenchmark *
ai_benchmark_new(
	void
){
	return (AiBenchmark *)g_object_new(AI_TYPE_BENCHMARK, NULL);
}

/**
 * ai_benchmark_get_name:
 * @self: a benchmark
 *
 * Returns: (transfer none) (nullable): the title
 */
const gchar *
ai_benchmark_get_name(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);
	return self->name;
}

/**
 * ai_benchmark_set_name:
 * @self: a benchmark
 * @name: (nullable): a title
 */
void
ai_benchmark_set_name(
	AiBenchmark *self,
	const gchar *name
){
	g_return_if_fail(AI_IS_BENCHMARK(self));
	if (g_strcmp0(self->name, name) == 0)
		return;
	g_free(self->name);
	self->name = g_strdup(name);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_NAME]);
}

/**
 * ai_benchmark_get_iterations:
 * @self: a benchmark
 *
 * Returns: measured runs per case per target
 */
guint
ai_benchmark_get_iterations(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), 0);
	return self->iterations;
}

/**
 * ai_benchmark_set_iterations:
 * @self: a benchmark
 * @iterations: measured runs per case per target, 1 to
 *   %AI_BENCHMARK_MAX_ITERATIONS
 */
void
ai_benchmark_set_iterations(
	AiBenchmark *self,
	guint        iterations
){
	g_return_if_fail(AI_IS_BENCHMARK(self));
	g_return_if_fail(iterations >= 1 && iterations <= AI_BENCHMARK_MAX_ITERATIONS);
	if (self->iterations == iterations)
		return;
	self->iterations = iterations;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ITERATIONS]);
}

/**
 * ai_benchmark_get_warmup:
 * @self: a benchmark
 *
 * Returns: unrecorded calls per target
 */
guint
ai_benchmark_get_warmup(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), 0);
	return self->warmup;
}

/**
 * ai_benchmark_set_warmup:
 * @self: a benchmark
 * @warmup: unrecorded calls per target before measuring starts, at most
 *   %AI_BENCHMARK_MAX_WARMUP
 */
void
ai_benchmark_set_warmup(
	AiBenchmark *self,
	guint        warmup
){
	g_return_if_fail(AI_IS_BENCHMARK(self));
	g_return_if_fail(warmup <= AI_BENCHMARK_MAX_WARMUP);
	if (self->warmup == warmup)
		return;
	self->warmup = warmup;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WARMUP]);
}

/**
 * ai_benchmark_get_timeout_ms:
 * @self: a benchmark
 *
 * Returns: the per-call deadline, 0 for none of the runner's own
 */
guint
ai_benchmark_get_timeout_ms(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), 0);
	return self->timeout_ms;
}

/**
 * ai_benchmark_set_timeout_ms:
 * @self: a benchmark
 * @timeout_ms: per-call deadline in milliseconds; 0 leaves it to the
 *   provider (decision clients default to 30000 of their own)
 */
void
ai_benchmark_set_timeout_ms(
	AiBenchmark *self,
	guint        timeout_ms
){
	g_return_if_fail(AI_IS_BENCHMARK(self));
	if (self->timeout_ms == timeout_ms)
		return;
	self->timeout_ms = timeout_ms;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TIMEOUT_MS]);
}

/**
 * ai_benchmark_get_config:
 * @self: a benchmark
 *
 * Returns: (transfer none) (nullable): the configuration targets are built with
 */
AiConfig *
ai_benchmark_get_config(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);
	return self->config;
}

/**
 * ai_benchmark_set_config:
 * @self: a benchmark
 * @config: (nullable): configuration for the provider factories; %NULL
 *   builds each target with a fresh default #AiConfig
 *
 * Invalidates any earlier ai_benchmark_prepare(). Not while running.
 */
void
ai_benchmark_set_config(
	AiBenchmark *self,
	AiConfig    *config
){
	g_return_if_fail(AI_IS_BENCHMARK(self));
	g_return_if_fail(!self->running);
	if (g_set_object(&self->config, config))
	{
		self->prepared = FALSE;
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CONFIG]);
	}
}

/**
 * ai_benchmark_add_target:
 * @self: a benchmark
 * @target: a target; its label must be unique here
 * @error: return location for an error
 *
 * Returns: %FALSE when the label is already taken, when @target wraps a
 *   client another target already wraps, or a run is in progress
 */
gboolean
ai_benchmark_add_target(
	AiBenchmark        *self,
	AiBenchmarkTarget  *target,
	GError            **error
){
	const gchar *label;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), FALSE);
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(target), FALSE);

	label = ai_benchmark_target_get_label(target);
	if (self->running)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "cannot add a target while the benchmark is running");
		return FALSE;
	}
	if (ai_benchmark_find_target(self, label) != NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "duplicate target label '%s'", label);
		return FALSE;
	}
	/* Build sets the model and properties on a wrapped client; sharing one
	 * would make two targets measure whichever was configured last. */
	if (ai_benchmark_target_get_object(target) != NULL)
	{
		guint i;

		for (i = 0; i < self->targets->len; i++)
		{
			AiBenchmarkTarget *other = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, i);

			if (ai_benchmark_target_get_object(other) == ai_benchmark_target_get_object(target))
			{
				g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				            "target '%s' wraps the same client as target '%s'; give each its own",
				            label, ai_benchmark_target_get_label(other));
				return FALSE;
			}
		}
	}
	g_ptr_array_add(self->targets, g_object_ref(target));
	self->prepared = FALSE;
	return TRUE;
}

/**
 * ai_benchmark_add_case:
 * @self: a benchmark
 * @bench_case: a case; its identifier must be unique here
 * @error: return location for an error
 *
 * Returns: %FALSE when the identifier is already taken or a run is in progress
 */
gboolean
ai_benchmark_add_case(
	AiBenchmark      *self,
	AiBenchmarkCase  *bench_case,
	GError          **error
){
	const gchar *id;
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), FALSE);
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(bench_case), FALSE);

	id = ai_benchmark_case_get_id(bench_case);
	if (self->running)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "cannot add a case while the benchmark is running");
		return FALSE;
	}
	for (i = 0; i < self->cases->len; i++)
	{
		if (g_str_equal(ai_benchmark_case_get_id((AiBenchmarkCase *)g_ptr_array_index(self->cases, i)), id))
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "duplicate case id '%s'", id);
			return FALSE;
		}
	}
	g_ptr_array_add(self->cases, g_object_ref(bench_case));
	return TRUE;
}

/**
 * ai_benchmark_get_targets:
 * @self: a benchmark
 *
 * Returns: (transfer none) (element-type AiBenchmarkTarget): the targets,
 *   in the order they run within each round
 */
GPtrArray *
ai_benchmark_get_targets(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);
	return self->targets;
}

/**
 * ai_benchmark_get_cases:
 * @self: a benchmark
 *
 * Returns: (transfer none) (element-type AiBenchmarkCase): the cases, in run order
 */
GPtrArray *
ai_benchmark_get_cases(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);
	return self->cases;
}

/**
 * ai_benchmark_find_target:
 * @self: a benchmark
 * @label: a target label
 *
 * Returns: (transfer none) (nullable): the target with that label
 */
AiBenchmarkTarget *
ai_benchmark_find_target(
	AiBenchmark *self,
	const gchar *label
){
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);
	g_return_val_if_fail(label != NULL, NULL);

	for (i = 0; i < self->targets->len; i++)
	{
		AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, i);

		if (g_str_equal(ai_benchmark_target_get_label(target), label))
			return target;
	}
	return NULL;
}

/**
 * ai_benchmark_select_targets:
 * @self: a benchmark
 * @labels: (array zero-terminated=1): labels to keep
 * @error: return location for an error
 *
 * Drops every target not named in @labels, keeping the original order.
 * Nothing is dropped when a label is unknown.
 *
 * Returns: %FALSE (naming the label and listing the real ones) when a
 *   label matches no target, or when a run is in progress
 */
gboolean
ai_benchmark_select_targets(
	AiBenchmark         *self,
	const gchar *const  *labels,
	GError             **error
){
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), FALSE);
	g_return_val_if_fail(labels != NULL, FALSE);

	/* The run's plan holds indexes into the target list. */
	if (self->running)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "cannot select targets while the benchmark is running");
		return FALSE;
	}

	/* Validate every label before changing anything. */
	for (i = 0; labels[i] != NULL; i++)
	{
		if (ai_benchmark_find_target(self, labels[i]) == NULL)
		{
			g_autoptr(GString) known = g_string_new(NULL);
			guint k;

			for (k = 0; k < self->targets->len; k++)
			{
				if (known->len > 0)
					g_string_append(known, ", ");
				g_string_append(known, ai_benchmark_target_get_label(
					(AiBenchmarkTarget *)g_ptr_array_index(self->targets, k)));
			}
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "no target labelled '%s' (targets: %s)", labels[i], known->str);
			return FALSE;
		}
	}

	for (i = self->targets->len; i > 0; i--)
	{
		AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, i - 1);

		if (!g_strv_contains(labels, ai_benchmark_target_get_label(target)))
			g_ptr_array_remove_index(self->targets, i - 1);
	}
	self->prepared = FALSE;
	return TRUE;
}

static gboolean
case_runs_on(
	AiBenchmarkCase   *bench_case,
	AiBenchmarkTarget *target
){
	return ai_benchmark_case_get_kind(bench_case) == ai_benchmark_target_get_kind(target);
}

/**
 * ai_benchmark_prepare:
 * @self: a benchmark
 * @error: return location for an error
 *
 * Builds every target's client and checks it can do its kind of work.
 * Makes no calls. A target that fails is skipped by the run and its
 * reason is kept for ai_benchmark_get_target_error(). ai_benchmark_run()
 * prepares by itself; calling this first is how a frontend validates a
 * suite (`ai benchmark --dry-run`).
 *
 * Returns: %TRUE when at least one target built and has a case of its
 *   kind; otherwise %FALSE with an error carrying the first reason.
 *   %FALSE while a run is in progress: the run is using the clients.
 */
gboolean
ai_benchmark_prepare(
	AiBenchmark  *self,
	GError      **error
){
	g_autofree gchar *first = NULL;
	gboolean runnable;
	guint i;
	guint k;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), FALSE);

	if (self->running)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "cannot prepare while the benchmark is running");
		return FALSE;
	}

	g_ptr_array_set_size(self->built, 0);
	g_hash_table_remove_all(self->build_errors);
	runnable = FALSE;

	for (i = 0; i < self->targets->len; i++)
	{
		AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, i);
		g_autoptr(GError) local = NULL;
		GObject *object;

		object = ai_benchmark_target_build(target, self->config, &local);
		g_ptr_array_add(self->built, object);
		if (object == NULL)
		{
			g_debug("benchmark: %s", local->message);
			g_hash_table_insert(self->build_errors,
			                    g_strdup(ai_benchmark_target_get_label(target)),
			                    g_strdup(local->message));
			if (first == NULL)
				first = g_strdup(local->message);
			continue;
		}
		for (k = 0; k < self->cases->len; k++)
			if (case_runs_on((AiBenchmarkCase *)g_ptr_array_index(self->cases, k), target))
				runnable = TRUE;
	}

	self->prepared = TRUE;
	if (runnable)
		return TRUE;

	if (first != NULL)
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "nothing to run: %s", first);
	else
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "nothing to run: no target has a case of its kind");
	return FALSE;
}

/**
 * ai_benchmark_get_target_error:
 * @self: a benchmark
 * @label: a target label
 *
 * Returns: (transfer none) (nullable): why the target could not be built
 *   by the last prepare or run, or %NULL when it built
 */
const gchar *
ai_benchmark_get_target_error(
	AiBenchmark *self,
	const gchar *label
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);
	g_return_val_if_fail(label != NULL, NULL);
	return (const gchar *)g_hash_table_lookup(self->build_errors, label);
}

/*
 * Lays out the whole run: per-target warmups on the target's first case,
 * then, per case and iteration, each target of that kind in turn.
 * Targets that did not build are left out.
 */
static GArray *
plan_jobs(
	AiBenchmark *self
){
	GArray *jobs;
	guint t;
	guint c;
	guint w;
	guint it;

	jobs = g_array_new(FALSE, FALSE, sizeof(Job));

	for (w = 0; w < self->warmup; w++)
	{
		for (t = 0; t < self->targets->len; t++)
		{
			AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, t);

			if (t >= self->built->len || g_ptr_array_index(self->built, t) == NULL)
				continue;
			for (c = 0; c < self->cases->len; c++)
			{
				if (case_runs_on((AiBenchmarkCase *)g_ptr_array_index(self->cases, c), target))
				{
					Job job = { t, c, w, TRUE };

					g_array_append_val(jobs, job);
					break;
				}
			}
		}
	}

	for (c = 0; c < self->cases->len; c++)
	{
		AiBenchmarkCase *bench_case = (AiBenchmarkCase *)g_ptr_array_index(self->cases, c);

		for (it = 0; it < self->iterations; it++)
		{
			for (t = 0; t < self->targets->len; t++)
			{
				if (t >= self->built->len || g_ptr_array_index(self->built, t) == NULL)
					continue;
				if (case_runs_on(bench_case, (AiBenchmarkTarget *)g_ptr_array_index(self->targets, t)))
				{
					Job job = { t, c, it, FALSE };

					g_array_append_val(jobs, job);
				}
			}
		}
	}
	return jobs;
}

/**
 * ai_benchmark_count_calls:
 * @self: a benchmark
 * @measured: (out) (optional): how many of those calls are recorded
 *
 * The run plan's size, from the last prepare. Before any prepare, every
 * target is assumed to build.
 *
 * Returns: total calls the run will make, warmups included
 */
guint
ai_benchmark_count_calls(
	AiBenchmark *self,
	guint       *measured
){
	g_autoptr(GArray) jobs = NULL;
	guint recorded;
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), 0);

	/*
	 * Unprepared: plan as if every target built. A running benchmark is
	 * always prepared, so this never touches the clients a run is using.
	 */
	if (!self->prepared && !self->running)
	{
		g_ptr_array_set_size(self->built, 0);
		for (i = 0; i < self->targets->len; i++)
			g_ptr_array_add(self->built, g_object_ref(g_ptr_array_index(self->targets, i)));
	}
	jobs = plan_jobs(self);
	if (!self->prepared && !self->running)
		g_ptr_array_set_size(self->built, 0);

	recorded = 0;
	for (i = 0; i < jobs->len; i++)
		if (!g_array_index(jobs, Job, i).warmup)
			recorded++;
	if (measured != NULL)
		*measured = recorded;
	return jobs->len;
}

/* ---- running ---------------------------------------------------- */

static void
run_state_clear_call(
	RunState *state
){
	if (state->timeout != NULL)
	{
		g_source_destroy(state->timeout);
		g_source_unref(state->timeout);
		state->timeout = NULL;
	}
	if (state->signal_object != NULL)
	{
		g_signal_handler_disconnect(state->signal_object, state->signal_id);
		g_clear_object(&state->signal_object);
		state->signal_id = 0;
	}
	if (state->call != NULL)
	{
		if (state->user != NULL && state->call_link != 0)
			g_cancellable_disconnect(state->user, state->call_link);
		state->call_link = 0;
		g_clear_object(&state->call);
	}
	g_list_free_full(state->messages, g_object_unref);
	state->messages = NULL;
	g_clear_pointer(&state->image_request, ai_image_request_free);
}

static void
run_state_free(
	gpointer data
){
	RunState *state;

	state = (RunState *)data;
	run_state_clear_call(state);
	g_array_unref(state->jobs);
	g_clear_object(&state->user);
	g_main_context_unref(state->context);
	g_free(state);
}

static gboolean
on_timeout(
	gpointer user_data
){
	RunState *state;

	state = (RunState *)user_data;
	state->timed_out = TRUE;
	/* This callback ends the source, so drop the held reference here. */
	g_source_unref(state->timeout);
	state->timeout = NULL;
	g_cancellable_cancel(state->call);
	return G_SOURCE_REMOVE;
}

static void
note_first_text(
	RunState *state
){
	if (state->first_text_us < 0)
		state->first_text_us = g_get_monotonic_time();
}

static void
on_event(
	GObject  *source,
	AiEvent  *event,
	gpointer  user_data
){
	(void)source;
	if (ai_event_get_kind(event) == AI_EVENT_TEXT_DELTA)
		note_first_text((RunState *)user_data);
}

static void
on_delta(
	GObject     *source,
	const gchar *text,
	gpointer     user_data
){
	(void)source;
	if (text != NULL && *text != '\0')
		note_first_text((RunState *)user_data);
}

/* What a user cancel needs to reach the call in flight. */
typedef struct
{
	GMainContext *context;
	GCancellable *call;
} CancelLink;

static void
cancel_link_free(
	gpointer data
){
	CancelLink *link;

	link = (CancelLink *)data;
	g_main_context_unref(link->context);
	g_object_unref(link->call);
	g_free(link);
}

static gboolean
cancel_call_now(
	gpointer call
){
	g_cancellable_cancel(G_CANCELLABLE(call));
	return G_SOURCE_REMOVE;
}

/*
 * The user's cancellable fired. Do not cancel the call inline: this may
 * be another thread (provider teardown would then run off the run's
 * context), and a provider that completes from its own cancelled handler
 * would reach run_state_clear_call(), which disconnects from the user's
 * cancellable inside its own emission -- a deadlock GLib documents.
 * Hop to the run's context instead.
 */
static void
cancel_call(
	GCancellable *user,
	gpointer      data
){
	CancelLink *link;
	GSource *idle;

	(void)user;
	link = (CancelLink *)data;
	idle = g_idle_source_new();
	g_source_set_callback(idle, cancel_call_now, g_object_ref(link->call), g_object_unref);
	g_source_attach(idle, link->context);
	g_source_unref(idle);
}

/*
 * Fills @sample from a finished call, by kind. Takes ownership of
 * nothing; each result is read and released here.
 */
static void
record_result(
	RunState          *state,
	Job               *job,
	GObject           *client,
	GAsyncResult      *result,
	AiBenchmarkSample *sample
){
	AiBenchmark *self = state->self;
	AiBenchmarkCase *bench_case = (AiBenchmarkCase *)g_ptr_array_index(self->cases, job->bench_case);
	AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, job->target);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *detail = NULL;
	AiBenchmarkOutcome outcome;
	gdouble brier;
	gdouble log_loss;

	brier = -1;
	log_loss = -1;
	outcome = AI_BENCHMARK_OUTCOME_UNSCORED;

	switch (ai_benchmark_target_get_kind(target))
	{
		case AI_BENCHMARK_KIND_CHAT:
		{
			g_autoptr(AiResponse) response = NULL;
			g_autofree gchar *text = NULL;
			AiUsage *usage;

			if (ai_benchmark_target_get_stream(target))
				response = ai_streamable_chat_stream_finish(AI_STREAMABLE(client), result, &error);
			else
				response = ai_provider_chat_finish(AI_PROVIDER(client), result, &error);
			if (response == NULL)
				break;
			usage = ai_response_get_usage(response);
			if (usage != NULL)
				ai_benchmark_sample_set_tokens(sample, ai_usage_get_input_tokens(usage),
				                               ai_usage_get_output_tokens(usage));
			ai_benchmark_sample_set_model(sample, ai_response_get_model(response));
			text = ai_response_get_text(response);
			outcome = ai_benchmark_case_score_text(bench_case, text, &detail);
			break;
		}
		case AI_BENCHMARK_KIND_DECISION:
		{
			g_autoptr(AiDecisionResponse) response = NULL;

			response = ai_decider_decide_finish(AI_DECIDER(client), result, &error);
			if (response == NULL)
				break;
			ai_benchmark_sample_set_tokens(sample, ai_decision_response_get_input_tokens(response),
			                               ai_decision_response_get_output_tokens(response));
			ai_benchmark_sample_set_model(sample, ai_decision_response_get_model(response));
			outcome = ai_benchmark_case_score_decision(bench_case, response, &brier, &log_loss, &detail);
			break;
		}
		case AI_BENCHMARK_KIND_EMBEDDING:
		{
			g_autoptr(AiEmbedding) embedding = NULL;

			embedding = ai_embedder_embed_finish(AI_EMBEDDER(client), result, &error);
			if (embedding == NULL)
				break;
			ai_benchmark_sample_set_model(sample, ai_embedding_get_model(embedding));
			outcome = ai_benchmark_case_score_embedding(bench_case, embedding, &detail);
			break;
		}
		case AI_BENCHMARK_KIND_IMAGE:
		{
			g_autoptr(AiImageResponse) response = NULL;

			response = ai_image_generator_generate_image_finish(AI_IMAGE_GENERATOR(client), result, &error);
			if (response == NULL)
				break;
			if (ai_image_response_get_image_count(response) == 0)
			{
				outcome = AI_BENCHMARK_OUTCOME_ERROR;
				g_set_error_literal(&error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
				                    "the response contained no image");
			}
			break;
		}
		default:
			break;
	}

	/* A timeout surfaces as the provider's cancellation; say what it was. */
	if (error != NULL)
	{
		g_autofree gchar *message = NULL;

		if (state->timed_out)
			message = g_strdup_printf("timed out after %u ms", self->timeout_ms);
		else
			message = g_strdup(error->message);
		g_debug("benchmark: %s/%s: %s", ai_benchmark_target_get_label(target),
		        ai_benchmark_case_get_id(bench_case), message);
		ai_benchmark_sample_set_error(sample, message);
		return;
	}

	ai_benchmark_sample_set_outcome(sample, outcome, brier, log_loss, detail);
	if (state->first_text_us >= 0)
		ai_benchmark_sample_set_ttft_us(sample, state->first_text_us - state->started_us);
}

static void
on_call_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GTask *task;
	RunState *state;
	AiBenchmark *self;
	Job *job;
	gint64 elapsed;
	g_autoptr(AiBenchmarkSample) sample = NULL;

	task = G_TASK(user_data);
	state = (RunState *)g_task_get_task_data(task);
	self = state->self;
	job = &g_array_index(state->jobs, Job, state->next);
	elapsed = g_get_monotonic_time() - state->started_us;

	/* The deadline stops mattering the moment the callback runs. */
	if (state->timeout != NULL)
	{
		g_source_destroy(state->timeout);
		g_source_unref(state->timeout);
		state->timeout = NULL;
	}

	/*
	 * A user cancel interrupts the run: the call in flight was cut short
	 * by us, so its sample would be an artefact. Finish the result so the
	 * provider's task is consumed, then stop.
	 */
	sample = ai_benchmark_sample_new(
		ai_benchmark_target_get_label((AiBenchmarkTarget *)g_ptr_array_index(self->targets, job->target)),
		ai_benchmark_case_get_id((AiBenchmarkCase *)g_ptr_array_index(self->cases, job->bench_case)),
		job->iteration, elapsed);
	record_result(state, job, source, result, sample);
	run_state_clear_call(state);

	if (state->user != NULL && g_cancellable_is_cancelled(state->user))
	{
		self->running = FALSE;
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
		                        "benchmark cancelled after %u samples", self->samples->len);
		g_object_unref(task);
		return;
	}

	if (!job->warmup)
	{
		g_ptr_array_add(self->samples, g_object_ref(sample));
		g_signal_emit(self, signals[SIGNAL_SAMPLE], 0, sample);
	}

	state->next++;
	run_next(task);
}

/*
 * Starts the job at state->next, or completes the task when there are
 * none left. Runs from an idle so a provider that completes inside its
 * own call cannot recurse through every job on one stack.
 */
static gboolean
start_job(
	gpointer user_data
){
	GTask *task;
	RunState *state;
	AiBenchmark *self;
	Job *job;
	AiBenchmarkTarget *target;
	AiBenchmarkCase *bench_case;
	GObject *client;

	task = G_TASK(user_data);
	state = (RunState *)g_task_get_task_data(task);
	self = state->self;

	if (state->user != NULL && g_cancellable_is_cancelled(state->user))
	{
		self->running = FALSE;
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
		                        "benchmark cancelled after %u samples", self->samples->len);
		g_object_unref(task);
		return G_SOURCE_REMOVE;
	}
	if (state->next >= state->jobs->len)
	{
		self->running = FALSE;
		g_task_return_boolean(task, TRUE);
		g_object_unref(task);
		return G_SOURCE_REMOVE;
	}

	job = &g_array_index(state->jobs, Job, state->next);
	target = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, job->target);
	bench_case = (AiBenchmarkCase *)g_ptr_array_index(self->cases, job->bench_case);
	client = (GObject *)g_ptr_array_index(self->built, job->target);

	/* A cancellable per call, linked to the caller's and to the deadline. */
	state->call = g_cancellable_new();
	state->timed_out = FALSE;
	state->first_text_us = -1;
	if (state->user != NULL)
	{
		CancelLink *link = g_new0(CancelLink, 1);

		link->context = g_main_context_ref(state->context);
		link->call = (GCancellable *)g_object_ref(state->call);
		state->call_link = g_cancellable_connect(state->user, G_CALLBACK(cancel_call),
		                                         link, cancel_link_free);
	}
	if (self->timeout_ms > 0)
	{
		state->timeout = g_timeout_source_new(self->timeout_ms);
		g_source_set_callback(state->timeout, on_timeout, state, NULL);
		g_source_attach(state->timeout, state->context);
	}

	/* Dispatch by kind; the clock starts immediately before the call. */
	switch (ai_benchmark_target_get_kind(target))
	{
		case AI_BENCHMARK_KIND_CHAT:
			state->messages = g_list_append(NULL, ai_message_new_user(ai_benchmark_case_get_prompt(bench_case)));
			if (ai_benchmark_target_get_stream(target))
			{
				state->signal_object = (GObject *)g_object_ref(client);
				if (AI_IS_EVENT_SOURCE(client))
					state->signal_id = g_signal_connect(client, "event", G_CALLBACK(on_event), state);
				else
					state->signal_id = g_signal_connect(client, "delta", G_CALLBACK(on_delta), state);
				state->started_us = g_get_monotonic_time();
				ai_streamable_chat_stream_async(AI_STREAMABLE(client), state->messages,
				                                ai_benchmark_case_get_system(bench_case),
				                                ai_benchmark_case_get_max_tokens(bench_case), NULL,
				                                state->call, on_call_done, task);
			}
			else
			{
				state->started_us = g_get_monotonic_time();
				ai_provider_chat_async(AI_PROVIDER(client), state->messages,
				                       ai_benchmark_case_get_system(bench_case),
				                       ai_benchmark_case_get_max_tokens(bench_case), NULL,
				                       state->call, on_call_done, task);
			}
			break;
		case AI_BENCHMARK_KIND_DECISION:
			state->started_us = g_get_monotonic_time();
			ai_decider_decide_async(AI_DECIDER(client), ai_benchmark_case_get_request(bench_case),
			                        state->call, on_call_done, task);
			break;
		case AI_BENCHMARK_KIND_EMBEDDING:
			state->started_us = g_get_monotonic_time();
			ai_embedder_embed_async(AI_EMBEDDER(client), ai_benchmark_case_get_texts(bench_case),
			                        ai_benchmark_target_get_model(target), state->call, on_call_done, task);
			break;
		case AI_BENCHMARK_KIND_IMAGE:
			state->image_request = ai_image_request_new(ai_benchmark_case_get_prompt(bench_case));
			if (ai_benchmark_target_get_model(target) != NULL)
				ai_image_request_set_model(state->image_request, ai_benchmark_target_get_model(target));
			ai_image_request_set_size(state->image_request, ai_benchmark_case_get_image_size(bench_case));
			ai_image_request_set_count(state->image_request, ai_benchmark_case_get_image_count(bench_case));
			state->started_us = g_get_monotonic_time();
			ai_image_generator_generate_image_async(AI_IMAGE_GENERATOR(client), state->image_request,
			                                        state->call, on_call_done, task);
			break;
		default:
			g_assert_not_reached();
	}
	return G_SOURCE_REMOVE;
}

static void
run_next(
	GTask *task
){
	RunState *state;
	GSource *idle;

	state = (RunState *)g_task_get_task_data(task);
	idle = g_idle_source_new();
	g_source_set_callback(idle, start_job, task, NULL);
	g_source_attach(idle, state->context);
	g_source_unref(idle);
}

/**
 * ai_benchmark_run_async:
 * @self: a benchmark
 * @cancellable: (nullable): stops the run; samples taken so far are kept
 * @callback: (scope async) (closure user_data): called when the run ends
 * @user_data: (nullable): data for @callback
 *
 * Builds every target, then runs the plan one call at a time on the
 * thread-default main context of the caller. Earlier samples are
 * discarded. #AiBenchmark::sample fires after each measured call.
 */
void
ai_benchmark_run_async(
	AiBenchmark         *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	RunState *state;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(AI_IS_BENCHMARK(self));

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, ai_benchmark_run_async);

	if (self->running)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "the benchmark is already running");
		g_object_unref(task);
		return;
	}
	if (!ai_benchmark_prepare(self, &error))
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		g_object_unref(task);
		return;
	}

	g_ptr_array_set_size(self->samples, 0);
	self->running = TRUE;

	state = g_new0(RunState, 1);
	state->self = self;
	state->context = g_main_context_ref_thread_default();
	state->jobs = plan_jobs(self);
	state->user = cancellable != NULL ? (GCancellable *)g_object_ref(cancellable) : NULL;
	state->first_text_us = -1;
	g_task_set_task_data(task, state, run_state_free);

	/* The task's own reference is dropped when it completes. */
	run_next(task);
}

/**
 * ai_benchmark_run_finish:
 * @self: a benchmark
 * @result: the result passed to the callback
 * @error: return location for an error
 *
 * Returns: %TRUE when every planned call ran; %FALSE when nothing could
 *   run or the run was cancelled (the samples taken are still available)
 */
gboolean
ai_benchmark_run_finish(
	AiBenchmark   *self,
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
	return g_task_propagate_boolean(G_TASK(result), error);
}

typedef struct
{
	GMainLoop *loop;
	gboolean   ok;
	GError    *error;
} SyncRun;

static void
on_sync_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	SyncRun *sync;

	sync = (SyncRun *)user_data;
	sync->ok = ai_benchmark_run_finish(AI_BENCHMARK(source), result, &sync->error);
	g_main_loop_quit(sync->loop);
}

/**
 * ai_benchmark_run:
 * @self: a benchmark
 * @cancellable: (nullable): stops the run
 * @error: return location for an error
 *
 * Blocks on a private main context. Every source a provider uses must
 * attach to the thread-default context, as ai-glib's own do; a test
 * server on the caller's normal context must run on another thread.
 *
 * Returns: as ai_benchmark_run_finish()
 */
gboolean
ai_benchmark_run(
	AiBenchmark   *self,
	GCancellable  *cancellable,
	GError       **error
){
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	SyncRun sync;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), FALSE);

	context = g_main_context_new();
	loop = g_main_loop_new(context, FALSE);
	sync.loop = loop;
	sync.ok = FALSE;
	sync.error = NULL;

	g_main_context_push_thread_default(context);
	ai_benchmark_run_async(self, cancellable, on_sync_done, &sync);
	g_main_loop_run(loop);
	g_main_context_pop_thread_default(context);

	if (sync.error != NULL)
		g_propagate_error(error, sync.error);
	return sync.ok;
}

/**
 * ai_benchmark_is_running:
 * @self: a benchmark
 *
 * Returns: whether a run is in progress
 */
gboolean
ai_benchmark_is_running(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), FALSE);
	return self->running;
}

/**
 * ai_benchmark_get_samples:
 * @self: a benchmark
 *
 * Returns: (transfer none) (element-type AiBenchmarkSample): the measured
 *   runs of the last run, in the order they ran
 */
GPtrArray *
ai_benchmark_get_samples(
	AiBenchmark *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);
	return self->samples;
}

/**
 * ai_benchmark_dup_summaries:
 * @self: a benchmark
 * @per_case: %TRUE for one summary per (target, case), %FALSE for one per target
 *
 * Targets come in their configured order and, with @per_case, cases in
 * theirs. A target that could not be built still gets a summary, with
 * zero runs, so it does not silently vanish from a report. Per case, only
 * the cases a target's kind runs are included.
 *
 * A #GList rather than a #GPtrArray: a GPtrArray handed over with its own
 * element free function is released twice by PyGObject, and this is the
 * call every binding makes. In C, free it with g_list_free_full() and
 * g_object_unref(), or declare it g_autolist(AiBenchmarkSummary).
 *
 * Returns: (transfer full) (element-type AiBenchmarkSummary): the summaries
 */
GList *
ai_benchmark_dup_summaries(
	AiBenchmark *self,
	gboolean     per_case
){
	GList *out;
	guint t;
	guint c;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);

	out = NULL;
	for (t = 0; t < self->targets->len; t++)
	{
		AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(self->targets, t);
		const gchar *label = ai_benchmark_target_get_label(target);

		if (!per_case)
		{
			out = g_list_prepend(out, ai_benchmark_summary_new(label, NULL, self->samples));
			continue;
		}
		for (c = 0; c < self->cases->len; c++)
		{
			AiBenchmarkCase *bench_case = (AiBenchmarkCase *)g_ptr_array_index(self->cases, c);

			if (case_runs_on(bench_case, target))
				out = g_list_prepend(out, ai_benchmark_summary_new(label,
					ai_benchmark_case_get_id(bench_case), self->samples));
		}
	}
	return g_list_reverse(out);
}
