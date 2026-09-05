/*
 * test-web-fetch.c - White-box + integration tests for web_fetch internals
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * These tests exercise the *static* helpers in ai-tool-executor.c
 * (html_to_text, web_fetch_truncate, web_fetch_should_upgrade,
 * web_fetch_build_body, and the response cache) that the public API does not
 * expose, by including the translation unit directly. Because the test links
 * the shared library, the locally-defined non-static symbols resolve against
 * the test object first and no multiple-definition occurs.
 *
 * web_fetch_build_body is the pure body-shaping stage of tool_web_fetch
 * (content-type dispatch, redirect notice, truncation), so the model-facing
 * output is covered deterministically without any network or HTTP server.
 */

#include <glib.h>

#include "convenience/ai-tool-executor.c"

/* ============================================================
 * html_to_text
 * ============================================================ */

static void
test_html_basic (void)
{
    const gchar      *html =
        "<html><head><title>T</title>"
        "<style>.x{color:red}</style>"
        "<script>var a = 1;</script></head>"
        "<body><h1>Heading</h1>"
        "<p>Hello <b>world</b>.</p>"
        "<a href=\"https://example.com\">link</a></body></html>";
    g_autofree gchar *text = NULL;

    text = html_to_text (html, strlen (html), NULL, NULL);
    g_assert_nonnull (text);

    g_assert_null (g_strstr_len (text, -1, "color:red"));   /* style dropped */
    g_assert_null (g_strstr_len (text, -1, "var a"));       /* script dropped */
    g_assert_null (g_strstr_len (text, -1, "T"));           /* <title> dropped */
    g_assert_nonnull (g_strstr_len (text, -1, "# Heading"));
    g_assert_nonnull (g_strstr_len (text, -1, "Hello"));
    g_assert_nonnull (g_strstr_len (text, -1, "world"));
    g_assert_nonnull (g_strstr_len (text, -1, "[link](https://example.com)"));
}

static void
test_html_empty (void)
{
    g_autofree gchar *text = html_to_text ("", 0, NULL, NULL);

    /* Empty input: libxml yields no root -> NULL, or an empty document. */
    g_assert_true (text == NULL || text[0] == '\0');
}

static void
test_html_malformed (void)
{
    /* Unclosed tags: HTML_PARSE_RECOVER must still extract the text. */
    const gchar      *html = "<p>hello<b>world<i>nested";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1, "hello"));
    g_assert_nonnull (g_strstr_len (text, -1, "world"));
    g_assert_nonnull (g_strstr_len (text, -1, "nested"));
}

static void
test_html_entities (void)
{
    /* Character entities in text nodes must be decoded by libxml. */
    const gchar      *html = "<p>a &amp; b &lt;c&gt; &#233;</p>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1, "a & b"));
    g_assert_nonnull (g_strstr_len (text, -1, "<c>"));
    g_assert_nonnull (g_strstr_len (text, -1, "\xc3\xa9")); /* é */
}

static void
test_html_all_heading_levels (void)
{
    const gchar      *html =
        "<h1>A</h1><h2>B</h2><h3>C</h3>"
        "<h4>D</h4><h5>E</h5><h6>F</h6>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1, "# A"));
    g_assert_nonnull (g_strstr_len (text, -1, "## B"));
    g_assert_nonnull (g_strstr_len (text, -1, "### C"));
    g_assert_nonnull (g_strstr_len (text, -1, "#### D"));
    g_assert_nonnull (g_strstr_len (text, -1, "##### E"));
    g_assert_nonnull (g_strstr_len (text, -1, "###### F"));
}

static void
test_html_not_a_heading (void)
{
    /* <h7> is not a real heading: text kept, no '#' prefix emitted. */
    const gchar      *html = "<h7>NotHead</h7>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1, "NotHead"));
    g_assert_null (g_strstr_len (text, -1, "# NotHead"));
}

static void
test_html_link_without_href (void)
{
    const gchar      *html = "<a>bare</a> and <a href=\"\">empty</a>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    /* No href / empty href: render the text only, no markdown brackets. */
    g_assert_nonnull (g_strstr_len (text, -1, "bare"));
    g_assert_nonnull (g_strstr_len (text, -1, "empty"));
    g_assert_null (g_strstr_len (text, -1, "[bare]"));
    g_assert_null (g_strstr_len (text, -1, "[empty]"));
}

static void
test_html_nested_inline (void)
{
    const gchar      *html = "<p>a <b><i>bold</i></b> c</p>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1, "a "));
    g_assert_nonnull (g_strstr_len (text, -1, "bold"));
    g_assert_nonnull (g_strstr_len (text, -1, " c"));
}

static void
test_html_utf8_preserved (void)
{
    const gchar      *html = "<p>caf\xc3\xa9 r\xc3\xa9sum\xc3\xa9</p>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_true (g_utf8_validate (text, -1, NULL));
    g_assert_nonnull (g_strstr_len (text, -1, "caf\xc3\xa9"));
}

static void
test_html_drops_noscript_and_head (void)
{
    const gchar      *html =
        "<head><title>TITLE</title></head>"
        "<body><noscript>NOSCRIPT</noscript><p>ok</p></body>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_null (g_strstr_len (text, -1, "TITLE"));
    g_assert_null (g_strstr_len (text, -1, "NOSCRIPT"));
    g_assert_nonnull (g_strstr_len (text, -1, "ok"));
}

static void
test_html_collapses_whitespace (void)
{
    const gchar      *html =
        "<body><p>one</p>\n\n\n\n<p>two</p>     <p>three</p></body>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_null (g_strstr_len (text, -1, "\n\n\n"));   /* <= 2 newlines */
    g_assert_nonnull (g_strstr_len (text, -1, "one"));
    g_assert_nonnull (g_strstr_len (text, -1, "two"));
    g_assert_nonnull (g_strstr_len (text, -1, "three"));
}

/* ============================================================
 * <img> -> ![alt](src), resolved against the page base URL
 * ============================================================ */

static void
test_html_img_relative_resolves_against_base (void)
{
    const gchar      *html = "<p><img src=\"/img/pic.png\" alt=\"A pic\"></p>";
    g_autofree gchar *text =
        html_to_text (html, strlen (html), NULL, "https://example.com/page");

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1,
                                    "![A pic](https://example.com/img/pic.png)"));
}

