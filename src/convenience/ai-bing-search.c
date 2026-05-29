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
#include "convenience/ai-search-http.h"
#include "core/ai-error.h"

#define BING_SEARCH_ENDPOINT "https://api.bing.microsoft.com/v7.0/search"
#define BING_DEFAULT_COUNT   10
#define BING_MAX_COUNT       50
#define BING_SESSION_TIMEOUT 30

struct _AiBingSearch
{
    GObject       parent_instance;
    gchar        *api_key;     /* owned, nullable */
    gchar        *endpoint;    /* owned; defaults to BING_SEARCH_ENDPOINT */
    SoupSession  *session;     /* owned */
};

static void ai_bing_search_iface_init (AiSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (
    AiBingSearch,
    ai_bing_search,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (AI_TYPE_SEARCH_PROVIDER, ai_bing_search_iface_init)
)

enum
{
    PROP_0,
    PROP_API_KEY,
    PROP_ENDPOINT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* Map AiSearchFreshness to Bing's freshness token (NULL = omit). Bing has no
 * "past year" window, so YEAR (and ANY) are omitted rather than approximated. */
static const gchar *
bing_freshness (AiSearchFreshness f)
{
    switch (f)
    {
        case AI_SEARCH_FRESHNESS_DAY:   return "Day";
        case AI_SEARCH_FRESHNESS_WEEK:  return "Week";
        case AI_SEARCH_FRESHNESS_MONTH: return "Month";
        default:                        return NULL;
    }
}

static const gchar *
bing_safesearch (AiSearchSafeSearch s)
{
    switch (s)
    {
        case AI_SEARCH_SAFE_OFF:    return "Off";
        case AI_SEARCH_SAFE_STRICT: return "Strict";
        default:                    return "Moderate";
    }
}

/* Append "&key=<escaped value>" to url. */
static void
append_param (GString *url, const gchar *key, const gchar *value)
{
    g_autofree gchar *enc = g_uri_escape_string (value, NULL, FALSE);

    g_string_append_printf (url, "&%s=%s", key, enc);
}

/* Build the full request URL from the query + options. */
static gchar *
build_bing_url (AiBingSearch *self, const gchar *query, AiSearchOptions *options)
{
    GString          *url;
    g_autofree gchar *raw_query = NULL;
    g_autofree gchar *enc_query = NULL;
    guint             count     = BING_DEFAULT_COUNT;
    guint             offset    = 0;
    const gchar      *site      = NULL;
    const gchar      *country   = NULL;
    const gchar      *language  = NULL;
    const gchar      *freshness = NULL;
    const gchar      *safe      = "Moderate";

    if (options != NULL)
    {
        count     = ai_search_options_get_count (options);
        offset    = ai_search_options_get_offset (options);
        site      = ai_search_options_get_site (options);
        country   = ai_search_options_get_country (options);
        language  = ai_search_options_get_language (options);
        freshness = bing_freshness (ai_search_options_get_freshness (options));
        safe      = bing_safesearch (ai_search_options_get_safesearch (options));
    }

    if (count == 0)
        count = BING_DEFAULT_COUNT;
    if (count > BING_MAX_COUNT)
        count = BING_MAX_COUNT;

    /* A site filter is expressed in the query itself (Bing has no param). */
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
    append_param (url, "safeSearch", safe);

    /* Region/language: prefer a full market when both are present. */
    if (language != NULL && *language != '\0' &&
        country != NULL && *country != '\0')
    {
        g_autofree gchar *up = g_ascii_strup (country, -1);
        g_autofree gchar *mkt = g_strdup_printf ("%s-%s", language, up);
        append_param (url, "mkt", mkt);
    }
    else if (country != NULL && *country != '\0')
    {
        append_param (url, "cc", country);
    }
    else if (language != NULL && *language != '\0')
    {
        append_param (url, "setLang", language);
    }

    return g_string_free (url, FALSE);
}

static GList *
ai_bing_search_do_search (
    AiSearchProvider  *provider,
    const gchar       *query,
    AiSearchOptions   *options,
    GCancellable      *cancellable,
    GError           **error
){
    AiBingSearch        *self = AI_BING_SEARCH (provider);
    g_autofree gchar    *url  = NULL;
    g_autoptr(JsonNode)  root = NULL;
    const gchar         *headers[3];
    JsonObject          *root_obj;
    JsonObject          *web_pages;
    JsonArray           *value_arr;
    GList               *results = NULL;
    guint                count;
    guint                i;

    if (self->api_key == NULL || *self->api_key == '\0')
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                             "Bing search: no API key configured");
        return NULL;
    }

    url = build_bing_url (self, query, options);

    headers[0] = "Ocp-Apim-Subscription-Key";
    headers[1] = self->api_key;
    headers[2] = NULL;

    root = ai_search_http_get_json (self->session, url,
                                    (const gchar *const *) headers,
                                    cancellable, error);
    if (root == NULL)
        return NULL;

    if (!JSON_NODE_HOLDS_OBJECT (root))
        return NULL;   /* unexpected shape -> treat as no results */

    root_obj = json_node_get_object (root);

    /* Bing legitimately omits webPages when there are no web results. */
    if (!json_object_has_member (root_obj, "webPages"))
        return NULL;

    web_pages = json_object_get_object_member (root_obj, "webPages");
    if (web_pages == NULL || !json_object_has_member (web_pages, "value"))
        return NULL;

    value_arr = json_object_get_array_member (web_pages, "value");
    count     = (guint) json_array_get_length (value_arr);

    for (i = 0; i < count; i++)
    {
        JsonObject     *item = json_array_get_object_element (value_arr, i);
        AiSearchResult *res;
        const gchar    *name    = NULL;
        const gchar    *url_str = NULL;
        const gchar    *snippet = NULL;

        if (item == NULL)
            continue;

        if (json_object_has_member (item, "name"))
            name = json_object_get_string_member (item, "name");
        if (json_object_has_member (item, "url"))
            url_str = json_object_get_string_member (item, "url");
        if (json_object_has_member (item, "snippet"))
            snippet = json_object_get_string_member (item, "snippet");

        res = ai_search_result_new (name, url_str, snippet);
        ai_search_result_set_rank (res, i + 1);

        if (json_object_has_member (item, "dateLastCrawled"))
            ai_search_result_set_age (
                res, json_object_get_string_member (item, "dateLastCrawled"));

        results = g_list_prepend (results, res);
    }

    return g_list_reverse (results);
}

