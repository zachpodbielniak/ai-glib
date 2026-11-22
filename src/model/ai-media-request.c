/*
 * ai-media-request.c - Parameters for an asynchronous media generation job
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Media services that queue a job (Higgsfield is the first) agree on very
 * little beyond "a model, usually a prompt, and a bag of model-specific
 * fields".  The fields differ per model -- one video model takes
 * `image_url`, another `image_urls`, a third `duration` as an enum -- so
 * this type does not try to be a superset the way AiImageRequest is.  It
 * carries the model, the prompt and an open JSON object of parameters, and
 * the service rejects what its model does not accept with an error that
 * names the field.
 */

#include "config.h"

#include "model/ai-media-request.h"

struct _AiMediaRequest
{
	GObject parent_instance;

	gchar      *model;
	gchar      *prompt;
	JsonObject *params;
	gchar      *idempotency_key;
	gchar      *webhook_url;
	guint       expires_after;
};

G_DEFINE_FINAL_TYPE(AiMediaRequest, ai_media_request, G_TYPE_OBJECT)

enum
{
	PROP_0,
	PROP_MODEL,
	PROP_PROMPT,
	PROP_IDEMPOTENCY_KEY,
	PROP_WEBHOOK_URL,
	PROP_EXPIRES_AFTER,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_media_request_finalize(GObject *object)
{
	AiMediaRequest *self = AI_MEDIA_REQUEST(object);

	g_clear_pointer(&self->model, g_free);
	g_clear_pointer(&self->prompt, g_free);
	g_clear_pointer(&self->params, json_object_unref);
	g_clear_pointer(&self->idempotency_key, g_free);
	g_clear_pointer(&self->webhook_url, g_free);

	G_OBJECT_CLASS(ai_media_request_parent_class)->finalize(object);
}

static void
ai_media_request_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiMediaRequest *self = AI_MEDIA_REQUEST(object);

	switch (prop_id)
	{
		case PROP_MODEL:
			g_value_set_string(value, self->model);
			break;
		case PROP_PROMPT:
			g_value_set_string(value, self->prompt);
			break;
		case PROP_IDEMPOTENCY_KEY:
			g_value_set_string(value, self->idempotency_key);
			break;
		case PROP_WEBHOOK_URL:
			g_value_set_string(value, self->webhook_url);
			break;
		case PROP_EXPIRES_AFTER:
			g_value_set_uint(value, self->expires_after);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
ai_media_request_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiMediaRequest *self = AI_MEDIA_REQUEST(object);

	switch (prop_id)
	{
		case PROP_MODEL:
			ai_media_request_set_model(self, g_value_get_string(value));
			break;
		case PROP_PROMPT:
			ai_media_request_set_prompt(self, g_value_get_string(value));
			break;
		case PROP_IDEMPOTENCY_KEY:
			ai_media_request_set_idempotency_key(self, g_value_get_string(value));
			break;
		case PROP_WEBHOOK_URL:
			ai_media_request_set_webhook_url(self, g_value_get_string(value));
			break;
		case PROP_EXPIRES_AFTER:
			ai_media_request_set_expires_after(self, g_value_get_uint(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
ai_media_request_class_init(AiMediaRequestClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize     = ai_media_request_finalize;
	object_class->get_property = ai_media_request_get_property;
	object_class->set_property = ai_media_request_set_property;

	/**
	 * AiMediaRequest:model:
	 *
	 * The model to run.  What a model id looks like is the service's
	 * business: for Higgsfield it is the endpoint path, such as
	 * `higgsfield-ai/soul/v2/standard`.
	 */
	properties[PROP_MODEL] =
		g_param_spec_string("model", NULL, NULL, NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiMediaRequest:prompt:
	 *
	 * The text prompt, sent as the `prompt` field.  %NULL sends none, for
	 * the models (motion transfer, upscaling) that take only media.
	 */
	properties[PROP_PROMPT] =
		g_param_spec_string("prompt", NULL, NULL, NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiMediaRequest:idempotency-key:
	 *
	 * The key a service uses to recognise a resubmission of the same job.
	 * %NULL until set or until ai_media_request_ensure_idempotency_key()
	 * generates one, after which resubmitting this request after a timeout
	 * returns the original job instead of paying for a second.
	 */
	properties[PROP_IDEMPOTENCY_KEY] =
		g_param_spec_string("idempotency-key", NULL, NULL, NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiMediaRequest:webhook-url:
	 *
	 * An HTTPS endpoint the service calls when the job finishes, or %NULL
	 * to rely on polling.
	 */
	properties[PROP_WEBHOOK_URL] =
		g_param_spec_string("webhook-url", NULL, NULL, NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiMediaRequest:expires-after:
	 *
	 * Seconds the service should keep the job's data after it finishes; 0
	 * keeps the account default.  Services may only allow shortening it.
	 */
	properties[PROP_EXPIRES_AFTER] =
		g_param_spec_uint("expires-after", NULL, NULL, 0, G_MAXUINT, 0,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                  G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_media_request_init(AiMediaRequest *self)
{
	self->params = json_object_new();
}

/**
 * ai_media_request_new:
 * @model: (nullable): the model to run
 * @prompt: (nullable): the text prompt
 *
 * Creates a media generation request.
 *
 * Returns: (transfer full): a new #AiMediaRequest
 */
AiMediaRequest *
ai_media_request_new(
	const gchar *model,
	const gchar *prompt
){
	g_autoptr(AiMediaRequest) self = NULL;

	self = g_object_new(AI_TYPE_MEDIA_REQUEST,
	                    "model", model,
	                    "prompt", prompt,
	                    NULL);

	return (AiMediaRequest *)g_steal_pointer(&self);
}

/**
 * ai_media_request_get_model:
 * @self: an #AiMediaRequest
 *
 * Returns: (transfer none) (nullable): the model id
 */
const gchar *
ai_media_request_get_model(AiMediaRequest *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), NULL);

	return self->model;
}

/**
 * ai_media_request_set_model:
 * @self: an #AiMediaRequest
 * @model: (nullable): the model id
 *
 * Sets the model to run.
 */
void
ai_media_request_set_model(
	AiMediaRequest *self,
	const gchar    *model
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));

	if (g_set_str(&self->model, model))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MODEL]);
	}
}

/**
 * ai_media_request_get_prompt:
 * @self: an #AiMediaRequest
 *
 * Returns: (transfer none) (nullable): the prompt
 */
const gchar *
ai_media_request_get_prompt(AiMediaRequest *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), NULL);

	return self->prompt;
}

/**
 * ai_media_request_set_prompt:
 * @self: an #AiMediaRequest
 * @prompt: (nullable): the prompt
 *
 * Sets the prompt, sent as the `prompt` field.  It takes precedence over a
 * `prompt` set with ai_media_request_set_param_string().
 */
void
ai_media_request_set_prompt(
	AiMediaRequest *self,
	const gchar    *prompt
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));

