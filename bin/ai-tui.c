/*
 * ai-tui.c - A terminal agent harness over AiConversation
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Deliberately thin. Everything about what a conversation *is* -- how tool
 * calls group, how a summary reads, which run of bytes is a filename --
 * lives in the library's view layer. What is left here is: turn an
 * AiStyleTag into an ncurses attribute, put characters on a screen, and
 * read keys. That is the whole of it, and it is the measure of whether the
 * split was drawn in the right place: an Emacs front-end replaces this file
 * and nothing else.
 */

#include <locale.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <ncurses.h>
#include <glib-unix.h>

#include <ai-glib.h>

/* ================================================================
 * Options
 * ================================================================ */

static gchar    *opt_provider = NULL;
static gchar    *opt_model = NULL;
static gchar    *opt_system = NULL;
static gchar    *opt_effort = NULL;
static gchar    *opt_dump = NULL;
static gchar   **opt_set = NULL;
static gint      opt_max_tokens = 4096;
static gint      opt_width = 0;
static gboolean  opt_no_stream = FALSE;
static gboolean  opt_continue = FALSE;
static gboolean  opt_skip_permissions = FALSE;
static gboolean  opt_local_tools = FALSE;
static gboolean  opt_yes = FALSE;
static gboolean  opt_dry_run = FALSE;
static gboolean  opt_no_expand = FALSE;
static gboolean  opt_no_agents = FALSE;
static gboolean  opt_version = FALSE;
static gboolean  opt_license = FALSE;

static const GOptionEntry option_entries[] = {
    { "provider", 'p', 0, G_OPTION_ARG_STRING, &opt_provider,
      "Provider: claude, openai, gemini, grok, ollama, claude-code, "
      "claude-tmux, opencode, grok-build, antigravity (agy), cursor", "NAME" },
    { "model", 'm', 0, G_OPTION_ARG_STRING, &opt_model,
      "Model id", "MODEL" },
    { "system", 's', 0, G_OPTION_ARG_STRING, &opt_system,
      "System prompt", "TEXT" },
    { "effort", 0, 0, G_OPTION_ARG_STRING, &opt_effort,
      "Reasoning effort: low, medium, high, xhigh, max", "LEVEL" },
    { "max-tokens", 0, 0, G_OPTION_ARG_INT, &opt_max_tokens,
      "Maximum tokens per response (default 4096)", "N" },
    { "no-stream", 0, 0, G_OPTION_ARG_NONE, &opt_no_stream,
      "Wait for each whole turn instead of streaming it", NULL },
    { "continue", 'c', 0, G_OPTION_ARG_NONE, &opt_continue,
      "Continue the provider's most recent session", NULL },
    { "skip-permissions", 0, 0, G_OPTION_ARG_NONE, &opt_skip_permissions,
      "Let a wrapped CLI run its tools without asking", NULL },
    { "local-tools", 0, 0, G_OPTION_ARG_NONE, &opt_local_tools,
      "Run tools in this process (HTTP providers only)", NULL },
    { "yes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes,
      "Approve every local tool call without asking", NULL },
    { "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_set,
      "Set a provider property (repeatable)", "PROP=VALUE" },
    { "no-expand", 0, 0, G_OPTION_ARG_NONE, &opt_no_expand,
      "Send input verbatim: no @ mentions, no / commands", NULL },
    { "no-agents", 0, 0, G_OPTION_ARG_NONE, &opt_no_agents,
      "Do not let the model start background agents", NULL },
    { "dump", 0, 0, G_OPTION_ARG_STRING, &opt_dump,
      "Run one prompt without a terminal and print the transcript", "PROMPT" },
    { "width", 0, 0, G_OPTION_ARG_INT, &opt_width,
      "Wrap width for --dump (0 for none)", "N" },
    { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run,
      "Print the command a CLI provider would run, then exit", NULL },
    { "version", 'v', 0, G_OPTION_ARG_NONE, &opt_version,
      "Print the version and exit", NULL },
    { "license", 0, 0, G_OPTION_ARG_NONE, &opt_license,
      "Print licensing information and exit", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
};

/* ================================================================
 * Colour
 * ================================================================ */

/*
 * One colour pair per style role.
 *
 * The library says "this run is a tool target"; deciding that means cyan is
 * this file's job and nobody else's. An Emacs front-end makes the same
 * decision with defface and shares none of this code.
 */
static short
pair_for_tag(AiStyleTag tag)
{
    return (short)(tag + 1);
}

/*
 * One colour per style tag, in enum order.
 *
 * The assertion below is the point. This used to be a list of init_pair()
 * calls, and a tag added to the library without one got an *uninitialised*
 * pair --- which ncurses renders as black on black, so the text was not
 * merely unstyled, it was invisible. Five tags were added at once and
 * every one of them disappeared: a typed /command, an @mention, and all
 * three states of the todo block.
 *
 * Indexed by tag, sized by AI_STYLE_N_TAGS: adding a tag now breaks this
 * build until somebody picks a colour, which is the loud failure the
 * silent one deserved.
 */
static const short TAG_COLOURS[] = {
    -1,             /* default       */
    COLOR_WHITE,    /* user-prompt   */
    COLOR_WHITE,    /* heading       */
    COLOR_BLUE,     /* dim           */
    COLOR_MAGENTA,  /* tool-name     */
    COLOR_CYAN,     /* tool-target   */
    COLOR_YELLOW,   /* tool-pending  */
    COLOR_GREEN,    /* tool-ok       */
    COLOR_RED,      /* tool-failed   */
    COLOR_GREEN,    /* added         */
    COLOR_RED,      /* removed       */
    COLOR_CYAN,     /* code          */
    COLOR_BLUE,     /* thinking      */
    COLOR_RED,      /* error         */
    COLOR_YELLOW,   /* status        */
    COLOR_BLUE,     /* link          */
    COLOR_BLUE,     /* marker        */
    COLOR_CYAN,     /* mention       */
    COLOR_MAGENTA,  /* command       */
    -1,             /* todo-pending  */
    COLOR_YELLOW,   /* todo-active   */
    COLOR_GREEN     /* todo-done     */
};

G_STATIC_ASSERT(G_N_ELEMENTS(TAG_COLOURS) == AI_STYLE_N_TAGS);

static void
init_colours(void)
{
    guint i;

    if (!has_colors())
    {
        return;
    }

    start_color();
    use_default_colors();

    for (i = 0; i < AI_STYLE_N_TAGS; i++)
    {
        init_pair(pair_for_tag((AiStyleTag)i), TAG_COLOURS[i], -1);
    }
}

static attr_t
attr_for_tag(AiStyleTag tag)
{
    attr_t attr = has_colors() ? COLOR_PAIR(pair_for_tag(tag)) : 0;

    switch (tag)
    {
        case AI_STYLE_USER_PROMPT:
        case AI_STYLE_HEADING:
        case AI_STYLE_TOOL_NAME:
        case AI_STYLE_COMMAND:
        case AI_STYLE_TODO_ACTIVE:
            attr |= A_BOLD;
            break;
        case AI_STYLE_DIM:
        case AI_STYLE_THINKING:
        case AI_STYLE_MARKER:
        case AI_STYLE_TODO_DONE:
            attr |= A_DIM;
            break;
        case AI_STYLE_MENTION:
            attr |= A_UNDERLINE;
            break;
        case AI_STYLE_ERROR:
            attr |= A_BOLD;
            break;
        default:
            break;
    }

    return attr;
}

/*
 * How long after a key burst to read once more, in milliseconds.
 *
 * Long enough that a real arrow-key sequence has arrived whole, short
 * enough that Escape feels immediate.
 */
#define ESCAPE_SETTLE_MS (30)

/* How many candidates the menu shows at once. */
#define MENU_MAX_ROWS (10)

/*
 * How often the spinner advances, in milliseconds.
 *
 * Fast enough to read as motion, slow enough that a turn spent waiting on
 * a slow model is not also spending a core on redrawing one glyph.
 */
#define SPINNER_INTERVAL_MS (110)

/*
 * How many background agents may run at once.
 *
 * A ceiling rather than a queue depth --- anything beyond it waits its
 * turn, nothing is dropped. Four because these are real model calls
 * being billed, and a model that decides to fan out twenty ways should
 * find out about the limit rather than the bill.
 */
#define AGENT_MAX_CONCURRENT (4)

/*
 * A keycode of our own, for a key ncurses has no name for.
 *
 * Above KEY_MAX so it cannot collide with anything ncurses returns; the
 * sequences that produce it are registered with define_key() at startup.
 */
#define KEY_SHIFT_ENTER (KEY_MAX + 1)

/*
 * How long a first ^C stays armed, in milliseconds.
 *
 * Long enough to be a deliberate second press, short enough that a ^C
 * pressed a minute ago cannot combine with one now to quit. Quitting a
 * session by accident costs the conversation, so the bar is two presses
 * that clearly belong together.
 */
#define INTERRUPT_WINDOW_MS (1500)

/*
 * The frames. Braille cells, because they animate in place without the
 * line reflowing --- every one is a single column wide.
 *
 * ai-tui already requires ncursesw and draws with box characters and
 * ballot boxes, so this adds no assumption the transcript did not.
 */
static const gchar *SPINNER_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};

/* ================================================================
 * The application
 * ================================================================ */

typedef struct
{
    AiConversation *conversation;
    GMainLoop      *loop;
    GCancellable   *cancellable;

    WINDOW         *transcript_win;
    WINDOW         *status_win;
    WINDOW         *input_win;

    GString        *input;
    guint           cursor;        /* byte offset into input */
    GPtrArray      *history;       /* previously sent lines */
    gint            history_pos;   /* -1 means "editing a new line" */

    gint            scroll;        /* first visible rendered row */
    gboolean        follow;        /* stick to the bottom */
    gint            selected;      /* block index, or -1 */

    guint           redraw_id;     /* pending idle redraw */
    gboolean        running;
    gboolean        approve_all;

    /* The harness layer: what /name and @path mean here. */
    AiResourceRegistry  *registry;
    AiCommandSet        *commands;
    AiCompletionContext *completion;

    /* The completion popup, live only while it is open. */
    AiCompletionResult  *candidates;
    guint                candidate_index;

    /* The first visible row. Kept across keystrokes so moving the
     * selection one row scrolls by one row, rather than recentring the
     * whole menu every time. */
    guint                candidate_first;

    /* Set by Escape, cleared by the next edit. Without it the menu would
     * reappear on the very next keystroke and Escape would do nothing. */
    gboolean             completion_dismissed;

    /* One-shot re-read, so a lone Escape is not stuck behind the next
     * keystroke. See on_key_settle(). */
    guint                settle_id;

    /* A ^C is waiting to see whether a second one follows. Disarmed by a
     * timer rather than by the next keystroke, so it is a window of time
     * and not a mode somebody can be left stuck in. */
    guint                interrupt_id;

    /* The spinner: a repeating timer that lives only while a turn does. */
    guint                spinner_id;
    guint                spinner_frame;

    /* Set only by --dump, and quit by whichever callback finishes the
     * turn. Polling the busy flag is not enough: a line that resolves to
     * a built-in never sets it at all. */
    GMainLoop           *dump_loop;
} App;

/* One rendered row: which block it came from, and its text. */
typedef struct
{
    AiViewBlock    *block;
    guint           block_index;
    AiRenderedText *rendered;
    guint           line_start;    /* byte offset within rendered */
    guint           line_len;
} Row;

static void app_schedule_redraw(App *app);
static void draw_completion(App *app);
static void completion_advance(App *app);
static void completion_refresh(App *app);
static gboolean completion_select(App *app, gint delta);
static void completion_accept(App *app);
static void completion_close(App *app);
static void on_input_sent(GObject *source, GAsyncResult *result,
                          gpointer user_data);
