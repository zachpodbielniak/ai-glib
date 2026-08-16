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
    { "no-expand", 0, 0, G_OPTION_ARG_NONE, &opt_no_expand,
      "Send input verbatim: no @ mentions, no / commands", NULL },
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

    /* The harness layer: what /name and @path mean here. */
    AiResourceRegistry  *registry;
    AiCommandSet        *commands;
    AiCompletionContext *completion;

    /* The completion popup, live only while it is open. */
    AiCompletionResult  *candidates;
    guint                candidate_index;

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
static void completion_accept(App *app);
static void completion_close(App *app);
static void on_input_sent(GObject *source, GAsyncResult *result,
                          gpointer user_data);
static void say(App *app, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

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

    /*
     * Highlight what the pipeline will act on, using the same scanner it
     * uses. A mention that will not resolve still lights up, which is
     * honest: it is a candidate, and the user can see it was noticed.
     */
    {
        GList       *mentions = ai_mention_scan(app->input->str);
        GList       *iter;
        const gchar *text = app->input->str;
        gsize        offset = 0;
        gint         column = 2;
        gboolean     is_command =
            ai_command_set_is_command_line(app->input->str);

        for (iter = mentions; iter != NULL; iter = iter->next)
        {
            const AiMention  *mention = iter->data;
            g_autofree gchar *before =
                g_strndup(text + offset, mention->start - offset);
            g_autofree gchar *span =
                g_strndup(text + mention->start, mention->len);

            wattrset(app->input_win, A_NORMAL);
            mvwaddnstr(app->input_win, 0, column, before,
                       MAX(0, width - column));
            column += (gint)ai_style_text_width(before);

            wattrset(app->input_win, attr_for_tag(AI_STYLE_MENTION));
            mvwaddnstr(app->input_win, 0, column, span,
                       MAX(0, width - column));
            column += (gint)ai_style_text_width(span);

            offset = mention->start + mention->len;
        }

        g_list_free_full(mentions, (GDestroyNotify)ai_mention_free);

        if (is_command && offset == 0)
        {
            /* The command name, up to the first space. */
            const gchar      *space = strchr(text, ' ');
            gsize             name_len =
                (space != NULL) ? (gsize)(space - text) : strlen(text);
            g_autofree gchar *name = g_strndup(text, name_len);

            wattrset(app->input_win, attr_for_tag(AI_STYLE_COMMAND));
            mvwaddnstr(app->input_win, 0, column, name,
                       MAX(0, width - column));
            column += (gint)ai_style_text_width(name);
            offset = name_len;
        }

        wattrset(app->input_win, A_NORMAL);
        mvwaddnstr(app->input_win, 0, column, text + offset,
                   MAX(0, width - column));
    }

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
                    "  Tab        complete /command or @path\n"
                    "  ^N         cycle tool and thinking blocks\n"
                    "  ^B         expand or collapse the selected block\n"
                    "  ^C         stop the current turn\n"
                    "  ^D         quit\n");

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
    else if (g_strcmp0(name, "cwd") == 0)
    {
        change_directory(app, arguments);
    }
    else if (g_strcmp0(name, "save") == 0)
    {
        save_transcript(app, arguments);
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

        /* Switching provider mid-conversation would need a new
         * transcript and a new history; say so rather than half-doing it. */
        say(app, "Provider: %s. Restart with -p to change it.",
            G_OBJECT_TYPE_NAME(provider));
    }
    else
    {
        say(app, "/%s is not implemented here.", name);
    }

    app_schedule_redraw(app);
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
                /* A visible menu means Enter is a choice, not a send. */
                if (app->candidates != NULL)
                {
                    completion_accept(app);
                    break;
                }

                app_send(app);
                break;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                completion_close(app);
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
                completion_advance(app);
                break;

            case 14:  /* ^N: cycle tool and thinking blocks */
                completion_close(app);
                select_next_tool_block(app);
                break;

            case 27:  /* Escape: dismiss the popup */
                completion_close(app);
                break;

            case 2:   /* ^B: toggle the selected block */
                completion_close(app);
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

                    /* The query is stale the moment the buffer changes. */
                    completion_close(app);
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
}

/*
 * Open the popup, or step through it if it is already open.
 *
 * All of the thinking is in ai_completion_context_query(): this decides nothing
 * about what completes where, which is why the same behaviour will come
 * out of an Emacs frontend calling the same function.
 */
static void
completion_advance(App *app)
{
    if (app->completion == NULL)
    {
        return;
    }

    if (app->candidates != NULL)
    {
        guint n = ai_completion_result_get_n_items(app->candidates);

        if (n > 0)
        {
            app->candidate_index = (app->candidate_index + 1) % n;
        }

        return;
    }

    app->candidates = ai_completion_context_query(app->completion, app->input->str,
                                          app->cursor);
    app->candidate_index = 0;

    if (ai_completion_result_get_n_items(app->candidates) == 0)
    {
        completion_close(app);
        return;
    }

    /*
     * One candidate needs no menu. Several that agree on a prefix get the
     * prefix inserted first, so a second Tab is a choice rather than a
     * repetition.
     */
    if (ai_completion_result_get_n_items(app->candidates) == 1)
    {
        completion_accept(app);
        return;
    }

    {
        g_autofree gchar *prefix =
            ai_completion_result_get_common_prefix(app->candidates);
        guint             start =
            ai_completion_result_get_start(app->candidates);
        guint             end = ai_completion_result_get_end(app->candidates);

        if (prefix != NULL && strlen(prefix) > (gsize)(end - start))
        {
            g_string_erase(app->input, (gssize)start, (gssize)(end - start));
            g_string_insert(app->input, (gssize)start, prefix);
            app->cursor = start + (guint)strlen(prefix);

            g_clear_object(&app->candidates);
            app->candidates = ai_completion_context_query(app->completion,
                                                  app->input->str,
                                                  app->cursor);
            app->candidate_index = 0;
        }
    }
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
                                              NULL, NULL, NULL))
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

/* Draw the popup over the bottom of the transcript. */
static void
draw_completion(App *app)
{
    guint n;
    gint  height;
    gint  width;
    gint  rows;
    gint  first;
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
    rows = MIN((gint)n, MIN(10, height));

    /* Keep the highlighted entry on screen when the list is long. */
    first = MAX(0, (gint)app->candidate_index - rows + 1);

    for (i = 0; i < rows; i++)
    {
        const gchar *display = NULL;
        const gchar *description = NULL;
        guint        index = (guint)(first + i);
        gboolean     selected = (index == app->candidate_index);
        gint         y = height - rows + i;

        if (!ai_completion_result_get_item_fields(app->candidates, index,
                                                  NULL, &display,
                                                  &description, NULL))
        {
            break;
        }

        wattrset(app->transcript_win,
                 selected ? A_REVERSE : attr_for_tag(AI_STYLE_DIM));
        mvwhline(app->transcript_win, y, 0, ' ', width);
        mvwaddnstr(app->transcript_win, y, 1, display, width - 2);

        if (description != NULL && width > 40)
        {
            mvwaddnstr(app->transcript_win, y, 32, description, width - 33);
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
