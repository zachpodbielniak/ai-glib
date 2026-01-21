/*
 * ai-client.c - Base client class
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-client.h"
#include "core/ai-error.h"

/*
 * Private data for AiClient.
 */
typedef struct
{
    AiConfig    *config;
    SoupSession *session;
    gchar       *model;
    gchar       *system_prompt;
    gint         max_tokens;
    gdouble      temperature;
} AiClientPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(AiClient, ai_client, G_TYPE_OBJECT)

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_CONFIG,
    PROP_MODEL,
    PROP_MAX_TOKENS,
    PROP_TEMPERATURE,
    PROP_SYSTEM_PROMPT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_client_finalize(GObject *object)
{
    AiClient *self = AI_CLIENT(object);
    AiClientPrivate *priv = ai_client_get_instance_private(self);

    g_clear_object(&priv->config);
    g_clear_object(&priv->session);
    g_clear_pointer(&priv->model, g_free);
    g_clear_pointer(&priv->system_prompt, g_free);

    G_OBJECT_CLASS(ai_client_parent_class)->finalize(object);
}

static void
ai_client_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiClient *self = AI_CLIENT(object);
    AiClientPrivate *priv = ai_client_get_instance_private(self);

    switch (prop_id)
    {
        case PROP_CONFIG:
            g_value_set_object(value, priv->config);
            break;
        case PROP_MODEL:
            g_value_set_string(value, priv->model);
            break;
        case PROP_MAX_TOKENS:
            g_value_set_int(value, priv->max_tokens);
            break;
        case PROP_TEMPERATURE:
            g_value_set_double(value, priv->temperature);
            break;
        case PROP_SYSTEM_PROMPT:
            g_value_set_string(value, priv->system_prompt);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_client_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiClient *self = AI_CLIENT(object);
    AiClientPrivate *priv = ai_client_get_instance_private(self);

    switch (prop_id)
    {
        case PROP_CONFIG:
            g_clear_object(&priv->config);
            priv->config = g_value_dup_object(value);
            break;
        case PROP_MODEL:
            g_clear_pointer(&priv->model, g_free);
            priv->model = g_value_dup_string(value);
            break;
        case PROP_MAX_TOKENS:
            priv->max_tokens = g_value_get_int(value);
            break;
        case PROP_TEMPERATURE:
            priv->temperature = g_value_get_double(value);
            break;
        case PROP_SYSTEM_PROMPT:
            g_clear_pointer(&priv->system_prompt, g_free);
            priv->system_prompt = g_value_dup_string(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_client_constructed(GObject *object)
{
    AiClient *self = AI_CLIENT(object);
    AiClientPrivate *priv = ai_client_get_instance_private(self);

    G_OBJECT_CLASS(ai_client_parent_class)->constructed(object);

    /* Create config if not provided */
    if (priv->config == NULL)
    {
        priv->config = ai_config_new();
    }

    /* Create soup session */
    priv->session = soup_session_new();
    soup_session_set_timeout(priv->session, ai_config_get_timeout(priv->config));
}

static void
ai_client_class_init(AiClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_client_finalize;
    object_class->get_property = ai_client_get_property;
    object_class->set_property = ai_client_set_property;
    object_class->constructed = ai_client_constructed;

    /* Virtual methods default to NULL - subclasses must implement */
    klass->build_request = NULL;
    klass->parse_response = NULL;
    klass->get_endpoint_url = NULL;
    klass->add_auth_headers = NULL;
    klass->parse_stream_chunk = NULL;

    /**
     * AiClient:config:
     *
     * The configuration for this client.
     */
    properties[PROP_CONFIG] =
        g_param_spec_object("config",
                            "Config",
                            "The configuration for this client",
                            AI_TYPE_CONFIG,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiClient:model:
     *
     * The model to use for requests.
     */
    properties[PROP_MODEL] =
        g_param_spec_string("model",
                            "Model",
                            "The model to use for requests",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClient:max-tokens:
     *
     * The default maximum tokens to generate.
     */
    properties[PROP_MAX_TOKENS] =
        g_param_spec_int("max-tokens",
                         "Max Tokens",
                         "The default maximum tokens to generate",
                         1, G_MAXINT, 4096,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClient:temperature:
     *
     * The temperature for response generation.
     */
    properties[PROP_TEMPERATURE] =
        g_param_spec_double("temperature",
                            "Temperature",
                            "The temperature for response generation",
                            0.0, 2.0, 1.0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClient:system-prompt:
     *
     * The default system prompt.
     */
    properties[PROP_SYSTEM_PROMPT] =
        g_param_spec_string("system-prompt",
                            "System Prompt",
                            "The default system prompt",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_client_init(AiClient *self)
{
    AiClientPrivate *priv = ai_client_get_instance_private(self);

    priv->config = NULL;
    priv->session = NULL;
    priv->model = NULL;
    priv->system_prompt = NULL;
    priv->max_tokens = 4096;
    priv->temperature = 1.0;
}

/**
 * ai_client_get_config:
 * @self: an #AiClient
 *
 * Gets the configuration for this client.
 *
 * Returns: (transfer none): the #AiConfig
 */
AiConfig *
ai_client_get_config(AiClient *self)
{
    AiClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLIENT(self), NULL);

    priv = ai_client_get_instance_private(self);
    return priv->config;
}

/**
 * ai_client_get_model:
 * @self: an #AiClient
 *
 * Gets the model name.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_client_get_model(AiClient *self)
{
    AiClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLIENT(self), NULL);

    priv = ai_client_get_instance_private(self);
    return priv->model;
}

/**
 * ai_client_set_model:
 * @self: an #AiClient
 * @model: the model name
 *
 * Sets the model to use for requests.
 */
void
ai_client_set_model(
    AiClient    *self,
    const gchar *model
){
    AiClientPrivate *priv;

    g_return_if_fail(AI_IS_CLIENT(self));

    priv = ai_client_get_instance_private(self);
    g_clear_pointer(&priv->model, g_free);
    priv->model = g_strdup(model);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MODEL]);
}

/**
 * ai_client_get_max_tokens:
 * @self: an #AiClient
 *
 * Gets the default max tokens setting.
 *
 * Returns: the max tokens
 */
gint
ai_client_get_max_tokens(AiClient *self)
{
    AiClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLIENT(self), 4096);

    priv = ai_client_get_instance_private(self);
    return priv->max_tokens;
}

/**
 * ai_client_set_max_tokens:
 * @self: an #AiClient
 * @max_tokens: the max tokens
 *
 * Sets the default max tokens for requests.
 */
void
ai_client_set_max_tokens(
    AiClient *self,
    gint      max_tokens
){
    AiClientPrivate *priv;

    g_return_if_fail(AI_IS_CLIENT(self));

    priv = ai_client_get_instance_private(self);
    priv->max_tokens = max_tokens;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAX_TOKENS]);
}

/**
 * ai_client_get_temperature:
 * @self: an #AiClient
 *
 * Gets the temperature setting.
 *
 * Returns: the temperature
 */
gdouble
ai_client_get_temperature(AiClient *self)
{
    AiClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLIENT(self), 1.0);

    priv = ai_client_get_instance_private(self);
    return priv->temperature;
}

/**
 * ai_client_set_temperature:
 * @self: an #AiClient
 * @temperature: the temperature (0.0 to 2.0)
 *
 * Sets the temperature for response generation.
 */
void
ai_client_set_temperature(
    AiClient *self,
    gdouble   temperature
){
    AiClientPrivate *priv;

    g_return_if_fail(AI_IS_CLIENT(self));

    priv = ai_client_get_instance_private(self);
    priv->temperature = CLAMP(temperature, 0.0, 2.0);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TEMPERATURE]);
}

