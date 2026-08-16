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
static gboolean  opt_version = FALSE;
static gboolean  opt_license = FALSE;

static const GOptionEntry option_entries[] = {
    { "provider", 'p', 0, G_OPTION_ARG_STRING, &opt_provider,
      "Provider: claude, openai, gemini, grok, ollama, claude-code, "
      "claude-tmux, opencode, grok-build", "NAME" },
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

static void
init_colours(void)
{
    if (!has_colors())
    {
        return;
    }

    start_color();
    use_default_colors();

    init_pair(pair_for_tag(AI_STYLE_DEFAULT),      -1,            -1);
    init_pair(pair_for_tag(AI_STYLE_USER_PROMPT),  COLOR_WHITE,   -1);
    init_pair(pair_for_tag(AI_STYLE_HEADING),      COLOR_WHITE,   -1);
    init_pair(pair_for_tag(AI_STYLE_DIM),          COLOR_BLUE,    -1);
    init_pair(pair_for_tag(AI_STYLE_TOOL_NAME),    COLOR_MAGENTA, -1);
    init_pair(pair_for_tag(AI_STYLE_TOOL_TARGET),  COLOR_CYAN,    -1);
    init_pair(pair_for_tag(AI_STYLE_TOOL_PENDING), COLOR_YELLOW,  -1);
    init_pair(pair_for_tag(AI_STYLE_TOOL_OK),      COLOR_GREEN,   -1);
    init_pair(pair_for_tag(AI_STYLE_TOOL_FAILED),  COLOR_RED,     -1);
    init_pair(pair_for_tag(AI_STYLE_ADDED),        COLOR_GREEN,   -1);
    init_pair(pair_for_tag(AI_STYLE_REMOVED),      COLOR_RED,     -1);
    init_pair(pair_for_tag(AI_STYLE_CODE),         COLOR_CYAN,    -1);
    init_pair(pair_for_tag(AI_STYLE_THINKING),     COLOR_BLUE,    -1);
    init_pair(pair_for_tag(AI_STYLE_ERROR),        COLOR_RED,     -1);
    init_pair(pair_for_tag(AI_STYLE_STATUS),       COLOR_YELLOW,  -1);
    init_pair(pair_for_tag(AI_STYLE_LINK),         COLOR_BLUE,    -1);
    init_pair(pair_for_tag(AI_STYLE_MARKER),       COLOR_BLUE,    -1);
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
            attr |= A_BOLD;
            break;
        case AI_STYLE_DIM:
        case AI_STYLE_THINKING:
        case AI_STYLE_MARKER:
            attr |= A_DIM;
            break;
        case AI_STYLE_ERROR:
            attr |= A_BOLD;
            break;
        default:
            break;
    }

    return attr;
}

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

    line = g_strdup_printf(" %s%s%s   %s%s",
                           ai_provider_get_name(AI_PROVIDER(provider)),
                           model != NULL ? " / " : "",
                           model != NULL ? model : "",
                           ai_conversation_get_busy(app->conversation)
                               ? "working... (^C to stop)"
                               : "ready",
                           app->follow ? "" : "   [scrolled]");

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

static void
draw_input(App *app)
{
    gint width = getmaxx(app->input_win);
    gint cursor_col;

    werase(app->input_win);
    wattrset(app->input_win, A_BOLD);
    mvwaddstr(app->input_win, 0, 0, "> ");
    wattrset(app->input_win, A_NORMAL);

    mvwaddnstr(app->input_win, 0, 2, app->input->str, width - 2);

    {
        g_autofree gchar *before = g_strndup(app->input->str, app->cursor);

        cursor_col = 2 + (gint)ai_style_text_width(before);
    }

    wmove(app->input_win, 0, MIN(cursor_col, width - 1));
    wnoutrefresh(app->input_win);
}