static void say(App *app, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static GObject *build_provider_named(const gchar *name,
                                     gboolean     initial,
                                     GError     **error);

/* ---------------------------------------------------------------- */

static void
row_free(gpointer data)
{
    Row *row = data;

    ai_rendered_text_unref(row->rendered);
    g_free(row);
}

/*
 * Flatten the transcript into screen rows at the current width.
 *
 * Each block is rendered once and split on its newlines, so a block that
 * wrapped to four lines contributes four rows that all point back at it.
 * That back-pointer is what makes selection and expand/collapse work
 * without the frontend tracking geometry of its own.
 */
static GPtrArray *
build_rows(App *app, gint width)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(row_free);
    AiTranscript *transcript = ai_conversation_get_transcript(app->conversation);
    guint n = ai_transcript_get_n_blocks(transcript);
    guint i;

    for (i = 0; i < n; i++)
    {
        AiViewBlock *block = ai_transcript_get_block(transcript, i);
        AiRenderedText *rendered = ai_view_block_render(block, (guint)width);
        const gchar *text = ai_rendered_text_get_text(rendered);
        const gchar *line = text;
        const gchar *p;

        if (i > 0)
        {
            /* A blank row between blocks, so the transcript breathes. */
            Row *spacer = g_new0(Row, 1);

            spacer->block = block;
            spacer->block_index = i;
            spacer->rendered = ai_rendered_text_ref(rendered);
            spacer->line_start = 0;
            spacer->line_len = 0;
            g_ptr_array_add(rows, spacer);
        }

        for (p = text; ; p++)
        {
            if (*p == '\n' || *p == '\0')
            {
                Row *row = g_new0(Row, 1);

                row->block = block;
                row->block_index = i;
                row->rendered = ai_rendered_text_ref(rendered);
                row->line_start = (guint)(line - text);
                row->line_len = (guint)(p - line);
                g_ptr_array_add(rows, row);

                if (*p == '\0')
                {
                    break;
                }

                line = p + 1;
            }
        }

        ai_rendered_text_unref(rendered);
    }

    return rows;
}

/* Draw one row, switching attributes as the spans say. */
static void
draw_row(App *app, WINDOW *win, gint y, Row *row, gboolean selected)
{
    const gchar *text = ai_rendered_text_get_text(row->rendered);
    guint offset = row->line_start;
    guint end = row->line_start + row->line_len;
    gint x = 0;

    (void)app;

    while (offset < end)
    {
        const gchar *p = text + offset;
        const gchar *next = g_utf8_next_char(p);
        gsize len = (gsize)(next - p);
        AiStyleTag tag = ai_rendered_text_get_tag_at(row->rendered, offset);
        attr_t attr = attr_for_tag(tag);
        gchar buf[8];

        if (selected)
        {
            attr |= A_REVERSE;
        }

        if (len >= sizeof buf)
        {
            break;
        }

        memcpy(buf, p, len);
        buf[len] = '\0';

        wattrset(win, attr);
        mvwaddstr(win, y, x, buf);

        x += g_unichar_iswide(g_utf8_get_char(p)) ? 2 : 1;
        offset += (guint)len;
    }

    wattrset(win, A_NORMAL);
}

/*
 * Advance one frame and redraw.
 *
 * Only the status bar changes, but a redraw is already coalesced onto an
 * idle and costs a hundred lines of text; a partial-update path here
 * would be a second way to draw the screen, for no gain anybody can see.
 */
static gboolean
on_spinner_tick(gpointer user_data)
{
    App *app = user_data;

    app->spinner_frame =
        (app->spinner_frame + 1) % G_N_ELEMENTS(SPINNER_FRAMES);

    app_schedule_redraw(app);

    return G_SOURCE_CONTINUE;
}

/*
 * Run the spinner exactly while a turn does.
 *
 * Driven from ::busy rather than started and stopped at each call site:
 * a turn can end through the callback, through cancellation, or through
 * an error, and a timer left running after one of those would be a
 * terminal animating forever with nothing behind it.
 */
static void
sync_spinner(App *app)
{
    gboolean busy = ai_conversation_get_busy(app->conversation);

    if (busy && app->spinner_id == 0)
    {
        app->spinner_frame = 0;
        app->spinner_id = g_timeout_add(SPINNER_INTERVAL_MS, on_spinner_tick,
                                        app);
    }
    else if (!busy && app->spinner_id != 0)
    {
        g_source_remove(app->spinner_id);
        app->spinner_id = 0;
    }
}

static void
on_busy_changed(
    GObject    *object,
    GParamSpec *pspec,
    gpointer    user_data
){
    App *app = user_data;

    (void)object;
    (void)pspec;

    sync_spinner(app);
    app_schedule_redraw(app);
}

/* "12s", "1m04s" --- the shape that stays the same width as it grows. */
static gchar *
format_elapsed(gint64 microseconds)
{
    gint64 seconds = microseconds / G_USEC_PER_SEC;

    if (seconds < 60)
    {
        return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);
    }

    return g_strdup_printf("%" G_GINT64_FORMAT "m%02" G_GINT64_FORMAT "s",
                           seconds / 60, seconds % 60);
}

static void
draw_status(App *app)
{
    GObject *provider = ai_conversation_get_provider(app->conversation);
    const gchar *model = NULL;
    gint width = getmaxx(app->status_win);
    g_autofree gchar *line = NULL;

    if (AI_IS_CLIENT(provider))
    {
        model = ai_client_get_model(AI_CLIENT(provider));
    }
    else if (AI_IS_CLI_CLIENT(provider))
    {
        model = ai_cli_client_get_model(AI_CLI_CLIENT(provider));
    }

    if (ai_conversation_get_busy(app->conversation))
    {
        const gchar      *activity =
            ai_conversation_get_activity(app->conversation);
        g_autofree gchar *elapsed = format_elapsed(
            ai_conversation_get_activity_elapsed(app->conversation));

        /*
         * The glyph animates, the words come from the conversation, and
         * the elapsed time is what tells a stalled turn from a slow one.
         * "^C to stop" is there because the moment somebody wants it is
         * the moment they are watching this line.
         */
        line = g_strdup_printf(" %s%s%s   %s %s… (%s · ^C to stop)%s",
                               ai_provider_get_name(AI_PROVIDER(provider)),
                               model != NULL ? " / " : "",
                               model != NULL ? model : "",
                               SPINNER_FRAMES[app->spinner_frame],
                               activity != NULL ? activity : "Working",
                               elapsed,
                               app->follow ? "" : "   [scrolled]");
    }
    else
    {
        /*
         * While a ^C is armed, say what a second one does.
         *
         * The window is a second and a half; a prompt that only appeared
         * in the transcript would arrive after it had closed. The status
         * line is already on screen and already being read.
         */
        line = g_strdup_printf(" %s%s%s   %s%s",
                               ai_provider_get_name(AI_PROVIDER(provider)),
                               model != NULL ? " / " : "",
                               model != NULL ? model : "",
                               app->interrupt_id != 0
                                   ? "^C again to quit"
                                   : "ready",
                               app->follow ? "" : "   [scrolled]");
    }

    werase(app->status_win);
    wattrset(app->status_win, A_REVERSE);
    mvwaddnstr(app->status_win, 0, 0, line, width);

    /* Pad to the full width so the reverse bar spans the terminal. */
    {
        gint used = (gint)ai_style_text_width(line);
        gint i;

        for (i = used; i < width; i++)
        {
            mvwaddch(app->status_win, 0, i, ' ');
        }
    }

    wattrset(app->status_win, A_NORMAL);
    wnoutrefresh(app->status_win);
}

/*
 * Laying the input line out, one character at a time.
 *
 * The input used to be one row, and drawing it was three calls to
 * mvwaddnstr. Since Enter inserts a newline it can be any number of
 * rows, and two questions now have to be answered about the same text:
 * how tall is it, and where does the cursor land. Answering them in two
 * places is how they come to disagree, so this walks the text once and
 * either draws or merely measures depending on whether it was given a
 * window.
 */
typedef struct
{
    WINDOW  *win;         /* NULL to measure without drawing */
    gint     width;       /* usable columns, after the gutter */
    gint     max_rows;    /* stop drawing past this; measuring is unbounded */
    gint     row;
    gint     col;

    gsize    consumed;    /* bytes emitted so far */
    gsize    cursor;      /* the byte offset we want a position for */
    gint     cursor_row;
    gint     cursor_col;
    gboolean cursor_set;
} InputPen;

/* Two columns for "> " on the first row, and the same indent on the rest
 * so a wrapped line stays under the one it continues. */
#define INPUT_GUTTER (2)

/* Rows the input may occupy before it starts scrolling instead of
 * growing. A prompt long enough to fill the screen has stopped being a
 * prompt and become a document --- which is what ^G is for. */
#define INPUT_MAX_ROWS (10)

static void
pen_note_cursor(InputPen *pen)
{
    if (pen->cursor_set || pen->consumed != pen->cursor)
    {
        return;
    }

    pen->cursor_row = pen->row;
    pen->cursor_col = pen->col;
    pen->cursor_set = TRUE;
}

static void
pen_emit(
    InputPen    *pen,
    const gchar *text,
    gsize        nbytes,
    attr_t       attr
){
    const gchar *p = text;
    const gchar *end = text + nbytes;

    while (p < end)
    {
        const gchar *next = g_utf8_next_char(p);
        gunichar     c = g_utf8_get_char(p);
        gint         w;

        if (next > end)
        {
            break;
        }

        pen_note_cursor(pen);

        if (c == '\n')
        {
            pen->row++;
            pen->col = 0;
            pen->consumed += (gsize)(next - p);
            p = next;
            continue;
        }

        /* Terminal columns, not characters --- the same rule the view
         * layer wraps by. */
        w = g_unichar_iswide(c) ? 2 : 1;

        if (pen->col + w > pen->width && pen->width > 0)
        {
            pen->row++;
            pen->col = 0;
        }

        if (pen->win != NULL && pen->row < pen->max_rows)
        {
            wattrset(pen->win, attr);
            mvwaddnstr(pen->win, pen->row, pen->col + INPUT_GUTTER, p,
                       (gint)(next - p));
        }

        pen->col += w;
        pen->consumed += (gsize)(next - p);
        p = next;
    }

    pen_note_cursor(pen);
}

/*
 * Walk the whole input, styling it as the pipeline sees it.
 *
 * The highlighting is the same scan the pipeline runs, so a mention that
 * will not resolve still lights up: it is a candidate, and the user can
 * see it was noticed.
 */
static void
input_walk(App *app, InputPen *pen)
{
    GList       *mentions = ai_mention_scan(app->input->str);
    GList       *iter;
    const gchar *text = app->input->str;
    gsize        offset = 0;
    gboolean     is_command = ai_command_set_is_command_line(app->input->str);

    if (is_command)
    {
        /* The command name, up to the first space or newline. */
        gsize name_len = strcspn(text, " \n");

        pen_emit(pen, text, name_len, attr_for_tag(AI_STYLE_COMMAND));
        offset = name_len;
    }

    for (iter = mentions; iter != NULL; iter = iter->next)
    {
        const AiMention *mention = iter->data;

        if (mention->start < offset)
        {
            continue;   /* inside the command name already drawn */
        }

        pen_emit(pen, text + offset, mention->start - offset, A_NORMAL);
        pen_emit(pen, text + mention->start, mention->len,
                 attr_for_tag(AI_STYLE_MENTION));

        offset = mention->start + mention->len;
    }

    g_list_free_full(mentions, (GDestroyNotify)ai_mention_free);

    pen_emit(pen, text + offset, strlen(text) - offset, A_NORMAL);
}

