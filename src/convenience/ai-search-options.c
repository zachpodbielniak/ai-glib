/*
 * ai-search-options.c - Tunable options for a web search
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "convenience/ai-search-options.h"

#define AI_SEARCH_DEFAULT_COUNT       10
#define AI_SEARCH_DEFAULT_FETCH_COUNT 3

struct _AiSearchOptions
{
    GObject             parent_instance;

    guint               count;
    AiSearchFreshness   freshness;
    AiSearchSafeSearch  safesearch;
    gchar              *country;       /* nullable */
    gchar              *language;      /* nullable */
    gchar              *site;          /* nullable */
    guint               offset;
    gboolean            fetch_content;
    guint               fetch_count;
};

G_DEFINE_TYPE(AiSearchOptions, ai_search_options, G_TYPE_OBJECT)

enum
{
    PROP_0,
    PROP_COUNT,
    PROP_FRESHNESS,
    PROP_SAFESEARCH,
    PROP_COUNTRY,
    PROP_LANGUAGE,
    PROP_SITE,
    PROP_OFFSET,
    PROP_FETCH_CONTENT,
    PROP_FETCH_COUNT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_search_options_finalize(GObject *object)
{
    AiSearchOptions *self = AI_SEARCH_OPTIONS(object);

    g_clear_pointer(&self->country, g_free);
    g_clear_pointer(&self->language, g_free);
    g_clear_pointer(&self->site, g_free);

    G_OBJECT_CLASS(ai_search_options_parent_class)->finalize(object);
}

static void
ai_search_options_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiSearchOptions *self = AI_SEARCH_OPTIONS(object);

    switch (prop_id)
    {
        case PROP_COUNT:
            g_value_set_uint(value, self->count);
            break;
        case PROP_FRESHNESS:
            g_value_set_enum(value, self->freshness);
            break;
        case PROP_SAFESEARCH:
            g_value_set_enum(value, self->safesearch);
            break;
        case PROP_COUNTRY:
            g_value_set_string(value, self->country);
            break;
        case PROP_LANGUAGE:
            g_value_set_string(value, self->language);
            break;
        case PROP_SITE:
            g_value_set_string(value, self->site);
            break;
        case PROP_OFFSET:
            g_value_set_uint(value, self->offset);
            break;
        case PROP_FETCH_CONTENT:
            g_value_set_boolean(value, self->fetch_content);
            break;
        case PROP_FETCH_COUNT:
            g_value_set_uint(value, self->fetch_count);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_search_options_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiSearchOptions *self = AI_SEARCH_OPTIONS(object);

    switch (prop_id)
    {
        case PROP_COUNT:
            ai_search_options_set_count(self, g_value_get_uint(value));
            break;
        case PROP_FRESHNESS:
            ai_search_options_set_freshness(self, g_value_get_enum(value));
            break;
        case PROP_SAFESEARCH:
            ai_search_options_set_safesearch(self, g_value_get_enum(value));
            break;
        case PROP_COUNTRY:
            ai_search_options_set_country(self, g_value_get_string(value));
            break;
        case PROP_LANGUAGE:
            ai_search_options_set_language(self, g_value_get_string(value));
            break;
        case PROP_SITE:
            ai_search_options_set_site(self, g_value_get_string(value));
            break;
        case PROP_OFFSET:
            ai_search_options_set_offset(self, g_value_get_uint(value));
            break;
        case PROP_FETCH_CONTENT:
            ai_search_options_set_fetch_content(self, g_value_get_boolean(value));
            break;
        case PROP_FETCH_COUNT:
            ai_search_options_set_fetch_count(self, g_value_get_uint(value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_search_options_class_init(AiSearchOptionsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_search_options_finalize;
    object_class->get_property = ai_search_options_get_property;
    object_class->set_property = ai_search_options_set_property;

    /**
     * AiSearchOptions:count:
     *
     * Maximum number of results to request (default 10).
     */
    properties[PROP_COUNT] =
        g_param_spec_uint("count", "Count",
                          "Maximum number of results to request",
                          0, 100, AI_SEARCH_DEFAULT_COUNT,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:freshness:
     *
     * Recency filter for results (default %AI_SEARCH_FRESHNESS_ANY).
     */
    properties[PROP_FRESHNESS] =
        g_param_spec_enum("freshness", "Freshness",
                          "Recency filter for results",
                          AI_TYPE_SEARCH_FRESHNESS, AI_SEARCH_FRESHNESS_ANY,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:safesearch:
     *
     * Safe-search level (default %AI_SEARCH_SAFE_MODERATE).
     */
    properties[PROP_SAFESEARCH] =
        g_param_spec_enum("safesearch", "Safe search",
                          "Safe-search filtering level",
                          AI_TYPE_SEARCH_SAFE_SEARCH, AI_SEARCH_SAFE_MODERATE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:country:
     *
     * Two-letter region/country code (e.g. "US"), or %NULL for the default.
     */
    properties[PROP_COUNTRY] =
        g_param_spec_string("country", "Country",
                            "Two-letter region/country code", NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:language:
     *
     * Two-letter language code (e.g. "en"), or %NULL for the default.
     */
    properties[PROP_LANGUAGE] =
        g_param_spec_string("language", "Language",
                            "Two-letter language code", NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:site:
     *
     * Restrict results to a single domain (e.g. "example.com"), or %NULL.
     */
    properties[PROP_SITE] =
        g_param_spec_string("site", "Site",
                            "Restrict results to a single domain", NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:offset:
     *
     * Result offset for pagination (default 0).
     */
    properties[PROP_OFFSET] =
        g_param_spec_uint("offset", "Offset",
                          "Result offset for pagination",
                          0, 1000, 0,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:fetch-content:
     *
     * When %TRUE, the executor fetches the top results' pages and attaches
     * their extracted text to each #AiSearchResult (default %FALSE).
     */
    properties[PROP_FETCH_CONTENT] =
        g_param_spec_boolean("fetch-content", "Fetch content",
                             "Fetch and attach top result page text", FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiSearchOptions:fetch-count:
     *
     * How many top results to fetch when #AiSearchOptions:fetch-content is
     * set (default 3, hard-capped at %AI_SEARCH_MAX_FETCH_COUNT).
     */
    properties[PROP_FETCH_COUNT] =
        g_param_spec_uint("fetch-count", "Fetch count",
                          "How many top results to fetch when fetch-content is set",
                          0, AI_SEARCH_MAX_FETCH_COUNT, AI_SEARCH_DEFAULT_FETCH_COUNT,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_search_options_init(AiSearchOptions *self)
{
    self->count         = AI_SEARCH_DEFAULT_COUNT;
    self->freshness     = AI_SEARCH_FRESHNESS_ANY;
    self->safesearch    = AI_SEARCH_SAFE_MODERATE;
    self->country       = NULL;
    self->language      = NULL;
    self->site          = NULL;
    self->offset        = 0;
    self->fetch_content = FALSE;
    self->fetch_count   = AI_SEARCH_DEFAULT_FETCH_COUNT;
}

/**
 * ai_search_options_new:
 *
 * Creates a new #AiSearchOptions with library defaults.
 *
 * Returns: (transfer full): a new #AiSearchOptions
 */
AiSearchOptions *
ai_search_options_new(void)
{
    return g_object_new(AI_TYPE_SEARCH_OPTIONS, NULL);
}

/**
 * ai_search_options_copy:
 * @self: an #AiSearchOptions
 *
 * Creates an independent copy of @self.
 *
 * Returns: (transfer full): a new #AiSearchOptions with the same values
 */
AiSearchOptions *
ai_search_options_copy(AiSearchOptions *self)
{
    AiSearchOptions *copy;

    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), NULL);

    copy = ai_search_options_new();
    copy->count         = self->count;
    copy->freshness     = self->freshness;
    copy->safesearch    = self->safesearch;
    copy->offset        = self->offset;
    copy->fetch_content = self->fetch_content;
    copy->fetch_count   = self->fetch_count;
    copy->country       = g_strdup(self->country);
    copy->language      = g_strdup(self->language);
    copy->site          = g_strdup(self->site);

    return copy;
}

guint
ai_search_options_get_count(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), AI_SEARCH_DEFAULT_COUNT);
    return self->count;
}

void
ai_search_options_set_count(
    AiSearchOptions *self,
    guint            count
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    self->count = count;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_COUNT]);
}

AiSearchFreshness
ai_search_options_get_freshness(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), AI_SEARCH_FRESHNESS_ANY);
    return self->freshness;
}

void
ai_search_options_set_freshness(
    AiSearchOptions   *self,
    AiSearchFreshness  freshness
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    self->freshness = freshness;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_FRESHNESS]);
}

AiSearchSafeSearch
ai_search_options_get_safesearch(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), AI_SEARCH_SAFE_MODERATE);
    return self->safesearch;
}

void
ai_search_options_set_safesearch(
    AiSearchOptions    *self,
    AiSearchSafeSearch  safesearch
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    self->safesearch = safesearch;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SAFESEARCH]);
}

const gchar *
ai_search_options_get_country(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), NULL);
    return self->country;
}

