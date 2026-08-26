/*
 * test-session-limit.c - Recognising an account's session usage limit
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A usage limit is not a failed request: the CLI never reaches the API,
 * writes one message of its own and exits.  Every layer above has to be
 * able to tell that apart from a real failure, and all of them ask these
 * functions -- so this is where the rule is pinned.
 */

#include <glib.h>

#include <string.h>

#include "core/ai-session-limit.h"
#include "providers/ai-claude-code-client-internal.h"

/* The real message, from a transcript. */
#define REAL_TEXT \
    "You've hit your session limit \xc2\xb7 resets 6:50pm (America/New_York)"

/* 2026-08-25T12:00:00Z */
#define NOON_UTC G_GINT64_CONSTANT(1787659200)

static void
test_the_real_signature_is_recognised(void)
{
    g_assert_true(ai_session_limit_looks_synthetic("<synthetic>", 0, 0, 0, 0));
}

/*
 * Both halves are required.  A real completion that happened to bill
 * nothing is not a limit, and a synthetic label on a message that
 * consumed tokens is something else again.
 */
static void
test_a_real_turn_is_not_a_limit(void)
{
    g_assert_false(ai_session_limit_looks_synthetic("claude-opus-5", 0, 0, 0, 0));
    g_assert_false(ai_session_limit_looks_synthetic("<synthetic>", 4, 278, 0, 0));
    g_assert_false(ai_session_limit_looks_synthetic(NULL, 0, 0, 0, 0));
}

/*
 * A turn served entirely from cache reports zero input and zero output
 * and has really run.  Reading only those two would park a working
 * agent until a reset that is not coming.
 */
static void
test_a_cached_turn_is_not_a_limit(void)
{
    g_assert_false(ai_session_limit_looks_synthetic("<synthetic>", 0, 0, 0, 900));
    g_assert_false(ai_session_limit_looks_synthetic("<synthetic>", 0, 0, 512, 0));
}

static void
test_the_reset_time_is_read(void)
{
    gint64 reset = 0;

    g_assert_true(ai_session_limit_parse_reset(REAL_TEXT, NOON_UTC, &reset));

    /*
     * 18:50 America/New_York on the same day is 22:50 UTC, which is
     * after noon UTC -- so it is today rather than tomorrow.
     */
    {
        g_autoptr(GTimeZone) ny = g_time_zone_new_identifier("America/New_York");
        g_autoptr(GDateTime) when = NULL;

        if (ny == NULL) {
            g_test_skip("this host has no tzdata for America/New_York");
            return;
        }

        when = g_date_time_new_from_unix_utc(reset);
        g_assert_nonnull(when);

        {
            g_autoptr(GDateTime) local = g_date_time_to_timezone(when, ny);

            g_assert_cmpint(g_date_time_get_hour(local), ==, 18);
            g_assert_cmpint(g_date_time_get_minute(local), ==, 50);
        }
    }

    g_assert_cmpint(reset, >, NOON_UTC);
}

/*
 * The message carries a time and no date, so it means the next
 * occurrence.  Reading it as today puts a morning reset hours in the
 * past when the limit is hit at night -- which downstream reads as
 * "already expired", so the agent tries again at once and the pause
 * accomplishes nothing.  This is the case a function that read the
 * clock itself could not be tested on.
 */
static void
test_a_reset_already_past_today_is_tomorrow(void)
{
    gint64 reset = 0;
    /* 2026-08-25T23:00:00Z, well after 06:50 anywhere westward. */
    gint64 late = NOON_UTC + (11 * 3600);

    g_assert_true(ai_session_limit_parse_reset(
        "You've hit your session limit \xc2\xb7 resets 6:50am (UTC)",
        late, &reset));

    g_assert_cmpint(reset, >, late);
    /* Within a day, not a week: it is the *next* occurrence. */
    g_assert_cmpint(reset - late, <, 24 * 3600);
}

static void
test_a_24_hour_clock_is_read(void)
{
    gint64 reset = 0;

    g_assert_true(ai_session_limit_parse_reset("resets 18:50 (UTC)",
                                               NOON_UTC, &reset));
    g_assert_cmpint(reset, ==, NOON_UTC + (6 * 3600) + (50 * 60));
}

/*
 * Noon and midnight are the two the twelve-hour clock gets wrong, and
 * getting them wrong puts the reset half a day out in the case somebody
 * is most likely to be watching.
 */
static void
test_noon_and_midnight(void)
{
    gint64 reset = 0;
    gint64 midnight_start = NOON_UTC - (11 * 3600); /* 01:00Z */

    g_assert_true(ai_session_limit_parse_reset("resets 12:00pm (UTC)",
                                               midnight_start, &reset));
    g_assert_cmpint(reset, ==, NOON_UTC);

    g_assert_true(ai_session_limit_parse_reset("resets 12:30am (UTC)",
                                               midnight_start, &reset));
    g_assert_cmpint(reset, ==, midnight_start - (30 * 60) + (24 * 3600) - 0);
}

