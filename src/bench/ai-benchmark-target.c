/*
 * ai-benchmark-target.c - One provider + model + settings to measure
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A target is a recipe, not an object: it names a provider and a model
 * and carries `--set`-style property assignments, and build() turns that
 * into the client the runner calls. Keeping it a recipe is what lets a
 * suite file be validated (--dry-run) without anything being called, and
 * lets one AiBenchmark be run twice against fresh clients.
 *
 * build() refuses a target whose object cannot do the work its kind
 * names -- `claude` asked to embed, a non-streaming client asked to
 * stream -- before a single call is made, rather than twenty runs in.
 *
 * The provider name is resolved by the same two factories `ai` and
 * `ai decide` use, so `jev` means TypeSafe here exactly as it does there,
 * and the property grammar is ai-property-util.h, the same parser as
 * `ai --set`.
 */

#include "bench/ai-benchmark-target.h"

#include "convenience/ai-provider-factory.h"
#include "core/ai-cli-client.h"
#include "core/ai-client.h"
#include "core/ai-decider.h"
#include "core/ai-embedder.h"
#include "core/ai-error.h"
#include "core/ai-image-generator.h"
#include "core/ai-property-util.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"

struct _AiBenchmarkTarget
{
	GObject          parent_instance;
	gchar           *label;
	AiBenchmarkKind  kind;
	gchar           *provider;
	gchar           *model;
	gboolean         stream;
	GPtrArray       *properties;  /* NULL-terminated array of gchar * */
	GObject         *object;      /* prebuilt, or NULL */
};

G_DEFINE_TYPE(AiBenchmarkTarget, ai_benchmark_target, G_TYPE_OBJECT)

static void
ai_benchmark_target_finalize(
	GObject *object
){
	AiBenchmarkTarget *self;

	self = AI_BENCHMARK_TARGET(object);
	g_free(self->label);
	g_free(self->provider);
	g_free(self->model);
	g_ptr_array_unref(self->properties);
	g_clear_object(&self->object);
	G_OBJECT_CLASS(ai_benchmark_target_parent_class)->finalize(object);
}

static void
ai_benchmark_target_class_init(
	AiBenchmarkTargetClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = ai_benchmark_target_finalize;
}

static void
ai_benchmark_target_init(
	AiBenchmarkTarget *self
){
	self->properties = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(self->properties, NULL);
}

/**
 * ai_benchmark_target_new:
 * @label: the name this target has in every output; unique within a benchmark
 * @kind: what the target is measured doing
 * @provider: a provider name: anything `ai -p` accepts for chat, embedding
 *   and image; `typesafe`, `jev`, `ollama` or `laya` for decisions
 * @model: (nullable): the model id, or %NULL for the provider's default
 *
 * Creates a recipe. Nothing is constructed or validated against the
 * provider until ai_benchmark_target_build().
 *
 * Returns: (transfer full): a new target
 */
AiBenchmarkTarget *
ai_benchmark_target_new(
	const gchar     *label,
	AiBenchmarkKind  kind,
	const gchar     *provider,
	const gchar     *model
){
	g_autoptr(AiBenchmarkTarget) self = NULL;

	g_return_val_if_fail(label != NULL && *label != '\0', NULL);
	g_return_val_if_fail(provider != NULL, NULL);

	self = (AiBenchmarkTarget *)g_object_new(AI_TYPE_BENCHMARK_TARGET, NULL);
	self->label = g_strdup(label);
	self->kind = kind;
	self->provider = g_strdup(provider);
	self->model = model != NULL && *model != '\0' ? g_strdup(model) : NULL;
	return (AiBenchmarkTarget *)g_steal_pointer(&self);
}

/**
 * ai_benchmark_target_new_for_object:
 * @label: the name this target has in every output
 * @kind: what the target is measured doing
 * @object: an already-configured client (any #AiProvider, #AiDecider,
 *   #AiEmbedder or #AiImageGenerator, matching @kind)
 *
 * Wraps a client the caller built, for a provider the factories do not
 * know or a configuration they cannot express. The same object is used
 * for every run. Property assignments still apply to it at build time,
 * which is why one object cannot back two targets in one benchmark: the
 * second target's model and properties would overwrite the first's.
 *
 * Returns: (transfer full): a new target
 */
AiBenchmarkTarget *
ai_benchmark_target_new_for_object(
	const gchar     *label,
	AiBenchmarkKind  kind,
	GObject         *object
){
	g_autoptr(AiBenchmarkTarget) self = NULL;

	g_return_val_if_fail(label != NULL && *label != '\0', NULL);
	g_return_val_if_fail(G_IS_OBJECT(object), NULL);

	self = (AiBenchmarkTarget *)g_object_new(AI_TYPE_BENCHMARK_TARGET, NULL);
	self->label = g_strdup(label);
	self->kind = kind;
	self->provider = g_strdup(G_OBJECT_TYPE_NAME(object));
	self->object = (GObject *)g_object_ref(object);
	return (AiBenchmarkTarget *)g_steal_pointer(&self);
}