static void
ai_bing_search_iface_init (AiSearchProviderInterface *iface)
{
    iface->search = ai_bing_search_do_search;
}

static void
ai_bing_search_get_property (
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiBingSearch *self = AI_BING_SEARCH (object);

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
ai_bing_search_set_property (
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiBingSearch *self = AI_BING_SEARCH (object);

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
                self->endpoint = g_strdup (BING_SEARCH_ENDPOINT);
            }
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
ai_bing_search_finalize (GObject *object)
{
    AiBingSearch *self = AI_BING_SEARCH (object);

    g_clear_pointer (&self->api_key, g_free);
    g_clear_pointer (&self->endpoint, g_free);
    g_clear_object  (&self->session);

    G_OBJECT_CLASS (ai_bing_search_parent_class)->finalize (object);
}

static void
ai_bing_search_class_init (AiBingSearchClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize     = ai_bing_search_finalize;
    object_class->get_property = ai_bing_search_get_property;
    object_class->set_property = ai_bing_search_set_property;

    /**
     * AiBingSearch:api-key:
     *
     * The Bing Search subscription key (Ocp-Apim-Subscription-Key).
     */
    properties[PROP_API_KEY] =
        g_param_spec_string ("api-key", "API key",
                             "Bing Search subscription key", NULL,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                             G_PARAM_STATIC_STRINGS);

    /**
     * AiBingSearch:endpoint:
     *
     * The search endpoint URL. Defaults to the public Bing endpoint; override
     * for a regional/proxy endpoint or for testing.
     */
    properties[PROP_ENDPOINT] =
        g_param_spec_string ("endpoint", "Endpoint",
                             "Search endpoint URL", BING_SEARCH_ENDPOINT,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
ai_bing_search_init (AiBingSearch *self)
{
    self->api_key  = NULL;
    self->endpoint = NULL;   /* set by the CONSTRUCT default */
    self->session  = soup_session_new ();
    g_object_set (self->session, "timeout", (guint) BING_SESSION_TIMEOUT, NULL);
}

AiBingSearch *
ai_bing_search_new (const gchar *api_key)
{
    g_return_val_if_fail (api_key != NULL, NULL);
    g_return_val_if_fail (*api_key != '\0', NULL);

    return g_object_new (AI_TYPE_BING_SEARCH, "api-key", api_key, NULL);
}