/* How many rows the input needs at WIDTH. */
static gint
input_rows_for(App *app, gint width)
{
    InputPen pen = { 0 };

    pen.width = MAX(1, width - INPUT_GUTTER);
    pen.max_rows = G_MAXINT;
    pen.cursor = app->cursor;

    input_walk(app, &pen);

    return CLAMP(pen.row + 1, 1, INPUT_MAX_ROWS);
}

static void
draw_input(App *app)
{
    gint     width = getmaxx(app->input_win);
    gint     rows = getmaxy(app->input_win);
    InputPen pen = { 0 };
    gint     i;

    werase(app->input_win);

    wattrset(app->input_win, A_BOLD);
    mvwaddstr(app->input_win, 0, 0, "> ");
    wattrset(app->input_win, A_NORMAL);

    pen.win = app->input_win;
    pen.width = MAX(1, width - INPUT_GUTTER);
    pen.max_rows = rows;
    pen.cursor = app->cursor;

    input_walk(app, &pen);

    /*
     * A continuation marker on every row after the first, so a wrapped
     * prompt reads as one thing rather than as several. Drawn after the
     * text because the gutter is outside the text's columns.
     */
    wattrset(app->input_win, attr_for_tag(AI_STYLE_DIM));

    for (i = 1; i < rows; i++)
    {
        mvwaddstr(app->input_win, i, 0, "\342\224\202 ");   /* │ */
    }

    wattrset(app->input_win, A_NORMAL);

    /*
     * The cursor may sit past the last row when the prompt has outgrown
     * INPUT_MAX_ROWS. Pinning it to the last visible row keeps it on
     * screen; the text above simply scrolls out of view, which is the
     * point at which ^G is the better tool anyway.
     */
    wmove(app->input_win,
          CLAMP(pen.cursor_row, 0, rows - 1),
          MIN(pen.cursor_col + INPUT_GUTTER, width - 1));
    wnoutrefresh(app->input_win);
}

/*
 * Give the input the rows it needs and let the transcript have the rest.
 *
 * Called before every draw rather than only on resize, because the input
 * changes height as it is typed into.
 */
static void
app_layout(App *app)
{
    gint height;
    gint width;
    gint rows;
    gint was;

    getmaxyx(stdscr, height, width);

    rows = input_rows_for(app, width);
    rows = CLAMP(rows, 1, MAX(1, height - 2));
    was = getmaxy(app->input_win);

    /* Anchored at the top, so shrinking it is always valid --- and
     * shrinking is what frees the rows the others are about to take. */
    wresize(app->transcript_win, MAX(1, height - 1 - rows), width);

    /*
     * A window may not extend past the bottom of the screen, not even
     * for the instant between two calls. So growing means moving up
     * before resizing, and shrinking means resizing before moving down.
     */
    if (rows >= was)
    {
        mvwin(app->input_win, MAX(0, height - rows), 0);
        wresize(app->input_win, rows, width);
    }
    else
    {
        wresize(app->input_win, rows, width);
        mvwin(app->input_win, MAX(0, height - rows), 0);
    }

    wresize(app->status_win, 1, width);
    mvwin(app->status_win, MAX(0, height - 1 - rows), 0);
}

static void
app_redraw(App *app)
{
    g_autoptr(GPtrArray) rows = NULL;
    gint height;
    gint width;
    gint first;
    gint i;

    /* The input decides how much room is left, so its height is settled
     * before anything is measured against the transcript window. */
    app_layout(app);

    height = getmaxy(app->transcript_win);
    width = getmaxx(app->transcript_win);

    if (width <= 0 || height <= 0)
    {
        return;
    }

    rows = build_rows(app, width);

    if (app->follow)
    {
        app->scroll = MAX(0, (gint)rows->len - height);
    }

    app->scroll = CLAMP(app->scroll, 0, MAX(0, (gint)rows->len - 1));
    first = app->scroll;

    werase(app->transcript_win);

    for (i = 0; i < height; i++)
    {
        gint index = first + i;
        Row *row;

        if (index >= (gint)rows->len)
        {
            break;
        }

        row = g_ptr_array_index(rows, index);
        draw_row(app, app->transcript_win, i, row,
                 app->selected >= 0 && (gint)row->block_index == app->selected);
    }

    wnoutrefresh(app->transcript_win);
    draw_completion(app);
    draw_status(app);
    draw_input(app);
    doupdate();
}

static gboolean
on_redraw_idle(gpointer user_data)
{
    App *app = user_data;

    app->redraw_id = 0;
    app_redraw(app);

    return G_SOURCE_REMOVE;
}

/*
 * Coalesce redraws onto an idle.
 *
 * A streamed reply produces a ::block-changed per token. Redrawing on each
 * one would spend the whole turn in refresh; one redraw per main-loop
 * iteration is indistinguishable to a reader and costs nothing.
 */
static void
app_schedule_redraw(App *app)
{
    if (app->redraw_id != 0 || !app->running)
    {
        return;
    }

    app->redraw_id = g_idle_add(on_redraw_idle, app);
}

/* ================================================================
 * Input
 * ================================================================ */

static void
app_send(App *app);

static void
input_insert(App *app, const gchar *text)
{
    g_string_insert(app->input, (gssize)app->cursor, text);
    app->cursor += (guint)strlen(text);
}

static void
input_backspace(App *app)
{
    const gchar *start;
    const gchar *previous;

    if (app->cursor == 0)
    {
        return;
    }

    start = app->input->str;
    previous = g_utf8_prev_char(start + app->cursor);

    g_string_erase(app->input, (gssize)(previous - start),
                   (gssize)(start + app->cursor - previous));
    app->cursor = (guint)(previous - start);
}

/* Move the cursor a whole character, never into the middle of one. */
static void
input_move(App *app, gint direction)
{
    const gchar *start = app->input->str;

    if (direction < 0 && app->cursor > 0)
    {
        app->cursor = (guint)(g_utf8_prev_char(start + app->cursor) - start);
    }
    else if (direction > 0 && app->cursor < app->input->len)
    {
        app->cursor = (guint)(g_utf8_next_char(start + app->cursor) - start);
    }
}

static void
input_recall(App *app, gint direction)
{
    if (app->history->len == 0)
    {
        return;
    }

    if (app->history_pos < 0)
    {
        app->history_pos = direction < 0 ? (gint)app->history->len - 1 : -1;
    }
    else
    {
        app->history_pos += direction < 0 ? -1 : 1;
    }

    if (app->history_pos < 0)
    {
        app->history_pos = 0;
    }

    if (app->history_pos >= (gint)app->history->len)
    {
        app->history_pos = -1;
        g_string_truncate(app->input, 0);
        app->cursor = 0;
        return;
    }

    g_string_assign(app->input,
                    g_ptr_array_index(app->history, app->history_pos));
    app->cursor = (guint)app->input->len;
}

/* Move the selection to the next tool block, wrapping at the end. */
static void
select_next_tool_block(App *app)
{
    AiTranscript *transcript = ai_conversation_get_transcript(app->conversation);
    guint n = ai_transcript_get_n_blocks(transcript);
    guint start;
    guint i;

    if (n == 0)
    {
        return;
    }

    start = app->selected < 0 ? 0 : (guint)app->selected + 1;

    for (i = 0; i < n; i++)
    {
        guint index = (start + i) % n;
        AiViewBlock *block = ai_transcript_get_block(transcript, index);
        AiViewBlockKind kind = ai_view_block_get_kind(block);

        if (kind == AI_VIEW_BLOCK_TOOL || kind == AI_VIEW_BLOCK_THINKING)
        {
            app->selected = (gint)index;
            return;
        }
    }

    app->selected = -1;
}

static void
toggle_selected(App *app)
{
    AiTranscript *transcript = ai_conversation_get_transcript(app->conversation);
    AiViewBlock *block;

    if (app->selected < 0)
    {
        return;
    }

    block = ai_transcript_get_block(transcript, (guint)app->selected);

    if (block != NULL)
    {
        ai_view_block_set_expanded(block, !ai_view_block_get_expanded(block));
    }
}

/*
 * One line of description, short enough for a listing.
 *
 * The descriptions in these files run to paragraphs --- one agent on this
 * machine has a four-hundred-word one with worked examples in it. A
 * listing wants the first clause.
 */
#define SUMMARY_MAX (72)

static gchar *
summarise(const gchar *description)
{
    if (description == NULL)
    {
        return g_strdup("");
    }

    if (g_utf8_strlen(description, -1) <= SUMMARY_MAX)
    {
        return g_strdup(description);
    }

    {
        const gchar      *cut = g_utf8_offset_to_pointer(description,
                                                         SUMMARY_MAX - 1);
        g_autofree gchar *head =
            g_strndup(description, (gsize)(cut - description));

        return g_strdup_printf("%s…", head);
    }
}

/* Append a line to the transcript as a local note. */
static void
say(App *app, const gchar *format, ...)
{
    g_autofree gchar *text = NULL;
    va_list           args;

    va_start(args, format);
    text = g_strdup_vprintf(format, args);
    va_end(args);

    {
        g_autoptr(AiViewBlock) block =
            ai_view_status_block_new(AI_VIEW_STATUS_INFO, text);

        ai_transcript_append(ai_conversation_get_transcript(app->conversation),
                             block);
    }
}

/* /help, /commands, /skills, /agents --- one listing, filtered. */
static void
list_resources(App *app, AiResourceKind kind, const gchar *heading)
{
    g_autoptr(GString) out = g_string_new(heading);
    GList             *items;
    GList             *iter;

    g_string_append_c(out, '\n');

    if (app->registry == NULL)
    {
        g_string_append(out, "  (no resource registry)");
        say(app, "%s", out->str);
        return;
    }

    items = ai_resource_registry_list(app->registry, kind);

    if (items == NULL)
    {
        g_auto(GStrv) paths =
            ai_resource_registry_get_search_paths(app->registry, kind);
        guint         i;

        /*
         * An empty listing is the moment somebody asks "why isn't my file
         * showing up", so answer it here rather than leaving them to
         * guess which of a dozen directories was meant.
         */
        g_string_append(out, "  none found. Searched:\n");

        for (i = 0; paths != NULL && paths[i] != NULL; i++)
        {
            g_string_append_printf(out, "    %s\n", paths[i]);
        }
    }

    for (iter = items; iter != NULL; iter = iter->next)
    {
        AiResource       *resource = iter->data;
        g_autofree gchar *summary =
            summarise(ai_resource_get_description(resource));

        g_string_append_printf(out, "  /%-28s %s  [%s]\n",
                               ai_resource_get_name(resource), summary,
                               ai_resource_get_origin(resource));
    }

    g_list_free(items);

    /* And what lost a name collision, with the path, so the answer to
     * "why is the wrong one running" is on screen. */
    {
        GList *shadowed = ai_resource_registry_list_shadowed(app->registry);

        for (iter = shadowed; iter != NULL; iter = iter->next)
        {
            if (ai_resource_get_kind(iter->data) != kind)
            {
                continue;
            }

            g_string_append_printf(out, "  (shadowed) %s -> %s\n",
                                   ai_resource_get_name(iter->data),
                                   ai_resource_get_path(iter->data));
        }

        g_list_free(shadowed);
    }

    say(app, "%s", out->str);
}

