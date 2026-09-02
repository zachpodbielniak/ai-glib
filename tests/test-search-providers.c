/*
 * test-search-providers.c - Tests for the ai-glib web search subsystem
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers AiSearchResult, AiSearchOptions, the AiSearchFreshness/SafeSearch
 * enums, the shared robust HTTP helper (retries/backoff/429/status mapping),
 * the Bing/Brave/DuckDuckGo providers, the result formatter, the env factory,
 * and the AiToolExecutor web_search integration (options, caching,
 * fetch_content enrichment).
 *
 * Network is avoided via a configurable raw-socket HTTP/1.1 test server that
 * serves canned bodies and can script a status sequence (e.g. 429-then-200)
 * with Retry-After: 0 so retry tests stay sub-second. Live tests are gated on
 * BING_API_KEY / BRAVE_API_KEY and skipped otherwise.
 */

#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "ai-glib.h"

/* Private helper, exported from the shared library. */
JsonNode *
ai_search_http_get_json (SoupSession *session, const gchar *url,
                         const gchar *const *header_pairs,
                         GCancellable *cancellable, GError **error);

/* ============================================================
 * Configurable raw-socket HTTP/1.1 test server
 * ============================================================ */

typedef struct
{
    GThread *thread;
    GSocket *listen_sock;
    guint    port;
    gint     stop;            /* atomic */
    GMutex   lock;

    /* response config (set before issuing a request) */
    guint    status;          /* success status (default 200) */
    gchar   *content_type;    /* default "application/json" */
    gchar   *body;            /* canned body (owned) */
    guint    fail_status;     /* status returned during the fail window */
    guint    fail_times;      /* serve fail_status this many times first */

    /* capture (guarded by lock) */
    guint    hits;
    gchar   *last_method;
    gchar   *last_target;     /* request target incl. query string */
    gchar   *last_request;    /* full raw request text */
    gchar   *last_body;       /* request body (POST) */
} TServer;

static const gchar *
reason_phrase (guint status)
{
    switch (status)
    {
        case 200: return "OK";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Status";
    }
}

static void
tserver_send_all (GSocket *sock, const gchar *data, gsize len)
{
    gsize off = 0;

    while (off < len)
    {
        gssize n = g_socket_send (sock, data + off, len - off, NULL, NULL);

        if (n <= 0)
            break;
        off += (gsize) n;
    }
}

static void
tserver_handle (TServer *ts, GSocket *conn)
{
    GString          *req = g_string_new (NULL);
    gchar             chunk[2048];
    gssize            n;
    const gchar      *hdr_end;
    glong             content_length = 0;
    const gchar      *cl;
    g_autofree gchar *method = NULL;
    g_autofree gchar *target = NULL;
    g_autofree gchar *body   = NULL;
    g_autofree gchar *resp   = NULL;
    const gchar      *sp1;
    const gchar      *sp2;
    guint             status;
    gboolean          is_fail;
    const gchar      *ctype;
    g_autofree gchar *rbody  = NULL;

    /* Read request headers. */
    while ((hdr_end = g_strstr_len (req->str, (gssize) req->len,
                                    "\r\n\r\n")) == NULL)
    {
        n = g_socket_receive (conn, chunk, sizeof chunk, NULL, NULL);
        if (n <= 0)
            break;
        g_string_append_len (req, chunk, n);
        if (req->len > 256 * 1024)
            break;
    }

    /* Content-Length -> read the body. */
    cl = g_strstr_len (req->str, (gssize) req->len, "Content-Length:");
    if (cl != NULL)
        content_length = strtol (cl + strlen ("Content-Length:"), NULL, 10);

    if (hdr_end != NULL && content_length > 0)
    {
        gsize have = req->len - (gsize) ((hdr_end + 4) - req->str);

        while ((glong) have < content_length)
        {
            n = g_socket_receive (conn, chunk, sizeof chunk, NULL, NULL);
            if (n <= 0)
                break;
            g_string_append_len (req, chunk, n);
            have += (gsize) n;
        }
        hdr_end = g_strstr_len (req->str, (gssize) req->len, "\r\n\r\n");
        if (hdr_end != NULL)
            body = g_strndup (hdr_end + 4, (gsize) content_length);
    }

    /* Parse "METHOD SP target SP HTTP/1.1". */
    sp1 = g_strstr_len (req->str, (gssize) req->len, " ");
    if (sp1 != NULL)
    {
        method = g_strndup (req->str, (gsize) (sp1 - req->str));
        sp2 = strchr (sp1 + 1, ' ');
        if (sp2 != NULL)
            target = g_strndup (sp1 + 1, (gsize) (sp2 - (sp1 + 1)));
    }

    /* Decide the response under the lock, recording the capture. */
    g_mutex_lock (&ts->lock);
    ts->hits++;
    is_fail = (ts->fail_times > 0 && ts->hits <= ts->fail_times);
    status  = is_fail ? ts->fail_status : ts->status;
    ctype   = (ts->content_type != NULL) ? ts->content_type
                                          : "application/json";
    rbody   = g_strdup (ts->body != NULL ? ts->body : "");
    g_clear_pointer (&ts->last_method, g_free);
    g_clear_pointer (&ts->last_target, g_free);
    g_clear_pointer (&ts->last_request, g_free);
    g_clear_pointer (&ts->last_body, g_free);
    ts->last_method  = g_strdup (method != NULL ? method : "");
    ts->last_target  = g_strdup (target != NULL ? target : "");
    ts->last_request = g_strdup (req->str);
    ts->last_body    = g_strdup (body != NULL ? body : "");
    g_mutex_unlock (&ts->lock);

    if (is_fail && status == 429)
        resp = g_strdup_printf (
            "HTTP/1.1 %u %s\r\nContent-Type: text/plain\r\n"
            "Retry-After: 0\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            status, reason_phrase (status));
    else if (is_fail)
        resp = g_strdup_printf (
            "HTTP/1.1 %u %s\r\nContent-Type: text/plain\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n",
            status, reason_phrase (status));
    else
        resp = g_strdup_printf (
            "HTTP/1.1 %u %s\r\nContent-Type: %s\r\n"
            "Content-Length: %lu\r\nConnection: close\r\n\r\n%s",
            status, reason_phrase (status), ctype,
            (gulong) strlen (rbody), rbody);

    tserver_send_all (conn, resp, strlen (resp));
    g_string_free (req, TRUE);
}

static gpointer
tserver_thread (gpointer data)
{
    TServer *ts = data;

    while (!g_atomic_int_get (&ts->stop))
    {
        GSocket *conn = g_socket_accept (ts->listen_sock, NULL, NULL);

        if (conn == NULL)
            continue;
        if (g_atomic_int_get (&ts->stop))
        {
            g_object_unref (conn);
            break;
        }
        g_socket_set_timeout (conn, 2);
        tserver_handle (ts, conn);
        g_socket_close (conn, NULL);
        g_object_unref (conn);
    }
    return NULL;
}

static TServer *
tserver_start (void)
{
    TServer        *ts  = g_new0 (TServer, 1);
    GError         *err = NULL;
    GInetAddress   *ia;
    GSocketAddress *bind_addr;
    GSocketAddress *local_addr;

    g_mutex_init (&ts->lock);
    ts->status       = 200;
    ts->content_type = g_strdup ("application/json");
    ts->body         = g_strdup ("");
    ts->fail_status  = 429;

    ts->listen_sock = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                                    G_SOCKET_PROTOCOL_TCP, &err);
    g_assert_no_error (err);
    g_socket_set_timeout (ts->listen_sock, 1);

    ia        = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    bind_addr = g_inet_socket_address_new (ia, 0);
    g_object_unref (ia);
    g_socket_bind (ts->listen_sock, bind_addr, TRUE, &err);
    g_object_unref (bind_addr);
    g_assert_no_error (err);
    g_socket_listen (ts->listen_sock, &err);
    g_assert_no_error (err);

    local_addr = g_socket_get_local_address (ts->listen_sock, &err);
    g_assert_no_error (err);
    ts->port = g_inet_socket_address_get_port (
        G_INET_SOCKET_ADDRESS (local_addr));
    g_object_unref (local_addr);

    ts->thread = g_thread_new ("ai-glib-test-search", tserver_thread, ts);
    return ts;
}