static void
test_html_img_protocol_relative_resolves (void)
{
    const gchar      *html = "<img src=\"//cdn.example.net/a.jpg\" alt=\"x\">";
    g_autofree gchar *text =
        html_to_text (html, strlen (html), NULL, "https://example.com/p");

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1,
                                    "![x](https://cdn.example.net/a.jpg)"));
}

static void
test_html_img_absolute_passthrough (void)
{
    const gchar      *html =
        "<img src=\"https://img.example/abs.webp\" alt=\"shot\">";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1,
                                    "![shot](https://img.example/abs.webp)"));
}

static void
test_html_img_no_alt (void)
{
    const gchar      *html = "<img src=\"https://img.example/x.png\">";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1,
                                    "![](https://img.example/x.png)"));
}

static void
test_html_img_no_src_ignored (void)
{
    /* An <img> without a usable src emits nothing and must not crash. */
    const gchar      *html = "<p>before<img alt=\"empty\">after</p>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL, NULL);

    g_assert_nonnull (text);
    g_assert_null (g_strstr_len (text, -1, "!["));
    g_assert_nonnull (g_strstr_len (text, -1, "before"));
    g_assert_nonnull (g_strstr_len (text, -1, "after"));
}

static void
test_html_img_honors_base_href (void)
{
    /* A <base href> overrides the page URL for resolution. */
    const gchar      *html =
        "<head><base href=\"https://cdn.example/assets/\"></head>"
        "<body><img src=\"logo.png\" alt=\"L\"></body>";
    g_autofree gchar *text =
        html_to_text (html, strlen (html), NULL, "https://example.com/p");

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1,
                                    "![L](https://cdn.example/assets/logo.png)"));
}

/* ============================================================
 * web_fetch_truncate (UTF-8-safe size cap)
 * ============================================================ */

static void
test_truncate_under_limit (void)
{
    GString *s = g_string_new ("short");

    web_fetch_truncate (s, 100);
    g_assert_cmpstr (s->str, ==, "short");   /* unchanged */
    g_string_free (s, TRUE);
}

static void
test_truncate_over_limit (void)
{
    GString *s = g_string_new (NULL);
    gint     i;

    for (i = 0; i < 200; i++)
        g_string_append_c (s, 'a');

    web_fetch_truncate (s, 50);

    g_assert_nonnull (g_strstr_len (s->str, -1, "[truncated at 100 KB]"));
    /* 50 retained bytes + the appended "\n\n[truncated at 100 KB]" marker. */
    g_assert_cmpuint (s->len, ==, 50 + strlen ("\n\n[truncated at 100 KB]"));
    g_string_free (s, TRUE);
}

static void
test_truncate_utf8_boundary (void)
{
    GString     *s = g_string_new (NULL);
    gint         i;
    const gchar *marker;
    gsize        body_len;

    /* 100 x 'é' (2 bytes each) = 200 bytes. Cutting at an odd offset would
     * split a multi-byte sequence; the helper must back up to a boundary. */
    for (i = 0; i < 100; i++)
        g_string_append (s, "\xc3\xa9");

    web_fetch_truncate (s, 51);

    marker = g_strstr_len (s->str, -1, "\n\n[truncated at 100 KB]");
    g_assert_nonnull (marker);

    body_len = (gsize) (marker - s->str);
    g_assert_true (g_utf8_validate (s->str, (gssize) body_len, NULL));
    g_assert_cmpuint (body_len % 2, ==, 0);   /* even -> not split */
    g_string_free (s, TRUE);
}

/* ============================================================
 * http -> https upgrade decision
 * ============================================================ */

static void
test_upgrade_decision (void)
{
    /* Real hostnames are upgraded. */
    g_assert_true  (web_fetch_should_upgrade ("http://example.com/x"));
    g_assert_true  (web_fetch_should_upgrade ("http://sub.example.org"));

    /* localhost and IP-literals are not. */
    g_assert_false (web_fetch_should_upgrade ("http://localhost:8080/x"));
    g_assert_false (web_fetch_should_upgrade ("http://127.0.0.1:9000/y"));
    g_assert_false (web_fetch_should_upgrade ("http://[::1]:9000/z"));
    g_assert_false (web_fetch_should_upgrade ("http://192.168.1.5/a"));

    /* Garbage URL with no host is not upgraded. */
    g_assert_false (web_fetch_should_upgrade ("not a url"));
}

/* ============================================================
 * response cache
 * ============================================================ */

static void
test_cache_roundtrip (void)
{
    g_autofree gchar *hit  = NULL;
    gchar            *miss = NULL;

    miss = web_fetch_cache_lookup ("https://nope.example/never-stored");
    g_assert_null (miss);

    web_fetch_cache_store ("https://example.com/page", "converted body text");
    hit = web_fetch_cache_lookup ("https://example.com/page");
    g_assert_nonnull (hit);
    g_assert_cmpstr (hit, ==, "converted body text");
}

static void
test_cache_overwrite (void)
{
    g_autofree gchar *hit = NULL;

    web_fetch_cache_store ("https://example.com/dup", "first");
    web_fetch_cache_store ("https://example.com/dup", "second");

    hit = web_fetch_cache_lookup ("https://example.com/dup");
    g_assert_nonnull (hit);
    g_assert_cmpstr (hit, ==, "second");
}

static void
test_cache_ttl_expiry (void)
{
    WebFetchCacheEntry *entry;
    gchar              *hit;

    web_fetch_cache_store ("https://ttl.example/x", "v");

    /* Age the entry past the TTL by rewinding its monotonic stamp. */
    entry = g_hash_table_lookup (web_fetch_cache, "https://ttl.example/x");
    g_assert_nonnull (entry);
    entry->stamp -= (WEB_FETCH_CACHE_TTL_US + G_USEC_PER_SEC);

    hit = web_fetch_cache_lookup ("https://ttl.example/x");
    g_assert_null (hit);    /* expired -> miss */

    /* Expired entry must have been evicted on access. */
    g_assert_false (g_hash_table_contains (web_fetch_cache,
                                           "https://ttl.example/x"));
}

