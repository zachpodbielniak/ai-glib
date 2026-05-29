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
#include "convenience/ai-search-http.h"
#include "core/ai-error.h"

#define BRAVE_SEARCH_ENDPOINT "https://api.search.brave.com/res/v1/web/search"
#define BRAVE_DEFAULT_COUNT   10
#define BRAVE_MAX_COUNT       20
#define BRAVE_SESSION_TIMEOUT 30

struct _AiBraveSearch
{
    GObject       parent_instance;
    gchar        *api_key;     /* owned, nullable */
    gchar        *endpoint;    /* owned; defaults to BRAVE_SEARCH_ENDPOINT */
    SoupSession  *session;     /* owned */
};

static void ai_brave_search_iface_init (AiSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (
    AiBraveSearch,
    ai_brave_search,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (AI_TYPE_SEARCH_PROVIDER, ai_brave_search_iface_init)
)

enum
{
    PROP_0,
    PROP_API_KEY,
    PROP_ENDPOINT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* Map AiSearchFreshness to Brave's freshness token (NULL = omit). */
static const gchar *
brave_freshness (AiSearchFreshness f)
{
    switch (f)
    {
        case AI_SEARCH_FRESHNESS_DAY:   return "pd";
        case AI_SEARCH_FRESHNESS_WEEK:  return "pw";
        case AI_SEARCH_FRESHNESS_MONTH: return "pm";
        case AI_SEARCH_FRESHNESS_YEAR:  return "py";
        default:                        return NULL;
    }
}

static const gchar *
brave_safesearch (AiSearchSafeSearch s)
{
    switch (s)
    {
        case AI_SEARCH_SAFE_OFF:    return "off";
        case AI_SEARCH_SAFE_STRICT: return "strict";
        default:                    return "moderate";
    }
}

static void
append_param (GString *url, const gchar *key, const gchar *value)
{
    g_autofree gchar *enc = g_uri_escape_string (value, NULL, FALSE);

    g_string_append_printf (url, "&%s=%s", key, enc);
}

static gchar *
build_brave_url (AiBraveSearch *self, const gchar *query,
                 AiSearchOptions *options)
{
    GString          *url;
    g_autofree gchar *raw_query = NULL;
    g_autofree gchar *enc_query = NULL;
    guint             count     = BRAVE_DEFAULT_COUNT;
    guint             offset    = 0;
    const gchar      *site      = NULL;
    const gchar      *country   = NULL;
    const gchar      *language  = NULL;
    const gchar      *freshness = NULL;
    const gchar      *safe      = "moderate";

    if (options != NULL)
    {
        count     = ai_search_options_get_count (options);
        offset    = ai_search_options_get_offset (options);
        site      = ai_search_options_get_site (options);
        country   = ai_search_options_get_country (options);
        language  = ai_search_options_get_language (options);
        freshness = brave_freshness (ai_search_options_get_freshness (options));
        safe      = brave_safesearch (ai_search_options_get_safesearch (options));
    }

    if (count == 0)
        count = BRAVE_DEFAULT_COUNT;
    if (count > BRAVE_MAX_COUNT)
        count = BRAVE_MAX_COUNT;

    if (site != NULL && *site != '\0')
        raw_query = g_strdup_printf ("%s site:%s", query, site);
    else
        raw_query = g_strdup (query);

    enc_query = g_uri_escape_string (raw_query, NULL, FALSE);

    url = g_string_new (self->endpoint);
    g_string_append_printf (url, "?q=%s", enc_query);
    g_string_append_printf (url, "&count=%u", count);
    if (offset > 0)
        g_string_append_printf (url, "&offset=%u", offset);
    if (freshness != NULL)
        append_param (url, "freshness", freshness);
    append_param (url, "safesearch", safe);
    if (country != NULL && *country != '\0')
        append_param (url, "country", country);
    if (language != NULL && *language != '\0')
        append_param (url, "search_lang", language);

    return g_string_free (url, FALSE);
}

static GList *
ai_brave_search_do_search (
    AiSearchProvider  *provider,
    const gchar       *query,
    AiSearchOptions   *options,
    GCancellable      *cancellable,
    GError           **error
){
    AiBraveSearch       *self = AI_BRAVE_SEARCH (provider);
    g_autofree gchar    *url  = NULL;
    g_autoptr(JsonNode)  root = NULL;
    const gchar         *headers[5];
    JsonObject          *root_obj;
    JsonObject          *web_obj;
    JsonArray           *results_arr;
    GList               *results = NULL;
    guint                count;
    guint                i;

    if (self->api_key == NULL || *self->api_key == '\0')
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                             "Brave search: no API key configured");
        return NULL;
    }

    url = build_brave_url (self, query, options);

    headers[0] = "X-Subscription-Token";
    headers[1] = self->api_key;
    headers[2] = "Accept";
    headers[3] = "application/json";
    headers[4] = NULL;

    root = ai_search_http_get_json (self->session, url,
                                    (const gchar *const *) headers,
                                    cancellable, error);
    if (root == NULL)
        return NULL;

    if (!JSON_NODE_HOLDS_OBJECT (root))
        return NULL;

    root_obj = json_node_get_object (root);

    /* Brave returns { "web": { "results": [...] } }. */
    if (!json_object_has_member (root_obj, "web"))
        return NULL;

    web_obj = json_object_get_object_member (root_obj, "web");
    if (web_obj == NULL || !json_object_has_member (web_obj, "results"))
        return NULL;

    results_arr = json_object_get_array_member (web_obj, "results");
    count       = (guint) json_array_get_length (results_arr);

    for (i = 0; i < count; i++)
    {
        JsonObject     *item = json_array_get_object_element (results_arr, i);
        AiSearchResult *res;
        const gchar    *title   = NULL;
        const gchar    *url_str  = NULL;
        const gchar    *descr   = NULL;

        if (item == NULL)
            continue;

        if (json_object_has_member (item, "title"))
            title = json_object_get_string_member (item, "title");
        if (json_object_has_member (item, "url"))
            url_str = json_object_get_string_member (item, "url");
        if (json_object_has_member (item, "description"))
            descr = json_object_get_string_member (item, "description");

        res = ai_search_result_new (title, url_str, descr);
        ai_search_result_set_rank (res, i + 1);

        /* Brave reports recency as "page_age" (and sometimes "age"). */
        if (json_object_has_member (item, "page_age"))
            ai_search_result_set_age (
                res, json_object_get_string_member (item, "page_age"));
        else if (json_object_has_member (item, "age"))
            ai_search_result_set_age (
                res, json_object_get_string_member (item, "age"));

        results = g_list_prepend (results, res);
    }

    return g_list_reverse (results);
}