static void
tserver_stop (TServer *ts)
{
    GSocket        *poke;
    GInetAddress   *ia;
    GSocketAddress *addr;

    g_atomic_int_set (&ts->stop, 1);

    poke = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                         G_SOCKET_PROTOCOL_TCP, NULL);
    if (poke != NULL)
    {
        ia   = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
        addr = g_inet_socket_address_new (ia, ts->port);
        g_object_unref (ia);
        g_socket_set_timeout (poke, 1);
        g_socket_connect (poke, addr, NULL, NULL);
        g_object_unref (addr);
        g_socket_close (poke, NULL);
        g_object_unref (poke);
    }

    g_thread_join (ts->thread);

    g_socket_close (ts->listen_sock, NULL);
    g_object_unref (ts->listen_sock);
    g_mutex_clear (&ts->lock);
    g_free (ts->content_type);
    g_free (ts->body);
    g_free (ts->last_method);
    g_free (ts->last_target);
    g_free (ts->last_request);
    g_free (ts->last_body);
    g_free (ts);
}

static void
tserver_set_response (TServer *ts, guint status, const gchar *ctype,
                      const gchar *body)
{
    g_mutex_lock (&ts->lock);
    ts->status = status;
    g_clear_pointer (&ts->content_type, g_free);
    ts->content_type = g_strdup (ctype);
    g_clear_pointer (&ts->body, g_free);
    ts->body = g_strdup (body != NULL ? body : "");
    g_mutex_unlock (&ts->lock);
}

static void
tserver_set_fail (TServer *ts, guint fail_status, guint fail_times)
{
    g_mutex_lock (&ts->lock);
    ts->fail_status = fail_status;
    ts->fail_times  = fail_times;
    g_mutex_unlock (&ts->lock);
}

static guint
tserver_hits (TServer *ts)
{
    guint h;

    g_mutex_lock (&ts->lock);
    h = ts->hits;
    g_mutex_unlock (&ts->lock);
    return h;
}

static gchar *
tserver_take_target (TServer *ts)
{
    gchar *t;

    g_mutex_lock (&ts->lock);
    t = g_strdup (ts->last_target);
    g_mutex_unlock (&ts->lock);
    return t;
}

static gchar *
tserver_take_request (TServer *ts)
{
    gchar *r;

    g_mutex_lock (&ts->lock);
    r = g_strdup (ts->last_request);
    g_mutex_unlock (&ts->lock);
    return r;
}

static gchar *
tserver_take_body (TServer *ts)
{
    gchar *b;

    g_mutex_lock (&ts->lock);
    b = g_strdup (ts->last_body);
    g_mutex_unlock (&ts->lock);
    return b;
}

static gchar *
tserver_url (TServer *ts, const gchar *path)
{
    return g_strdup_printf ("http://127.0.0.1:%u%s", ts->port, path);
}

/* ---- black hole: accepts at the kernel level, never replies ---- */

typedef struct { GSocket *socket; guint port; } BlackHole;

static BlackHole *
black_hole_start (void)
{
    BlackHole      *bh  = g_new0 (BlackHole, 1);
    GError         *err = NULL;
    GInetAddress   *ia;
    GSocketAddress *bind_addr;
    GSocketAddress *local_addr;

    bh->socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                               G_SOCKET_PROTOCOL_TCP, &err);
    g_assert_no_error (err);
    ia        = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    bind_addr = g_inet_socket_address_new (ia, 0);
    g_object_unref (ia);
    g_socket_bind (bh->socket, bind_addr, TRUE, &err);
    g_object_unref (bind_addr);
    g_assert_no_error (err);
    g_socket_listen (bh->socket, &err);
    g_assert_no_error (err);
    local_addr = g_socket_get_local_address (bh->socket, &err);
    g_assert_no_error (err);
    bh->port = g_inet_socket_address_get_port (
        G_INET_SOCKET_ADDRESS (local_addr));
    g_object_unref (local_addr);
    return bh;
}

static void
black_hole_stop (BlackHole *bh)
{
    g_socket_close (bh->socket, NULL);
    g_object_unref (bh->socket);
    g_free (bh);
}

/* ============================================================
 * Canned payloads
 * ============================================================ */

static const gchar *BING_JSON_2 =
    "{\"webPages\":{\"value\":["
    "{\"name\":\"Bing One\",\"url\":\"https://one.example/a\","
    "\"snippet\":\"First snippet\",\"dateLastCrawled\":\"2024-01-02\"},"
    "{\"name\":\"Bing Two\",\"url\":\"https://two.example/b\","
    "\"snippet\":\"Second snippet\"}"
    "]}}";

static const gchar *BRAVE_JSON_2 =
    "{\"web\":{\"results\":["
    "{\"title\":\"Brave One\",\"url\":\"https://one.example/a\","
    "\"description\":\"First desc\",\"page_age\":\"2024-03-04\"},"
    "{\"title\":\"Brave Two\",\"url\":\"https://two.example/b\","
    "\"description\":\"Second desc\"}"
    "]}}";

static const gchar *DDG_HTML =
    "<html><body><table>"
    "<tr><td>1.</td><td>"
    "<a rel=\"nofollow\" class=\"result-link\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fone&rut=x\">"
    "First Result</a></td></tr>"
    "<tr><td></td><td class=\"result-snippet\">Snippet one.</td></tr>"
    "<tr><td>2.</td><td>"
    "<a class=\"result-link\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Ftwo\">"
    "Second Result</a></td></tr>"
    "<tr><td></td><td class=\"result-snippet\">Snippet two with &amp; sign."
    "</td></tr>"
    "<tr><td>3.</td><td>"
    "<a class=\"result-link\" href=\"https://direct.example/three\">"
    "Third Direct</a></td></tr>"
    "<tr><td></td><td class=\"result-snippet\">Snippet three.</td></tr>"
    "<tr><td>4.</td><td>"
    "<a class=\"result-link\" href=\"//proto.example/four\">"
    "Fourth Proto</a></td></tr>"
    "<tr><td></td><td class=\"result-snippet\">Snippet four.</td></tr>"
    "</table></body></html>";

/* ============================================================
 * AiSearchResult
 * ============================================================ */