static void
test_cache_self_cleaning_sweep (void)
{
    WebFetchCacheEntry *old;
    g_autofree gchar   *fresh = NULL;

    web_fetch_cache_store ("https://sweep.example/fresh", "f");
    web_fetch_cache_store ("https://sweep.example/old",   "o");

    old = g_hash_table_lookup (web_fetch_cache, "https://sweep.example/old");
    g_assert_nonnull (old);
    old->stamp -= (WEB_FETCH_CACHE_TTL_US + G_USEC_PER_SEC);

    /* Looking up an unrelated, still-fresh key triggers the opportunistic
     * sweep that drops the aged entry. */
    fresh = web_fetch_cache_lookup ("https://sweep.example/fresh");
    g_assert_cmpstr (fresh, ==, "f");
    g_assert_false (g_hash_table_contains (web_fetch_cache,
                                           "https://sweep.example/old"));
}

/* ============================================================
 * web_fetch_build_body (content-type dispatch + redirect notice + truncate)
 *
 * This is the pure body-shaping stage of tool_web_fetch, exercised directly
 * with synthetic inputs. The HTTP transport itself (status codes, the
 * http->https probe, User-Agent/timeout) is thin glue best covered by the
 * end-to-end manual check; everything that shapes model-facing output is here.
 * ============================================================ */

static void
test_body_html_converted (void)
{
    const gchar      *html = "<h1>Hi</h1><p>Body text</p><script>secret()</script>";
    g_autofree gchar *out  =
        web_fetch_build_body (html, strlen (html), "text/html", NULL,
                              NULL, NULL, NULL);

    g_assert_nonnull (out);
    g_assert_nonnull (g_strstr_len (out, -1, "# Hi"));
    g_assert_nonnull (g_strstr_len (out, -1, "Body text"));
    g_assert_null (g_strstr_len (out, -1, "secret"));   /* script dropped */
    g_assert_null (g_strstr_len (out, -1, "<h1>"));     /* not raw HTML */
}

static void
test_body_html_img_resolved_against_final_url (void)
{
    /* build_body passes the final URL as the base, so a relative <img>
     * src becomes an absolute, fetchable URL in the model-facing output. */
    const gchar      *html = "<p><img src=\"pics/x.png\" alt=\"X\"></p>";
    g_autofree gchar *out  =
        web_fetch_build_body (html, strlen (html), "text/html", NULL,
                              NULL, NULL, "https://site.example/a/b");

    g_assert_nonnull (out);
    g_assert_nonnull (g_strstr_len (out, -1,
                                    "![X](https://site.example/a/pics/x.png)"));
}

static void
test_body_xhtml_converted (void)
{
    const gchar      *html = "<h2>Sub</h2><p>z</p>";
    g_autofree gchar *out  =
        web_fetch_build_body (html, strlen (html), "application/xhtml+xml",
                              NULL, NULL, NULL, NULL);

    g_assert_nonnull (out);
    g_assert_nonnull (g_strstr_len (out, -1, "## Sub"));
}

static void
test_body_json_passthrough (void)
{
    const gchar      *json = "{\"key\":\"value\",\"n\":1}";
    g_autofree gchar *out  =
        web_fetch_build_body (json, strlen (json), "application/json", NULL,
                              NULL, NULL, NULL);

    /* JSON is returned verbatim, not HTML-converted. */
    g_assert_cmpstr (out, ==, "{\"key\":\"value\",\"n\":1}");
}

static void
test_body_plain_text_passthrough (void)
{
    const gchar      *txt = "just <b>plain</b> text & co";
    g_autofree gchar *out =
        web_fetch_build_body (txt, strlen (txt), "text/plain", NULL,
                              NULL, NULL, NULL);

    /* text/plain is not converted: tags/entities are left untouched. */
    g_assert_cmpstr (out, ==, "just <b>plain</b> text & co");
}

static void
test_body_null_content_type_passthrough (void)
{
    const gchar      *txt = "no content type";
    g_autofree gchar *out =
        web_fetch_build_body (txt, strlen (txt), NULL, NULL,
                              NULL, NULL, NULL);

    g_assert_cmpstr (out, ==, "no content type");
}

static void
test_body_binary_placeholder (void)
{
    const gchar       data[] = { 0x00, 0x01, 0x02, 0x03, (gchar) 0xff };
    g_autofree gchar *out    =
        web_fetch_build_body (data, sizeof (data), "application/octet-stream",
                              NULL, NULL, NULL, NULL);

    g_assert_nonnull (out);
    g_assert_nonnull (g_strstr_len (out, -1, "[binary content"));
    g_assert_nonnull (g_strstr_len (out, -1, "application/octet-stream"));
    g_assert_nonnull (g_strstr_len (out, -1, "5 bytes"));   /* size reported */
}

static void
test_body_redirect_notice (void)
{
    const gchar      *txt = "ok";
    g_autofree gchar *out =
        web_fetch_build_body (txt, strlen (txt), "text/plain", NULL,
                              "a.example", "b.example",
                              "https://b.example/landing");

    g_assert_nonnull (out);
    g_assert_true (g_str_has_prefix (out, "[redirected to https://b.example/landing]"));
    g_assert_nonnull (g_strstr_len (out, -1, "ok"));
}

static void
test_body_no_redirect_notice_same_host (void)
{
    const gchar      *txt = "ok";
    g_autofree gchar *out =
        web_fetch_build_body (txt, strlen (txt), "text/plain", NULL,
                              "a.example", "a.example", "https://a.example/p");

    /* Same host -> no notice. */
    g_assert_null (g_strstr_len (out, -1, "[redirected"));
    g_assert_cmpstr (out, ==, "ok");
}

static void
test_body_no_redirect_notice_null_hosts (void)
{
    const gchar      *txt = "ok";
    g_autofree gchar *out =
        web_fetch_build_body (txt, strlen (txt), "text/plain", NULL,
                              NULL, NULL, NULL);

    g_assert_null (g_strstr_len (out, -1, "[redirected"));
}

static void
test_body_truncates_large (void)
{
    GString          *big = g_string_new (NULL);
    gint              i;
    g_autofree gchar *out = NULL;

    /* > 100 KB of plain text must come back truncated with the marker. */
    for (i = 0; i < 200 * 1024; i++)
        g_string_append_c (big, 'a');

    out = web_fetch_build_body (big->str, big->len, "text/plain", NULL,
                                NULL, NULL, NULL);
    g_assert_nonnull (out);
    g_assert_nonnull (g_strstr_len (out, -1, "[truncated at 100 KB]"));
    g_assert_cmpuint (strlen (out), <=,
                      WEB_FETCH_MAX_BYTES + strlen ("\n\n[truncated at 100 KB]"));
    g_string_free (big, TRUE);
}