/*
 * Text with no reset in it must say so rather than inventing one.  A
 * fabricated reset is worse than none: it parks an agent until a time
 * that means nothing.
 */
static void
test_text_without_a_reset(void)
{
    gint64 reset = 12345;

    g_assert_false(ai_session_limit_parse_reset(NULL, NOON_UTC, &reset));
    g_assert_false(ai_session_limit_parse_reset("all good", NOON_UTC, &reset));
    g_assert_false(ai_session_limit_parse_reset("resets soon", NOON_UTC,
                                                &reset));
    g_assert_false(ai_session_limit_parse_reset("resets 25:00 (UTC)",
                                                NOON_UTC, &reset));
    /* "6:5" is not a time. */
    g_assert_false(ai_session_limit_parse_reset("resets 6:5 (UTC)",
                                                NOON_UTC, &reset));

    g_assert_cmpint(reset, ==, 12345);
}

/*
 * A zone this host does not know falls back to local rather than
 * failing.  An hour out is a far better answer than "no reset", which
 * every layer above reads as a limit that cannot be waited out.
 */
static void
test_an_unknown_zone_still_parses(void)
{
    gint64 reset = 0;

    g_assert_true(ai_session_limit_parse_reset(
        "resets 6:50pm (Mars/Olympus_Mons)", NOON_UTC, &reset));
    g_assert_cmpint(reset, >, 0);
}

static void
test_the_sentence_says_what_happened(void)
{
    g_autofree gchar *known = ai_session_limit_format(NOON_UTC);
    g_autofree gchar *unknown = ai_session_limit_format(0);

    g_assert_nonnull(strstr(known, "session usage limit"));
    g_assert_nonnull(strstr(known, "resets at"));

    /* Unknown must not read as though a time were given. */
    g_assert_nonnull(strstr(unknown, "did not say when"));
}

/*
 * The real line, as the CLI writes it.
 *
 * Byte for byte from a transcript on the machine this was found on,
 * middle dot and all -- the classification is what was missing, so a
 * test built from a paraphrase would be testing a sentence nobody
 * emits.
 */
#define REAL_LINE \
    "{\"type\":\"assistant\",\"timestamp\":\"2026-08-25T20:17:40.682Z\"," \
    "\"message\":{\"model\":\"<synthetic>\"," \
    "\"stop_reason\":\"stop_sequence\",\"stop_sequence\":\"\"," \
    "\"usage\":{\"input_tokens\":0,\"output_tokens\":0," \
    "\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":0}," \
    "\"content\":[{\"type\":\"text\"," \
    "\"text\":\"You've hit your session limit \xc2\xb7 resets 6:50pm " \
    "(America/New_York)\"}]}}"

static void
test_a_real_transcript_line_is_classified(void)
{
    gint64 reset = 0;

    g_assert_true(ai_claude_code_line_is_session_limit(REAL_LINE, NOON_UTC,
                                                       &reset));
    g_assert_cmpint(reset, >, NOON_UTC);
}

/*
 * An ordinary turn on the same shape must not be classified, or every
 * working agent is parked.  This is the assertion that keeps the
 * detection narrow.
 */
static void
test_an_ordinary_turn_is_not_classified(void)
{
    gint64 reset = 12345;

    g_assert_false(ai_claude_code_line_is_session_limit(
        "{\"type\":\"assistant\",\"message\":{\"model\":\"claude-opus-5\","
        "\"usage\":{\"input_tokens\":10,\"output_tokens\":20},"
        "\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}}",
        NOON_UTC, &reset));

    /* Not an assistant line at all. */
    g_assert_false(ai_claude_code_line_is_session_limit(
        "{\"type\":\"system\",\"subtype\":\"init\"}", NOON_UTC, &reset));

    /* Not JSON. */
    g_assert_false(ai_claude_code_line_is_session_limit("garbage", NOON_UTC,
                                                        &reset));
    g_assert_false(ai_claude_code_line_is_session_limit(NULL, NOON_UTC,
                                                        &reset));

    g_assert_cmpint(reset, ==, 12345);
}

/*
 * A limit whose message does not name a reset is still a limit, with an
 * unknown reset rather than none -- the two mean different things to
 * every layer above and must not collapse.
 */
static void
test_a_limit_without_a_stated_reset(void)
{
    gint64 reset = 999;

    g_assert_true(ai_claude_code_line_is_session_limit(
        "{\"type\":\"assistant\",\"message\":{\"model\":\"<synthetic>\","
        "\"usage\":{\"input_tokens\":0,\"output_tokens\":0},"
        "\"content\":[{\"type\":\"text\",\"text\":\"limit reached\"}]}}",
        NOON_UTC, &reset));

    g_assert_cmpint(reset, ==, 0);
}

/*
 * The notice round-trips.
 *
 * Two halves of one format written apart is exactly how a supervisor
 * comes to silently stop noticing the thing it was built to notice --
 * the agent goes on emitting and the watcher goes on not matching, and
 * nothing anywhere reports a problem.
 */