static void
show_help(App *app)
{
    g_autoptr(GString) out = g_string_new("Commands\n");
    GList             *commands;
    GList             *iter;

    if (app->commands == NULL)
    {
        say(app, "No command set is configured.");
        return;
    }

    commands = ai_command_set_list(app->commands);

    for (iter = commands; iter != NULL; iter = iter->next)
    {
        AiCommand   *command = iter->data;
        const gchar *hint = ai_command_get_argument_hint(command);
        const gchar *description = ai_command_get_description(command);
        g_autofree gchar *name =
            g_strdup_printf("/%s%s%s", ai_command_get_name(command),
                            hint != NULL ? " " : "",
                            hint != NULL ? hint : "");

        {
            g_autofree gchar *summary = summarise(description);

            g_string_append_printf(out, "  %-30s %s  [%s]\n", name, summary,
                                   ai_command_get_origin(command));
        }
    }

    g_list_free_full(commands, g_object_unref);

    g_string_append(out,
                    "\nKeys\n"
                    "  Enter        send\n"
                    "  Alt-Enter    a new line (Shift-Enter too, where the\n"
                    "               terminal encodes it distinctly)\n"
                    "  ^G           edit the prompt in $EDITOR\n"
                    "  ^C           stop the turn, then clear the line,\n"
                    "               then quit on a second press\n"
                    "  ^D           quit, on an empty line\n"
                    "  Tab          complete /command or @path\n"
                    "  ^N           cycle tool and thinking blocks\n"
                    "  ^B           expand or collapse the selected block\n"
                    "  ^U           clear the line\n");

    say(app, "%s", out->str);

    say(app, "%s", out->str);
}

static void
show_tools(App *app)
{
    g_autoptr(GString) out = g_string_new("Tools\n");
    AiToolExecutor    *executor =
        ai_conversation_get_executor(app->conversation);
    GList             *iter;

    if (!ai_conversation_get_local_tools(app->conversation))
    {
        g_string_append(out,
                        "  (local tools are off; the provider runs its own)\n");
    }

    for (iter = ai_tool_executor_get_tools(executor); iter != NULL;
         iter = iter->next)
    {
        g_string_append_printf(out, "  %-14s %s\n",
                               ai_tool_get_name(iter->data),
                               ai_tool_get_description(iter->data));
    }

    say(app, "%s", out->str);
}

static void
show_todos(App *app)
{
    AiToolExecutor    *executor =
        ai_conversation_get_executor(app->conversation);
    g_autoptr(GString) out = g_string_new("Todos\n");
    guint              n = ai_tool_executor_get_n_todos(executor);
    guint              i;

    if (n == 0)
    {
        say(app, "No todos.");
        return;
    }

    for (i = 0; i < n; i++)
    {
        const gchar *label = NULL;
        AiTodoState  state = AI_TODO_PENDING;

        ai_tool_executor_get_todo_fields(executor, i, &label, &state);
        g_string_append_printf(out, "  [%s] %s\n",
                               ai_todo_state_to_string(state), label);
    }

    say(app, "%s", out->str);
}

/*
 * What the background agents are doing.
 *
 * Distinct from /agents, which lists the agent *definitions* found on
 * disk. This is the running ones -- the answer to "is that review
 * finished yet?".
 */
static void
show_running(App *app)
{
    AiBrigade         *brigade =
        ai_conversation_get_brigade(app->conversation);
    g_autoptr(GString) out = g_string_new("Background agents\n");
    g_autoptr(GList)   agents = NULL;
    GList             *iter;

    if (brigade == NULL)
    {
        say(app, "Background agents are not enabled.");
        return;
    }

    agents = ai_brigade_list(brigade);

    if (agents == NULL)
    {
        say(app, "No background agents.");
        return;
    }

    for (iter = agents; iter != NULL; iter = iter->next)
    {
        AiAgent     *agent = iter->data;
        const gchar *what  = ai_agent_get_description(agent);

        g_string_append_printf(out, "  %-16s %-10s %3" G_GINT64_FORMAT "s  %s\n",
                               ai_agent_get_id(agent),
                               ai_agent_state_to_string(
                                   ai_agent_get_state(agent)),
                               ai_agent_get_elapsed_ms(agent) / 1000,
                               what != NULL ? what : "");
    }

    say(app, "%s", out->str);
}

static void
kill_agent(App *app, const gchar *arguments)
{
    AiBrigade *brigade = ai_conversation_get_brigade(app->conversation);
    AiAgent   *agent;

    if (brigade == NULL)
    {
        say(app, "Background agents are not enabled.");
        return;
    }

    if (arguments == NULL || arguments[0] == '\0')
    {
        say(app, "/kill needs an agent id, or \"all\".");
        return;
    }

    if (g_strcmp0(arguments, "all") == 0)
    {
        guint n = ai_brigade_cancel_all(brigade);

        say(app, "Stopped %u agent%s.", n, n == 1 ? "" : "s");
        return;
    }

    agent = ai_brigade_get(brigade, arguments);

    if (agent == NULL)
    {
        say(app, "No agent '%s'. /running lists them.", arguments);
        return;
    }

    ai_agent_cancel(agent);
    say(app, "Stopped '%s'.", arguments);
}

/*
 * A background agent stopped.
 *
 * Says so on screen straight away. The model is told separately, by the
 * executor, at the next turn boundary -- but the person watching should
 * not have to send a message to discover that the thing they started ten
 * minutes ago has finished.
 */
static void
on_agent_finished(
    AiConversation *conversation,
    const gchar    *agent_id,
    gint            state,
    gpointer        user_data
){
    App         *app = user_data;
    AiAgent     *agent;
    const gchar *what = NULL;

    (void)conversation;

    agent = ai_brigade_get(ai_conversation_get_brigade(app->conversation),
                           agent_id);

    /* The agent may already have been reaped by the model's agent_result
     * between the brigade emitting and this running, in which case its
     * description is gone and the id is all there is to say. */
    if (agent != NULL)
    {
        what = ai_agent_get_description(agent);
    }

    if (what != NULL && what[0] != '\0')
    {
        say(app, "Agent '%s' %s: %s", agent_id,
            ai_agent_state_to_string((AiAgentState)state), what);
    }
    else
    {
        say(app, "Agent '%s' %s.", agent_id,
            ai_agent_state_to_string((AiAgentState)state));
    }

    app_schedule_redraw(app);
}

static void
save_transcript(App *app, const gchar *path)
{
    g_autofree gchar *text = NULL;
    g_autoptr(GError) error = NULL;

    if (path == NULL || path[0] == '\0')
    {
        say(app, "/save needs a path.");
        return;
    }

    text = ai_transcript_to_text(
        ai_conversation_get_transcript(app->conversation), 0);

    if (!g_file_set_contents(path, text, -1, &error))
    {
        say(app, "Could not write %s: %s", path, error->message);
        return;
    }

    say(app, "Wrote %s", path);
}

/*
 * /export <format> [path]
 *
 * The path is optional because the format already implies an extension,
 * and a session worth exporting is usually one you want to file rather
 * than name. Omitting it writes ai-session-<pid>.<ext> in the working
 * directory, which is somewhere the user can find it.
 */
static void
export_transcript(App *app, const gchar *arguments)
{
    g_auto(GStrv) parts = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *chosen = NULL;
    g_autoptr(GError) error = NULL;
    AiExportFormat format;
    const gchar *path;

    if (arguments == NULL || arguments[0] == '\0')
    {
        say(app, "/export needs a format: text, markdown or org.");
        return;
    }

    parts = g_strsplit(arguments, " ", 2);

    if (!ai_export_format_from_string(parts[0], &format))
    {
        say(app, "Unknown format '%s'. Use text, markdown or org.",
            parts[0]);
        return;
    }

    path = (parts[1] != NULL && parts[1][0] != '\0') ? parts[1] : NULL;

    if (path == NULL)
    {
        g_autofree gchar *name =
            g_strdup_printf("ai-session-%d.%s", (int) getpid(),
                            ai_export_format_extension(format));

        chosen = g_build_filename(
            ai_conversation_get_working_directory(app->conversation),
            name, NULL);
        path = chosen;
    }

    text = ai_transcript_export(
        ai_conversation_get_transcript(app->conversation), format);

    if (!g_file_set_contents(path, text, -1, &error))
    {
        say(app, "Could not write %s: %s", path, error->message);
        return;
    }

    say(app, "Wrote %s", path);
}

static void
change_directory(App *app, const gchar *path)
{
    if (path == NULL || path[0] == '\0')
    {
        say(app, "%s", ai_conversation_get_working_directory(app->conversation));
        return;
    }

    if (!g_file_test(path, G_FILE_TEST_IS_DIR))
    {
        say(app, "No such directory: %s", path);
        return;
    }

    ai_conversation_set_working_directory(app->conversation, path);

    if (app->completion != NULL)
    {
        ai_completion_context_set_working_directory(app->completion, path);
    }

    say(app, "Working directory: %s", path);
}

static void
show_expansion(App *app, const gchar *line)
{
    g_autoptr(AiCommandResult) resolved = NULL;
    g_autoptr(GError)          error = NULL;
    g_autofree gchar          *expanded = NULL;
    const gchar               *source = line;

    if (line == NULL || line[0] == '\0')
    {
        say(app, "/expand needs something to expand.");
        return;
    }

    resolved = ai_conversation_resolve_input(app->conversation, line, NULL,
                                             &error);

    if (error != NULL)
    {
        say(app, "%s", error->message);
        return;
    }

    if (resolved != NULL &&
        ai_command_result_get_outcome(resolved) != AI_COMMAND_OUTCOME_NOT_A_COMMAND)
    {
        if (ai_command_result_get_outcome(resolved) ==
            AI_COMMAND_OUTCOME_BUILTIN)
        {
            say(app, "/%s is a built-in; nothing would be sent.",
                ai_command_result_get_name(resolved));
            return;
        }

        source = ai_command_result_get_prompt(resolved);
    }

    expanded = ai_mention_expand(
        source != NULL ? source : "",
        ai_conversation_get_working_directory(app->conversation), 0, NULL);

    say(app, "Would send:\n%s", expanded);
}

/*
 * Act on a built-in.
 *
 * The dispatch is on the name because AiCommandSet decided what a name
 * means; this file only knows what to do about it. Growing the set is a
 * struct literal there plus a case here, and nothing in between.
 */