static void
test_result_basic (void)
{
    g_autoptr(AiSearchResult) r =
        ai_search_result_new ("Title", "https://example.com/p?x=1", "snip");

    g_assert_cmpstr (ai_search_result_get_title (r), ==, "Title");
    g_assert_cmpstr (ai_search_result_get_url (r), ==,
                     "https://example.com/p?x=1");
    g_assert_cmpstr (ai_search_result_get_snippet (r), ==, "snip");
    g_assert_cmpstr (ai_search_result_get_source (r), ==, "example.com");
    g_assert_null (ai_search_result_get_age (r));
    g_assert_null (ai_search_result_get_content (r));
    g_assert_cmpuint (ai_search_result_get_rank (r), ==, 0);

    ai_search_result_set_rank (r, 3);
    ai_search_result_set_age (r, "2 days ago");
    ai_search_result_set_content (r, "page text");
    g_assert_cmpuint (ai_search_result_get_rank (r), ==, 3);
    g_assert_cmpstr (ai_search_result_get_age (r), ==, "2 days ago");
    g_assert_cmpstr (ai_search_result_get_content (r), ==, "page text");

    /* Empty age/content clears back to NULL. */
    ai_search_result_set_age (r, "");
    ai_search_result_set_content (r, "");
    g_assert_null (ai_search_result_get_age (r));
    g_assert_null (ai_search_result_get_content (r));
}

static void
test_result_null_fields (void)
{
    g_autoptr(AiSearchResult) r = ai_search_result_new (NULL, NULL, NULL);

    /* NULL inputs become "" (never NULL) for title/url/snippet/source. */
    g_assert_cmpstr (ai_search_result_get_title (r), ==, "");
    g_assert_cmpstr (ai_search_result_get_url (r), ==, "");
    g_assert_cmpstr (ai_search_result_get_snippet (r), ==, "");
    g_assert_cmpstr (ai_search_result_get_source (r), ==, "");
}

static void
test_result_source_derivation (void)
{
    struct { const gchar *url; const gchar *host; } cases[] = {
        { "https://example.com/path",        "example.com" },
        { "http://sub.example.org",          "sub.example.org" },
        { "https://example.com:8443/x",      "example.com" },
        { "https://user:pw@host.example/p",  "host.example" },
        { "http://192.168.0.1/a",            "192.168.0.1" },
        { "not a url",                       "" },
        { "",                                "" }
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
        g_autoptr(AiSearchResult) r =
            ai_search_result_new ("t", cases[i].url, "s");

        g_assert_cmpstr (ai_search_result_get_source (r), ==, cases[i].host);
    }

    /* IPv6 literal: host comes back without brackets. */
    {
        g_autoptr(AiSearchResult) r =
            ai_search_result_new ("t", "https://[::1]:9000/z", "s");
        const gchar *host = ai_search_result_get_source (r);

        g_assert_nonnull (host);
        g_assert_nonnull (g_strstr_len (host, -1, "::1"));
    }
}

/* ============================================================
 * AiSearchOptions
 * ============================================================ */

static void
test_options_defaults (void)
{
    g_autoptr(AiSearchOptions) o = ai_search_options_new ();

    g_assert_cmpuint (ai_search_options_get_count (o), ==, 10);
    g_assert_cmpint (ai_search_options_get_freshness (o), ==,
                     AI_SEARCH_FRESHNESS_ANY);
    g_assert_cmpint (ai_search_options_get_safesearch (o), ==,
                     AI_SEARCH_SAFE_MODERATE);
    g_assert_null (ai_search_options_get_country (o));
    g_assert_null (ai_search_options_get_language (o));
    g_assert_null (ai_search_options_get_site (o));
    g_assert_cmpuint (ai_search_options_get_offset (o), ==, 0);
    g_assert_false (ai_search_options_get_fetch_content (o));
    g_assert_cmpuint (ai_search_options_get_fetch_count (o), ==, 3);
}

static void
test_options_setters_and_cap (void)
{
    g_autoptr(AiSearchOptions) o = ai_search_options_new ();

    ai_search_options_set_count (o, 25);
    ai_search_options_set_freshness (o, AI_SEARCH_FRESHNESS_WEEK);
    ai_search_options_set_safesearch (o, AI_SEARCH_SAFE_STRICT);
    ai_search_options_set_country (o, "US");
    ai_search_options_set_language (o, "en");
    ai_search_options_set_site (o, "example.com");
    ai_search_options_set_offset (o, 20);
    ai_search_options_set_fetch_content (o, TRUE);

    g_assert_cmpuint (ai_search_options_get_count (o), ==, 25);
    g_assert_cmpint (ai_search_options_get_freshness (o), ==,
                     AI_SEARCH_FRESHNESS_WEEK);
    g_assert_cmpint (ai_search_options_get_safesearch (o), ==,
                     AI_SEARCH_SAFE_STRICT);
    g_assert_cmpstr (ai_search_options_get_country (o), ==, "US");
    g_assert_cmpstr (ai_search_options_get_language (o), ==, "en");
    g_assert_cmpstr (ai_search_options_get_site (o), ==, "example.com");
    g_assert_cmpuint (ai_search_options_get_offset (o), ==, 20);
    g_assert_true (ai_search_options_get_fetch_content (o));

    /* fetch_count is hard-capped at AI_SEARCH_MAX_FETCH_COUNT. */
    ai_search_options_set_fetch_count (o, 99);
    g_assert_cmpuint (ai_search_options_get_fetch_count (o), ==,
                      AI_SEARCH_MAX_FETCH_COUNT);
}

static void
test_options_copy_independent (void)
{
    g_autoptr(AiSearchOptions) a = ai_search_options_new ();
    g_autoptr(AiSearchOptions) b = NULL;

    ai_search_options_set_count (a, 7);
    ai_search_options_set_site (a, "a.example");
    b = ai_search_options_copy (a);

    /* Mutating the original must not touch the copy. */
    ai_search_options_set_count (a, 99);
    ai_search_options_set_site (a, "b.example");

    g_assert_cmpuint (ai_search_options_get_count (b), ==, 7);
    g_assert_cmpstr (ai_search_options_get_site (b), ==, "a.example");
}

/* ============================================================
 * enums
 * ============================================================ */

static void
test_enums_roundtrip (void)
{
    g_assert_cmpint (ai_search_freshness_from_string ("day"), ==,
                     AI_SEARCH_FRESHNESS_DAY);
    g_assert_cmpint (ai_search_freshness_from_string ("WEEK"), ==,
                     AI_SEARCH_FRESHNESS_WEEK);
    g_assert_cmpint (ai_search_freshness_from_string ("m"), ==,
                     AI_SEARCH_FRESHNESS_MONTH);
    g_assert_cmpstr (ai_search_freshness_to_string (AI_SEARCH_FRESHNESS_YEAR),
                     ==, "year");
    /* Unknown -> ANY. */
    g_assert_cmpint (ai_search_freshness_from_string ("bogus"), ==,
                     AI_SEARCH_FRESHNESS_ANY);
    g_assert_cmpint (ai_search_freshness_from_string (NULL), ==,
                     AI_SEARCH_FRESHNESS_ANY);

    g_assert_cmpint (ai_search_safe_search_from_string ("off"), ==,
                     AI_SEARCH_SAFE_OFF);
    g_assert_cmpint (ai_search_safe_search_from_string ("strict"), ==,
                     AI_SEARCH_SAFE_STRICT);
    g_assert_cmpstr (ai_search_safe_search_to_string (AI_SEARCH_SAFE_MODERATE),
                     ==, "moderate");
    g_assert_cmpint (ai_search_safe_search_from_string ("bogus"), ==,
                     AI_SEARCH_SAFE_MODERATE);

    /* GTypes are registered for introspection. */
    g_assert_true (g_type_from_name ("AiSearchFreshness") != 0);
    g_assert_true (g_type_from_name ("AiSearchSafeSearch") != 0);
}

/* ============================================================
 * ai_search_results_format
 * ============================================================ */

