/*
 * ai-brave-search.c - Brave Web Search provider
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "convenience/ai-brave-search.h"
#include "convenience/ai-search-provider.h"
#include "core/ai-error.h"

#define BRAVE_SEARCH_ENDPOINT "https://api.search.brave.com/res/v1/web/search"
#define BRAVE_RESULT_COUNT    10

struct _AiBraveSearch
{
    GObject       parent_instance;
    gchar        *api_key;    /* owned */
    SoupSession  *session;    /* owned */
};

static void ai_brave_search_iface_init (AiSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (
    AiBraveSearch,
    ai_brave_search,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (AI_TYPE_SEARCH_PROVIDER, ai_brave_search_iface_init)
)

static gchar *
ai_brave_search_do_search (
    AiSearchProvider  *provider,
    const gchar       *query,
    GCancellable      *cancellable,
    GError           **error
){
    AiBraveSearch *self = AI_BRAVE_SEARCH (provider);
    g_autofree gchar *encoded  = NULL;
    g_autofree gchar *url      = NULL;
    g_autoptr(SoupMessage)  msg    = NULL;
    g_autoptr(GBytes)       bytes  = NULL;
    g_autoptr(JsonParser)   parser = NULL;
    JsonNode    *root;
    JsonObject  *root_obj;
    JsonArray   *results_arr;
    GString     *result;
    guint        status_code;
    gsize        body_size;
    const gchar *body_data;
    guint        count;
    guint        i;

    g_return_val_if_fail (self->api_key != NULL, NULL);

    encoded = g_uri_escape_string (query, NULL, FALSE);
    url     = g_strdup_printf ("%s?q=%s&count=%d",
                               BRAVE_SEARCH_ENDPOINT, encoded, BRAVE_RESULT_COUNT);

    msg = soup_message_new ("GET", url);
    if (msg == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "Brave search: failed to create request for URL: %s", url);
        return NULL;
    }

    soup_message_headers_replace (soup_message_get_request_headers (msg),
                                  "X-Subscription-Token",
                                  self->api_key);
    soup_message_headers_replace (soup_message_get_request_headers (msg),
                                  "Accept",
                                  "application/json");

    bytes = soup_session_send_and_read (self->session, msg, cancellable, error);
    if (bytes == NULL)
        return NULL;

    status_code = soup_message_get_status (msg);
    if (status_code < 200 || status_code >= 300)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                     "Brave search: HTTP %u", status_code);
        return NULL;
    }

    body_data = g_bytes_get_data (bytes, &body_size);

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, body_data, (gssize)body_size, error))
        return NULL;

    root     = json_parser_get_root (parser);
    root_obj = json_node_get_object (root);

    /* Brave returns { "web": { "results": [...] } } */
    if (!json_object_has_member (root_obj, "web"))
    {
        /* No web results */
        return g_strdup ("");
    }

    {
        JsonObject *web_obj = json_object_get_object_member (root_obj, "web");

        if (!json_object_has_member (web_obj, "results"))
            return g_strdup ("");

        results_arr = json_object_get_array_member (web_obj, "results");
    }

    count  = (guint)json_array_get_length (results_arr);
    result = g_string_new (NULL);

    for (i = 0; i < count; i++)
    {
        JsonObject  *item    = json_array_get_object_element (results_arr, i);
        const gchar *title   = "";
        const gchar *url_str = "";
        const gchar *descr   = "";

        if (json_object_has_member (item, "title"))
            title = json_object_get_string_member (item, "title");
        if (json_object_has_member (item, "url"))
            url_str = json_object_get_string_member (item, "url");
        if (json_object_has_member (item, "description"))
            descr = json_object_get_string_member (item, "description");

        g_string_append_printf (result, "%s\n%s\n%s\n---\n",
                                title, url_str, descr);
    }

    return g_string_free (result, FALSE);
}

static void
ai_brave_search_iface_init (AiSearchProviderInterface *iface)
{
    iface->search = ai_brave_search_do_search;
}

static void
ai_brave_search_finalize (GObject *object)
{
    AiBraveSearch *self = AI_BRAVE_SEARCH (object);

    g_clear_pointer (&self->api_key, g_free);
    g_clear_object  (&self->session);

    G_OBJECT_CLASS (ai_brave_search_parent_class)->finalize (object);
}

static void
ai_brave_search_class_init (AiBraveSearchClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = ai_brave_search_finalize;
}

static void
ai_brave_search_init (AiBraveSearch *self)
{
    self->api_key = NULL;
    self->session = soup_session_new ();
}

AiBraveSearch *
ai_brave_search_new (const gchar *api_key)
{
    AiBraveSearch *self;

    g_return_val_if_fail (api_key != NULL, NULL);
    g_return_val_if_fail (*api_key != '\0', NULL);

    self = g_object_new (AI_TYPE_BRAVE_SEARCH, NULL);
    self->api_key = g_strdup (api_key);

    return self;
}