static void
handle_builtin(App *app, AiCommandResult *result)
{
    const gchar *name = ai_command_result_get_name(result);
    const gchar *arguments = ai_command_result_get_arguments(result);

    if (g_strcmp0(name, "quit") == 0)
    {
        app->running = FALSE;

        /* Under --dump the loop belongs to the caller below, which quits
         * it itself once the turn is accounted for. */
        if (app->dump_loop == NULL)
        {
            g_main_loop_quit(app->loop);
        }

        return;
    }

    if (g_strcmp0(name, "clear") == 0)
    {
        ai_conversation_clear(app->conversation);
        app->selected = -1;
        app->follow = TRUE;
    }
    else if (g_strcmp0(name, "help") == 0)
    {
        show_help(app);
    }
    else if (g_strcmp0(name, "commands") == 0)
    {
        list_resources(app, AI_RESOURCE_COMMAND, "Commands from disk");
    }
    else if (g_strcmp0(name, "skills") == 0)
    {
        list_resources(app, AI_RESOURCE_SKILL, "Skills");
    }
    else if (g_strcmp0(name, "agents") == 0)
    {
        list_resources(app, AI_RESOURCE_AGENT, "Agents");
    }
    else if (g_strcmp0(name, "reload") == 0)
    {
        if (app->registry != NULL)
        {
            ai_resource_registry_scan(app->registry);
        }

        say(app, "Rescanned.");
    }
    else if (g_strcmp0(name, "tools") == 0)
    {
        show_tools(app);
    }
    else if (g_strcmp0(name, "todos") == 0)
    {
        show_todos(app);
    }
    else if (g_strcmp0(name, "running") == 0)
    {
        show_running(app);
    }
    else if (g_strcmp0(name, "kill") == 0)
    {
        kill_agent(app, arguments);
    }
    else if (g_strcmp0(name, "cwd") == 0)
    {
        change_directory(app, arguments);
    }
    else if (g_strcmp0(name, "save") == 0)
    {
        save_transcript(app, arguments);
    }
    else if (g_strcmp0(name, "export") == 0)
    {
        export_transcript(app, arguments);
    }
    else if (g_strcmp0(name, "expand") == 0)
    {
        show_expansion(app, arguments);
    }
    else if (g_strcmp0(name, "model") == 0)
    {
        GObject *provider = ai_conversation_get_provider(app->conversation);

        if (arguments == NULL || arguments[0] == '\0')
        {
            say(app, "%s", AI_IS_CLIENT(provider)
                    ? ai_client_get_model(AI_CLIENT(provider))
                    : ai_cli_client_get_model(AI_CLI_CLIENT(provider)));
        }
        else if (AI_IS_CLIENT(provider))
        {
            ai_client_set_model(AI_CLIENT(provider), arguments);
        }
        else if (AI_IS_CLI_CLIENT(provider))
        {
            ai_cli_client_set_model(AI_CLI_CLIENT(provider), arguments);
        }
    }
    else if (g_strcmp0(name, "provider") == 0)
    {
        GObject *provider = ai_conversation_get_provider(app->conversation);

        if (arguments == NULL || arguments[0] == '\0')
        {
            say(app, "Provider: %s",
                ai_provider_get_name(AI_PROVIDER(provider)));
        }
        else
        {
            g_autofree gchar *requested = g_strdup(arguments);
            g_autoptr(GObject) replacement = NULL;
            g_autoptr(GError) error = NULL;
            gboolean had_local_tools =
                ai_conversation_get_local_tools(app->conversation);

            g_strstrip(requested);
            replacement = build_provider_named(requested, FALSE, &error);

            if (replacement == NULL
                || !ai_conversation_set_provider(app->conversation,
                                                 replacement, &error))
            {
                say(app, "Provider unchanged: %s",
                    error != NULL ? error->message : "switch failed");
            }
            else
            {
                say(app, "Provider switched to %s. Context preserved.",
                    ai_provider_get_name(AI_PROVIDER(replacement)));

                if (had_local_tools
                    && !ai_conversation_get_local_tools(app->conversation))
                {
                    say(app, "Local tools disabled: CLI providers run "
                        "their own tools.");
                }
            }
        }
    }
    else
    {
        say(app, "/%s is not implemented here.", name);
    }

    app_schedule_redraw(app);
}

/*
 * Which editor, and how to run it.
 *
 * $VISUAL before $EDITOR because that is what the two mean: VISUAL is the
 * full-screen one, EDITOR the line editor of last resort, and this needs
 * a screen. Parsed as a command line rather than a bare path, since both
 * are routinely set to something with arguments --- `emacs -nw`, `code
 * -w`, `emacsclient -t`.
 */
static gchar **
resolve_editor(GError **error)
{
    const gchar *spec;
    gchar      **argv = NULL;

    spec = g_getenv("VISUAL");

    if (spec == NULL || spec[0] == '\0')
    {
        spec = g_getenv("EDITOR");
    }

    if (spec == NULL || spec[0] == '\0')
    {
        spec = "vi";
    }

    if (!g_shell_parse_argv(spec, NULL, &argv, error))
    {
        return NULL;
    }

    return argv;
}

/*
 * Write the prompt to a file, hand the terminal to $EDITOR, take it back.
 *
 * A prompt worth more than a line or two wants a real editor: paragraphs,
 * a paste that keeps its shape, the keybindings already in somebody's
 * fingers. Rather than grow a text editor inside this one, the file goes
 * out and comes back.
 *
 * The turn keeps running while the editor is open --- the spawn blocks
 * this thread, so nothing repaints, but callbacks queue up and are
 * dispatched the moment the main loop turns again.
 */
static void
edit_in_editor(App *app)
{
    g_autoptr(GError)  error = NULL;
    g_auto(GStrv)      editor = NULL;
    g_autoptr(GPtrArray) argv = NULL;
    g_autofree gchar  *path = NULL;
    g_autofree gchar  *edited = NULL;
    gint               fd;
    gint               status = 0;
    guint              i;

    editor = resolve_editor(&error);

    if (editor == NULL)
    {
        say(app, "Cannot read $VISUAL/$EDITOR: %s", error->message);
        return;
    }

    /* .md so an editor picks a prose mode: soft wrap and a spell checker
     * beat C indentation for something that is going to a model. */
    fd = g_file_open_tmp("ai-tui-prompt-XXXXXX.md", &path, &error);

    if (fd < 0)
    {
        say(app, "Cannot make a temporary file: %s", error->message);
        return;
    }

    close(fd);

    if (!g_file_set_contents(path, app->input->str, (gssize)app->input->len,
                             &error))
    {
        say(app, "Cannot write %s: %s", path, error->message);
        g_unlink(path);
        return;
    }

    argv = g_ptr_array_new();

    for (i = 0; editor[i] != NULL; i++)
    {
        g_ptr_array_add(argv, editor[i]);
    }

    g_ptr_array_add(argv, path);
    g_ptr_array_add(argv, NULL);

    /*
     * Hand the terminal over. def_prog_mode() remembers this program's
     * settings so reset_prog_mode() can put them back --- an editor
     * leaves the terminal however it likes, and without this ai-tui
     * comes back to a screen it no longer controls.
     */
    def_prog_mode();
    endwin();

    if (!g_spawn_sync(NULL, (gchar **)argv->pdata, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                      NULL, NULL, NULL, NULL, &status, &error))
    {
        reset_prog_mode();
        clearok(curscr, TRUE);
        app_schedule_redraw(app);

        say(app, "Cannot run %s: %s", editor[0], error->message);
        g_unlink(path);
        return;
    }

    reset_prog_mode();

    /* The editor painted over everything; nothing ncurses believes about
     * the screen is true any more. */
    clearok(curscr, TRUE);

    /*
     * A non-zero exit is taken as "leave it alone".
     *
     * Quitting an editor without saving is how somebody says they
     * changed their mind, and the file on disk may be a half-finished
     * draft rather than a prompt. Reading it back anyway would make the
     * cancel do the opposite of cancelling.
     */
    if (!g_spawn_check_wait_status(status, NULL))
    {
        say(app, "%s exited without saving; the prompt is unchanged.",
            editor[0]);
        g_unlink(path);
        app_schedule_redraw(app);
        return;
    }

    if (!g_file_get_contents(path, &edited, NULL, &error))
    {
        say(app, "Cannot read %s back: %s", path, error->message);
        g_unlink(path);
        app_schedule_redraw(app);
        return;
    }

    g_unlink(path);

    /*
     * One trailing newline is an artefact of every editor that ends a
     * file properly, not something the user typed. Interior blank lines
     * are left exactly as written.
     */
    {
        gsize len = strlen(edited);

        while (len > 0 && edited[len - 1] == '\n')
        {
            edited[--len] = '\0';
        }

        g_string_assign(app->input, edited);
        app->cursor = (guint)app->input->len;
    }

    app->completion_dismissed = FALSE;
    app_schedule_redraw(app);
}

/* The window has passed; a further ^C starts again rather than quits. */
static gboolean
on_interrupt_expired(gpointer user_data)
{
    App *app = user_data;

    app->interrupt_id = 0;
    app_schedule_redraw(app);

    return G_SOURCE_REMOVE;
}

static void
interrupt_disarm(App *app)
{
    if (app->interrupt_id != 0)
    {
        g_source_remove(app->interrupt_id);
        app->interrupt_id = 0;
    }
}

static void
interrupt_arm(App *app)
{
    interrupt_disarm(app);
    app->interrupt_id = g_timeout_add(INTERRUPT_WINDOW_MS,
                                      on_interrupt_expired, app);
}

/*
 * ^C, which does whatever there is to interrupt.
 *
 * In order: stop a turn, throw away a half-written prompt, and only then
 * --- pressed twice, close together, with nothing left to interrupt ---
 * leave. Every step short of the last is recoverable, which is the point:
 * the key somebody reaches for to stop a runaway answer should not also
 * be the key that ends the session and loses the conversation with it.
 *
 * Returns %TRUE when the program should stop.
 */
static gboolean
handle_interrupt(App *app)
{
    if (ai_conversation_get_busy(app->conversation))
    {
        ai_conversation_cancel(app->conversation);
        interrupt_disarm(app);
        return FALSE;
    }

    if (app->candidates != NULL)
    {
        completion_close(app);
        interrupt_disarm(app);
        return FALSE;
    }

    if (app->input->len > 0)
    {
        g_string_truncate(app->input, 0);
        app->cursor = 0;
        app->history_pos = -1;

        /* Armed, so a second press leaves --- which is what "^C ^C to
         * quit" means to anybody who has used a shell. */
        interrupt_arm(app);
        return FALSE;
    }

    if (app->interrupt_id != 0)
    {
        interrupt_disarm(app);
        return TRUE;
    }

    interrupt_arm(app);
    return FALSE;
}

static gboolean
on_sigint(gpointer user_data)
{
    App *app = user_data;

    if (handle_interrupt(app))
    {
        app->running = FALSE;
        g_main_loop_quit(app->loop);
        return G_SOURCE_REMOVE;
    }

    app_schedule_redraw(app);

    return G_SOURCE_CONTINUE;
}