static GList *
make_results (void)
{
    GList          *l = NULL;
    AiSearchResult *r;

    r = ai_search_result_new ("Alpha", "https://a.example/1", "Snippet A");
    ai_search_result_set_rank (r, 1);
    ai_search_result_set_age (r, "1 day ago");
    l = g_list_append (l, r);

    r = ai_search_result_new ("Beta", "https://b.example/2", "Snippet B");
    ai_search_result_set_rank (r, 2);
    ai_search_result_set_content (r, "BETA PAGE CONTENT");
    l = g_list_append (l, r);

    return l;
}

static void
test_format_basic (void)
{
    GList            *results = make_results ();
    g_autofree gchar *out     = ai_search_results_format (results, "q", FALSE);

    g_assert_nonnull (out);
    g_assert_nonnull (g_strstr_len (out, -1, "1. Alpha"));
    g_assert_nonnull (g_strstr_len (out, -1, "2. Beta"));
    g_assert_nonnull (g_strstr_len (out, -1, "https://a.example/1"));
    g_assert_nonnull (g_strstr_len (out, -1, "Source: a.example"));
    g_assert_nonnull (g_strstr_len (out, -1, "Age: 1 day ago"));
    g_assert_nonnull (g_strstr_len (out, -1, "Snippet A"));
    /* include_content == FALSE: content omitted. */
    g_assert_null (g_strstr_len (out, -1, "BETA PAGE CONTENT"));

    g_list_free_full (results, g_object_unref);
}

static void
test_format_with_content (void)
{
    GList            *results = make_results ();
    g_autofree gchar *out     = ai_search_results_format (results, "q", TRUE);

    g_assert_nonnull (g_strstr_len (out, -1, "Content:"));
    g_assert_nonnull (g_strstr_len (out, -1, "BETA PAGE CONTENT"));

    g_list_free_full (results, g_object_unref);
}

static void
test_format_empty (void)
{
    g_autofree gchar *out  = ai_search_results_format (NULL, "kittens", FALSE);
    g_autofree gchar *out2 = ai_search_results_format (NULL, NULL, FALSE);

    g_assert_cmpstr (out, ==, "No results found for: kittens");
    g_assert_cmpstr (out2, ==, "No results found.");
}

/* ============================================================
 * shared HTTP helper: robustness matrix
 * ============================================================ */

static void
test_http_timeout (void)
{
    BlackHole              *bh      = black_hole_start ();
    g_autoptr(SoupSession)  session = soup_session_new ();
    g_autofree gchar       *url     = NULL;
    g_autoptr(JsonNode)     node    = NULL;
    g_autoptr(GError)       err     = NULL;

    g_object_set (session, "timeout", (guint) 1, NULL);
    url = g_strdup_printf ("http://127.0.0.1:%u/", bh->port);

    node = ai_search_http_get_json (session, url, NULL, NULL, &err);
    g_assert_null (node);
    g_assert_error (err, AI_ERROR, AI_ERROR_TIMEOUT);

    black_hole_stop (bh);
}

static void
test_http_cancelled (void)
{
    g_autoptr(SoupSession)  session = soup_session_new ();
    g_autoptr(GCancellable) cancel  = g_cancellable_new ();
    g_autoptr(JsonNode)     node    = NULL;
    g_autoptr(GError)       err     = NULL;

    g_cancellable_cancel (cancel);
    node = ai_search_http_get_json (session, "http://127.0.0.1:1/", NULL,
                                    cancel, &err);
    g_assert_null (node);
    g_assert_error (err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* Drive the helper through the Bing provider (so the real provider path is
 * exercised) against the loopback server. */
static AiBingSearch *
bing_on (TServer *ts)
{
    g_autofree gchar *endpoint = tserver_url (ts, "/bing");

    return g_object_new (AI_TYPE_BING_SEARCH,
                         "api-key", "test-key",
                         "endpoint", endpoint, NULL);
}

static void
test_http_429_then_ok (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_response (ts, 200, "application/json", BING_JSON_2);
    tserver_set_fail (ts, 429, 1);   /* first request 429, then 200 */

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (g_list_length (res), ==, 2);
    g_assert_cmpuint (tserver_hits (ts), ==, 2);   /* retried exactly once */

    g_list_free_full (res, g_object_unref);
    tserver_stop (ts);
}

static void
test_http_429_exhausted (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_fail (ts, 429, 10);   /* always 429 */

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_null (res);
    g_assert_error (err, AI_ERROR, AI_ERROR_RATE_LIMITED);

    tserver_stop (ts);
}

static void
test_http_5xx_then_ok (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_response (ts, 200, "application/json", BING_JSON_2);
    tserver_set_fail (ts, 503, 2);   /* 503, 503, then 200 */

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (g_list_length (res), ==, 2);
    g_assert_cmpuint (tserver_hits (ts), ==, 3);

    g_list_free_full (res, g_object_unref);
    tserver_stop (ts);
}

static void
test_http_5xx_exhausted (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_fail (ts, 500, 10);

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_null (res);
    g_assert_error (err, AI_ERROR, AI_ERROR_SERVER_ERROR);

    tserver_stop (ts);
}

static void
test_http_401_no_retry (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_response (ts, 401, "text/plain", "");

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_null (res);
    g_assert_error (err, AI_ERROR, AI_ERROR_INVALID_API_KEY);
    g_assert_cmpuint (tserver_hits (ts), ==, 1);   /* no retry */

    tserver_stop (ts);
}

static void
test_http_403_no_retry (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_response (ts, 403, "text/plain", "");

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_null (res);
    g_assert_error (err, AI_ERROR, AI_ERROR_PERMISSION_DENIED);
    g_assert_cmpuint (tserver_hits (ts), ==, 1);

    tserver_stop (ts);
}

static void
test_http_404_no_retry (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_response (ts, 404, "text/plain", "");

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_null (res);
    g_assert_error (err, AI_ERROR, AI_ERROR_INVALID_REQUEST);
    g_assert_cmpuint (tserver_hits (ts), ==, 1);

    tserver_stop (ts);
}

static void
test_http_malformed_json (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    tserver_set_response (ts, 200, "application/json", "{ not valid json ");

    bing = bing_on (ts);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                      NULL, &err);
    g_assert_null (res);
    g_assert_error (err, AI_ERROR, AI_ERROR_INVALID_RESPONSE);

    tserver_stop (ts);
}

/* ============================================================
 * Bing provider
 * ============================================================ */

static void
test_bing_parse (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    AiSearchResult          *r0;
    g_autoptr(GError)        err  = NULL;

    tserver_set_response (ts, 200, "application/json", BING_JSON_2);
    bing = bing_on (ts);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                     NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (g_list_length (res), ==, 2);

    r0 = res->data;
    g_assert_cmpstr (ai_search_result_get_title (r0), ==, "Bing One");
    g_assert_cmpstr (ai_search_result_get_url (r0), ==, "https://one.example/a");
    g_assert_cmpstr (ai_search_result_get_snippet (r0), ==, "First snippet");
    g_assert_cmpstr (ai_search_result_get_source (r0), ==, "one.example");
    g_assert_cmpstr (ai_search_result_get_age (r0), ==, "2024-01-02");
    g_assert_cmpuint (ai_search_result_get_rank (r0), ==, 1);
    /* Second result omits dateLastCrawled -> age NULL. */
    g_assert_null (ai_search_result_get_age (res->next->data));

    g_list_free_full (res, g_object_unref);
    tserver_stop (ts);
}

static void
test_bing_missing_webpages (void)
{
    TServer                 *ts   = tserver_start ();
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    /* No "webPages" member: zero results, not an error. */
    tserver_set_response (ts, 200, "application/json", "{\"_type\":\"x\"}");
    bing = bing_on (ts);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                     NULL, &err);
    g_assert_no_error (err);
    g_assert_null (res);

    tserver_stop (ts);
}