/**
 * ai_client_get_system_prompt:
 * @self: an #AiClient
 *
 * Gets the default system prompt.
 *
 * Returns: (transfer none) (nullable): the system prompt
 */
const gchar *
ai_client_get_system_prompt(AiClient *self)
{
    AiClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLIENT(self), NULL);

    priv = ai_client_get_instance_private(self);
    return priv->system_prompt;
}

/**
 * ai_client_set_system_prompt:
 * @self: an #AiClient
 * @system_prompt: (nullable): the system prompt
 *
 * Sets the default system prompt for requests.
 */
void
ai_client_set_system_prompt(
    AiClient    *self,
    const gchar *system_prompt
){
    AiClientPrivate *priv;

    g_return_if_fail(AI_IS_CLIENT(self));

    priv = ai_client_get_instance_private(self);
    g_clear_pointer(&priv->system_prompt, g_free);
    priv->system_prompt = g_strdup(system_prompt);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SYSTEM_PROMPT]);
}

/**
 * ai_client_get_soup_session:
 * @self: an #AiClient
 *
 * Gets the SoupSession used for HTTP requests.
 *
 * Returns: (transfer none): the #SoupSession
 */
SoupSession *
ai_client_get_soup_session(AiClient *self)
{
    AiClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLIENT(self), NULL);

    priv = ai_client_get_instance_private(self);
    return priv->session;
}