/* ============================================================
 * Live HTTP transport (the thin glue the white-box tests skip)
 *
 * Covered here over a real loopback socket:
 *   - request headers on the wire (User-Agent / Accept) via web_fetch_get
 *   - non-2xx -> AI_ERROR_SERVER_ERROR mapping (tool_web_fetch)
 *   - end-to-end 200 text/html -> converted body
 *   - the http -> https speculative upgrade with fall-back to http
 *   - the SoupSession I/O timeout
 *
 * libsoup's synchronous send_and_read() drives its own private GMainContext,
 * so an in-process SoupServer on the SAME thread deadlocks (the server's
 * callbacks never run while the client call blocks). Worse, a *plain* HTTP
 * SoupServer that receives a TLS ClientHello buffers it forever, stalling the
 * https probe until the 30 s timeout — useless for the fall-back test.
 *
 * The harness is therefore a tiny raw-socket HTTP/1.1 server on its own
 * GThread (blocking accept loop, no GMainContext). It classifies each
 * connection by its first byte: 0x16 is a TLS ClientHello (the https probe),
 * which it fast-closes so the client's handshake fails in milliseconds and
 * web_fetch falls back to http; anything else is parsed as HTTP and answered
 * by path. This makes every path deterministic and sub-second.
 * ============================================================ */

typedef struct
{
    GThread *thread;
    GSocket *listen_sock;
    guint    port;
    gint     stop;            /* atomic stop flag */
    GMutex   lock;            /* guards the capture fields below */
    gchar   *last_request;    /* full request text of the most recent http hit */
    guint    http_hits;       /* plain-HTTP requests served */
    guint    tls_hits;        /* TLS ClientHellos fast-closed */
} RawServer;

static void
raw_send_all (GSocket *sock, const gchar *data, gsize len)
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

/* Drain one HTTP request off CONN (FIRST/FIRSTLEN are bytes already read),
 * route by path, write a fixed response, and record the request. */
static void
raw_serve_http (
    RawServer    *rs,
    GSocket      *conn,
    const gchar  *first,
    gsize         firstlen
){
    GString          *req   = g_string_new_len (first, (gssize) firstlen);
    g_autofree gchar *path  = NULL;
    g_autofree gchar *resp  = NULL;
    const gchar      *line_end;
    const gchar      *sp1;
    const gchar      *sp2;
    const gchar      *body;
    const gchar      *ctype;
    const gchar      *reason;
    guint             status;
    gchar             chunk[1024];

    /* Read up to the end of the request headers. */
    while (g_strstr_len (req->str, (gssize) req->len, "\r\n\r\n") == NULL)
    {
        gssize n = g_socket_receive (conn, chunk, sizeof chunk, NULL, NULL);

        if (n <= 0)
            break;
        g_string_append_len (req, chunk, n);
        if (req->len > 64 * 1024)   /* runaway guard */
            break;
    }

    /* Parse the path out of "GET <path> HTTP/1.1". */
    line_end = g_strstr_len (req->str, (gssize) req->len, "\r\n");
    sp1      = (line_end != NULL) ? memchr (req->str, ' ', line_end - req->str)
                                  : NULL;
    sp2      = (sp1 != NULL) ? memchr (sp1 + 1, ' ', line_end - (sp1 + 1))
                             : NULL;
    if (sp1 != NULL && sp2 != NULL)
        path = g_strndup (sp1 + 1, (gsize) (sp2 - (sp1 + 1)));

    if (g_strcmp0 (path, "/notfound") == 0)
    {
        status = 404; reason = "Not Found";
        ctype  = "text/plain"; body = "nope";
    }
    else if (g_strcmp0 (path, "/error") == 0)
    {
        status = 500; reason = "Internal Server Error";
        ctype  = "text/plain"; body = "boom";
    }
    else if (g_strcmp0 (path, "/html") == 0)
    {
        status = 200; reason = "OK";
        ctype  = "text/html; charset=utf-8";
        body   = "<html><head><title>T</title>"
                 "<script>secret()</script></head>"
                 "<body><h1>Live Heading</h1><p>Live body.</p></body></html>";
    }
    else
    {
        status = 200; reason = "OK";
        ctype  = "text/plain"; body = "hello from server";
    }

    resp = g_strdup_printf (
        "HTTP/1.1 %u %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, reason, ctype, (gulong) strlen (body), body);

    raw_send_all (conn, resp, strlen (resp));

    g_mutex_lock (&rs->lock);
    g_clear_pointer (&rs->last_request, g_free);
    rs->last_request = g_string_free (req, FALSE);   /* transfer to capture */
    rs->http_hits++;
    g_mutex_unlock (&rs->lock);
}

static gpointer
raw_server_thread (gpointer data)
{
    RawServer *rs = data;

    while (!g_atomic_int_get (&rs->stop))
    {
        GSocket *conn;
        gchar    first[1];
        gssize   n;

        conn = g_socket_accept (rs->listen_sock, NULL, NULL);
        if (conn == NULL)
            continue;                          /* accept timeout: re-check stop */
        if (g_atomic_int_get (&rs->stop))      /* woken by the shutdown poke */
        {
            g_object_unref (conn);
            break;
        }

        g_socket_set_timeout (conn, 2);

        n = g_socket_receive (conn, first, 1, NULL, NULL);
        if (n == 1)
        {
            if (first[0] == 0x16)
            {
                /* TLS ClientHello: the https probe. Fast-close it. */
                g_mutex_lock (&rs->lock);
                rs->tls_hits++;
                g_mutex_unlock (&rs->lock);
            }
            else
            {
                raw_serve_http (rs, conn, first, 1);
            }
        }

        g_socket_close (conn, NULL);
        g_object_unref (conn);
    }

    return NULL;
}

static RawServer *
raw_server_start (void)
{
    RawServer      *rs  = g_new0 (RawServer, 1);
    GError         *err = NULL;
    GInetAddress   *ia;
    GSocketAddress *bind_addr;
    GSocketAddress *local_addr;

    g_mutex_init (&rs->lock);

    rs->listen_sock = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                                    G_SOCKET_PROTOCOL_TCP, &err);
    g_assert_no_error (err);
    g_socket_set_timeout (rs->listen_sock, 1);   /* wake to re-check stop */

    ia        = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    bind_addr = g_inet_socket_address_new (ia, 0);
    g_object_unref (ia);
    g_socket_bind (rs->listen_sock, bind_addr, TRUE, &err);
    g_object_unref (bind_addr);
    g_assert_no_error (err);

    g_socket_listen (rs->listen_sock, &err);
    g_assert_no_error (err);

    local_addr = g_socket_get_local_address (rs->listen_sock, &err);
    g_assert_no_error (err);
    rs->port = g_inet_socket_address_get_port (
        G_INET_SOCKET_ADDRESS (local_addr));
    g_object_unref (local_addr);

    rs->thread = g_thread_new ("ai-glib-test-http", raw_server_thread, rs);
    return rs;
}

