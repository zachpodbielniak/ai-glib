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

gboolean
ai_session_limit_parse_reset(const gchar *text,
                            gint64       now,
                            gint64      *reset_out)
{
    const gchar *at;
    const gchar *after_clock = NULL;
    gint minutes;
    g_autoptr(GTimeZone) zone = NULL;
    g_autoptr(GDateTime) now_local = NULL;
    g_autoptr(GDateTime) candidate = NULL;

    if (text == NULL)
        return FALSE;

    at = strstr(text, "resets");

    if (at == NULL)
        return FALSE;

    at += strlen("resets");

    while (*at == ' ')
        at++;

    minutes = parse_clock(at, &after_clock);

    if (minutes < 0)
        return FALSE;

    zone = parse_zone(after_clock);
    now_local = g_date_time_new_from_unix_utc(now);

    if (now_local == NULL)
        return FALSE;

    {
        g_autoptr(GDateTime) in_zone = g_date_time_to_timezone(now_local, zone);

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
            g_autoptr(GDateTime) tomorrow =
                g_date_time_add_days(candidate, 1);

            if (tomorrow == NULL)
                return FALSE;

            if (reset_out != NULL)
                *reset_out = g_date_time_to_unix(tomorrow);

            return TRUE;
        }
    }

    if (reset_out != NULL)
        *reset_out = g_date_time_to_unix(candidate);

    return TRUE;
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