static void
test_bing_params_and_auth (void)
{
    TServer                  *ts   = tserver_start ();
    g_autoptr(AiBingSearch)   bing = NULL;
    g_autoptr(AiSearchOptions) o   = ai_search_options_new ();
    GList                    *res;
    g_autoptr(GError)         err  = NULL;
    g_autofree gchar         *target  = NULL;
    g_autofree gchar         *request = NULL;

    tserver_set_response (ts, 200, "application/json", BING_JSON_2);
    bing = bing_on (ts);

    ai_search_options_set_count (o, 5);
    ai_search_options_set_freshness (o, AI_SEARCH_FRESHNESS_WEEK);
    ai_search_options_set_safesearch (o, AI_SEARCH_SAFE_STRICT);
    ai_search_options_set_country (o, "US");
    ai_search_options_set_language (o, "en");
    ai_search_options_set_site (o, "example.com");
    ai_search_options_set_offset (o, 10);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "kittens", o,
                                     NULL, &err);
    g_assert_no_error (err);
    g_list_free_full (res, g_object_unref);

    target  = tserver_take_target (ts);
    request = tserver_take_request (ts);

    g_assert_nonnull (g_strstr_len (target, -1, "count=5"));
    g_assert_nonnull (g_strstr_len (target, -1, "freshness=Week"));
    g_assert_nonnull (g_strstr_len (target, -1, "safeSearch=Strict"));
    g_assert_nonnull (g_strstr_len (target, -1, "mkt=en-US"));
    g_assert_nonnull (g_strstr_len (target, -1, "offset=10"));
    /* site filter folded into q as "site:example.com" (": " -> %3A). */
    g_assert_nonnull (g_strstr_len (target, -1, "site%3Aexample.com"));
    /* API key sent in the Bing auth header. */
    g_assert_nonnull (g_strstr_len (request, -1,
                                    "Ocp-Apim-Subscription-Key: test-key"));

    tserver_stop (ts);
}

static void
test_bing_freshness_year_omitted (void)
{
    TServer                  *ts   = tserver_start ();
    g_autoptr(AiBingSearch)   bing = NULL;
    g_autoptr(AiSearchOptions) o   = ai_search_options_new ();
    GList                    *res;
    g_autofree gchar         *target = NULL;
    g_autoptr(GError)         err  = NULL;

    tserver_set_response (ts, 200, "application/json", BING_JSON_2);
    bing = bing_on (ts);

    ai_search_options_set_freshness (o, AI_SEARCH_FRESHNESS_YEAR);
    res = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", o,
                                     NULL, &err);
    g_assert_no_error (err);
    g_list_free_full (res, g_object_unref);

    target = tserver_take_target (ts);
    g_assert_null (g_strstr_len (target, -1, "freshness="));

    tserver_stop (ts);
}

/* ============================================================
 * Brave provider
 * ============================================================ */

static AiBraveSearch *
brave_on (TServer *ts)
{
    g_autofree gchar *endpoint = tserver_url (ts, "/brave");

    return g_object_new (AI_TYPE_BRAVE_SEARCH,
                         "api-key", "brave-key",
                         "endpoint", endpoint, NULL);
}

static void
test_brave_parse (void)
{
    TServer                  *ts    = tserver_start ();
    g_autoptr(AiBraveSearch)  brave = NULL;
    GList                    *res;
    AiSearchResult           *r0;
    g_autoptr(GError)         err   = NULL;

    tserver_set_response (ts, 200, "application/json", BRAVE_JSON_2);
    brave = brave_on (ts);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (brave), "q", NULL,
                                     NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (g_list_length (res), ==, 2);

    r0 = res->data;
    g_assert_cmpstr (ai_search_result_get_title (r0), ==, "Brave One");
    g_assert_cmpstr (ai_search_result_get_url (r0), ==, "https://one.example/a");
    g_assert_cmpstr (ai_search_result_get_snippet (r0), ==, "First desc");
    g_assert_cmpstr (ai_search_result_get_age (r0), ==, "2024-03-04");

    g_list_free_full (res, g_object_unref);
    tserver_stop (ts);
}

static void
test_brave_params_and_auth (void)
{
    TServer                   *ts    = tserver_start ();
    g_autoptr(AiBraveSearch)   brave = NULL;
    g_autoptr(AiSearchOptions) o     = ai_search_options_new ();
    GList                     *res;
    g_autofree gchar          *target  = NULL;
    g_autofree gchar          *request = NULL;
    g_autoptr(GError)          err   = NULL;

    tserver_set_response (ts, 200, "application/json", BRAVE_JSON_2);
    brave = brave_on (ts);

    ai_search_options_set_freshness (o, AI_SEARCH_FRESHNESS_WEEK);
    ai_search_options_set_country (o, "US");
    ai_search_options_set_language (o, "en");
    ai_search_options_set_safesearch (o, AI_SEARCH_SAFE_STRICT);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (brave), "q", o,
                                     NULL, &err);
    g_assert_no_error (err);
    g_list_free_full (res, g_object_unref);

    target  = tserver_take_target (ts);
    request = tserver_take_request (ts);

    g_assert_nonnull (g_strstr_len (target, -1, "freshness=pw"));
    g_assert_nonnull (g_strstr_len (target, -1, "country=US"));
    g_assert_nonnull (g_strstr_len (target, -1, "search_lang=en"));
    g_assert_nonnull (g_strstr_len (target, -1, "safesearch=strict"));
    g_assert_nonnull (g_strstr_len (request, -1,
                                    "X-Subscription-Token: brave-key"));

    tserver_stop (ts);
}

static void
test_brave_missing_web (void)
{
    TServer                  *ts    = tserver_start ();
    g_autoptr(AiBraveSearch)  brave = NULL;
    GList                    *res;
    g_autoptr(GError)         err   = NULL;

    tserver_set_response (ts, 200, "application/json", "{\"query\":{}}");
    brave = brave_on (ts);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (brave), "q", NULL,
                                     NULL, &err);
    g_assert_no_error (err);
    g_assert_null (res);

    tserver_stop (ts);
}

/* ============================================================
 * Both providers against bodies of the wrong shape
 * ============================================================ */

/*
 * A search API is somebody else's server, and both providers read it by
 * presence: `has_member("webPages")` and then
 * json_object_get_object_member() on it.  Presence is not type, so
 * {"webPages": 5} logged a Json-CRITICAL, handed back NULL, and the NULL
 * went into json_object_get_array_member() for a second one -- fatal
 * under GTest and under G_DEBUG=fatal-warnings, and an empty result list
 * without them.
 *
 * One table through both, because each ignores the keys it does not know
 * and a body aimed at one is a fine no-op for the other.  The assertion
 * is mostly that the process is still running; beyond that, a body that
 * cannot be read must come back as no results rather than as an error,
 * which is what "no web results" already means here.
 */