static void
raw_server_stop (RawServer *rs)
{
    GSocket        *poke;
    GInetAddress   *ia;
    GSocketAddress *addr;

    g_atomic_int_set (&rs->stop, 1);

    /* Poke our own listener so a blocked accept() returns at once. */
    poke = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                         G_SOCKET_PROTOCOL_TCP, NULL);
    if (poke != NULL)
    {
        ia   = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
        addr = g_inet_socket_address_new (ia, rs->port);
        g_object_unref (ia);
        g_socket_set_timeout (poke, 1);
        g_socket_connect (poke, addr, NULL, NULL);   /* result irrelevant */
        g_object_unref (addr);
        g_socket_close (poke, NULL);
        g_object_unref (poke);
    }

    g_thread_join (rs->thread);

    g_socket_close (rs->listen_sock, NULL);
    g_object_unref (rs->listen_sock);
    g_mutex_clear (&rs->lock);
    g_free (rs->last_request);
    g_free (rs);
}

static gchar *
raw_server_take_request (RawServer *rs)
{
    gchar *out;

    g_mutex_lock (&rs->lock);
    out = g_strdup (rs->last_request);
    g_mutex_unlock (&rs->lock);
    return out;
}

/* ---- a "black hole": accepts at the kernel level, never replies ---- */

typedef struct
{
    GSocket *socket;
    guint    port;
} BlackHole;

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

    g_socket_listen (bh->socket, &err);   /* never accept() -> silent peer */
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

/* ---- a GResolver that maps every name to IPv4 loopback ----
 *
 * Installed as the default resolver only for the fall-back test, so a
 * real-looking hostname (which web_fetch_should_upgrade() agrees to upgrade)
 * still lands on our loopback server. 127.0.0.1 / localhost would skip the
 * upgrade entirely and never exercise the probe. */

#define TEST_TYPE_RESOLVER (test_resolver_get_type ())
G_DECLARE_FINAL_TYPE (TestResolver, test_resolver, TEST, RESOLVER, GResolver)

struct _TestResolver
{
    GResolver parent_instance;
};

G_DEFINE_TYPE (TestResolver, test_resolver, G_TYPE_RESOLVER)

static GList *
test_resolver_loopback (void)
{
    return g_list_append (NULL,
        g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4));
}

static GList *
test_resolver_lookup_by_name (
    GResolver     *resolver,
    const gchar   *hostname,
    GCancellable  *cancellable,
    GError       **error
){
    (void) resolver; (void) hostname; (void) cancellable; (void) error;
    return test_resolver_loopback ();
}

static GList *
test_resolver_lookup_by_name_with_flags (
    GResolver                 *resolver,
    const gchar               *hostname,
    GResolverNameLookupFlags   flags,
    GCancellable              *cancellable,
    GError                   **error
){
    (void) flags;
    return test_resolver_lookup_by_name (resolver, hostname, cancellable, error);
}

static void
test_resolver_lookup_by_name_async (
    GResolver           *resolver,
    const gchar         *hostname,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task = g_task_new (resolver, cancellable, callback, user_data);

    (void) hostname;
    g_task_return_pointer (task, test_resolver_loopback (),
                           (GDestroyNotify) g_resolver_free_addresses);
    g_object_unref (task);
}

static void
test_resolver_lookup_by_name_with_flags_async (
    GResolver                 *resolver,
    const gchar               *hostname,
    GResolverNameLookupFlags   flags,
    GCancellable              *cancellable,
    GAsyncReadyCallback        callback,
    gpointer                   user_data
){
    (void) flags;
    test_resolver_lookup_by_name_async (resolver, hostname, cancellable,
                                        callback, user_data);
}

static GList *
test_resolver_lookup_finish (
    GResolver     *resolver,
    GAsyncResult  *result,
    GError       **error
){
    (void) resolver;
    return g_task_propagate_pointer (G_TASK (result), error);
}

static void
test_resolver_class_init (TestResolverClass *klass)
{
    GResolverClass *rc = G_RESOLVER_CLASS (klass);

    rc->lookup_by_name                   = test_resolver_lookup_by_name;
    rc->lookup_by_name_async             = test_resolver_lookup_by_name_async;
    rc->lookup_by_name_finish            = test_resolver_lookup_finish;
    rc->lookup_by_name_with_flags        = test_resolver_lookup_by_name_with_flags;
    rc->lookup_by_name_with_flags_async  = test_resolver_lookup_by_name_with_flags_async;
    rc->lookup_by_name_with_flags_finish = test_resolver_lookup_finish;
}

static void
test_resolver_init (TestResolver *self)
{
    (void) self;
}

/* ---- a mock AiProvider for the prompt-extraction sub-model path ---- */

#define MOCK_TYPE_PROVIDER (mock_provider_get_type ())
G_DECLARE_FINAL_TYPE (MockProvider, mock_provider, MOCK, PROVIDER, GObject)

struct _MockProvider
{
    GObject  parent_instance;
    gchar   *reply;            /* canned reply text (NULL -> empty) */
    gboolean fail;             /* TRUE -> chat returns an error */
    /* captures */
    gchar   *seen_system;
    gchar   *seen_user_text;
    gint     seen_max_tokens;
    gint     calls;
};