static void
app_redraw(App *app)
{
    g_autoptr(GPtrArray) rows = NULL;
    gint height = getmaxy(app->transcript_win);
    gint width = getmaxx(app->transcript_win);
    gint first;
    gint i;

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

/* A slash command, or FALSE if the line is an ordinary prompt. */
static gboolean
handle_command(App *app, const gchar *line)
{
    if (g_strcmp0(line, "/quit") == 0 || g_strcmp0(line, "/q") == 0)
    {
        app->running = FALSE;
        g_main_loop_quit(app->loop);
        return TRUE;
    }

    if (g_strcmp0(line, "/clear") == 0)
    {
        ai_conversation_clear(app->conversation);
        app->selected = -1;
        app->follow = TRUE;
        app_schedule_redraw(app);
        return TRUE;
    }

    if (g_str_has_prefix(line, "/model "))
    {
        GObject *provider = ai_conversation_get_provider(app->conversation);
        const gchar *model = line + strlen("/model ");

        if (AI_IS_CLIENT(provider))
        {
            ai_client_set_model(AI_CLIENT(provider), model);
        }
        else if (AI_IS_CLI_CLIENT(provider))
        {
            ai_cli_client_set_model(AI_CLI_CLIENT(provider), model);
        }

        app_schedule_redraw(app);
        return TRUE;
    }

    return FALSE;
}

static gboolean
on_key(gint fd, GIOCondition condition, gpointer user_data)
{
    App *app = user_data;
    gint ch;

    (void)fd;
    (void)condition;

    while ((ch = wgetch(app->input_win)) != ERR)
    {
        switch (ch)
        {
            case '\n':
            case '\r':
            case KEY_ENTER:
                app_send(app);
                break;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                input_backspace(app);
                break;

            case KEY_LEFT:
                input_move(app, -1);
                break;

            case KEY_RIGHT:
                input_move(app, 1);
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
                break;

            case KEY_UP:
                input_recall(app, -1);
                break;

            case KEY_DOWN:
                input_recall(app, 1);
                break;

            case KEY_PPAGE:
                app->follow = FALSE;
                app->scroll -= getmaxy(app->transcript_win) / 2;
                break;

            case KEY_NPAGE:
                app->scroll += getmaxy(app->transcript_win) / 2;
                app->follow = TRUE;   /* re-clamped on redraw */
                break;

            case '\t':
                select_next_tool_block(app);
                break;

            case 2:   /* ^B: toggle the selected block */
                toggle_selected(app);
                break;

            case 3:   /* ^C: stop the turn, never the program */
                ai_conversation_cancel(app->conversation);
                break;

            case 4:   /* ^D */
                if (app->input->len == 0)
                {
                    app->running = FALSE;
                    g_main_loop_quit(app->loop);
                    return G_SOURCE_REMOVE;
                }
                break;

            default:
                if (ch >= 32 && ch < 127)
                {
                    gchar text[2] = { (gchar)ch, '\0' };
                    input_insert(app, text);
                }
                break;
        }

        app_schedule_redraw(app);
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

    wresize(app->transcript_win, MAX(1, height - 2), width);
    wresize(app->status_win, 1, width);
    mvwin(app->status_win, MAX(0, height - 2), 0);
    wresize(app->input_win, 1, width);
    mvwin(app->input_win, MAX(0, height - 1), 0);

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

    if (handle_command(app, line))
    {
        return;
    }

    if (ai_conversation_get_busy(app->conversation))
    {
        return;
    }

    app->follow = TRUE;
    g_clear_object(&app->cancellable);
    app->cancellable = g_cancellable_new();

    ai_conversation_send_async(app->conversation, line, app->cancellable,
                               on_sent, app);
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
build_provider(GError **error)
{
    g_autoptr(AiConfig) config = ai_config_new();
    const gchar *name;
    GObject *provider;

    name = opt_provider != NULL ? opt_provider : g_getenv("AI_PROVIDER");

    if (name == NULL)
    {
        name = "claude";
    }

    provider = ai_provider_factory_new_from_string(name, config, error);

    if (provider == NULL)
    {
        return NULL;
    }

    if (AI_IS_CLIENT(provider))
    {
        AiClient *c = AI_CLIENT(provider);

        if (opt_model != NULL)  ai_client_set_model(c, opt_model);
        if (opt_system != NULL) ai_client_set_system_prompt(c, opt_system);
        ai_client_set_max_tokens(c, opt_max_tokens);
    }
    else if (AI_IS_CLI_CLIENT(provider))
    {
        AiCliClient *c = AI_CLI_CLIENT(provider);

        if (opt_model != NULL)  ai_cli_client_set_model(c, opt_model);
        if (opt_system != NULL) ai_cli_client_set_system_prompt(c, opt_system);
        if (opt_effort != NULL) ai_cli_client_set_effort_level(c, opt_effort);
        ai_cli_client_set_max_tokens(c, opt_max_tokens);
    }

    /*
     * By property name, so a provider that grows a knob needs no change
     * here -- the same reason `ai` resolves --continue this way.
     */
    if (opt_continue &&
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

    if (!apply_property_overrides(provider, error))
    {
        g_object_unref(provider);
        return NULL;
    }

    return provider;
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
        ai_conversation_send_async(app.conversation, opt_dump, NULL,
                                   on_sent, &app);

        while (ai_conversation_get_busy(app.conversation))
        {
            g_main_context_iteration(NULL, TRUE);
        }

        text = ai_transcript_to_text(
            ai_conversation_get_transcript(app.conversation),
            (guint)MAX(0, opt_width));

        g_print("%s", text);

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

    /*
     * Keys come through the main loop rather than a blocking read, so the
     * provider's asynchronous I/O runs while the user types.
     */
    g_unix_fd_add(STDIN_FILENO, G_IO_IN, on_key, &app);
    g_unix_signal_add(SIGWINCH, on_resize, &app);

    app_redraw(&app);
    g_main_loop_run(app.loop);

    endwin();

    g_clear_object(&app.cancellable);
    g_object_unref(app.conversation);
    g_object_unref(provider);
    g_string_free(app.input, TRUE);
    g_ptr_array_unref(app.history);
    g_main_loop_unref(app.loop);

    return 0;
}