/**
 * ai_benchmark_target_get_label:
 * @self: a target
 *
 * Returns: (transfer none): the label
 */
const gchar *
ai_benchmark_target_get_label(
	AiBenchmarkTarget *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), NULL);
	return self->label;
}

/**
 * ai_benchmark_target_get_kind:
 * @self: a target
 *
 * Returns: the kind
 */
AiBenchmarkKind
ai_benchmark_target_get_kind(
	AiBenchmarkTarget *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), AI_BENCHMARK_KIND_CHAT);
	return self->kind;
}

/**
 * ai_benchmark_target_get_provider:
 * @self: a target
 *
 * Returns: (transfer none): the provider name, or the wrapped object's
 *   type name for ai_benchmark_target_new_for_object()
 */
const gchar *
ai_benchmark_target_get_provider(
	AiBenchmarkTarget *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), NULL);
	return self->provider;
}

/**
 * ai_benchmark_target_get_model:
 * @self: a target
 *
 * Returns: (transfer none) (nullable): the requested model, or %NULL for
 *   the provider default
 */
const gchar *
ai_benchmark_target_get_model(
	AiBenchmarkTarget *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), NULL);
	return self->model;
}

/**
 * ai_benchmark_target_get_object:
 * @self: a target
 *
 * Returns: (transfer none) (nullable): the client wrapped by
 *   ai_benchmark_target_new_for_object(), or %NULL for a target the
 *   factories build
 */
GObject *
ai_benchmark_target_get_object(
	AiBenchmarkTarget *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), NULL);
	return self->object;
}

/**
 * ai_benchmark_target_get_stream:
 * @self: a target
 *
 * Returns: whether a chat target streams (and so reports time to first token)
 */
gboolean
ai_benchmark_target_get_stream(
	AiBenchmarkTarget *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), FALSE);
	return self->stream;
}

/**
 * ai_benchmark_target_set_stream:
 * @self: a target
 * @stream: stream chat turns through #AiStreamable
 *
 * Only meaningful for %AI_BENCHMARK_KIND_CHAT. A streamed run records the
 * time to the first text delta as well as the whole call.
 */
void
ai_benchmark_target_set_stream(
	AiBenchmarkTarget *self,
	gboolean           stream
){
	g_return_if_fail(AI_IS_BENCHMARK_TARGET(self));
	self->stream = stream != FALSE;
}

/**
 * ai_benchmark_target_add_property:
 * @self: a target
 * @assignment: `NAME=VALUE`, or a bare `NAME` meaning `NAME=true`; the
 *   grammar of `ai --set`
 * @error: return location for an error
 *
 * Queues a property assignment applied to the client at build time.
 * Only the shape is checked here; whether the client has that property
 * is checked by ai_benchmark_target_build().
 *
 * Returns: %TRUE when @assignment has a non-empty name
 */
gboolean
ai_benchmark_target_add_property(
	AiBenchmarkTarget  *self,
	const gchar        *assignment,
	GError            **error
){
	g_autofree gchar *name = NULL;
	const gchar *eq;

	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), FALSE);
	g_return_val_if_fail(assignment != NULL, FALSE);

	eq = strchr(assignment, '=');
	name = eq != NULL ? g_strndup(assignment, (gsize)(eq - assignment)) : g_strdup(assignment);
	g_strstrip(name);
	if (*name == '\0')
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "target '%s': property assignment '%s' has no name",
		            self->label, assignment);
		return FALSE;
	}

	/* Keep the array NULL-terminated: replace the terminator, re-add it. */
	g_ptr_array_index(self->properties, self->properties->len - 1) = g_strdup(assignment);
	g_ptr_array_add(self->properties, NULL);
	return TRUE;
}

/**
 * ai_benchmark_target_get_properties:
 * @self: a target
 *
 * Returns: (transfer none) (array zero-terminated=1): the queued assignments
 */
const gchar * const *
ai_benchmark_target_get_properties(
	AiBenchmarkTarget *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), NULL);
	return (const gchar * const *)self->properties->pdata;
}

/**
 * ai_benchmark_target_has_property:
 * @self: a target
 * @name: a property name
 *
 * Returns: whether an assignment for @name is queued
 */
gboolean
ai_benchmark_target_has_property(
	AiBenchmarkTarget *self,
	const gchar       *name
){
	guint i;
	gsize length;

	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	length = strlen(name);
	for (i = 0; i + 1 < self->properties->len; i++)
	{
		const gchar *assignment = (const gchar *)g_ptr_array_index(self->properties, i);

		if (strncmp(assignment, name, length) == 0 &&
		    (assignment[length] == '=' || assignment[length] == '\0'))
			return TRUE;
	}
	return FALSE;
}