static void mock_provider_iface_init (AiProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (MockProvider, mock_provider, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (AI_TYPE_PROVIDER, mock_provider_iface_init))

static void
mock_provider_finalize (GObject *object)
{
    MockProvider *mp = MOCK_PROVIDER (object);

    g_free (mp->reply);
    g_free (mp->seen_system);
    g_free (mp->seen_user_text);

    G_OBJECT_CLASS (mock_provider_parent_class)->finalize (object);
}

static void
mock_provider_class_init (MockProviderClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = mock_provider_finalize;
}

static void
mock_provider_init (MockProvider *self)
{
    (void) self;
}

static void
mock_provider_chat_async (
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    MockProvider *mp  = MOCK_PROVIDER (provider);
    GTask        *task;
    GString      *acc;
    GList        *l;

    (void) tools;

    mp->calls++;
    mp->seen_max_tokens = max_tokens;
    g_clear_pointer (&mp->seen_system, g_free);
    mp->seen_system = g_strdup (system_prompt);

    acc = g_string_new (NULL);
    for (l = messages; l != NULL; l = l->next)
    {
        gchar *t = ai_message_get_text (AI_MESSAGE (l->data));

        if (t != NULL)
        {
            g_string_append (acc, t);
            g_free (t);
        }
    }
    g_clear_pointer (&mp->seen_user_text, g_free);
    mp->seen_user_text = g_string_free (acc, FALSE);

    task = g_task_new (provider, cancellable, callback, user_data);

    if (mp->fail)
    {
        g_task_return_new_error (task, AI_ERROR, AI_ERROR_SERVER_ERROR,
                                 "mock provider failure");
        g_object_unref (task);
        return;
    }

    {
        AiResponse    *resp = ai_response_new ("mock-id", "mock-model");
        AiTextContent *txt  =
            ai_text_content_new (mp->reply != NULL ? mp->reply : "");

        ai_response_add_content_block (resp, (AiContentBlock *) txt);
        g_task_return_pointer (task, resp, g_object_unref);
    }
    g_object_unref (task);
}

static AiResponse *
mock_provider_chat_finish (
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void) provider;
    return g_task_propagate_pointer (G_TASK (result), error);
}

static AiProviderType
mock_provider_get_provider_type (AiProvider *provider)
{
    (void) provider;
    return AI_PROVIDER_CLAUDE;
}

static const gchar *
mock_provider_get_name (AiProvider *provider)
{
    (void) provider;
    return "mock";
}

static const gchar *
mock_provider_get_default_model (AiProvider *provider)
{
    (void) provider;
    return "mock-model";
}

static void
mock_provider_iface_init (AiProviderInterface *iface)
{
    iface->get_provider_type = mock_provider_get_provider_type;
    iface->get_name          = mock_provider_get_name;
    iface->get_default_model = mock_provider_get_default_model;
    iface->chat_async        = mock_provider_chat_async;
    iface->chat_finish       = mock_provider_chat_finish;
}

static MockProvider *
mock_provider_new (const gchar *reply, gboolean fail)
{
    MockProvider *mp = g_object_new (MOCK_TYPE_PROVIDER, NULL);

    mp->reply = g_strdup (reply);
    mp->fail  = fail;
    return mp;
}

static AiToolUse *
wf_tool_use (const gchar *json)
{
    return ai_tool_use_new_from_json_string ("wf-test", "web_fetch", json);
}

/* ============================================================
 * transport tests
 * ============================================================ */

static void
test_wire_request_headers (void)
{
    RawServer              *rs;
    g_autoptr(SoupSession)  session = NULL;
    g_autoptr(SoupMessage)  msg     = NULL;
    g_autoptr(GBytes)       bytes   = NULL;
    g_autoptr(GError)       err     = NULL;
    g_autofree gchar       *url     = NULL;
    g_autofree gchar       *request = NULL;

    rs      = raw_server_start ();
    session = soup_session_new ();
    url     = g_strdup_printf ("http://127.0.0.1:%u/ok", rs->port);

    /* web_fetch_get is the production helper that injects the headers. */
    bytes = web_fetch_get (session, url, &msg, NULL, &err);
    g_assert_no_error (err);
    g_assert_nonnull (bytes);
    g_assert_cmpuint (soup_message_get_status (msg), ==, 200);

    request = raw_server_take_request (rs);
    g_assert_nonnull (request);
    /* Request line carried our GET + path. */
    g_assert_nonnull (g_strstr_len (request, -1, "GET /ok HTTP/1.1"));
    /* User-Agent set verbatim on the wire. */
    g_assert_nonnull (g_strstr_len (request, -1, WEB_FETCH_USER_AGENT));
    /* Accept advertises HTML so servers content-negotiate toward text. */
    g_assert_nonnull (g_strstr_len (request, -1, "text/html"));

    raw_server_stop (rs);
}

static void
test_wire_http_404 (void)
{
    RawServer                *rs;
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;
    g_autofree gchar         *json     = NULL;

    rs   = raw_server_start ();
    exec = ai_tool_executor_new ();

    json     = g_strdup_printf (
        "{\"url\": \"http://127.0.0.1:%u/notfound\"}", rs->port);
    tool_use = wf_tool_use (json);
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    g_assert_null (result);
    g_assert_error (err, AI_ERROR, AI_ERROR_SERVER_ERROR);
    g_assert_nonnull (g_strstr_len (err->message, -1, "HTTP 404"));

    raw_server_stop (rs);
}

static void
test_wire_http_500 (void)
{
    RawServer                *rs;
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;
    g_autofree gchar         *json     = NULL;

    rs   = raw_server_start ();
    exec = ai_tool_executor_new ();

    json     = g_strdup_printf (
        "{\"url\": \"http://127.0.0.1:%u/error\"}", rs->port);
    tool_use = wf_tool_use (json);
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    g_assert_null (result);
    g_assert_error (err, AI_ERROR, AI_ERROR_SERVER_ERROR);
    g_assert_nonnull (g_strstr_len (err->message, -1, "HTTP 500"));

    raw_server_stop (rs);
}