static const gchar *malformed_search_bodies[] = {
    "null",
    "[]",
    "7",
    "{\"webPages\":5}",
    "{\"webPages\":null}",
    "{\"webPages\":[]}",
    "{\"webPages\":{\"value\":9}}",
    "{\"webPages\":{\"value\":{}}}",
    "{\"webPages\":{\"value\":[1,2,3]}}",
    "{\"webPages\":{\"value\":[null]}}",
    "{\"webPages\":{\"value\":[{\"name\":7,\"url\":{},"
        "\"snippet\":[],\"dateLastCrawled\":9}]}}",
    "{\"web\":5}",
    "{\"web\":null}",
    "{\"web\":{\"results\":9}}",
    "{\"web\":{\"results\":[1,2,3]}}",
    "{\"web\":{\"results\":[null]}}",
    "{\"web\":{\"results\":[{\"title\":7,\"url\":[],"
        "\"description\":{},\"page_age\":9,\"age\":[]}]}}",
    NULL
};

static void
test_bing_malformed_shapes (void)
{
    gsize i;

    for (i = 0; malformed_search_bodies[i] != NULL; i++)
    {
        TServer                 *ts   = tserver_start ();
        g_autoptr(AiBingSearch)  bing = NULL;
        GList                   *res;
        g_autoptr(GError)        err  = NULL;

        tserver_set_response (ts, 200, "application/json",
                              malformed_search_bodies[i]);
        bing = bing_on (ts);

        res = ai_search_provider_search (AI_SEARCH_PROVIDER (bing), "q", NULL,
                                         NULL, &err);
        g_assert_no_error (err);
        g_list_free_full (res, g_object_unref);

        tserver_stop (ts);
    }
}

static void
test_brave_malformed_shapes (void)
{
    gsize i;

    for (i = 0; malformed_search_bodies[i] != NULL; i++)
    {
        TServer                  *ts    = tserver_start ();
        g_autoptr(AiBraveSearch)  brave = NULL;
        GList                    *res;
        g_autoptr(GError)         err   = NULL;

        tserver_set_response (ts, 200, "application/json",
                              malformed_search_bodies[i]);
        brave = brave_on (ts);

        res = ai_search_provider_search (AI_SEARCH_PROVIDER (brave), "q", NULL,
                                         NULL, &err);
        g_assert_no_error (err);
        g_list_free_full (res, g_object_unref);

        tserver_stop (ts);
    }
}

/* ============================================================
 * DuckDuckGo provider
 * ============================================================ */

static AiDuckDuckGoSearch *
ddg_on (TServer *ts)
{
    g_autofree gchar *endpoint = tserver_url (ts, "/ddg");

    return g_object_new (AI_TYPE_DUCKDUCKGO_SEARCH, "endpoint", endpoint, NULL);
}

static void
test_ddg_parse (void)
{
    TServer                      *ts  = tserver_start ();
    g_autoptr(AiDuckDuckGoSearch)  ddg = NULL;
    GList                        *res;
    AiSearchResult               *r0;
    AiSearchResult               *r1;
    AiSearchResult               *r2;
    AiSearchResult               *r3;
    g_autofree gchar             *method = NULL;
    g_autofree gchar             *body   = NULL;
    g_autoptr(GError)             err  = NULL;

    tserver_set_response (ts, 200, "text/html", DDG_HTML);
    ddg = ddg_on (ts);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (ddg), "kittens", NULL,
                                     NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (g_list_length (res), ==, 4);

    r0 = res->data;
    r1 = res->next->data;
    r2 = res->next->next->data;
    r3 = res->next->next->next->data;

    /* uddg redirect decoded to the real URL. */
    g_assert_cmpstr (ai_search_result_get_title (r0), ==, "First Result");
    g_assert_cmpstr (ai_search_result_get_url (r0), ==, "https://example.com/one");
    g_assert_cmpstr (ai_search_result_get_snippet (r0), ==, "Snippet one.");
    g_assert_cmpuint (ai_search_result_get_rank (r0), ==, 1);

    g_assert_cmpstr (ai_search_result_get_url (r1), ==, "https://example.org/two");
    /* HTML entity decoded in the snippet. */
    g_assert_nonnull (g_strstr_len (ai_search_result_get_snippet (r1), -1,
                                    "with & sign"));

    /* Direct href passed through. */
    g_assert_cmpstr (ai_search_result_get_url (r2), ==,
                     "https://direct.example/three");
    /* Protocol-relative href gets https:. */
    g_assert_cmpstr (ai_search_result_get_url (r3), ==,
                     "https://proto.example/four");

    /* Request was a POST whose body carried the query. */
    method = g_strdup (ts->last_method);
    body   = tserver_take_body (ts);
    g_assert_cmpstr (method, ==, "POST");
    g_assert_nonnull (g_strstr_len (body, -1, "q=kittens"));

    g_list_free_full (res, g_object_unref);
    tserver_stop (ts);
}

static void
test_ddg_count_truncates (void)
{
    TServer                      *ts  = tserver_start ();
    g_autoptr(AiDuckDuckGoSearch)  ddg = NULL;
    g_autoptr(AiSearchOptions)     o   = ai_search_options_new ();
    GList                        *res;
    g_autoptr(GError)             err  = NULL;

    tserver_set_response (ts, 200, "text/html", DDG_HTML);
    ddg = ddg_on (ts);

    ai_search_options_set_count (o, 2);
    res = ai_search_provider_search (AI_SEARCH_PROVIDER (ddg), "q", o,
                                     NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (g_list_length (res), ==, 2);

    g_list_free_full (res, g_object_unref);
    tserver_stop (ts);
}

static void
test_ddg_garbage_is_empty (void)
{
    TServer                      *ts  = tserver_start ();
    g_autoptr(AiDuckDuckGoSearch)  ddg = NULL;
    GList                        *res;
    g_autoptr(GError)             err  = NULL;

    tserver_set_response (ts, 200, "text/html", "garbage <<< not html >>>");
    ddg = ddg_on (ts);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (ddg), "q", NULL,
                                     NULL, &err);
    g_assert_no_error (err);   /* best-effort: empty, not error */
    g_assert_null (res);

    tserver_stop (ts);
}

static void
test_ddg_non_200_is_empty (void)
{
    TServer                      *ts  = tserver_start ();
    g_autoptr(AiDuckDuckGoSearch)  ddg = NULL;
    GList                        *res;
    g_autoptr(GError)             err  = NULL;

    tserver_set_response (ts, 500, "text/plain", "");
    ddg = ddg_on (ts);

    res = ai_search_provider_search (AI_SEARCH_PROVIDER (ddg), "q", NULL,
                                     NULL, &err);
    g_assert_no_error (err);
    g_assert_null (res);
    /* DDG retries once on a transient failure (2 attempts total). */
    g_assert_cmpuint (tserver_hits (ts), ==, 2);

    tserver_stop (ts);
}

/* ============================================================
 * construction + env factory
 * ============================================================ */

static void
test_construction (void)
{
    g_autoptr(AiBingSearch)       bing  = ai_bing_search_new ("k");
    g_autoptr(AiBraveSearch)      brave = ai_brave_search_new ("k");
    g_autoptr(AiDuckDuckGoSearch) ddg   = ai_duckduckgo_search_new ();

    g_assert_true (AI_IS_SEARCH_PROVIDER (bing));
    g_assert_true (AI_IS_SEARCH_PROVIDER (brave));
    g_assert_true (AI_IS_SEARCH_PROVIDER (ddg));
}

