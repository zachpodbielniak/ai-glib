/*
 * ai-cli-limit.c - Classifying what a wrapped CLI says when it is
 *                  limited, throttled or signed out
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "core/ai-cli-limit-private.h"
#include "core/ai-session-limit.h"

/* How much of the CLI's own message an error repeats. */
#define AI_CLI_LIMIT_EXCERPT (400)

typedef struct
{
    const gchar             *cli_name;
    const AiCliLimitPattern *patterns;
} Registration;

static GQuark
registration_quark(void)
{
    return g_quark_from_static_string("ai-cli-limit-patterns");
}

void
ai_cli_limit_register(GType                    type,
                      const gchar             *cli_name,
                      const AiCliLimitPattern *patterns)
{
    Registration *reg;

    g_return_if_fail(g_type_is_a(type, AI_TYPE_CLI_CLIENT));
    g_return_if_fail(patterns != NULL);

    /*
     * Leaked on purpose: a class datum lives as long as the type, and a
     * GType is never unregistered.  class_init runs once per type, so
     * this is one allocation per CLI for the life of the process.
     */
    reg = g_new0(Registration, 1);
    reg->cli_name = cli_name;
    reg->patterns = patterns;
    g_type_set_qdata(type, registration_quark(), reg);
}

/* The nearest registration up @type's parent chain. */
static const Registration *
registration_for(GType type)
{
    for (; type != 0 && type != G_TYPE_OBJECT; type = g_type_parent(type)) {
        const Registration *reg = g_type_get_qdata(type, registration_quark());

        if (reg != NULL)
            return reg;
    }

    return NULL;
}

/* Case-insensitive strstr, ASCII only: every needle is ASCII. */
static gboolean
contains_ci(const gchar *haystack, const gchar *needle)
{
    gsize len = strlen(needle);

    if (len == 0)
        return FALSE;

    for (; *haystack != '\0'; haystack++)
        if (g_ascii_strncasecmp(haystack, needle, len) == 0)
            return TRUE;

    return FALSE;
}

/*
 * The CLI's words on one line and bounded.  Logs and the supervisor
 * read errors a line at a time, and a CLI that dumps a whole HTTP body
 * should not become a kilobyte of error string.
 */
static gchar *
excerpt(const gchar *text)
{
    gchar *out = g_strndup(text, AI_CLI_LIMIT_EXCERPT);
    gchar *p;

    for (p = out; *p != '\0'; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';

    /* g_strndup may have cut a UTF-8 sequence in half. */
    if (!g_utf8_validate(out, -1, (const gchar **)&p))
        *p = '\0';

    return g_strstrip(out);
}

gboolean
ai_cli_limit_classify(const AiCliLimitPattern  *patterns,
                      const gchar              *cli_name,
                      const gchar              *text,
                      gint64                    now,
                      GError                  **error)
{
    const AiCliLimitPattern *p;
    g_autofree gchar *said = NULL;
    const gchar *who;
    gint64 reset = 0;
    gboolean has_reset;

    if (patterns == NULL || text == NULL || *text == '\0')
        return FALSE;

    for (p = patterns; p->needle != NULL; p++)
        if (contains_ci(text, p->needle))
            break;

    if (p->needle == NULL)
        return FALSE;

    who = cli_name != NULL ? cli_name : "the CLI";
    said = excerpt(text);
    has_reset = ai_session_limit_parse_reset(text, now, &reset) && reset > 0;

    switch (p->code) {
    case AI_ERROR_SESSION_LIMIT:
        {
            /*
             * claude-code's shape exactly: the sentence, then the
             * notice.  Whatever already matches that error -- a
             * supervisor reading the notice, a person reading the
             * sentence -- reads this one the same way.  The CLI's own
             * words follow, because "resets at 18:50" is ours and the
             * plan name in theirs is the thing a person acts on.
             */
            g_autofree gchar *why = ai_session_limit_format(has_reset ? reset : 0);
            g_autofree gchar *notice = ai_session_limit_notice_new(has_reset ? reset : 0);

            g_set_error(error, AI_ERROR, AI_ERROR_SESSION_LIMIT,
                        "%s %s (%s: %s)", why, notice, who, said);
            break;
        }

    case AI_ERROR_RATE_LIMITED:
        if (has_reset) {
            /*
             * A stated retry time is a pause like a session limit's, and
             * the notice lets the same supervisor wait it out instead of
             * retrying into the same refusal.
             */
            g_autofree gchar *notice = ai_session_limit_notice_new(reset);

            g_set_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED,
                        "%s is rate limited %s (%s: %s)",
                        who, notice, who, said);
        } else {
            g_set_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED,
                        "%s is rate limited (%s: %s)", who, who, said);
        }
        break;

    case AI_ERROR_INVALID_API_KEY:
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                    "%s is not signed in, or its credentials were refused "
                    "(%s: %s)", who, who, said);
        break;

    case AI_ERROR_INSUFFICIENT_QUOTA:
        g_set_error(error, AI_ERROR, AI_ERROR_INSUFFICIENT_QUOTA,
                    "%s has no quota or credits left (%s: %s)",
                    who, who, said);
        break;

    default:
        g_set_error(error, AI_ERROR, (gint)p->code, "%s: %s", who, said);
        break;
    }

    return TRUE;
}

void
ai_cli_limit_reclassify(AiCliClient  *self,
                        GError      **error)
{
    const Registration *reg;
    GError *replacement = NULL;
    gint64 now;

    if (error == NULL || *error == NULL || self == NULL)
        return;

    if ((*error)->domain != AI_ERROR ||
        ((*error)->code != AI_ERROR_CLI_EXECUTION &&
         (*error)->code != AI_ERROR_CLI_PARSE_ERROR))
        return;

    reg = registration_for(G_TYPE_FROM_INSTANCE(self));

    if (reg == NULL)
        return;

    now = g_get_real_time() / G_USEC_PER_SEC;

    if (!ai_cli_limit_classify(reg->patterns, reg->cli_name,
                               (*error)->message, now, &replacement))
        return;

    g_clear_error(error);
    *error = replacement;
}