static void
test_wire_html_converted (void)
{
    RawServer                *rs;
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;
    g_autofree gchar         *json     = NULL;

    rs   = raw_server_start ();
    exec = ai_tool_executor_new ();

    json     = g_strdup_printf (
        "{\"url\": \"http://127.0.0.1:%u/html\"}", rs->port);
    tool_use = wf_tool_use (json);
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    /* Full pipeline over a real socket: fetch -> content-type dispatch ->
     * html_to_text -> build_body. */
    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_nonnull (g_strstr_len (result, -1, "# Live Heading"));
    g_assert_nonnull (g_strstr_len (result, -1, "Live body."));
    g_assert_null (g_strstr_len (result, -1, "secret"));   /* script dropped */
    g_assert_null (g_strstr_len (result, -1, "<h1>"));     /* not raw HTML */

    raw_server_stop (rs);
}

static void
test_wire_https_fallback (void)
{
    RawServer                *rs;
    GResolver                *saved;
    TestResolver             *mock_res;
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;
    g_autofree gchar         *json     = NULL;
    g_autofree gchar         *request  = NULL;
    guint                     http_hits;
    guint                     tls_hits;

    rs = raw_server_start ();

    saved    = g_resolver_get_default ();          /* +1 ref */
    mock_res = g_object_new (TEST_TYPE_RESOLVER, NULL);
    g_resolver_set_default (G_RESOLVER (mock_res));

    exec     = ai_tool_executor_new ();
    json     = g_strdup_printf (
        "{\"url\": \"http://webfetch.test:%u/fallback\"}", rs->port);
    tool_use = wf_tool_use (json);
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    /* Restore before asserting so a failure can't leave the global default
     * dangling at our about-to-be-freed mock. */
    g_resolver_set_default (saved);
    g_object_unref (saved);
    g_object_unref (mock_res);

    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_nonnull (g_strstr_len (result, -1, "hello from server"));

    g_mutex_lock (&rs->lock);
    http_hits = rs->http_hits;
    tls_hits  = rs->tls_hits;
    g_mutex_unlock (&rs->lock);

    /* The https probe was attempted (and fast-failed) AND the http fall-back
     * actually reached the server. */
    g_assert_cmpuint (tls_hits,  >=, 1);
    g_assert_cmpuint (http_hits, >=, 1);

    request = raw_server_take_request (rs);
    g_assert_nonnull (request);
    g_assert_nonnull (g_strstr_len (request, -1, "GET /fallback HTTP/1.1"));

    raw_server_stop (rs);
}

static void
test_wire_timeout (void)
{
    BlackHole              *bh;
    g_autoptr(SoupSession)  session = NULL;
    g_autoptr(SoupMessage)  msg     = NULL;
    g_autoptr(GBytes)       bytes   = NULL;
    g_autoptr(GError)       err     = NULL;
    g_autofree gchar       *url     = NULL;

    /* Lock the production timeout constant alongside the live-timeout check. */
    g_assert_cmpint (WEB_FETCH_TIMEOUT_SECS, ==, 30);

    bh      = black_hole_start ();
    session = soup_session_new ();
    g_object_set (session, "timeout", (guint) 1, NULL);   /* 1 s, not 30 */
    url     = g_strdup_printf ("http://127.0.0.1:%u/", bh->port);

    /* Peer accepts but never replies: send_and_read must hit the I/O timeout
     * (the same mechanism tool_web_fetch arms with the 30 s value). */
    bytes = web_fetch_get (session, url, &msg, NULL, &err);
    g_assert_null (bytes);
    g_assert_nonnull (err);

    black_hole_stop (bh);
}

/* ============================================================
 * prompt-extraction sub-model path (web_fetch_extract)
 * ============================================================ */

static void
test_extract_success (void)
{
    g_autoptr(MockProvider)  mock = NULL;
    g_autofree gchar        *out  = NULL;

    mock = mock_provider_new ("EXTRACTED SUMMARY", FALSE);

    out = web_fetch_extract (AI_PROVIDER (mock),
                             "https://docs.example/page",
                             "the full converted page content",
                             "Summarise in one line",
                             NULL);

    g_assert_cmpstr (out, ==, "EXTRACTED SUMMARY");
    g_assert_cmpint (mock->calls, ==, 1);

    /* The sub-call wove url + content + task into the user message, set a
     * system prompt, and passed the default token budget. */
    g_assert_nonnull (mock->seen_user_text);
    g_assert_nonnull (g_strstr_len (mock->seen_user_text, -1,
                                    "https://docs.example/page"));
    g_assert_nonnull (g_strstr_len (mock->seen_user_text, -1,
                                    "the full converted page content"));
    g_assert_nonnull (g_strstr_len (mock->seen_user_text, -1,
                                    "Summarise in one line"));
    g_assert_nonnull (mock->seen_system);
    g_assert_nonnull (g_strstr_len (mock->seen_system, -1, "extract"));
    g_assert_cmpint (mock->seen_max_tokens, ==, DEFAULT_MAX_TOKENS);
}

static void
test_extract_error_returns_null (void)
{
    g_autoptr(MockProvider) mock = NULL;
    gchar                  *out;

    mock = mock_provider_new (NULL, TRUE);   /* provider errors */

    /* The failure is logged as a warning and swallowed (caller falls back to
     * the raw content), so expect the warning rather than abort on it. */
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "*prompt extraction failed*");
    out = web_fetch_extract (AI_PROVIDER (mock),
                             "https://docs.example/page",
                             "content", "task", NULL);
    g_test_assert_expected_messages ();

    g_assert_null (out);
    g_assert_cmpint (mock->calls, ==, 1);
}

/* Full tool_web_fetch prompt branch: cache-seeded body (no network) routed
 * through the active provider for extraction. */
static void
test_extract_via_tool_web_fetch (void)
{
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(MockProvider)   mock     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;
    const gchar              *url      = "https://extract.example/seeded-doc";

    exec = ai_tool_executor_new ();
    mock = mock_provider_new ("CONDENSED", FALSE);

    /* Seed the cache so tool_web_fetch takes the hit path (no socket) and
     * goes straight to the prompt-extraction branch. */
    web_fetch_cache_store (url, "SEEDED BODY CONTENT");

    /* active_provider is normally set for the duration of a run(); set it
     * directly for the test. */
    exec->active_provider = AI_PROVIDER (mock);

    tool_use = wf_tool_use (
        "{\"url\": \"https://extract.example/seeded-doc\", "
        "\"prompt\": \"condense\"}");
    result = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    exec->active_provider = NULL;   /* borrowed; drop before mock is freed */

    g_assert_no_error (err);
    g_assert_cmpstr (result, ==, "CONDENSED");
    g_assert_cmpint (mock->calls, ==, 1);
    g_assert_nonnull (g_strstr_len (mock->seen_user_text, -1,
                                    "SEEDED BODY CONTENT"));
}