static void
test_factory_env (void)
{
    g_autoptr(AiSearchProvider) p = NULL;

    g_unsetenv ("BRAVE_API_KEY");
    g_unsetenv ("BING_API_KEY");

    /* Neither key: keyless DuckDuckGo default. */
    p = ai_search_provider_new_default (NULL);
    g_assert_true (AI_IS_DUCKDUCKGO_SEARCH (p));
    g_clear_object (&p);

    /* Only Bing. */
    g_setenv ("BING_API_KEY", "x", TRUE);
    p = ai_search_provider_new_default (NULL);
    g_assert_true (AI_IS_BING_SEARCH (p));
    g_clear_object (&p);

    /* Brave wins over Bing. */
    g_setenv ("BRAVE_API_KEY", "y", TRUE);
    p = ai_search_provider_new_default (NULL);
    g_assert_true (AI_IS_BRAVE_SEARCH (p));
    g_clear_object (&p);

    g_unsetenv ("BRAVE_API_KEY");
    g_unsetenv ("BING_API_KEY");
}

/* ============================================================
 * MockSearchProvider (for executor-layer tests)
 * ============================================================ */

#define MOCK_TYPE_SEARCH (mock_search_get_type ())
G_DECLARE_FINAL_TYPE (MockSearch, mock_search, MOCK, SEARCH, GObject)

struct _MockSearch
{
    GObject  parent_instance;
    gint     calls;
    GList   *template;   /* GList<AiSearchResult>, owned */
};