	if (g_set_str(&self->prompt, prompt))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PROMPT]);
	}
}

/**
 * ai_media_request_set_image_url:
 * @self: an #AiMediaRequest
 * @url: (nullable): a public URL of the input image, or %NULL to remove it
 *
 * Sets the `image_url` parameter, which is what image-to-video and
 * image-editing models most often call their input.  A model that names
 * it differently (`image_urls`, `start_image_url`) takes the same URL
 * through ai_media_request_set_param_json() or _set_param_string().
 */
void
ai_media_request_set_image_url(
	AiMediaRequest *self,
	const gchar    *url
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));

	if (url == NULL)
	{
		ai_media_request_remove_param(self, "image_url");
		return;
	}
	ai_media_request_set_param_string(self, "image_url", url);
}

/**
 * ai_media_request_set_param_string:
 * @self: an #AiMediaRequest
 * @name: the parameter name
 * @value: the value
 *
 * Sets a string parameter, replacing any earlier value of @name.
 */
void
ai_media_request_set_param_string(
	AiMediaRequest *self,
	const gchar    *name,
	const gchar    *value
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));
	g_return_if_fail(name != NULL && name[0] != '\0');
	g_return_if_fail(value != NULL);

	json_object_set_string_member(self->params, name, value);
}

/**
 * ai_media_request_set_param_int:
 * @self: an #AiMediaRequest
 * @name: the parameter name
 * @value: the value
 *
 * Sets an integer parameter, replacing any earlier value of @name.
 */
void
ai_media_request_set_param_int(
	AiMediaRequest *self,
	const gchar    *name,
	gint64          value
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));
	g_return_if_fail(name != NULL && name[0] != '\0');

	json_object_set_int_member(self->params, name, value);
}

/**
 * ai_media_request_set_param_double:
 * @self: an #AiMediaRequest
 * @name: the parameter name
 * @value: the value
 *
 * Sets a floating-point parameter, replacing any earlier value of @name.
 */
void
ai_media_request_set_param_double(
	AiMediaRequest *self,
	const gchar    *name,
	gdouble         value
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));
	g_return_if_fail(name != NULL && name[0] != '\0');

	json_object_set_double_member(self->params, name, value);
}

/**
 * ai_media_request_set_param_boolean:
 * @self: an #AiMediaRequest
 * @name: the parameter name
 * @value: the value
 *
 * Sets a boolean parameter, replacing any earlier value of @name.
 */
void
ai_media_request_set_param_boolean(
	AiMediaRequest *self,
	const gchar    *name,
	gboolean        value
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));
	g_return_if_fail(name != NULL && name[0] != '\0');

	json_object_set_boolean_member(self->params, name, value);
}

/**
 * ai_media_request_set_param_json:
 * @self: an #AiMediaRequest
 * @name: the parameter name
 * @value: (transfer none): any JSON value: an array of URLs, an object
 *
 * Sets a parameter to a copy of @value, replacing any earlier value of
 * @name.  This is the route for arrays and nested objects.
 */
void
ai_media_request_set_param_json(
	AiMediaRequest *self,
	const gchar    *name,
	JsonNode       *value
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));
	g_return_if_fail(name != NULL && name[0] != '\0');
	g_return_if_fail(value != NULL);

	json_object_set_member(self->params, name, json_node_copy(value));
}