static void
test_extract_private_context (void)
{
    if (g_test_subprocess ())
    {
        g_autoptr(GMainContext) context = g_main_context_new ();

        g_main_context_push_thread_default (context);
        test_extract_via_tool_web_fetch ();
        g_main_context_pop_thread_default (context);
        return;
    }

    /* A mismatched nested loop hangs even after the mock completes its task. */
    g_test_trap_subprocess (NULL, 3 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_passed ();
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/ai-glib/web-fetch/extract/private-context",
                     test_extract_private_context);

    /* html_to_text */
    g_test_add_func ("/ai-glib/web-fetch/html/basic", test_html_basic);
    g_test_add_func ("/ai-glib/web-fetch/html/empty", test_html_empty);
    g_test_add_func ("/ai-glib/web-fetch/html/malformed", test_html_malformed);
    g_test_add_func ("/ai-glib/web-fetch/html/entities", test_html_entities);
    g_test_add_func ("/ai-glib/web-fetch/html/all-headings",
                     test_html_all_heading_levels);
    g_test_add_func ("/ai-glib/web-fetch/html/not-a-heading",
                     test_html_not_a_heading);
    g_test_add_func ("/ai-glib/web-fetch/html/link-without-href",
                     test_html_link_without_href);
    g_test_add_func ("/ai-glib/web-fetch/html/nested-inline",
                     test_html_nested_inline);
    g_test_add_func ("/ai-glib/web-fetch/html/utf8", test_html_utf8_preserved);
    g_test_add_func ("/ai-glib/web-fetch/html/drops-noscript-head",
                     test_html_drops_noscript_and_head);
    g_test_add_func ("/ai-glib/web-fetch/html/whitespace",
                     test_html_collapses_whitespace);

    /* <img> extraction */
    g_test_add_func ("/ai-glib/web-fetch/html/img-relative",
                     test_html_img_relative_resolves_against_base);
    g_test_add_func ("/ai-glib/web-fetch/html/img-protocol-relative",
                     test_html_img_protocol_relative_resolves);
    g_test_add_func ("/ai-glib/web-fetch/html/img-absolute",
                     test_html_img_absolute_passthrough);
    g_test_add_func ("/ai-glib/web-fetch/html/img-no-alt",
                     test_html_img_no_alt);
    g_test_add_func ("/ai-glib/web-fetch/html/img-no-src",
                     test_html_img_no_src_ignored);
    g_test_add_func ("/ai-glib/web-fetch/html/img-base-href",
                     test_html_img_honors_base_href);

    /* web_fetch_truncate */
    g_test_add_func ("/ai-glib/web-fetch/truncate/under", test_truncate_under_limit);
    g_test_add_func ("/ai-glib/web-fetch/truncate/over", test_truncate_over_limit);
    g_test_add_func ("/ai-glib/web-fetch/truncate/utf8-boundary",
                     test_truncate_utf8_boundary);

    /* upgrade decision */
    g_test_add_func ("/ai-glib/web-fetch/upgrade-decision", test_upgrade_decision);

    /* cache */
    g_test_add_func ("/ai-glib/web-fetch/cache/roundtrip", test_cache_roundtrip);
    g_test_add_func ("/ai-glib/web-fetch/cache/overwrite", test_cache_overwrite);
    g_test_add_func ("/ai-glib/web-fetch/cache/ttl-expiry", test_cache_ttl_expiry);
    g_test_add_func ("/ai-glib/web-fetch/cache/self-cleaning",
                     test_cache_self_cleaning_sweep);

    /* build-body: content-type dispatch, redirect notice, truncation */
    g_test_add_func ("/ai-glib/web-fetch/body/html", test_body_html_converted);
    g_test_add_func ("/ai-glib/web-fetch/body/html-img-resolved",
                     test_body_html_img_resolved_against_final_url);
    g_test_add_func ("/ai-glib/web-fetch/body/xhtml", test_body_xhtml_converted);
    g_test_add_func ("/ai-glib/web-fetch/body/json", test_body_json_passthrough);
    g_test_add_func ("/ai-glib/web-fetch/body/plain",
                     test_body_plain_text_passthrough);
    g_test_add_func ("/ai-glib/web-fetch/body/null-content-type",
                     test_body_null_content_type_passthrough);
    g_test_add_func ("/ai-glib/web-fetch/body/binary",
                     test_body_binary_placeholder);
    g_test_add_func ("/ai-glib/web-fetch/body/redirect-notice",
                     test_body_redirect_notice);
    g_test_add_func ("/ai-glib/web-fetch/body/no-redirect-same-host",
                     test_body_no_redirect_notice_same_host);
    g_test_add_func ("/ai-glib/web-fetch/body/no-redirect-null-hosts",
                     test_body_no_redirect_notice_null_hosts);
    g_test_add_func ("/ai-glib/web-fetch/body/truncates-large",
                     test_body_truncates_large);

    /* live HTTP transport over a loopback socket */
    g_test_add_func ("/ai-glib/web-fetch/wire/request-headers",
                     test_wire_request_headers);
    g_test_add_func ("/ai-glib/web-fetch/wire/http-404",
                     test_wire_http_404);
    g_test_add_func ("/ai-glib/web-fetch/wire/http-500",
                     test_wire_http_500);
    g_test_add_func ("/ai-glib/web-fetch/wire/html-converted",
                     test_wire_html_converted);
    g_test_add_func ("/ai-glib/web-fetch/wire/https-fallback",
                     test_wire_https_fallback);
    g_test_add_func ("/ai-glib/web-fetch/wire/timeout",
                     test_wire_timeout);

    /* prompt-extraction sub-model path */
    g_test_add_func ("/ai-glib/web-fetch/extract/success",
                     test_extract_success);
    g_test_add_func ("/ai-glib/web-fetch/extract/error-returns-null",
                     test_extract_error_returns_null);
    g_test_add_func ("/ai-glib/web-fetch/extract/via-tool-web-fetch",
                     test_extract_via_tool_web_fetch);

    return g_test_run ();
}
