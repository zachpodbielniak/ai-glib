/*
 * ai-duckduckgo-search.c - Keyless DuckDuckGo web search provider
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include <string.h>
#include <libsoup/soup.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include "convenience/ai-duckduckgo-search.h"
#include "convenience/ai-search-provider.h"
#include "core/ai-error.h"

#define DDG_ENDPOINT        "https://lite.duckduckgo.com/lite/"
#define DDG_DEFAULT_COUNT   10
#define DDG_SESSION_TIMEOUT 20
#define DDG_MAX_ATTEMPTS    2
/* A browser-like User-Agent: the keyless endpoints serve datacenter clients
 * an interstitial when the UA looks like a bot. */
#define DDG_USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"

struct _AiDuckDuckGoSearch
{
    GObject       parent_instance;
    gchar        *endpoint;    /* owned; defaults to DDG_ENDPOINT */
    SoupSession  *session;     /* owned */
};

static void ai_duckduckgo_search_iface_init (AiSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (
    AiDuckDuckGoSearch,
    ai_duckduckgo_search,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (AI_TYPE_SEARCH_PROVIDER,
                           ai_duckduckgo_search_iface_init)
)

enum
{
    PROP_0,
    PROP_ENDPOINT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* ---- HTML parsing helpers ---- */

static gboolean
ddg_class_contains (xmlNode *node, const gchar *token)
{
    xmlChar  *cls;
    gboolean  found = FALSE;

    cls = xmlGetProp (node, (const xmlChar *) "class");
    if (cls != NULL)
    {
        found = (strstr ((const gchar *) cls, token) != NULL);
        xmlFree (cls);
    }
    return found;
}

/* Gather all descendant text into ACC. */
static void
ddg_collect_text (xmlNode *node, GString *acc)
{
    xmlNode *cur;

    for (cur = node; cur != NULL; cur = cur->next)
    {
        if (cur->type == XML_TEXT_NODE)
        {
            if (cur->content != NULL)
                g_string_append (acc, (const gchar *) cur->content);
        }
        else if (cur->type == XML_ELEMENT_NODE)
        {
            ddg_collect_text (cur->children, acc);
        }
    }
}

/* Normalized (whitespace-collapsed, trimmed) text of a node's subtree. */
static gchar *
ddg_node_text (xmlNode *node)
{
    g_autoptr(GString)  raw   = g_string_new (NULL);
    GString            *clean = g_string_new (NULL);
    const gchar        *p;
    gboolean            in_space = FALSE;

    ddg_collect_text (node, raw);

    for (p = raw->str; *p != '\0'; p++)
    {
        if (g_ascii_isspace (*p))
        {
            in_space = TRUE;
        }
        else
        {
            if (in_space && clean->len > 0)
                g_string_append_c (clean, ' ');
            in_space = FALSE;
            g_string_append_c (clean, *p);
        }
    }

    return g_string_free (clean, FALSE);
}

/* Decode a DuckDuckGo result href into a real URL. Handles the
 * /l/?uddg=<pct-encoded> redirect wrapper, protocol-relative "//host", and
 * already-direct hrefs. */
static gchar *
ddg_decode_href (const gchar *href)
{
    const gchar *u;

    if (href == NULL || *href == '\0')
        return NULL;

    u = strstr (href, "uddg=");
    if (u != NULL)
    {
        const gchar      *val = u + 5;
        const gchar      *amp = strchr (val, '&');
        gsize             len = (amp != NULL) ? (gsize) (amp - val)
                                              : strlen (val);
        g_autofree gchar *enc = g_strndup (val, len);
        gchar            *dec = g_uri_unescape_string (enc, NULL);

        return (dec != NULL) ? dec : g_strdup (enc);
    }

    if (g_str_has_prefix (href, "//"))
        return g_strconcat ("https:", href, NULL);

    return g_strdup (href);
}

typedef struct
{
    GList          *results;   /* GList<AiSearchResult> */
    AiSearchResult *last;       /* most-recent result awaiting a snippet */
} DdgParseCtx;

/* An <a> is a result link if it carries DDG's result class or its href is a
 * /l/?uddg= redirect (robust against class-name churn). */
static gboolean
ddg_is_result_anchor (xmlNode *node, const xmlChar *href)
{
    if (ddg_class_contains (node, "result-link") ||
        ddg_class_contains (node, "result__a"))
        return TRUE;
    if (href != NULL && strstr ((const gchar *) href, "uddg=") != NULL)
        return TRUE;
    return FALSE;
}

static void
ddg_walk (xmlNode *node, DdgParseCtx *ctx)
{
    xmlNode *cur;

    for (cur = node; cur != NULL; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
            continue;

        if (cur->name != NULL &&
            g_ascii_strcasecmp ((const gchar *) cur->name, "a") == 0)
        {
            xmlChar *href = xmlGetProp (cur, (const xmlChar *) "href");

            if (ddg_is_result_anchor (cur, href))
            {
                g_autofree gchar *url   = ddg_decode_href ((const gchar *) href);
                g_autofree gchar *title = ddg_node_text (cur);

                if (url != NULL && *url != '\0' &&
                    title != NULL && *title != '\0')
                {
                    AiSearchResult *res =
                        ai_search_result_new (title, url, NULL);

                    ctx->results = g_list_append (ctx->results, res);
                    ctx->last = res;
                }

                if (href != NULL)
                    xmlFree (href);
                continue;   /* don't descend into the anchor */
            }

            if (href != NULL)
                xmlFree (href);
        }
        else if (ddg_class_contains (cur, "result-snippet") ||
                 ddg_class_contains (cur, "result__snippet"))
        {
            if (ctx->last != NULL &&
                (ai_search_result_get_snippet (ctx->last) == NULL ||
                 *ai_search_result_get_snippet (ctx->last) == '\0'))
            {
                g_autofree gchar *snip = ddg_node_text (cur);

                ai_search_result_set_snippet (ctx->last, snip);
            }
            continue;
        }

        ddg_walk (cur->children, ctx);
    }
}

/* Free results past INDEX (0-based keep count), returning the truncated head. */
static GList *
ddg_truncate (GList *results, guint keep)
{
    GList *node;
    guint  i = 0;

    for (node = results; node != NULL; node = node->next, i++)
    {
        if (i == keep)
        {
            /* Split: free node..end, terminate the kept prefix. */
            if (node->prev != NULL)
                node->prev->next = NULL;
            else
                results = NULL;
            node->prev = NULL;
            g_list_free_full (node, g_object_unref);
            break;
        }
    }
    return results;
}

/* ---- search ---- */

static const gchar *
ddg_df (AiSearchFreshness f)
{
    switch (f)
    {
        case AI_SEARCH_FRESHNESS_DAY:   return "d";
        case AI_SEARCH_FRESHNESS_WEEK:  return "w";
        case AI_SEARCH_FRESHNESS_MONTH: return "m";
        case AI_SEARCH_FRESHNESS_YEAR:  return "y";
        default:                        return NULL;
    }
}

/* DuckDuckGo kp: strict=1, moderate=-1, off=-2 (best-effort). */
static const gchar *
ddg_kp (AiSearchSafeSearch s)
{
    switch (s)
    {
        case AI_SEARCH_SAFE_STRICT: return "1";
        case AI_SEARCH_SAFE_OFF:    return "-2";
        default:                    return "-1";
    }
}

static gchar *
build_ddg_body (const gchar *query, AiSearchOptions *options)
{
    GString          *body;
    g_autofree gchar *raw_query = NULL;
    g_autofree gchar *q_enc     = NULL;
    const gchar      *site      = NULL;
    const gchar      *country   = NULL;
    const gchar      *language  = NULL;
    const gchar      *df        = NULL;
    const gchar      *kp        = "-1";

    if (options != NULL)
    {
        site     = ai_search_options_get_site (options);
        country  = ai_search_options_get_country (options);
        language = ai_search_options_get_language (options);
        df       = ddg_df (ai_search_options_get_freshness (options));
        kp       = ddg_kp (ai_search_options_get_safesearch (options));
    }

    if (site != NULL && *site != '\0')
        raw_query = g_strdup_printf ("%s site:%s", query, site);
    else
        raw_query = g_strdup (query);

    q_enc = g_uri_escape_string (raw_query, NULL, FALSE);

    body = g_string_new (NULL);
    g_string_append_printf (body, "q=%s", q_enc);
    if (df != NULL)
        g_string_append_printf (body, "&df=%s", df);
    g_string_append_printf (body, "&kp=%s", kp);

    /* Region: DDG's kl is "<country>-<lang>", e.g. "us-en". Best-effort. */
    if (country != NULL && *country != '\0')
    {
        g_autofree gchar *cc = g_ascii_strdown (country, -1);
        g_autofree gchar *lang = (language != NULL && *language != '\0')
                                 ? g_ascii_strdown (language, -1)
                                 : g_strdup ("en");
        g_autofree gchar *kl = g_strdup_printf ("%s-%s", cc, lang);
        g_autofree gchar *kl_enc = g_uri_escape_string (kl, NULL, FALSE);

        g_string_append_printf (body, "&kl=%s", kl_enc);
    }

    return g_string_free (body, FALSE);
}

static GList *
ai_duckduckgo_search_do_search (
    AiSearchProvider  *provider,
    const gchar       *query,
    AiSearchOptions   *options,
    GCancellable      *cancellable,
    GError           **error
){
    AiDuckDuckGoSearch *self = AI_DUCKDUCKGO_SEARCH (provider);
    g_autofree gchar   *body = NULL;
    g_autoptr(GBytes)   resp = NULL;
    guint               attempt;
    guint               keep = DDG_DEFAULT_COUNT;
    const gchar        *data;
    gsize               size;
    htmlDocPtr          doc;
    xmlNode            *root;
    DdgParseCtx         ctx = { NULL, NULL };
    GList              *node;
    guint               rank;

    if (options != NULL && ai_search_options_get_count (options) > 0)
        keep = ai_search_options_get_count (options);

    body = build_ddg_body (query, options);

    /* Best-effort: a transient failure yields an empty result list, not an
     * error, so DuckDuckGo flakiness can't derail the agent loop. Only
     * cancellation propagates as an error. */
    for (attempt = 0; attempt < DDG_MAX_ATTEMPTS; attempt++)
    {
        g_autoptr(SoupMessage) msg       = NULL;
        g_autoptr(GError)      local_err = NULL;
        g_autoptr(GBytes)      req_body  = NULL;
        SoupMessageHeaders    *headers;
        guint                  status;

        if (g_cancellable_set_error_if_cancelled (cancellable, error))
            return NULL;

        msg = soup_message_new ("POST", self->endpoint);
        if (msg == NULL)
            return NULL;   /* malformed endpoint: degrade to empty */

        headers = soup_message_get_request_headers (msg);
        soup_message_headers_replace (headers, "User-Agent", DDG_USER_AGENT);
        soup_message_headers_replace (headers, "Accept",
                                      "text/html,application/xhtml+xml");

        req_body = g_bytes_new (body, strlen (body));
        soup_message_set_request_body_from_bytes (
            msg, "application/x-www-form-urlencoded", req_body);

        resp = soup_session_send_and_read (self->session, msg, cancellable,
                                            &local_err);

        if (resp == NULL)
        {
            if (g_error_matches (local_err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
                g_propagate_error (error, g_steal_pointer (&local_err));
                return NULL;
            }
            if (attempt + 1 < DDG_MAX_ATTEMPTS)
                continue;
            return NULL;   /* degrade to empty */
        }

        status = soup_message_get_status (msg);
        if (status < 200 || status >= 300)
        {
            g_clear_pointer (&resp, g_bytes_unref);
            if (attempt + 1 < DDG_MAX_ATTEMPTS)
                continue;
            return NULL;   /* degrade to empty */
        }

        break;   /* got a 2xx body */
    }

    if (resp == NULL)
        return NULL;

    data = g_bytes_get_data (resp, &size);

    doc = htmlReadMemory (data, (int) size, NULL, "UTF-8",
                          HTML_PARSE_RECOVER | HTML_PARSE_NOERROR
                          | HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (doc == NULL)
        return NULL;

    root = xmlDocGetRootElement (doc);
    if (root != NULL)
        ddg_walk (root, &ctx);
    xmlFreeDoc (doc);

    ctx.results = ddg_truncate (ctx.results, keep);

    rank = 1;
    for (node = ctx.results; node != NULL; node = node->next)
        ai_search_result_set_rank (AI_SEARCH_RESULT (node->data), rank++);

    return ctx.results;
}

static void
ai_duckduckgo_search_iface_init (AiSearchProviderInterface *iface)
{
    iface->search = ai_duckduckgo_search_do_search;
}

static void
ai_duckduckgo_search_get_property (
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiDuckDuckGoSearch *self = AI_DUCKDUCKGO_SEARCH (object);

    switch (prop_id)
    {
        case PROP_ENDPOINT:
            g_value_set_string (value, self->endpoint);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
ai_duckduckgo_search_set_property (
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiDuckDuckGoSearch *self = AI_DUCKDUCKGO_SEARCH (object);

    switch (prop_id)
    {
        case PROP_ENDPOINT:
            g_clear_pointer (&self->endpoint, g_free);
            self->endpoint = g_value_dup_string (value);
            if (self->endpoint == NULL || *self->endpoint == '\0')
            {
                g_clear_pointer (&self->endpoint, g_free);
                self->endpoint = g_strdup (DDG_ENDPOINT);
            }
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
ai_duckduckgo_search_finalize (GObject *object)
{
    AiDuckDuckGoSearch *self = AI_DUCKDUCKGO_SEARCH (object);

    g_clear_pointer (&self->endpoint, g_free);
    g_clear_object  (&self->session);

    G_OBJECT_CLASS (ai_duckduckgo_search_parent_class)->finalize (object);
}

static void
ai_duckduckgo_search_class_init (AiDuckDuckGoSearchClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize     = ai_duckduckgo_search_finalize;
    object_class->get_property = ai_duckduckgo_search_get_property;
    object_class->set_property = ai_duckduckgo_search_set_property;

    /**
     * AiDuckDuckGoSearch:endpoint:
     *
     * The DuckDuckGo HTML endpoint URL. Defaults to the public "lite"
     * endpoint; override for a proxy or for testing.
     */
    properties[PROP_ENDPOINT] =
        g_param_spec_string ("endpoint", "Endpoint",
                             "DuckDuckGo HTML endpoint URL", DDG_ENDPOINT,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
ai_duckduckgo_search_init (AiDuckDuckGoSearch *self)
{
    self->endpoint = NULL;   /* set by the CONSTRUCT default */
    self->session  = soup_session_new ();
    g_object_set (self->session, "timeout", (guint) DDG_SESSION_TIMEOUT, NULL);
}

AiDuckDuckGoSearch *
ai_duckduckgo_search_new (void)
{
    return g_object_new (AI_TYPE_DUCKDUCKGO_SEARCH, NULL);
}