static void
test_the_notice_round_trips(void)
{
    g_autofree gchar *line = ai_session_limit_notice_new(NOON_UTC);
    gint64 reset = 0;

    g_assert_true(ai_session_limit_notice_parse(line, &reset));
    g_assert_cmpint(reset, ==, NOON_UTC);

    /* Embedded in a log line with a prefix, as it really arrives. */
    {
        g_autofree gchar *wrapped =
            g_strdup_printf("** Message: 04:00:00.000: %s", line);

        reset = 0;
        g_assert_true(ai_session_limit_notice_parse(wrapped, &reset));
        g_assert_cmpint(reset, ==, NOON_UTC);
    }
}

/*
 * A notice with no reset is still a notice.  Reading it as "not paused"
 * is the more dangerous of the two mistakes, so it is the one pinned.
 */
static void
test_a_notice_without_a_reset(void)
{
    g_autofree gchar *line = ai_session_limit_notice_new(0);
    gint64 reset = 5;

    g_assert_true(ai_session_limit_notice_parse(line, &reset));
    g_assert_cmpint(reset, ==, 0);
}

static void
test_an_ordinary_log_line_is_not_a_notice(void)
{
    gint64 reset = 7;

    g_assert_false(ai_session_limit_notice_parse(NULL, &reset));
    g_assert_false(ai_session_limit_notice_parse("starting up", &reset));
    g_assert_false(ai_session_limit_notice_parse(
        "Session 'x': AI call failed: something", &reset));

    g_assert_cmpint(reset, ==, 7);
}

/*
 * The sentence and the notice compose, and the notice survives it.
 *
 * This is the seam that actually broke: the first version recovered the
 * reset by re-reading the human sentence, which says "resets at 18:50"
 * -- and the time parser, reading the word after "resets", found "at"
 * and returned no reset at all.  A supervisor would have seen "paused"
 * with no time and waited for ever, and nothing would have reported a
 * fault.
 *
 * So the two are composed once, at the source, and this asserts that
 * the machine half is still recoverable from the composed string.
 */
static void
test_the_sentence_and_the_notice_compose(void)
{
    g_autofree gchar *why = ai_session_limit_format(NOON_UTC);
    g_autofree gchar *notice = ai_session_limit_notice_new(NOON_UTC);
    g_autofree gchar *composed = g_strdup_printf("%s %s", why, notice);
    gint64 reset = 0;

    g_assert_true(ai_session_limit_notice_parse(composed, &reset));
    g_assert_cmpint(reset, ==, NOON_UTC);

    /* And a person can still read it. */
    g_assert_nonnull(strstr(composed, "session usage limit"));
}

/*
 * The prose is deliberately *not* machine-readable, and saying so here
 * stops somebody restoring the shortcut that failed.
 */
static void
test_the_prose_is_not_a_machine_format(void)
{
    g_autofree gchar *why = ai_session_limit_format(NOON_UTC);
    gint64 reset = 999;

    g_assert_false(ai_session_limit_parse_reset(why, NOON_UTC, &reset));
    g_assert_cmpint(reset, ==, 999);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/session-limit/real-signature",
                    test_the_real_signature_is_recognised);
    g_test_add_func("/session-limit/a-real-turn-is-not-one",
                    test_a_real_turn_is_not_a_limit);
    g_test_add_func("/session-limit/a-cached-turn-is-not-one",
                    test_a_cached_turn_is_not_a_limit);
    g_test_add_func("/session-limit/reset-time-is-read",
                    test_the_reset_time_is_read);
    g_test_add_func("/session-limit/past-today-is-tomorrow",
                    test_a_reset_already_past_today_is_tomorrow);
    g_test_add_func("/session-limit/24-hour-clock",
                    test_a_24_hour_clock_is_read);
    g_test_add_func("/session-limit/noon-and-midnight",
                    test_noon_and_midnight);
    g_test_add_func("/session-limit/no-reset-in-the-text",
                    test_text_without_a_reset);
    g_test_add_func("/session-limit/unknown-zone",
                    test_an_unknown_zone_still_parses);
    g_test_add_func("/session-limit/the-sentence",
                    test_the_sentence_says_what_happened);

    g_test_add_func("/session-limit/real-transcript-line",
                    test_a_real_transcript_line_is_classified);
    g_test_add_func("/session-limit/ordinary-turn-not-classified",
                    test_an_ordinary_turn_is_not_classified);
    g_test_add_func("/session-limit/limit-without-a-reset",
                    test_a_limit_without_a_stated_reset);

    g_test_add_func("/session-limit/notice-round-trip",
                    test_the_notice_round_trips);
    g_test_add_func("/session-limit/notice-without-a-reset",
                    test_a_notice_without_a_reset);
    g_test_add_func("/session-limit/ordinary-line-is-not-a-notice",
                    test_an_ordinary_log_line_is_not_a_notice);
    g_test_add_func("/session-limit/sentence-and-notice-compose",
                    test_the_sentence_and_the_notice_compose);
    g_test_add_func("/session-limit/prose-is-not-machine-readable",
                    test_the_prose_is_not_a_machine_format);

    return g_test_run();
}
