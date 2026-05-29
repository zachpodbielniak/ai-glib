/*
 * ai-search-result.c - A single web search result
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <gio/gio.h>

#include "convenience/ai-search-result.h"

struct _AiSearchResult
{
    GObject  parent_instance;

    gchar   *title;     /* never NULL ("" if absent) */
    gchar   *url;       /* never NULL ("" if absent) */
    gchar   *snippet;   /* never NULL ("" if absent) */
    gchar   *source;    /* host derived from url; never NULL ("" if none) */
    gchar   *age;       /* nullable: human/ISO date string when known */
    gchar   *content;   /* nullable: fetched page text (fetch_content) */
    guint    rank;      /* 1-based position; 0 until assigned */
};

G_DEFINE_TYPE(AiSearchResult, ai_search_result, G_TYPE_OBJECT)

/* Extract the host ("source domain") from a URL. Returns "" when the URL has
 * no parseable host. Relaxed parsing so odd-but-real result URLs still yield
 * a host instead of being dropped. */
static gchar *
derive_source(const gchar *url)
{
    g_autoptr(GUri)  uri  = NULL;
    const gchar     *host;

    if (url == NULL || *url == '\0')
        return g_strdup("");

    uri = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (uri == NULL)
        return g_strdup("");

    host = g_uri_get_host(uri);
    if (host == NULL)
        return g_strdup("");

    return g_strdup(host);
}

static void
ai_search_result_finalize(GObject *object)
{
    AiSearchResult *self = AI_SEARCH_RESULT(object);

    g_clear_pointer(&self->title, g_free);
    g_clear_pointer(&self->url, g_free);
    g_clear_pointer(&self->snippet, g_free);
    g_clear_pointer(&self->source, g_free);
    g_clear_pointer(&self->age, g_free);
    g_clear_pointer(&self->content, g_free);

    G_OBJECT_CLASS(ai_search_result_parent_class)->finalize(object);
}

static void
ai_search_result_class_init(AiSearchResultClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_search_result_finalize;
}

static void
ai_search_result_init(AiSearchResult *self)
{
    self->title   = g_strdup("");
    self->url     = g_strdup("");
    self->snippet = g_strdup("");
    self->source  = g_strdup("");
    self->age     = NULL;
    self->content = NULL;
    self->rank    = 0;
}

/**
 * ai_search_result_new:
 * @title: (nullable): the result title
 * @url: (nullable): the result URL
 * @snippet: (nullable): a short snippet/description
 *
 * Creates a new #AiSearchResult. The source domain is derived from @url.
 * @age, @content and @rank start unset and may be filled by the provider
 * or by the fetch-content enrichment step.
 *
 * Returns: (transfer full): a new #AiSearchResult
 */
AiSearchResult *
ai_search_result_new(
    const gchar *title,
    const gchar *url,
    const gchar *snippet
){
    AiSearchResult *self = g_object_new(AI_TYPE_SEARCH_RESULT, NULL);

    ai_search_result_set_title(self, title);
    ai_search_result_set_url(self, url);
    ai_search_result_set_snippet(self, snippet);

    return self;
}

/**
 * ai_search_result_get_title:
 * @self: an #AiSearchResult
 *
 * Returns: (transfer none): the title (never %NULL; "" if absent)
 */
const gchar *
ai_search_result_get_title(AiSearchResult *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_RESULT(self), NULL);
    return self->title;
}

/**
 * ai_search_result_get_url:
 * @self: an #AiSearchResult
 *
 * Returns: (transfer none): the URL (never %NULL; "" if absent)
 */
const gchar *
ai_search_result_get_url(AiSearchResult *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_RESULT(self), NULL);
    return self->url;
}

/**
 * ai_search_result_get_snippet:
 * @self: an #AiSearchResult
 *
 * Returns: (transfer none): the snippet (never %NULL; "" if absent)
 */
const gchar *
ai_search_result_get_snippet(AiSearchResult *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_RESULT(self), NULL);
    return self->snippet;
}

/**
 * ai_search_result_get_age:
 * @self: an #AiSearchResult
 *
 * Returns: (transfer none) (nullable): the age/date string, or %NULL if unknown
 */