static void
ai_brave_search_iface_init (AiSearchProviderInterface *iface)
{
    iface->search = ai_brave_search_do_search;
}

static void
ai_brave_search_get_property (
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiBraveSearch *self = AI_BRAVE_SEARCH (object);

    switch (prop_id)
    {
        case PROP_API_KEY:
            g_value_set_string (value, self->api_key);
            break;
        case PROP_ENDPOINT:
            g_value_set_string (value, self->endpoint);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
ai_brave_search_set_property (
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiBraveSearch *self = AI_BRAVE_SEARCH (object);

    switch (prop_id)
    {
        case PROP_API_KEY:
            g_clear_pointer (&self->api_key, g_free);
            self->api_key = g_value_dup_string (value);
            break;
        case PROP_ENDPOINT:
            g_clear_pointer (&self->endpoint, g_free);
            self->endpoint = g_value_dup_string (value);
            if (self->endpoint == NULL || *self->endpoint == '\0')
            {
                g_clear_pointer (&self->endpoint, g_free);
                self->endpoint = g_strdup (BRAVE_SEARCH_ENDPOINT);
            }
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
ai_brave_search_finalize (GObject *object)
{
    AiBraveSearch *self = AI_BRAVE_SEARCH (object);

    g_clear_pointer (&self->api_key, g_free);
    g_clear_pointer (&self->endpoint, g_free);
    g_clear_object  (&self->session);

    G_OBJECT_CLASS (ai_brave_search_parent_class)->finalize (object);
}

static void
ai_brave_search_class_init (AiBraveSearchClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize     = ai_brave_search_finalize;
    object_class->get_property = ai_brave_search_get_property;
    object_class->set_property = ai_brave_search_set_property;

    /**
     * AiBraveSearch:api-key:
     *
     * The Brave Search API key (X-Subscription-Token).
     */
    properties[PROP_API_KEY] =
        g_param_spec_string ("api-key", "API key",
                             "Brave Search subscription token", NULL,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                             G_PARAM_STATIC_STRINGS);

    /**
     * AiBraveSearch:endpoint:
     *
     * The search endpoint URL. Defaults to the public Brave endpoint;
     * override for a proxy endpoint or for testing.
     */
    properties[PROP_ENDPOINT] =
        g_param_spec_string ("endpoint", "Endpoint",
                             "Search endpoint URL", BRAVE_SEARCH_ENDPOINT,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
ai_brave_search_init (AiBraveSearch *self)
{
    self->api_key  = NULL;
    self->endpoint = NULL;   /* set by the CONSTRUCT default */
    self->session  = soup_session_new ();
    g_object_set (self->session, "timeout", (guint) BRAVE_SESSION_TIMEOUT, NULL);
}

AiBraveSearch *
ai_brave_search_new (const gchar *api_key)
{
    g_return_val_if_fail (api_key != NULL, NULL);
    g_return_val_if_fail (*api_key != '\0', NULL);

    return g_object_new (AI_TYPE_BRAVE_SEARCH, "api-key", api_key, NULL);
}
