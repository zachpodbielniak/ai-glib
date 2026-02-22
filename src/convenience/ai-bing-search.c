/*
 * ai-bing-search.c - Bing Web Search provider
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "convenience/ai-bing-search.h"
#include "convenience/ai-search-provider.h"
#include "core/ai-error.h"

#define BING_SEARCH_ENDPOINT "https://api.bing.microsoft.com/v7.0/search"
#define BING_RESULT_COUNT    10

struct _AiBingSearch
{
    GObject       parent_instance;
    gchar        *api_key;    /* owned */
    SoupSession  *session;    /* owned */
};

static void ai_bing_search_iface_init (AiSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (
    AiBingSearch,
    ai_bing_search,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (AI_TYPE_SEARCH_PROVIDER, ai_bing_search_iface_init)
)

static gchar *
ai_bing_search_do_search (
    AiSearchProvider  *provider,
    const gchar       *query,
    GCancellable      *cancellable,
    GError           **error
){
    AiBingSearch *self = AI_BING_SEARCH (provider);
    g_autofree gchar *encoded  = NULL;
    g_autofree gchar *url      = NULL;
    g_autoptr(SoupMessage)  msg    = NULL;
    g_autoptr(GBytes)       bytes  = NULL;
    g_autoptr(JsonParser)   parser = NULL;
    JsonNode    *root;
    JsonObject  *root_obj;
    JsonObject  *web_pages;
    JsonArray   *value_arr;
    GString     *result;
    guint        status_code;
    gsize        body_size;
    const gchar *body_data;
    guint        count;
    guint        i;

    g_return_val_if_fail (self->api_key != NULL, NULL);

    encoded = g_uri_escape_string (query, NULL, FALSE);
    url     = g_strdup_printf ("%s?q=%s&count=%d",
                               BING_SEARCH_ENDPOINT, encoded, BING_RESULT_COUNT);

    msg = soup_message_new ("GET", url);
    if (msg == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "Bing search: failed to create request for URL: %s", url);
        return NULL;
    }

    soup_message_headers_replace (soup_message_get_request_headers (msg),
                                  "Ocp-Apim-Subscription-Key",
                                  self->api_key);

    bytes = soup_session_send_and_read (self->session, msg, cancellable, error);
    if (bytes == NULL)
        return NULL;

    status_code = soup_message_get_status (msg);
    if (status_code < 200 || status_code >= 300)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                     "Bing search: HTTP %u", status_code);
        return NULL;
    }

    body_data = g_bytes_get_data (bytes, &body_size);

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, body_data, (gssize)body_size, error))
        return NULL;

    root     = json_parser_get_root (parser);
    root_obj = json_node_get_object (root);

    if (!json_object_has_member (root_obj, "webPages"))
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                             "Bing search: response missing 'webPages'");
        return NULL;
    }

    web_pages = json_object_get_object_member (root_obj, "webPages");

    if (!json_object_has_member (web_pages, "value"))
    {
        /* No results */
        return g_strdup ("");
    }

    value_arr = json_object_get_array_member (web_pages, "value");
    count     = (guint)json_array_get_length (value_arr);
    result    = g_string_new (NULL);

    for (i = 0; i < count; i++)
    {
        JsonObject  *item    = json_array_get_object_element (value_arr, i);
        const gchar *name    = "";
        const gchar *url_str = "";
        const gchar *snippet = "";

        if (json_object_has_member (item, "name"))
            name = json_object_get_string_member (item, "name");
        if (json_object_has_member (item, "url"))
            url_str = json_object_get_string_member (item, "url");
        if (json_object_has_member (item, "snippet"))
            snippet = json_object_get_string_member (item, "snippet");

        g_string_append_printf (result, "%s\n%s\n%s\n---\n",
                                name, url_str, snippet);
    }

    return g_string_free (result, FALSE);
}

static void
ai_bing_search_iface_init (AiSearchProviderInterface *iface)
{
    iface->search = ai_bing_search_do_search;
}

static void
ai_bing_search_finalize (GObject *object)
{
    AiBingSearch *self = AI_BING_SEARCH (object);

    g_clear_pointer (&self->api_key, g_free);
    g_clear_object  (&self->session);

    G_OBJECT_CLASS (ai_bing_search_parent_class)->finalize (object);
}

static void
ai_bing_search_class_init (AiBingSearchClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = ai_bing_search_finalize;
}

static void
ai_bing_search_init (AiBingSearch *self)
{
    self->api_key = NULL;
    self->session = soup_session_new ();
}

AiBingSearch *
ai_bing_search_new (const gchar *api_key)
{
    AiBingSearch *self;

    g_return_val_if_fail (api_key != NULL, NULL);
    g_return_val_if_fail (*api_key != '\0', NULL);

    self = g_object_new (AI_TYPE_BING_SEARCH, NULL);
    self->api_key = g_strdup (api_key);

    return self;
}