/**
 * ai_client_chat_sync:
 * @self: an #AiClient
 * @messages: (element-type AiMessage): the conversation messages
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Performs a synchronous chat completion request.
 * This is a convenience method that calls the async methods internally.
 *
 * Returns: (transfer full) (nullable): the #AiResponse, or %NULL on error
 */
AiResponse *
ai_client_chat_sync(
    AiClient      *self,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
){
    AiClientClass *klass;
    AiClientPrivate *priv;
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    g_autoptr(GBytes) response_bytes = NULL;
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *response_json;
    const gchar *response_data;
    gsize response_len;

    g_return_val_if_fail(AI_IS_CLIENT(self), NULL);

    klass = AI_CLIENT_GET_CLASS(self);
    priv = ai_client_get_instance_private(self);

    g_return_val_if_fail(klass->build_request != NULL, NULL);
    g_return_val_if_fail(klass->parse_response != NULL, NULL);
    g_return_val_if_fail(klass->get_endpoint_url != NULL, NULL);

    /* Build request */
    request_json = klass->build_request(self, messages, priv->system_prompt,
                                        priv->max_tokens, NULL);
    if (request_json == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "Failed to build request");
        return NULL;
    }

    /* Serialize to JSON string */
    {
        g_autoptr(JsonGenerator) gen = json_generator_new();
        json_generator_set_root(gen, request_json);
        request_body = json_generator_to_data(gen, NULL);
    }

    /* Get endpoint URL */
    url = klass->get_endpoint_url(self);
    if (url == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                    "Failed to get endpoint URL");
        return NULL;
    }

    /* Create HTTP request */
    msg = soup_message_new("POST", url);
    if (msg == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "Failed to create HTTP request for URL: %s", url);
        return NULL;
    }

    /* Set headers */
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");

    /* Add auth headers */
    if (klass->add_auth_headers != NULL)
    {
        klass->add_auth_headers(self, msg);
    }

    /* Set request body */
    soup_message_set_request_body_from_bytes(msg, "application/json",
        g_bytes_new(request_body, strlen(request_body)));

    /* Send request */
    response_bytes = soup_session_send_and_read(priv->session, msg, cancellable, error);
    if (response_bytes == NULL)
    {
        return NULL;
    }

    /* Check HTTP status */
    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(msg)))
    {
        guint status = soup_message_get_status(msg);

        if (status == 401 || status == 403)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                        "Authentication failed (HTTP %u)", status);
        }
        else if (status == 429)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED,
                        "Rate limited (HTTP %u)", status);
        }
        else if (status >= 500)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                        "Server error (HTTP %u)", status);
        }
        else
        {
            g_set_error(error, AI_ERROR, AI_ERROR_NETWORK_ERROR,
                        "Request failed (HTTP %u)", status);
        }

        return NULL;
    }

    /* Parse response */
    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, error))
    {
        return NULL;
    }

    response_json = json_parser_get_root(parser);

    return klass->parse_response(self, response_json, error);
}