static gboolean
drain_keys(App *app)
{
    gint ch;

    while ((ch = wgetch(app->input_win)) != ERR)
    {
        switch (ch)
        {
            case '\n':
            case '\r':
            case KEY_ENTER:
                /* A visible menu means Enter is a choice, not an edit. */
                if (app->candidates != NULL)
                {
                    completion_accept(app);
                    break;
                }

                app_send(app);
                break;

            case KEY_SHIFT_ENTER:
                /*
                 * A newline, for a prompt worth more than one line.
                 *
                 * Only reachable where the terminal has been configured
                 * to encode Shift+Enter distinctly --- a plain one sends
                 * the same carriage return as Enter and lands in the
                 * case above. Alt+Enter is the binding that always
                 * works, and does the same thing.
                 */
                input_insert(app, "\n");
                app->completion_dismissed = FALSE;
                completion_refresh(app);
                break;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                input_backspace(app);
                app->completion_dismissed = FALSE;
                completion_refresh(app);
                break;

            case KEY_LEFT:
                input_move(app, -1);
                completion_refresh(app);
                break;

            case KEY_RIGHT:
                input_move(app, 1);
                completion_refresh(app);
                break;

            case KEY_HOME:
            case 1:   /* ^A */
                app->cursor = 0;
                break;

            case KEY_END:
            case 5:   /* ^E */
                app->cursor = (guint)app->input->len;
                break;

            case 21:  /* ^U */
                g_string_truncate(app->input, 0);
                app->cursor = 0;
                completion_close(app);
                break;

            case KEY_UP:
                /* The menu owns the arrows while it is showing; the
                 * history gets them back the moment it is not. */
                if (!completion_select(app, -1))
                {
                    input_recall(app, -1);
                }
                break;

            case KEY_DOWN:
                if (!completion_select(app, 1))
                {
                    input_recall(app, 1);
                }
                break;

            case KEY_PPAGE:
                if (completion_select(app, -MENU_MAX_ROWS))
                {
                    break;
                }

                app->follow = FALSE;
                app->scroll -= getmaxy(app->transcript_win) / 2;
                break;

            case KEY_NPAGE:
                if (completion_select(app, MENU_MAX_ROWS))
                {
                    break;
                }

                app->scroll += getmaxy(app->transcript_win) / 2;
                app->follow = TRUE;   /* re-clamped on redraw */
                break;

            case '\t':
                completion_advance(app);
                break;

            case KEY_BTAB:  /* Shift-Tab */
                completion_select(app, -1);
                break;

            case 14:  /* ^N: cycle tool and thinking blocks */
                completion_close(app);
                select_next_tool_block(app);
                break;

            case 27:
            {
                /*
                 * Escape, or the lead byte of Alt+<key>.
                 *
                 * ncurses assembles the sequences it knows into KEY_*
                 * codes, but Alt+Enter is not one of them --- it arrives
                 * as ESC followed by a carriage return in the same read
                 * burst. One peek separates the two, and anything else
                 * goes back on the queue to be handled as itself.
                 */
                gint next = wgetch(app->input_win);

                if (next == '\r' || next == '\n' || next == KEY_ENTER)
                {
                    input_insert(app, "\n");
                    app->completion_dismissed = FALSE;
                    completion_refresh(app);
                    break;
                }

                if (next != ERR)
                {
                    ungetch(next);
                }

                /* A real Escape: dismiss the menu until the line changes. */
                app->completion_dismissed = TRUE;
                completion_close(app);
                break;
            }

            case 2:   /* ^B: toggle the selected block */
                completion_close(app);
                toggle_selected(app);
                break;

            case 3:   /* ^C: stop the turn, clear the line, then quit */
                if (handle_interrupt(app))
                {
                    app->running = FALSE;
                    g_main_loop_quit(app->loop);
                    return G_SOURCE_REMOVE;
                }
                break;

            case 4:   /* ^D: quit, on an empty line */
                if (app->input->len == 0)
                {
                    app->running = FALSE;
                    g_main_loop_quit(app->loop);
                    return G_SOURCE_REMOVE;
                }
                break;

            case 7:   /* ^G: hand the prompt to $EDITOR */
                completion_close(app);
                edit_in_editor(app);
                break;

            default:
                if (ch >= 32 && ch < 127)
                {
                    gchar text[2] = { (gchar)ch, '\0' };

                    input_insert(app, text);

                    /* The menu follows the line: typing "/" opens it and
                     * every character after narrows it. */
                    app->completion_dismissed = FALSE;
                    completion_refresh(app);
                }
                break;
        }

        app_schedule_redraw(app);
    }

    return G_SOURCE_CONTINUE;
}

/*
 * Read again, shortly.
 *
 * ncurses in nodelay mode cannot tell a lone Escape from the start of an
 * arrow key: it returns ERR and keeps the byte, waiting for a follow-up
 * that only another read will produce. Since keys reach us through the
 * main loop rather than a blocking read, nothing would ever come back for
 * it --- so pressing Escape did nothing at all until the *next*
 * keystroke, which then arrived behind it.
 *
 * One short timer after each burst flushes it. Imperceptible, and it is
 * the difference between Escape working and Escape being a mystery.
 */
static gboolean
on_key_settle(gpointer user_data)
{
    App *app = user_data;

    app->settle_id = 0;
    drain_keys(app);

    return G_SOURCE_REMOVE;
}

static gboolean
on_key(gint fd, GIOCondition condition, gpointer user_data)
{
    App *app = user_data;

    (void)fd;
    (void)condition;

    drain_keys(app);

    if (app->settle_id == 0)
    {
        app->settle_id = g_timeout_add(ESCAPE_SETTLE_MS, on_key_settle, app);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
on_resize(gpointer user_data)
{
    App *app = user_data;
    gint height;
    gint width;

    endwin();
    refresh();
    getmaxyx(stdscr, height, width);

    (void)height;
    (void)width;

    app_layout(app);

    /*
     * Every block re-wraps to the new width. The library caches per width,
     * so this is a re-render rather than a re-derivation.
     */
    app_schedule_redraw(app);

    return G_SOURCE_CONTINUE;
}

/* ================================================================
 * Sending
 * ================================================================ */

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(GError) error = NULL;

    ai_conversation_send_finish(AI_CONVERSATION(source), result, &error);

    /* Any failure is already a status block; nothing more to say here. */
    app->follow = TRUE;
    app_schedule_redraw(app);

    if (app->dump_loop != NULL)
    {
        g_main_loop_quit(app->dump_loop);
    }
}

/*
 * A line that went through the input pipeline.
 *
 * It either sent a turn or resolved to a built-in this program has to
 * act on --- the conversation cannot know what /clear means to a
 * terminal, so it hands the decision back.
 */
static void
on_input_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    App *app = user_data;
    g_autoptr(AiCommandResult) command = NULL;
    g_autoptr(GError)          error = NULL;

    ai_conversation_send_input_finish(AI_CONVERSATION(source), result,
                                      &command, &error);

    if (error != NULL)
    {
        say(app, "%s", error->message);
    }
    else if (command != NULL)
    {
        handle_builtin(app, command);
    }

    app->follow = TRUE;
    app_schedule_redraw(app);

    if (app->dump_loop != NULL)
    {
        g_main_loop_quit(app->dump_loop);
    }
}

/* ================================================================
 * Completion
 * ================================================================ */

static void
completion_close(App *app)
{
    g_clear_object(&app->candidates);
    app->candidate_index = 0;
    app->candidate_first = 0;
}

/*
 * Recompute the menu for whatever is under the cursor.
 *
 * Called after every input change, and purely passive --- it never
 * inserts anything. That split is what lets the menu appear the moment
 * you type "/" without the act of showing it also editing your line.
 */
static void
completion_refresh(App *app)
{
    if (app->completion == NULL || app->completion_dismissed)
    {
        return;
    }

    g_clear_object(&app->candidates);
    app->candidate_index = 0;
    app->candidate_first = 0;

    if (app->input->len == 0)
    {
        return;
    }

    app->candidates = ai_completion_context_query(app->completion,
                                                  app->input->str,
                                                  app->cursor);

    /* Nothing to offer is the same as no menu. */
    if (ai_completion_result_get_n_items(app->candidates) == 0)
    {
        g_clear_object(&app->candidates);
    }
}

/*
 * Move the highlight, wrapping at both ends.
 *
 * Returns %FALSE when there is no menu, which is how the arrow keys know
 * to fall through to the input history instead --- one key, two jobs,
 * decided by what is on screen.
 */
static gboolean
completion_select(App *app, gint delta)
{
    guint n;

    if (app->candidates == NULL)
    {
        return FALSE;
    }

    n = ai_completion_result_get_n_items(app->candidates);

    if (n == 0)
    {
        return FALSE;
    }

    app->candidate_index =
        (guint)(((gint)app->candidate_index + delta + (gint)n) % (gint)n);

    return TRUE;
}

/*
 * Tab: take what is on offer.
 *
 * One candidate is inserted outright. Several get their common prefix
 * inserted the first time, if that adds anything, and step the highlight
 * afterwards --- so the first Tab makes progress and the second is a
 * choice rather than a repetition.
 */
static void
completion_advance(App *app)
{
    guint n;

    if (app->completion == NULL)
    {
        return;
    }

    /* Escape then Tab means "actually, show me". */
    app->completion_dismissed = FALSE;

    if (app->candidates == NULL)
    {
        completion_refresh(app);
    }

    if (app->candidates == NULL)
    {
        return;
    }

    n = ai_completion_result_get_n_items(app->candidates);

    if (n == 1)
    {
        completion_accept(app);
        return;
    }

    {
        g_autofree gchar *prefix =
            ai_completion_result_get_common_prefix(app->candidates);
        guint             begin =
            ai_completion_result_get_start(app->candidates);
        guint             finish =
            ai_completion_result_get_end(app->candidates);

        if (prefix != NULL && strlen(prefix) > (gsize)(finish - begin))
        {
            g_string_erase(app->input, (gssize)begin,
                           (gssize)(finish - begin));
            g_string_insert(app->input, (gssize)begin, prefix);
            app->cursor = begin + (guint)strlen(prefix);

            completion_refresh(app);
            return;
        }
    }

    app->candidate_index = (app->candidate_index + 1) % n;
}

/* Replace the queried range with the highlighted candidate. */
static void
completion_accept(App *app)
{
    const gchar *text = NULL;
    guint        start;
    guint        end;

    if (app->candidates == NULL)
    {
        return;
    }

    if (!ai_completion_result_get_item_fields(app->candidates,
                                              app->candidate_index, &text,
                                              NULL, NULL, NULL, NULL))
    {
        completion_close(app);
        return;
    }

    start = ai_completion_result_get_start(app->candidates);
    end = ai_completion_result_get_end(app->candidates);

    /* The range came from the library; the frontend does not re-derive
     * where the token began. */
    g_string_erase(app->input, (gssize)start, (gssize)(end - start));
    g_string_insert(app->input, (gssize)start, text);
    app->cursor = start + (guint)strlen(text);

    completion_close(app);
}

/*
 * Truncate to @columns terminal columns, with an ellipsis if it did not
 * fit. Returns a new string.
 */
static gchar *
fit_to_width(const gchar *text, gint columns)
{
    const gchar *p;
    gint         used = 0;

    if (text == NULL)
    {
        return g_strdup("");
    }

    if (columns <= 1)
    {
        return g_strdup("");
    }

    if ((gint)ai_style_text_width(text) <= columns)
    {
        return g_strdup(text);
    }

    /* It does not fit, so one column goes to the ellipsis. Deciding that
     * up front is what stops a name that fits exactly from losing its
     * last character to a truncation that was not needed. */
    for (p = text; *p != '\0'; p = g_utf8_next_char(p))
    {
        gint w = g_unichar_iswide(g_utf8_get_char(p)) ? 2 : 1;

        if (used + w > columns - 1)
        {
            g_autofree gchar *head = g_strndup(text, (gsize)(p - text));

            return g_strdup_printf("%s…", head);
        }

        used += w;
    }

    return g_strdup(text);
}

/*
 * Draw the completion menu over the bottom of the transcript.
 *
 * Contrast is the whole job here. The first version drew both columns in
 * the same dim blue, which on a dark theme was legible only if you
 * already knew what it said. Now the name carries the weight, the
 * description is the one that recedes, and the row under the cursor is
 * reversed --- three levels, so the eye lands on the name first.
 */