/**
 * ai_media_request_has_param:
 * @self: an #AiMediaRequest
 * @name: the parameter name
 *
 * Returns: whether @name has been set
 */
gboolean
ai_media_request_has_param(
	AiMediaRequest *self,
	const gchar    *name
){
	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	return json_object_has_member(self->params, name);
}

/**
 * ai_media_request_remove_param:
 * @self: an #AiMediaRequest
 * @name: the parameter name
 *
 * Removes @name, if it was set.
 */
void
ai_media_request_remove_param(
	AiMediaRequest *self,
	const gchar    *name
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));
	g_return_if_fail(name != NULL);

	if (json_object_has_member(self->params, name))
	{
		json_object_remove_member(self->params, name);
	}
}

/**
 * ai_media_request_to_json:
 * @self: an #AiMediaRequest
 *
 * Serialises the request body: every parameter, plus `prompt` when one is
 * set.  The model, idempotency key, webhook and retention are not in the
 * body; a service sends them in the path, headers or query.
 *
 * Returns: (transfer full): the JSON object, as text
 */
gchar *
ai_media_request_to_json(AiMediaRequest *self)
{
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	JsonObject *body;
	GList *members;
	GList *iter;

	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), NULL);

	/* A fresh object, so serialising never touches the stored parameters. */
	body = json_object_new();
	members = json_object_get_members(self->params);
	for (iter = members; iter != NULL; iter = iter->next)
	{
		const gchar *name = (const gchar *)iter->data;

		json_object_set_member(body, name,
		                       json_node_copy(json_object_get_member(self->params, name)));
	}
	g_list_free(members);

	if (self->prompt != NULL)
	{
		json_object_set_string_member(body, "prompt", self->prompt);
	}

	root = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(root, body);

	generator = json_generator_new();
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

/**
 * ai_media_request_get_idempotency_key:
 * @self: an #AiMediaRequest
 *
 * Returns: (transfer none) (nullable): the idempotency key, if any
 */
const gchar *
ai_media_request_get_idempotency_key(AiMediaRequest *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), NULL);

	return self->idempotency_key;
}

/**
 * ai_media_request_set_idempotency_key:
 * @self: an #AiMediaRequest
 * @key: (nullable): the key, 1 to 255 visible ASCII characters
 *
 * Sets the idempotency key.  A service refuses a key outside its own rules
 * when the job is submitted, not here.
 */
void
ai_media_request_set_idempotency_key(
	AiMediaRequest *self,
	const gchar    *key
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));

	if (g_set_str(&self->idempotency_key, key))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IDEMPOTENCY_KEY]);
	}
}

/**
 * ai_media_request_ensure_idempotency_key:
 * @self: an #AiMediaRequest
 *
 * Returns the idempotency key, generating a random UUID the first time.
 *
 * The key is kept on the request, so submitting the same request again
 * after an ambiguous failure (a timeout, a dropped connection, a 5xx)
 * reaches the original job rather than starting -- and paying for -- a
 * second one.  Create a new request for a genuinely new job.
 *
 * Returns: (transfer none): the idempotency key
 */
const gchar *
ai_media_request_ensure_idempotency_key(AiMediaRequest *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), NULL);

	if (self->idempotency_key == NULL)
	{
		self->idempotency_key = g_uuid_string_random();
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IDEMPOTENCY_KEY]);
	}

	return self->idempotency_key;
}

/**
 * ai_media_request_get_webhook_url:
 * @self: an #AiMediaRequest
 *
 * Returns: (transfer none) (nullable): the webhook URL, if any
 */
const gchar *
ai_media_request_get_webhook_url(AiMediaRequest *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), NULL);

	return self->webhook_url;
}

/**
 * ai_media_request_set_webhook_url:
 * @self: an #AiMediaRequest
 * @url: (nullable): an HTTPS endpoint, or %NULL for none
 *
 * Sets the endpoint the service notifies when the job finishes.
 */
void
ai_media_request_set_webhook_url(
	AiMediaRequest *self,
	const gchar    *url
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));

	if (g_set_str(&self->webhook_url, url))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WEBHOOK_URL]);
	}
}

/**
 * ai_media_request_get_expires_after:
 * @self: an #AiMediaRequest
 *
 * Returns: the requested retention in seconds, or 0 for the default
 */
guint
ai_media_request_get_expires_after(AiMediaRequest *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_REQUEST(self), 0);

	return self->expires_after;
}

/**
 * ai_media_request_set_expires_after:
 * @self: an #AiMediaRequest
 * @seconds: seconds to keep the job's data after it finishes, or 0
 *
 * Sets the requested retention.
 */
void
ai_media_request_set_expires_after(
	AiMediaRequest *self,
	guint           seconds
){
	g_return_if_fail(AI_IS_MEDIA_REQUEST(self));

	if (self->expires_after != seconds)
	{
		self->expires_after = seconds;
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_EXPIRES_AFTER]);
	}
}