void
ai_search_options_set_country(
    AiSearchOptions *self,
    const gchar     *country
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    g_clear_pointer(&self->country, g_free);
    self->country = (country != NULL && *country != '\0')
                    ? g_strdup(country) : NULL;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_COUNTRY]);
}

const gchar *
ai_search_options_get_language(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), NULL);
    return self->language;
}

void
ai_search_options_set_language(
    AiSearchOptions *self,
    const gchar     *language
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    g_clear_pointer(&self->language, g_free);
    self->language = (language != NULL && *language != '\0')
                     ? g_strdup(language) : NULL;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LANGUAGE]);
}

const gchar *
ai_search_options_get_site(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), NULL);
    return self->site;
}

void
ai_search_options_set_site(
    AiSearchOptions *self,
    const gchar     *site
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    g_clear_pointer(&self->site, g_free);
    self->site = (site != NULL && *site != '\0') ? g_strdup(site) : NULL;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SITE]);
}

guint
ai_search_options_get_offset(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), 0);
    return self->offset;
}

void
ai_search_options_set_offset(
    AiSearchOptions *self,
    guint            offset
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    self->offset = offset;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_OFFSET]);
}

gboolean
ai_search_options_get_fetch_content(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), FALSE);
    return self->fetch_content;
}

void
ai_search_options_set_fetch_content(
    AiSearchOptions *self,
    gboolean         fetch_content
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    self->fetch_content = fetch_content;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_FETCH_CONTENT]);
}

guint
ai_search_options_get_fetch_count(AiSearchOptions *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_OPTIONS(self), AI_SEARCH_DEFAULT_FETCH_COUNT);
    return self->fetch_count;
}

void
ai_search_options_set_fetch_count(
    AiSearchOptions *self,
    guint            fetch_count
){
    g_return_if_fail(AI_IS_SEARCH_OPTIONS(self));
    /* Hard cap: fetch-content runs sequential blocking fetches, so keep the
     * worst-case latency bounded regardless of caller input. */
    if (fetch_count > AI_SEARCH_MAX_FETCH_COUNT)
        fetch_count = AI_SEARCH_MAX_FETCH_COUNT;
    self->fetch_count = fetch_count;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_FETCH_COUNT]);
}