const gchar *
ai_search_result_get_age(AiSearchResult *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_RESULT(self), NULL);
    return self->age;
}

/**
 * ai_search_result_get_source:
 * @self: an #AiSearchResult
 *
 * Returns: (transfer none): the source host derived from the URL (never %NULL)
 */
const gchar *
ai_search_result_get_source(AiSearchResult *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_RESULT(self), NULL);
    return self->source;
}

/**
 * ai_search_result_get_rank:
 * @self: an #AiSearchResult
 *
 * Returns: the 1-based rank, or 0 if unassigned
 */
guint
ai_search_result_get_rank(AiSearchResult *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_RESULT(self), 0);
    return self->rank;
}

/**
 * ai_search_result_get_content:
 * @self: an #AiSearchResult
 *
 * Returns: (transfer none) (nullable): fetched page text, or %NULL if not fetched
 */
const gchar *
ai_search_result_get_content(AiSearchResult *self)
{
    g_return_val_if_fail(AI_IS_SEARCH_RESULT(self), NULL);
    return self->content;
}

/**
 * ai_search_result_set_title:
 * @self: an #AiSearchResult
 * @title: (nullable): the title (%NULL becomes "")
 */
void
ai_search_result_set_title(
    AiSearchResult *self,
    const gchar    *title
){
    g_return_if_fail(AI_IS_SEARCH_RESULT(self));
    g_clear_pointer(&self->title, g_free);
    self->title = g_strdup(title != NULL ? title : "");
}

/**
 * ai_search_result_set_url:
 * @self: an #AiSearchResult
 * @url: (nullable): the URL (%NULL becomes ""); the source domain is re-derived
 */
void
ai_search_result_set_url(
    AiSearchResult *self,
    const gchar    *url
){
    g_return_if_fail(AI_IS_SEARCH_RESULT(self));
    g_clear_pointer(&self->url, g_free);
    self->url = g_strdup(url != NULL ? url : "");
    g_clear_pointer(&self->source, g_free);
    self->source = derive_source(self->url);
}

/**
 * ai_search_result_set_snippet:
 * @self: an #AiSearchResult
 * @snippet: (nullable): the snippet (%NULL becomes "")
 */
void
ai_search_result_set_snippet(
    AiSearchResult *self,
    const gchar    *snippet
){
    g_return_if_fail(AI_IS_SEARCH_RESULT(self));
    g_clear_pointer(&self->snippet, g_free);
    self->snippet = g_strdup(snippet != NULL ? snippet : "");
}

/**
 * ai_search_result_set_age:
 * @self: an #AiSearchResult
 * @age: (nullable): the age/date string, or %NULL to clear
 */
void
ai_search_result_set_age(
    AiSearchResult *self,
    const gchar    *age
){
    g_return_if_fail(AI_IS_SEARCH_RESULT(self));
    g_clear_pointer(&self->age, g_free);
    self->age = (age != NULL && *age != '\0') ? g_strdup(age) : NULL;
}

/**
 * ai_search_result_set_source:
 * @self: an #AiSearchResult
 * @source: (nullable): override the source host (%NULL becomes "")
 */
void
ai_search_result_set_source(
    AiSearchResult *self,
    const gchar    *source
){
    g_return_if_fail(AI_IS_SEARCH_RESULT(self));
    g_clear_pointer(&self->source, g_free);
    self->source = g_strdup(source != NULL ? source : "");
}

/**
 * ai_search_result_set_rank:
 * @self: an #AiSearchResult
 * @rank: the 1-based rank
 */
void
ai_search_result_set_rank(
    AiSearchResult *self,
    guint           rank
){
    g_return_if_fail(AI_IS_SEARCH_RESULT(self));
    self->rank = rank;
}

/**
 * ai_search_result_set_content:
 * @self: an #AiSearchResult
 * @content: (nullable): fetched page text, or %NULL to clear
 */
void
ai_search_result_set_content(
    AiSearchResult *self,
    const gchar    *content
){
    g_return_if_fail(AI_IS_SEARCH_RESULT(self));
    g_clear_pointer(&self->content, g_free);
    self->content = (content != NULL && *content != '\0')
                    ? g_strdup(content) : NULL;
}
