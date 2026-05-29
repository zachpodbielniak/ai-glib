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

    text = html_to_text (html, strlen (html), NULL);
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
    g_autofree gchar *text = html_to_text ("", 0, NULL);

    /* Empty input: libxml yields no root -> NULL, or an empty document. */
    g_assert_true (text == NULL || text[0] == '\0');
}

static void
test_html_malformed (void)
{
    /* Unclosed tags: HTML_PARSE_RECOVER must still extract the text. */
    const gchar      *html = "<p>hello<b>world<i>nested";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

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
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

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
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

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
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1, "NotHead"));
    g_assert_null (g_strstr_len (text, -1, "# NotHead"));
}

static void
test_html_link_without_href (void)
{
    const gchar      *html = "<a>bare</a> and <a href=\"\">empty</a>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

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
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

    g_assert_nonnull (text);
    g_assert_nonnull (g_strstr_len (text, -1, "a "));
    g_assert_nonnull (g_strstr_len (text, -1, "bold"));
    g_assert_nonnull (g_strstr_len (text, -1, " c"));
}

static void
test_html_utf8_preserved (void)
{
    const gchar      *html = "<p>caf\xc3\xa9 r\xc3\xa9sum\xc3\xa9</p>";
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

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
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

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
    g_autofree gchar *text = html_to_text (html, strlen (html), NULL);

    g_assert_nonnull (text);
    g_assert_null (g_strstr_len (text, -1, "\n\n\n"));   /* <= 2 newlines */
    g_assert_nonnull (g_strstr_len (text, -1, "one"));
    g_assert_nonnull (g_strstr_len (text, -1, "two"));
    g_assert_nonnull (g_strstr_len (text, -1, "three"));
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

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);

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

    return g_test_run ();
}