static void mock_search_iface_init (AiSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (MockSearch, mock_search, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (AI_TYPE_SEARCH_PROVIDER, mock_search_iface_init))

static AiSearchResult *
copy_result (AiSearchResult *r)
{
    AiSearchResult *c = ai_search_result_new (ai_search_result_get_title (r),
                                              ai_search_result_get_url (r),
                                              ai_search_result_get_snippet (r));
    ai_search_result_set_age (c, ai_search_result_get_age (r));
    ai_search_result_set_rank (c, ai_search_result_get_rank (r));
    return c;
}

static GList *
mock_search_do (AiSearchProvider *provider, const gchar *query,
                AiSearchOptions *options, GCancellable *cancellable,
                GError **error)
{
    MockSearch *m = MOCK_SEARCH (provider);
    GList      *out = NULL;
    GList      *l;

    (void) query; (void) options;

    if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return NULL;

    m->calls++;
    for (l = m->template; l != NULL; l = l->next)
        out = g_list_append (out, copy_result (l->data));
    return out;
}

static void
mock_search_iface_init (AiSearchProviderInterface *iface)
{
    iface->search = mock_search_do;
}

static void
mock_search_finalize (GObject *object)
{
    MockSearch *m = MOCK_SEARCH (object);

    g_list_free_full (m->template, g_object_unref);
    G_OBJECT_CLASS (mock_search_parent_class)->finalize (object);
}

static void
mock_search_class_init (MockSearchClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = mock_search_finalize;
}

static void
mock_search_init (MockSearch *self)
{
    self->calls    = 0;
    self->template = NULL;
}

static void
mock_search_add (MockSearch *m, const gchar *title, const gchar *url,
                 const gchar *snippet)
{
    m->template = g_list_append (m->template,
                                 ai_search_result_new (title, url, snippet));
}

/* ============================================================
 * AiToolExecutor web_search integration
 * ============================================================ */

static AiToolUse *
web_search_use (const gchar *json)
{
    return ai_tool_use_new_from_json_string ("ws-test", "web_search", json);
}

static void
test_executor_schema (void)
{
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new ();
    MockSearch               *mock = g_object_new (MOCK_TYPE_SEARCH, NULL);
    GList                    *tools;
    GList                    *iter;
    AiTool                   *web_search = NULL;
    g_autoptr(JsonNode)       params = NULL;
    g_autofree gchar         *json = NULL;

    ai_tool_executor_set_search_provider (exec, AI_SEARCH_PROVIDER (mock));
    g_object_unref (mock);

    tools = ai_tool_executor_get_tools (exec);
    for (iter = tools; iter != NULL; iter = iter->next)
        if (g_strcmp0 (ai_tool_get_name (iter->data), "web_search") == 0)
            web_search = iter->data;
    g_assert_nonnull (web_search);

    params = ai_tool_get_parameters_json (web_search);
    json = json_to_string (params, FALSE);
    g_assert_nonnull (g_strstr_len (json, -1, "\"query\""));
    g_assert_nonnull (g_strstr_len (json, -1, "\"count\""));
    g_assert_nonnull (g_strstr_len (json, -1, "\"freshness\""));
    g_assert_nonnull (g_strstr_len (json, -1, "\"safesearch\""));
    g_assert_nonnull (g_strstr_len (json, -1, "\"site\""));
    g_assert_nonnull (g_strstr_len (json, -1, "\"fetch_content\""));
}

static void
test_executor_no_provider (void)
{
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new ();
    g_autoptr(AiToolUse)      use  = web_search_use ("{\"query\":\"x\"}");
    g_autofree gchar         *out  = NULL;
    g_autoptr(GError)         err  = NULL;

    out = ai_tool_executor_execute (exec, use, NULL, &err);
    g_assert_null (out);
    g_assert_nonnull (err);
}

static void
test_executor_missing_query (void)
{
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new ();
    MockSearch               *mock = g_object_new (MOCK_TYPE_SEARCH, NULL);
    g_autoptr(AiToolUse)      use  = web_search_use ("{}");
    g_autofree gchar         *out  = NULL;
    g_autoptr(GError)         err  = NULL;

    ai_tool_executor_set_search_provider (exec, AI_SEARCH_PROVIDER (mock));
    g_object_unref (mock);

    out = ai_tool_executor_execute (exec, use, NULL, &err);
    g_assert_null (out);
    g_assert_nonnull (err);
}

static void
test_executor_formats_and_caches (void)
{
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new ();
    MockSearch               *mock = g_object_new (MOCK_TYPE_SEARCH, NULL);
    g_autoptr(AiToolUse)      use1 = NULL;
    g_autoptr(AiToolUse)      use2 = NULL;
    g_autoptr(AiToolUse)      use3 = NULL;
    g_autofree gchar         *out1 = NULL;
    g_autofree gchar         *out2 = NULL;
    g_autofree gchar         *out3 = NULL;
    g_autoptr(GError)         err  = NULL;

    mock_search_add (mock, "Result A", "https://a.example/1", "Snip A");
    mock_search_add (mock, "Result B", "https://b.example/2", "Snip B");
    ai_tool_executor_set_search_provider (exec, AI_SEARCH_PROVIDER (mock));

    /* A unique query so the process-global cache can't be pre-populated. */
    use1 = web_search_use ("{\"query\":\"exec-cache-unique-1\"}");
    out1 = ai_tool_executor_execute (exec, use1, NULL, &err);
    g_assert_no_error (err);
    g_assert_nonnull (g_strstr_len (out1, -1, "Result A"));
    g_assert_nonnull (g_strstr_len (out1, -1, "https://b.example/2"));
    g_assert_cmpint (mock->calls, ==, 1);

    /* Identical call -> served from cache, provider not hit again. */
    use2 = web_search_use ("{\"query\":\"exec-cache-unique-1\"}");
    out2 = ai_tool_executor_execute (exec, use2, NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (out2, ==, out1);
    g_assert_cmpint (mock->calls, ==, 1);

    /* Different options -> different cache key -> provider hit again. */
    use3 = web_search_use ("{\"query\":\"exec-cache-unique-1\",\"count\":5}");
    out3 = ai_tool_executor_execute (exec, use3, NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpint (mock->calls, ==, 2);

    g_object_unref (mock);
}

static void
test_executor_fetch_content (void)
{
    TServer                  *ts   = tserver_start ();
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new ();
    MockSearch               *mock = g_object_new (MOCK_TYPE_SEARCH, NULL);
    g_autofree gchar         *page_url = NULL;
    g_autofree gchar         *json     = NULL;
    g_autoptr(AiToolUse)      use  = NULL;
    g_autofree gchar         *out  = NULL;
    g_autoptr(GError)         err  = NULL;

    tserver_set_response (ts, 200, "text/html",
        "<html><body><h1>Page</h1><p>UNIQUE PAGE BODY TEXT</p></body></html>");

    /* Point the mock result at a unique loopback URL (unique so the global
     * web_fetch cache can't shortcut it). */
    page_url = tserver_url (ts, "/fetchtest-unique");
    mock_search_add (mock, "Fetchable", page_url, "snip");
    ai_tool_executor_set_search_provider (exec, AI_SEARCH_PROVIDER (mock));

    json = g_strdup_printf (
        "{\"query\":\"exec-fetch-unique\",\"fetch_content\":true}");
    use  = web_search_use (json);
    out  = ai_tool_executor_execute (exec, use, NULL, &err);

    g_assert_no_error (err);
    g_assert_nonnull (out);
    g_assert_nonnull (g_strstr_len (out, -1, "Content:"));
    g_assert_nonnull (g_strstr_len (out, -1, "UNIQUE PAGE BODY TEXT"));
    g_assert_cmpuint (tserver_hits (ts), >=, 1);

    g_object_unref (mock);
    tserver_stop (ts);
}

/* ============================================================
 * live tests (gated on API keys / network)
 * ============================================================ */

static void
test_bing_live (void)
{
    const gchar             *key = g_getenv ("BING_API_KEY");
    g_autoptr(AiBingSearch)  bing = NULL;
    GList                   *res;
    g_autoptr(GError)        err  = NULL;

    if (key == NULL || *key == '\0')
    {
        g_test_skip ("BING_API_KEY not set");
        return;
    }
    bing = ai_bing_search_new (key);
    res  = ai_search_provider_search (AI_SEARCH_PROVIDER (bing),
                                      "ai-glib library", NULL, NULL, &err);
    g_assert_no_error (err);
    g_list_free_full (res, g_object_unref);
}

static void
test_brave_live (void)
{
    const gchar              *key = g_getenv ("BRAVE_API_KEY");
    g_autoptr(AiBraveSearch)  brave = NULL;
    GList                    *res;
    g_autoptr(GError)         err  = NULL;

    if (key == NULL || *key == '\0')
    {
        g_test_skip ("BRAVE_API_KEY not set");
        return;
    }
    brave = ai_brave_search_new (key);
    res   = ai_search_provider_search (AI_SEARCH_PROVIDER (brave),
                                       "ai-glib library", NULL, NULL, &err);
    g_assert_no_error (err);
    g_list_free_full (res, g_object_unref);
}

static void
test_ddg_live (void)
{
    g_autoptr(AiDuckDuckGoSearch) ddg = NULL;
    GList                        *res;
    g_autoptr(GError)             err  = NULL;

    if (g_getenv ("AI_GLIB_TEST_NETWORK") == NULL)
    {
        g_test_skip ("set AI_GLIB_TEST_NETWORK=1 to run the DuckDuckGo live test");
        return;
    }
    ddg = ai_duckduckgo_search_new ();
    res = ai_search_provider_search (AI_SEARCH_PROVIDER (ddg),
                                     "gnu emacs", NULL, NULL, &err);
    /* Best-effort: never errors, but may be empty if DDG throttles. */
    g_assert_no_error (err);
    g_list_free_full (res, g_object_unref);
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/ai-glib/search/result/basic", test_result_basic);
    g_test_add_func ("/ai-glib/search/result/null-fields", test_result_null_fields);
    g_test_add_func ("/ai-glib/search/result/source", test_result_source_derivation);

    g_test_add_func ("/ai-glib/search/options/defaults", test_options_defaults);
    g_test_add_func ("/ai-glib/search/options/setters", test_options_setters_and_cap);
    g_test_add_func ("/ai-glib/search/options/copy", test_options_copy_independent);

    g_test_add_func ("/ai-glib/search/enums/roundtrip", test_enums_roundtrip);

    g_test_add_func ("/ai-glib/search/format/basic", test_format_basic);
    g_test_add_func ("/ai-glib/search/format/content", test_format_with_content);
    g_test_add_func ("/ai-glib/search/format/empty", test_format_empty);

    g_test_add_func ("/ai-glib/search/http/timeout", test_http_timeout);
    g_test_add_func ("/ai-glib/search/http/cancelled", test_http_cancelled);
    g_test_add_func ("/ai-glib/search/http/429-then-ok", test_http_429_then_ok);
    g_test_add_func ("/ai-glib/search/http/429-exhausted", test_http_429_exhausted);
    g_test_add_func ("/ai-glib/search/http/5xx-then-ok", test_http_5xx_then_ok);
    g_test_add_func ("/ai-glib/search/http/5xx-exhausted", test_http_5xx_exhausted);
    g_test_add_func ("/ai-glib/search/http/401", test_http_401_no_retry);
    g_test_add_func ("/ai-glib/search/http/403", test_http_403_no_retry);
    g_test_add_func ("/ai-glib/search/http/404", test_http_404_no_retry);
    g_test_add_func ("/ai-glib/search/http/malformed", test_http_malformed_json);

    g_test_add_func ("/ai-glib/search/bing/parse", test_bing_parse);
    g_test_add_func ("/ai-glib/search/bing/missing-webpages", test_bing_missing_webpages);
    g_test_add_func ("/ai-glib/search/bing/params", test_bing_params_and_auth);
    g_test_add_func ("/ai-glib/search/bing/year-omitted", test_bing_freshness_year_omitted);

    g_test_add_func ("/ai-glib/search/brave/parse", test_brave_parse);
    g_test_add_func ("/ai-glib/search/brave/params", test_brave_params_and_auth);
    g_test_add_func ("/ai-glib/search/brave/missing-web", test_brave_missing_web);
    g_test_add_func ("/ai-glib/search/bing/malformed", test_bing_malformed_shapes);
    g_test_add_func ("/ai-glib/search/brave/malformed",
                     test_brave_malformed_shapes);

    g_test_add_func ("/ai-glib/search/ddg/parse", test_ddg_parse);
    g_test_add_func ("/ai-glib/search/ddg/count", test_ddg_count_truncates);
    g_test_add_func ("/ai-glib/search/ddg/garbage", test_ddg_garbage_is_empty);
    g_test_add_func ("/ai-glib/search/ddg/non-200", test_ddg_non_200_is_empty);

    g_test_add_func ("/ai-glib/search/construction", test_construction);
    g_test_add_func ("/ai-glib/search/factory-env", test_factory_env);

    g_test_add_func ("/ai-glib/search/executor/schema", test_executor_schema);
    g_test_add_func ("/ai-glib/search/executor/no-provider", test_executor_no_provider);
    g_test_add_func ("/ai-glib/search/executor/missing-query", test_executor_missing_query);
    g_test_add_func ("/ai-glib/search/executor/cache", test_executor_formats_and_caches);
    g_test_add_func ("/ai-glib/search/executor/fetch-content", test_executor_fetch_content);

    g_test_add_func ("/ai-glib/search/bing/live", test_bing_live);
    g_test_add_func ("/ai-glib/search/brave/live", test_brave_live);
    g_test_add_func ("/ai-glib/search/ddg/live", test_ddg_live);

    return g_test_run ();
}