static void
draw_completion(App *app)
{
    guint n;
    gint  height;
    gint  width;
    gint  rows;
    gint  first;
    gint  name_column;
    gint  origin_width;
    gint  i;

    if (app->candidates == NULL)
    {
        return;
    }

    n = ai_completion_result_get_n_items(app->candidates);

    if (n == 0)
    {
        return;
    }

    getmaxyx(app->transcript_win, height, width);

    /* One line goes to the rule, which is what separates the menu from
     * the conversation behind it. */
    rows = MIN((gint)n, MIN(MENU_MAX_ROWS, height - 1));

    if (rows < 1)
    {
        return;
    }

    /*
     * Scroll only as far as it takes to bring the selection back into
     * view. Recomputing the top from the selection instead would make
     * every downward step jump the whole menu, which is unreadable to
     * navigate by.
     */
    first = (gint)app->candidate_first;

    if ((gint)app->candidate_index < first)
    {
        first = (gint)app->candidate_index;
    }
    else if ((gint)app->candidate_index >= first + rows)
    {
        first = (gint)app->candidate_index - rows + 1;
    }

    first = CLAMP(first, 0, MAX(0, (gint)n - rows));
    app->candidate_first = (guint)first;

    /* Size the name column to what is actually showing, so short names do
     * not push the descriptions half a screen away. */
    name_column = 0;
    origin_width = 0;

    for (i = 0; i < rows; i++)
    {
        const gchar *display = NULL;
        const gchar *origin = NULL;

        if (ai_completion_result_get_item_fields(app->candidates,
                                                 (guint)(first + i), NULL,
                                                 &display, NULL, &origin,
                                                 NULL))
        {
            name_column = MAX(name_column, (gint)ai_style_text_width(display));

            if (origin != NULL)
            {
                origin_width = MAX(origin_width,
                                   (gint)ai_style_text_width(origin));
            }
        }
    }

    name_column = CLAMP(name_column + 3, 12, MAX(12, width / 2));

    /* The origin gets its own right-hand column rather than trailing the
     * description, because the description is what gets truncated --- and
     * the origin is the one piece that tells two same-named commands
     * apart. */
    origin_width = (origin_width > 0) ? origin_width + 3 : 0;

    /* The rule, with a count when there is more than fits. */
    {
        gint y = height - rows - 1;

        wattrset(app->transcript_win, A_DIM);
        mvwhline(app->transcript_win, y, 0, ACS_HLINE, width);

        if ((gint)n > rows)
        {
            g_autofree gchar *count =
                g_strdup_printf(" %u/%u ", app->candidate_index + 1, n);
            gint              at = MAX(0, width - (gint)strlen(count) - 2);

            mvwaddstr(app->transcript_win, y, at, count);
        }
    }

    for (i = 0; i < rows; i++)
    {
        const gchar *display = NULL;
        const gchar *description = NULL;
        const gchar *origin = NULL;
        guint        index = (guint)(first + i);
        gboolean     selected = (index == app->candidate_index);
        gint         y = height - rows + i;
        gint         text_room = width - name_column - origin_width - 2;
        g_autofree gchar *name = NULL;

        if (!ai_completion_result_get_item_fields(app->candidates, index,
                                                  NULL, &display,
                                                  &description, &origin,
                                                  NULL))
        {
            break;
        }

        /* Fill first, so the row is a band rather than text floating over
         * whatever the transcript had there. */
        wattrset(app->transcript_win, selected ? A_REVERSE : A_NORMAL);
        mvwhline(app->transcript_win, y, 0, ' ', width);

        name = fit_to_width(display, name_column - 3);
        wattrset(app->transcript_win, selected ? A_REVERSE : A_BOLD);
        mvwaddstr(app->transcript_win, y, 2, name);

        if (description != NULL && description[0] != '\0' && text_room > 8)
        {
            g_autofree gchar *summary =
                fit_to_width(description, text_room);

            /*
             * Plain A_DIM, not the DIM style tag: that one is blue, which
             * is what made the first version of this menu unreadable on a
             * dark theme. And a reversed row keeps one attribute
             * throughout --- A_DIM inside a highlight reads as a
             * rendering fault, not as a hierarchy.
             */
            wattrset(app->transcript_win, selected ? A_REVERSE : A_DIM);
            mvwaddstr(app->transcript_win, y, name_column, summary);
        }

        if (origin != NULL && origin[0] != '\0' && origin_width > 0 &&
            width > name_column + origin_width)
        {
            gint at = width - (gint)ai_style_text_width(origin) - 2;

            wattrset(app->transcript_win, selected ? A_REVERSE : A_DIM);
            mvwaddstr(app->transcript_win, y, at, origin);
        }
    }

    wattrset(app->transcript_win, A_NORMAL);
    wnoutrefresh(app->transcript_win);
}

static void
app_send(App *app)
{
    g_autofree gchar *line = NULL;

    if (app->input->len == 0)
    {
        return;
    }

    line = g_strdup(app->input->str);
    g_strstrip(line);

    g_string_truncate(app->input, 0);
    app->cursor = 0;
    app->history_pos = -1;

    if (line[0] == '\0')
    {
        return;
    }

    g_ptr_array_add(app->history, g_strdup(line));

    if (ai_conversation_get_busy(app->conversation))
    {
        return;
    }

    app->follow = TRUE;
    g_clear_object(&app->cancellable);
    app->cancellable = g_cancellable_new();

    /*
     * One call for the whole pipeline. This file no longer knows what a
     * slash means, what an @ means, or which of those a wrapped CLI wants
     * to handle itself --- and an Emacs frontend will call the same thing.
     */
    if (opt_no_expand)
    {
        ai_conversation_send_async(app->conversation, line, app->cancellable,
                                   on_sent, app);
        return;
    }

    ai_conversation_send_input_async(app->conversation, line,
                                     app->cancellable, on_input_sent, app);
}

/* ================================================================
 * Approval
 * ================================================================ */

/*
 * Ask the user, from inside the signal handler.
 *
 * A nested loop on the *thread-default* context, not the global default:
 * a caller driving this from a private context would never dispatch a
 * global-default source, and the prompt would hang. The turn genuinely
 * cannot proceed until the answer arrives, so blocking here is correct
 * rather than merely convenient.
 */
static gint
on_approval_requested(
    AiConversation *conversation,
    AiToolUse      *tool_use,
    gpointer        user_data
){
    App *app = user_data;
    g_autoptr(GMainLoop) loop = NULL;
    g_autofree gchar *prompt = NULL;
    const gchar *target;
    gint answer = AI_TOOL_APPROVAL_DEFAULT;
    gint width;

    (void)conversation;

    if (app->approve_all)
    {
        return AI_TOOL_APPROVAL_ALLOW;
    }

    {
        g_autoptr(AiToolCall) call = ai_tool_call_new(tool_use);

        target = ai_tool_call_get_target(call);
        prompt = g_strdup_printf(" run %s%s%s ?  [y]es  [n]o  [a]lways  [d]eny all ",
                                 ai_tool_use_get_name(tool_use),
                                 target != NULL ? ": " : "",
                                 target != NULL ? target : "");
    }

    width = getmaxx(app->input_win);

    werase(app->input_win);
    wattrset(app->input_win, A_REVERSE | A_BOLD);
    mvwaddnstr(app->input_win, 0, 0, prompt, width);
    wattrset(app->input_win, A_NORMAL);
    wrefresh(app->input_win);

    loop = g_main_loop_new(g_main_context_get_thread_default(), FALSE);

    while (answer == AI_TOOL_APPROVAL_DEFAULT)
    {
        gint ch = wgetch(app->input_win);

        switch (ch)
        {
            case 'y': case 'Y': answer = AI_TOOL_APPROVAL_ALLOW; break;
            case 'n': case 'N': answer = AI_TOOL_APPROVAL_DENY; break;
            case 'a': case 'A': answer = AI_TOOL_APPROVAL_ALLOW_ALWAYS; break;
            case 'd': case 'D': answer = AI_TOOL_APPROVAL_DENY_ALL; break;
            case 3:             answer = AI_TOOL_APPROVAL_DENY_ALL; break;
            case ERR:
                /*
                 * No key yet. Turn the main loop over once so the rest of
                 * the program -- the provider's I/O above all -- keeps
                 * running while we wait.
                 */
                g_main_context_iteration(g_main_context_get_thread_default(),
                                         FALSE);
                g_usleep(10000);
                break;
            default:
                break;
        }
    }

    app_schedule_redraw(app);

    return answer;
}

/* ================================================================
 * Provider setup
 * ================================================================ */

/*
 * Parse a --set value into whatever type the property wants.
 *
 * The same conversions `ai --set` performs; an unparseable value is an
 * error rather than a silent zero, because a run that quietly ignored the
 * bound you asked for is worse than one that refuses to start.
 */
static gboolean
value_from_string(GValue *value, GParamSpec *pspec, const gchar *text)
{
    GType type = G_PARAM_SPEC_VALUE_TYPE(pspec);
    gchar *end = NULL;

    g_value_init(value, type);

    if (type == G_TYPE_STRING)
    {
        g_value_set_string(value, text);
        return TRUE;
    }

    if (type == G_TYPE_BOOLEAN)
    {
        if (g_ascii_strcasecmp(text, "true") == 0 ||
            g_ascii_strcasecmp(text, "yes") == 0 ||
            g_ascii_strcasecmp(text, "on") == 0 ||
            g_strcmp0(text, "1") == 0)
        {
            g_value_set_boolean(value, TRUE);
            return TRUE;
        }

        if (g_ascii_strcasecmp(text, "false") == 0 ||
            g_ascii_strcasecmp(text, "no") == 0 ||
            g_ascii_strcasecmp(text, "off") == 0 ||
            g_strcmp0(text, "0") == 0)
        {
            g_value_set_boolean(value, FALSE);
            return TRUE;
        }

        return FALSE;
    }

    if (type == G_TYPE_INT || type == G_TYPE_UINT ||
        type == G_TYPE_INT64 || type == G_TYPE_UINT64)
    {
        gint64 parsed = g_ascii_strtoll(text, &end, 10);

        if (end == text || *end != '\0')
        {
            return FALSE;
        }

        if (type == G_TYPE_INT)        g_value_set_int(value, (gint)parsed);
        else if (type == G_TYPE_UINT)  g_value_set_uint(value, (guint)parsed);
        else if (type == G_TYPE_INT64) g_value_set_int64(value, parsed);
        else                           g_value_set_uint64(value, (guint64)parsed);

        return TRUE;
    }

    if (type == G_TYPE_DOUBLE || type == G_TYPE_FLOAT)
    {
        gdouble parsed = g_ascii_strtod(text, &end);

        if (end == text || *end != '\0')
        {
            return FALSE;
        }

        if (type == G_TYPE_DOUBLE) g_value_set_double(value, parsed);
        else                       g_value_set_float(value, (gfloat)parsed);

        return TRUE;
    }

    return FALSE;
}

/* Apply --set NAME=VALUE by GObject property, as `ai` does. */
static gboolean
apply_property_overrides(GObject *provider, GError **error)
{
    gsize i;

    if (opt_set == NULL)
    {
        return TRUE;
    }

    for (i = 0; opt_set[i] != NULL; i++)
    {
        g_auto(GStrv) parts = g_strsplit(opt_set[i], "=", 2);
        const gchar *name = parts[0];
        const gchar *value = parts[1] != NULL ? parts[1] : "true";
        GParamSpec *pspec;

        pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(provider), name);

        if (pspec == NULL)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                        "%s has no property '%s'",
                        G_OBJECT_TYPE_NAME(provider), name);
            return FALSE;
        }

        if (!(pspec->flags & G_PARAM_WRITABLE))
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                        "property '%s' is read-only", name);
            return FALSE;
        }

        {
            GValue parsed = G_VALUE_INIT;

            if (!value_from_string(&parsed, pspec, value))
            {
                g_value_unset(&parsed);
                g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                            "cannot parse '%s' for property '%s'", value, name);
                return FALSE;
            }

            g_object_set_property(provider, name, &parsed);
            g_value_unset(&parsed);
        }
    }

    return TRUE;
}

