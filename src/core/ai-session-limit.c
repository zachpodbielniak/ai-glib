/*
 * ai-session-limit.c - Recognising an account's session usage limit
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-session-limit.h"

gboolean
ai_session_limit_looks_synthetic(const gchar *model,
                                 gint64       input_tokens,
                                 gint64       output_tokens,
                                 gint64       cache_creation_tokens,
                                 gint64       cache_read_tokens)
{
    if (model == NULL)
        return FALSE;

    if (g_strcmp0(model, AI_SESSION_LIMIT_SYNTHETIC_MODEL) != 0)
        return FALSE;

    return input_tokens == 0 && output_tokens == 0 &&
           cache_creation_tokens == 0 && cache_read_tokens == 0;
}

/*
 * Reads "6:50pm" or "6:50 PM" or "18:50" into minutes since midnight.
 *
 * Returns -1 when the cursor is not on a time at all, which is how the
 * caller distinguishes "resets" appearing in some other sentence from a
 * real reset time.
 */
static gint
parse_clock(const gchar *at, const gchar **end_out)
{
    gint64 hour;
    gint64 minute;
    gchar *end = NULL;

    hour = g_ascii_strtoll(at, &end, 10);

    if (end == at || hour < 0 || hour > 23)
        return -1;

    if (*end != ':')
        return -1;

    at = end + 1;
    minute = g_ascii_strtoll(at, &end, 10);

    if (end == at || minute < 0 || minute > 59)
        return -1;

    /* (end - at) rather than a digit count: "6:5" is not a time. */
    if (end - at != 2)
        return -1;

    while (*end == ' ')
        end++;

    if (g_ascii_strncasecmp(end, "pm", 2) == 0) {
        /*
         * 12pm is noon and 12am is midnight, so the wrap is not a plain
         * add.  Getting this backwards puts the reset twelve hours out
         * in the one case somebody is most likely to be watching.
         */
        if (hour < 12)
            hour += 12;

        end += 2;
    } else if (g_ascii_strncasecmp(end, "am", 2) == 0) {
        if (hour == 12)
            hour = 0;

        end += 2;
    }

    if (end_out != NULL)
        *end_out = end;

    return (gint)(hour * 60 + minute);
}

/*
 * The zone in "(America/New_York)", if there is one.
 */
static GTimeZone *
parse_zone(const gchar *after_clock)
{
    const gchar *open;
    const gchar *close;
    g_autofree gchar *name = NULL;
    GTimeZone *zone;

    if (after_clock == NULL)
        return g_time_zone_new_local();

    while (*after_clock == ' ')
        after_clock++;

    if (*after_clock != '(')
        return g_time_zone_new_local();

    open = after_clock + 1;
    close = strchr(open, ')');

    if (close == NULL || close == open)
        return g_time_zone_new_local();

    name = g_strndup(open, (gsize)(close - open));
    zone = g_time_zone_new_identifier(name);

    /*
     * An unknown zone is the local one rather than a refusal.  Being an
     * hour out is a much better answer than reporting no reset at all,
     * which downstream reads as "this cannot be waited out".
     */
    if (zone == NULL)
        zone = g_time_zone_new_local();

    return zone;
}

/*
 * The next occurrence of @minutes past midnight in @zone, strictly after
 * @now.  Shared by every form that names a wall-clock time with no date.
 */
static gboolean
reset_from_clock(gint minutes, GTimeZone *zone, gint64 now, gint64 *reset_out)
{
    g_autoptr(GDateTime) now_utc = NULL;
    g_autoptr(GDateTime) in_zone = NULL;
    g_autoptr(GDateTime) candidate = NULL;

    now_utc = g_date_time_new_from_unix_utc(now);

    if (now_utc == NULL)
        return FALSE;

    in_zone = g_date_time_to_timezone(now_utc, zone);

    if (in_zone == NULL)
        return FALSE;

    candidate = g_date_time_new(zone,
                                g_date_time_get_year(in_zone),
                                g_date_time_get_month(in_zone),
                                g_date_time_get_day_of_month(in_zone),
                                minutes / 60, minutes % 60, 0.0);

    if (candidate == NULL)
        return FALSE;

    /*
     * The message carries a time and no date, so it means the *next*
     * occurrence.  A limit reported at 23:00 that resets at 06:50 is
     * tomorrow morning, and reading it as today puts the reset eight
     * hours in the past -- which downstream reads as "already
     * expired", so the agent tries again immediately and the whole
     * point of the pause is lost.
     */
    if (g_date_time_to_unix(candidate) <= now) {
        g_autoptr(GDateTime) tomorrow = g_date_time_add_days(candidate, 1);

        if (tomorrow == NULL)
            return FALSE;

        if (reset_out != NULL)
            *reset_out = g_date_time_to_unix(tomorrow);

        return TRUE;
    }

    if (reset_out != NULL)
        *reset_out = g_date_time_to_unix(candidate);

    return TRUE;
}

/*
 * "6:50pm (America/New_York)" or "3:45 PM": a clock, then an optional
 * zone in parentheses (local when there is none).
 */