/*
 * The interface a kind needs, and its name for the error message.
 */
static gboolean
object_can(
	GObject          *object,
	AiBenchmarkKind   kind,
	gboolean          stream,
	const gchar     **missing
){
	switch (kind)
	{
		case AI_BENCHMARK_KIND_CHAT:
			if (!AI_IS_PROVIDER(object))
			{
				*missing = "AiProvider";
				return FALSE;
			}
			if (stream && !AI_IS_STREAMABLE(object))
			{
				*missing = "AiStreamable";
				return FALSE;
			}
			return TRUE;
		case AI_BENCHMARK_KIND_DECISION:
			*missing = "AiDecider";
			return AI_IS_DECIDER(object);
		case AI_BENCHMARK_KIND_EMBEDDING:
			*missing = "AiEmbedder";
			return AI_IS_EMBEDDER(object);
		case AI_BENCHMARK_KIND_IMAGE:
			*missing = "AiImageGenerator";
			return AI_IS_IMAGE_GENERATOR(object);
		default:
			*missing = "a known benchmark kind";
			return FALSE;
	}
}

/*
 * Where the model goes depends on the kind. Chat sets it on the client.
 * Decisions set the `model` property, except Ollama, whose chat model and
 * decision model are different properties and must not overwrite each
 * other. Embedding and image take the model per call, so nothing is set.
 */
static void
apply_model(
	GObject         *object,
	AiBenchmarkKind  kind,
	const gchar     *model
){
	GObjectClass *klass;

	if (model == NULL)
		return;

	klass = G_OBJECT_GET_CLASS(object);
	switch (kind)
	{
		case AI_BENCHMARK_KIND_CHAT:
			if (AI_IS_CLIENT(object))
				ai_client_set_model(AI_CLIENT(object), model);
			else if (AI_IS_CLI_CLIENT(object))
				ai_cli_client_set_model(AI_CLI_CLIENT(object), model);
			else if (g_object_class_find_property(klass, "model") != NULL)
				g_object_set(object, "model", model, NULL);
			break;
		case AI_BENCHMARK_KIND_DECISION:
			if (g_object_class_find_property(klass, "decision-model") != NULL)
				g_object_set(object, "decision-model", model, NULL);
			else if (g_object_class_find_property(klass, "model") != NULL)
				g_object_set(object, "model", model, NULL);
			break;
		case AI_BENCHMARK_KIND_EMBEDDING:
		case AI_BENCHMARK_KIND_IMAGE:
		default:
			break;
	}
}

/**
 * ai_benchmark_target_build:
 * @self: a target
 * @config: (nullable): configuration handed to the provider factory
 * @error: return location for an error
 *
 * Constructs the client (or takes a reference on the wrapped one),
 * checks it implements what the kind needs, sets the model and applies
 * every queued property assignment. Errors name the target.
 *
 * Returns: (transfer full) (nullable): the client, or %NULL on error
 */
GObject *
ai_benchmark_target_build(
	AiBenchmarkTarget  *self,
	AiConfig           *config,
	GError            **error
){
	g_autoptr(GObject) object = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *missing;
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK_TARGET(self), NULL);

	/* 1. Construct, through the same factories as `ai` and `ai decide`. */
	if (self->object != NULL)
		object = (GObject *)g_object_ref(self->object);
	else if (self->kind == AI_BENCHMARK_KIND_DECISION)
		object = ai_provider_factory_new_decider(self->provider, config, &local);
	else
		object = ai_provider_factory_new_from_string(self->provider, config, &local);

	if (object == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "target '%s': %s",
		            self->label, local != NULL ? local->message : "could not build provider");
		return NULL;
	}

	/* 2. Refuse a client that cannot do this kind of work at all. */
	missing = NULL;
	if (!object_can(object, self->kind, self->stream, &missing))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "target '%s': provider '%s' cannot run %s%s (it does not implement %s)",
		            self->label, self->provider,
		            self->stream ? "streamed " : "",
		            ai_benchmark_kind_to_string(self->kind), missing);
		return NULL;
	}

	/* 3. Model, then the explicit assignments, which may override it. */
	apply_model(object, self->kind, self->model);
	for (i = 0; i + 1 < self->properties->len; i++)
	{
		const gchar *assignment = (const gchar *)g_ptr_array_index(self->properties, i);

		if (!ai_property_set_from_assignment(object, assignment, &local))
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "target '%s': %s",
			            self->label, local->message);
			return NULL;
		}
	}

	return (GObject *)g_steal_pointer(&object);
}