static GObject *
build_provider_named(
    const gchar *name,
    gboolean     initial,
    GError     **error
){
    g_autoptr(AiConfig) config = ai_config_new();
    GObject *provider;

    provider = ai_provider_factory_new_from_string(name, config, error);

    if (provider == NULL)
    {
        return NULL;
    }

    if (AI_IS_CLIENT(provider))
    {
        AiClient *c = AI_CLIENT(provider);

        if (initial && opt_model != NULL) ai_client_set_model(c, opt_model);
        if (initial && opt_system != NULL)
            ai_client_set_system_prompt(c, opt_system);
        ai_client_set_max_tokens(c, opt_max_tokens);
    }
    else if (AI_IS_CLI_CLIENT(provider))
    {
        AiCliClient *c = AI_CLI_CLIENT(provider);

        if (initial && opt_model != NULL)
            ai_cli_client_set_model(c, opt_model);
        if (initial && opt_system != NULL)
            ai_cli_client_set_system_prompt(c, opt_system);
        if (opt_effort != NULL)
            ai_cli_client_set_effort_level(c, opt_effort);
        ai_cli_client_set_max_tokens(c, opt_max_tokens);
    }

    /*
     * A provider switch deliberately does not inherit a native session.
     * ai_conversation_set_provider() clears one even if a caller supplied a
     * preconfigured object; only startup honors --continue.
     */
    if (initial && opt_continue &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
                                     "continue-session") != NULL)
    {
        g_object_set(provider, "continue-session", TRUE, NULL);
    }

    if (opt_skip_permissions &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
                                     "skip-permissions") != NULL)
    {
        g_object_set(provider, "skip-permissions", TRUE, NULL);
    }

    if (initial && !apply_property_overrides(provider, error))
    {
        g_object_unref(provider);
        return NULL;
    }

    return provider;
}

static GObject *
build_provider(GError **error)
{
    const gchar *name;

    name = opt_provider != NULL ? opt_provider : g_getenv("AI_PROVIDER");

    if (name == NULL)
    {
        name = "claude";
    }

    return build_provider_named(name, TRUE, error);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    GObject *provider;
    App app;

    setlocale(LC_ALL, "");

    context = g_option_context_new("- a terminal agent harness");
    g_option_context_add_main_entries(context, option_entries, NULL);
    g_option_context_set_summary(context,
        "Drives any ai-glib provider from a terminal, showing prose,\n"
        "reasoning and grouped tool calls as they happen.");

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_printerr("ai-tui: %s\n", error->message);
        return 1;
    }

    if (opt_version)
    {
        g_print("ai-tui %s\n", AI_GLIB_VERSION_STRING);
        return 0;
    }

    if (opt_license)
    {
        g_print("ai-tui, part of ai-glib.\n"
                "Copyright (C) 2026\n"
                "SPDX-License-Identifier: AGPL-3.0-or-later\n");
        return 0;
    }

    provider = build_provider(&error);

    if (provider == NULL)
    {
        g_printerr("ai-tui: %s\n", error->message);
        return 1;
    }

    memset(&app, 0, sizeof app);
    app.conversation = ai_conversation_new(provider);
    app.input = g_string_new(NULL);
    app.history = g_ptr_array_new_with_free_func(g_free);
    app.history_pos = -1;
    app.selected = -1;
    app.follow = TRUE;
    app.approve_all = opt_yes;

    ai_conversation_set_stream(app.conversation, !opt_no_stream);
    ai_conversation_set_max_tokens(app.conversation, opt_max_tokens);

    if (opt_system != NULL)
    {
        ai_conversation_set_system_prompt(app.conversation, opt_system);
    }

    if (opt_local_tools)
    {
        ai_conversation_set_local_tools(app.conversation, TRUE);

        if (!ai_conversation_get_local_tools(app.conversation))
        {
            g_printerr("ai-tui: note: %s runs its own tools; "
                       "--local-tools ignored\n",
                       G_OBJECT_TYPE_NAME(provider));
        }
    }

    /*
     * The harness layer.
     *
     * Built even when --no-expand is given, because /help and the
     * listings are how somebody works out why their file is not being
     * found --- and that is exactly the moment they will have turned
     * expansion off.
     */
    {
        g_autofree gchar *cwd = g_get_current_dir();

        app.registry = ai_resource_registry_new();
        ai_resource_registry_set_working_directory(app.registry, cwd);
        ai_resource_registry_scan(app.registry);
        ai_resource_registry_set_watching(app.registry, TRUE);

        app.commands = ai_command_set_new(app.registry);
        app.completion = ai_completion_context_new(app.commands, cwd);

        ai_conversation_set_command_set(app.conversation, app.commands);
        ai_conversation_set_working_directory(app.conversation, cwd);
    }

    /*
     * Background agents, on by default -- ai-tui is the reference
     * frontend and this is what it is for. An application embedding the
     * harness gets none of this unless it asks: the library creates no
     * brigade of its own, precisely because unattended model runs that
     * outlive a turn are a grant an embedder should make deliberately.
     *
     * --no-agents is the way out for somebody who wants ai-tui without
     * it.
     */
    if (!opt_no_agents)
    {
        ai_conversation_enable_background_agents(app.conversation,
                                                 AGENT_MAX_CONCURRENT);

        g_signal_connect(app.conversation, "agent-finished",
                         G_CALLBACK(on_agent_finished), &app);
    }

    /*
     * --dry-run: what the CLI provider would actually run.
     *
     * Through the build_argv vtable, so it covers every CLI provider
     * without this file knowing their names -- the same rule `ai` follows.
     * claude-tmux is the exception there because it bypasses the argv
     * pipeline entirely; here it simply reports that.
     */
    if (opt_dry_run)
    {
        if (!AI_IS_CLI_CLIENT(provider))
        {
            g_print("%s is an HTTP provider; there is no command to show.\n",
                    ai_provider_get_name(AI_PROVIDER(provider)));
        }
        else
        {
            AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(provider);
            g_autoptr(AiMessage) message =
                ai_message_new_user(opt_dump != NULL ? opt_dump : "(prompt)");
            GList *messages = g_list_append(NULL, message);

            if (klass->build_argv == NULL)
            {
                g_print("%s does not build a command line.\n",
                        G_OBJECT_TYPE_NAME(provider));
            }
            else
            {
                g_auto(GStrv) command = klass->build_argv(
                    AI_CLI_CLIENT(provider), messages, opt_system,
                    opt_max_tokens, !opt_no_stream);
                g_autofree gchar *resolved =
                    ai_cli_client_resolve_executable(AI_CLI_CLIENT(provider),
                                                     NULL);
                gsize i;

                if (command != NULL && resolved != NULL)
                {
                    g_free(command[0]);
                    command[0] = g_steal_pointer(&resolved);
                }

                for (i = 0; command != NULL && command[i] != NULL; i++)
                {
                    g_print("%s%s", i > 0 ? " " : "", command[i]);
                }

                g_print("\n");
            }

            g_list_free(messages);
        }

        g_clear_object(&app.completion);
        g_clear_object(&app.commands);
        g_clear_object(&app.registry);
        g_object_unref(app.conversation);
        g_object_unref(provider);
        g_string_free(app.input, TRUE);
        g_ptr_array_unref(app.history);

        return 0;
    }

    /* --dump: one turn, no terminal. What the tests drive. */
    if (opt_dump != NULL)
    {
        g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
        g_autofree gchar *text = NULL;

        app.loop = loop;
        app.dump_loop = loop;

        if (opt_no_expand)
        {
            ai_conversation_send_async(app.conversation, opt_dump, NULL,
                                       on_sent, &app);
        }
        else
        {
            ai_conversation_send_input_async(app.conversation, opt_dump, NULL,
                                             on_input_sent, &app);
        }

        g_main_loop_run(loop);
        app.dump_loop = NULL;

        text = ai_transcript_to_text(
            ai_conversation_get_transcript(app.conversation),
            (guint)MAX(0, opt_width));

        g_print("%s", text);

        g_clear_object(&app.completion);
        g_clear_object(&app.commands);
        g_clear_object(&app.registry);
        g_object_unref(app.conversation);
        g_object_unref(provider);
        g_string_free(app.input, TRUE);
        g_ptr_array_unref(app.history);

        return 0;
    }

    if (!isatty(STDIN_FILENO))
    {
        g_printerr("ai-tui: stdin is not a terminal; use --dump PROMPT "
                   "to run one turn non-interactively\n");
        g_object_unref(app.conversation);
        g_object_unref(provider);
        g_string_free(app.input, TRUE);
        g_ptr_array_unref(app.history);
        return 1;
    }

    /* ---- Terminal ---- */

    initscr();
    set_escdelay(ESCAPE_SETTLE_MS);
    init_colours();
    cbreak();
    noecho();
    nonl();
    curs_set(1);

    {
        gint height;
        gint width;

        getmaxyx(stdscr, height, width);

        app.transcript_win = newwin(MAX(1, height - 2), width, 0, 0);
        app.status_win = newwin(1, width, MAX(0, height - 2), 0);
        app.input_win = newwin(1, width, MAX(0, height - 1), 0);
    }

    keypad(app.input_win, TRUE);
    nodelay(app.input_win, TRUE);

    /*
     * Teach ncurses the sequences a terminal sends for Shift+Enter.
     *
     * There is no standard one. A plain terminal sends a carriage return
     * for Shift+Enter exactly as it does for Enter --- which is why every
     * harness that binds the two differently ships a configuration step.
     * These are the two encodings a terminal produces once it has been
     * configured to distinguish them: the kitty keyboard protocol's, and
     * xterm's modifyOtherKeys. Defining both costs nothing where the
     * terminal sends neither, and means no setup at all where it sends
     * one.
     *
     * Alt+Enter needs none of this and works everywhere, which is why it
     * is the binding the documentation leads with.
     */
    define_key("\033[13;2u", KEY_SHIFT_ENTER);
    define_key("\033[27;2;13~", KEY_SHIFT_ENTER);

    app.running = TRUE;
    app.loop = g_main_loop_new(NULL, FALSE);

    g_signal_connect_swapped(ai_conversation_get_transcript(app.conversation),
                             "items-changed",
                             G_CALLBACK(app_schedule_redraw), &app);
    g_signal_connect_swapped(ai_conversation_get_transcript(app.conversation),
                             "block-changed",
                             G_CALLBACK(app_schedule_redraw), &app);
    g_signal_connect(app.conversation, "approval-requested",
                     G_CALLBACK(on_approval_requested), &app);

    /* The spinner follows ::busy, and the words follow ::activity. */
    g_signal_connect(app.conversation, "notify::busy",
                     G_CALLBACK(on_busy_changed), &app);
    g_signal_connect_swapped(app.conversation, "notify::activity",
                             G_CALLBACK(app_schedule_redraw), &app);

    /*
     * Keys come through the main loop rather than a blocking read, so the
     * provider's asynchronous I/O runs while the user types.
     */
    g_unix_fd_add(STDIN_FILENO, G_IO_IN, on_key, &app);
    g_unix_signal_add(SIGWINCH, on_resize, &app);

    /*
     * ^C arrives as a signal, not as a keystroke.
     *
     * cbreak() turns off line buffering but leaves ISIG on, so the
     * terminal driver raises SIGINT before ncurses ever sees the byte ---
     * which meant the `case 3:` in drain_keys() had never once run, and
     * ^C killed the program outright despite everything claiming it
     * cancelled the turn.
     *
     * Handled here rather than by switching to raw(), which would also
     * take away ^Z and flow control. g_unix_signal_add() dispatches on
     * the main loop, so this is ordinary code and not a signal handler.
     */
    g_unix_signal_add(SIGINT, on_sigint, &app);

    app_redraw(&app);
    g_main_loop_run(app.loop);

    endwin();

    if (app.settle_id != 0)
    {
        g_source_remove(app.settle_id);
        app.settle_id = 0;
    }

    if (app.spinner_id != 0)
    {
        g_source_remove(app.spinner_id);
        app.spinner_id = 0;
    }

    completion_close(&app);
    g_clear_object(&app.completion);
    g_clear_object(&app.commands);
    g_clear_object(&app.registry);
    g_clear_object(&app.cancellable);
    g_object_unref(app.conversation);
    g_object_unref(provider);
    g_string_free(app.input, TRUE);
    g_ptr_array_unref(app.history);
    g_main_loop_unref(app.loop);

    return 0;
}