static gboolean
parse_clock_reset(const gchar *at, gint64 now, gint64 *reset_out)
{
    const gchar *after_clock = NULL;
    gint minutes;
    g_autoptr(GTimeZone) zone = NULL;

    minutes = parse_clock(at, &after_clock);

    if (minutes < 0)
        return FALSE;

    zone = parse_zone(after_clock);

    return reset_from_clock(minutes, zone, now, reset_out);
}

/*
 * Case-insensitive strstr.  The CLIs capitalise the same phrase
 * differently ("Try again in", "try again in", "Retry-After"), and none
 * of the phrases matched here contain anything but ASCII.
 */
static const gchar *
find_ci(const gchar *haystack, const gchar *needle)
{
    gsize len = strlen(needle);

    for (; *haystack != '\0'; haystack++)
        if (g_ascii_strncasecmp(haystack, needle, len) == 0)
            return haystack;

    return NULL;
}

/*
 * The seconds one duration unit is worth, or 0 for a word that is not a
 * unit.  Spelled out and abbreviated both, because codex writes "3 days
 * 2 hours", gemini-cli writes "3h20m5s" and the APIs write "30s".
 */
static gdouble
unit_seconds(const gchar *unit)
{
    static const struct {
        const gchar *name;
        gdouble      seconds;
    } units[] = {
        { "d", 86400.0 }, { "day", 86400.0 }, { "days", 86400.0 },
        { "h", 3600.0 }, { "hr", 3600.0 }, { "hrs", 3600.0 },
        { "hour", 3600.0 }, { "hours", 3600.0 },
        { "m", 60.0 }, { "min", 60.0 }, { "mins", 60.0 },
        { "minute", 60.0 }, { "minutes", 60.0 },
        { "s", 1.0 }, { "sec", 1.0 }, { "secs", 1.0 },
        { "second", 1.0 }, { "seconds", 1.0 },
        { "ms", 0.001 }, { "millisecond", 0.001 }, { "milliseconds", 0.001 }
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(units); i++)
        if (g_ascii_strcasecmp(unit, units[i].name) == 0)
            return units[i].seconds;

    return 0.0;
}

/*
 * "3h 20m", "3 days 2 hours 5 minutes", "3h20m5s", "23.5s", or -- when
 * @bare_is_seconds, as a Retry-After value is -- a plain "120".
 *
 * Stops at the first word that is not a number followed by a unit, so
 * the rest of a sentence is ignored rather than refused.  No number at
 * all ("try again in a few minutes") is no duration: inventing one
 * would park an agent until a time that means nothing.
 */
static gboolean
parse_duration(const gchar *at, gboolean bare_is_seconds, gint64 *seconds_out)
{
    gdouble total = 0.0;
    guint parts = 0;

    for (;;) {
        gdouble value;
        gdouble per_unit;
        gchar unit[16];
        gsize unit_len = 0;
        gchar *end = NULL;

        while (*at == ' ' || *at == ',' || *at == ':')
            at++;

        if (parts > 0 && g_ascii_strncasecmp(at, "and ", 4) == 0)
            at += 4;

        if (!g_ascii_isdigit(*at))
            break;

        value = g_ascii_strtod(at, &end);

        if (end == at || value < 0.0)
            break;

        at = end;

        while (*at == ' ')
            at++;

        while (g_ascii_isalpha(at[unit_len]) && unit_len < sizeof(unit) - 1) {
            unit[unit_len] = at[unit_len];
            unit_len++;
        }

        unit[unit_len] = '\0';

        if (unit_len == 0) {
            if (!bare_is_seconds || parts > 0)
                break;

            total += value;
            parts++;
            break;
        }

        per_unit = unit_seconds(unit);

        if (per_unit <= 0.0)
            break;

        total += value * per_unit;
        parts++;
        at += unit_len;
    }

    if (parts == 0)
        return FALSE;

    /* Rounded up: waking a second late costs nothing, a second early
     * costs another refused call. */
    if (seconds_out != NULL)
        *seconds_out = (gint64)total + ((gdouble)(gint64)total < total ? 1 : 0);

    return TRUE;
}

/*
 * codex's dated form, "Sep 5th, 2025 3:45 PM" (core/src/error.rs,
 * format "%b %-d{suffix}, %Y %-I:%M %p", in the local zone).  A reset
 * that is not today carries its date, so it is used as written rather
 * than as the next occurrence.
 */
static gboolean
parse_dated_reset(const gchar *at, gint64 *reset_out)
{
    static const gchar *months[] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"
    };
    gint month = 0;
    gint64 day;
    gint64 year;
    gint minutes;
    gchar *end = NULL;
    guint i;
    g_autoptr(GTimeZone) zone = NULL;
    g_autoptr(GDateTime) when = NULL;

    for (i = 0; i < G_N_ELEMENTS(months); i++)
        if (g_ascii_strncasecmp(at, months[i], 3) == 0)
            month = (gint)i + 1;

    if (month == 0)
        return FALSE;

    while (g_ascii_isalpha(*at))
        at++;

    while (*at == ' ' || *at == '.')
        at++;

    day = g_ascii_strtoll(at, &end, 10);

    if (end == at || day < 1 || day > 31)
        return FALSE;

    at = end;

    /* "st", "nd", "rd", "th" */
    while (g_ascii_isalpha(*at))
        at++;

    while (*at == ',' || *at == ' ')
        at++;

    year = g_ascii_strtoll(at, &end, 10);

    if (end == at || year < 1970 || year > 9999)
        return FALSE;

    at = end;

    while (*at == ' ' || *at == ',')
        at++;

    minutes = parse_clock(at, NULL);

    if (minutes < 0)
        return FALSE;

    zone = g_time_zone_new_local();
    when = g_date_time_new(zone, (gint)year, month, (gint)day,
                           minutes / 60, minutes % 60, 0.0);

    if (when == NULL)
        return FALSE;

    if (reset_out != NULL)
        *reset_out = g_date_time_to_unix(when);

    return TRUE;
}

/*
 * The relative forms.  Each phrase is followed by a duration measured
 * from @now; the Retry-After spellings take a bare number as seconds,
 * the way the HTTP header does.
 */
static const struct {
    const gchar *phrase;
    gboolean     bare_is_seconds;
} relative_phrases[] = {
    { "try again in", FALSE },     /* codex (older), cursor, grok */
    { "resets in", FALSE },
    { "reset in", FALSE },
    { "reset after", FALSE },      /* gemini-cli: "reset after 3h20m5s" */
    { "resets after", FALSE },
    { "retry in", FALSE },         /* gemini-cli: "Please retry in 23.5s" */
    { "retrying in", FALSE },      /* opencode's retry status */
    { "retry after", TRUE },
    { "retry-after", TRUE },
    { "retry_after", TRUE }
};

gboolean
ai_session_limit_parse_reset(const gchar *text,
                            gint64       now,
                            gint64      *reset_out)
{
    const gchar *at;
    guint i;

    if (text == NULL)
        return FALSE;

    /* claude-code: "resets 6:50pm (America/New_York)". */
    for (at = find_ci(text, "resets"); at != NULL;
         at = find_ci(at + 1, "resets")) {
        const gchar *clock = at + strlen("resets");

        while (*clock == ' ')
            clock++;

        if (parse_clock_reset(clock, now, reset_out))
            return TRUE;
    }

    /* codex: "Try again at 3:45 PM." or "Try again at Sep 5th, 2025 3:45 PM." */
    for (at = find_ci(text, "try again at"); at != NULL;
         at = find_ci(at + 1, "try again at")) {
        const gchar *when = at + strlen("try again at");

        while (*when == ' ')
            when++;

        if (parse_clock_reset(when, now, reset_out) ||
            parse_dated_reset(when, reset_out))
            return TRUE;
    }

    for (i = 0; i < G_N_ELEMENTS(relative_phrases); i++) {
        for (at = find_ci(text, relative_phrases[i].phrase); at != NULL;
             at = find_ci(at + 1, relative_phrases[i].phrase)) {
            gint64 seconds = 0;

            if (!parse_duration(at + strlen(relative_phrases[i].phrase),
                                relative_phrases[i].bare_is_seconds,
                                &seconds))
                continue;

            if (reset_out != NULL)
                *reset_out = now + seconds;

            return TRUE;
        }
    }

    return FALSE;
}

gchar *
ai_session_limit_format(gint64 reset)
{
    g_autoptr(GDateTime) when = NULL;
    g_autofree gchar *stamp = NULL;

    if (reset <= 0)
        return g_strdup("the account's session usage limit is reached, and "
                        "the message did not say when it resets");

    when = g_date_time_new_from_unix_local(reset);

    if (when == NULL)
        return g_strdup("the account's session usage limit is reached");

    stamp = g_date_time_format(when, "%H:%M on %A %-d %B");

    return g_strdup_printf("the account's session usage limit is reached; "
                           "it resets at %s", stamp);
}

gchar *
ai_session_limit_notice_new(gint64 reset)
{
    return g_strdup_printf("%s reset=%" G_GINT64_FORMAT,
                           AI_SESSION_LIMIT_NOTICE_PREFIX,
                           reset > 0 ? reset : 0);
}

gboolean
ai_session_limit_notice_parse(const gchar *line, gint64 *reset_out)
{
    const gchar *at;
    const gchar *value;
    gchar *end = NULL;
    gint64 reset;

    if (line == NULL)
        return FALSE;

    at = strstr(line, AI_SESSION_LIMIT_NOTICE_PREFIX);

    if (at == NULL)
        return FALSE;

    value = strstr(at, "reset=");

    /*
     * The marker without a reset is still a notice.  Refusing here
     * would turn "paused, I do not know until when" into "not paused",
     * which is the more dangerous of the two readings.
     */
    if (value == NULL) {
        if (reset_out != NULL)
            *reset_out = 0;

        return TRUE;
    }

    value += strlen("reset=");
    reset = g_ascii_strtoll(value, &end, 10);

    if (end == value || reset < 0)
        reset = 0;

    if (reset_out != NULL)
        *reset_out = reset;

    return TRUE;
}
