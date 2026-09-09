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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <ncurses.h>
#include <glib-unix.h>

#include <ai-glib.h>
#include "ai-launch.h"
#include "ai-mcp-options.h"
#include "ai-tui-theme.h"
#include "ai-tui-history.h"
#include "ai-tui-herdr.h"
#include "ai-tui-panel.h"

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
static gboolean  opt_no_herdr = FALSE;
static gboolean  opt_version = FALSE;
static gboolean  opt_license = FALSE;
static gboolean  opt_launch = FALSE;
static gboolean  opt_launch_cmd = FALSE;
static gboolean  opt_launch_cmd_print = FALSE;
static gchar *opt_theme = NULL;
static gboolean opt_list_themes = FALSE;
static gboolean opt_no_animation = FALSE;
static gboolean theme_explicit = FALSE;

static const GOptionEntry option_entries[] = {
	{ "no-herdr", 0, 0, G_OPTION_ARG_NONE, &opt_no_herdr,
	  "Disable automatic herdr pane lifecycle reporting", NULL },
	{ "theme", 0, 0, G_OPTION_ARG_STRING, &opt_theme, "Terminal theme (overrides AI_TUI_THEME and NO_COLOR)", "NAME" },
	{ "list-themes", 0, 0, G_OPTION_ARG_NONE, &opt_list_themes, "List terminal themes without loading a provider", NULL },
	{ "no-animation", 0, 0, G_OPTION_ARG_NONE, &opt_no_animation, "Disable decorative motion (elapsed time still updates)", NULL },
    { "provider", 'p', 0, G_OPTION_ARG_STRING, &opt_provider,
      "Provider: claude, openai, gemini, grok, ollama, claude-code, "
      "claude-tmux, opencode, grok-build, antigravity (agy), cursor, codex-cli "
      "(omitted/default: saved ai-tui defaults)", "NAME" },
    { "model", 'm', 0, G_OPTION_ARG_STRING, &opt_model,
      "Model id (omitted/default: matching ai-tui saved model, else native)", "MODEL" },
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
	{ "launch", 0, 0, G_OPTION_ARG_NONE, &opt_launch,
	  "Open the provider's native interactive CLI instead of this harness", NULL },
	{ "launch-cmd", 0, 0, G_OPTION_ARG_NONE, &opt_launch_cmd,
	  "Print the native interactive CLI command without running it", NULL },
	{ "launch-cmd-print", 0, 0, G_OPTION_ARG_NONE, &opt_launch_cmd_print,
	  "Print the native plain-text noninteractive command without running it", NULL },
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
    COLOR_GREEN,    /* todo-done     */
	COLOR_MAGENTA,  /* syntax-keyword */
	COLOR_GREEN,    /* syntax-string */
	COLOR_BLUE,     /* syntax-comment */
	COLOR_YELLOW,   /* syntax-number */
	COLOR_YELLOW,   /* syntax-type */
	COLOR_CYAN      /* syntax-function */
};

G_STATIC_ASSERT(G_N_ELEMENTS(TAG_COLOURS) == AI_STYLE_N_TAGS);

static void
init_colours(void)
{
    guint i;
	const TuiTheme *theme = &THEMES[theme_index];
	short bg = -1, surface = -1, fg = -1;
	gboolean native;

    theme_colour = has_colors() && !g_str_equal(theme->name, "monochrome") &&
		(theme_explicit || g_getenv("NO_COLOR") == NULL);
    if (!theme_colour)
    {
        return;
    }

	if (start_color() == ERR)
	{
		theme_colour = FALSE;
		return;
	}
	if (use_default_colors() == ERR) { bg = COLOR_BLACK; surface = bg; fg = COLOR_WHITE; }
	native = g_str_equal(theme->name, "terminal") || COLORS < 256;
	if (!native) { bg = theme_nearest(theme->background); surface = theme_nearest(theme->surface); fg = theme_nearest(theme->text); }
	if (COLORS < 8 || COLOR_PAIRS <= PAIR_PANEL_ACCENT) { theme_colour = FALSE; return; }

    for (i = 0; i < AI_STYLE_N_TAGS; i++)
    {
		short colour = TAG_COLOURS[i];
		if (!native)
		{
			guint rgb = theme->text;
			switch (colour) {
			case COLOR_BLUE: rgb = theme->muted; break;
			case COLOR_MAGENTA: rgb = theme->accent; break;
			case COLOR_CYAN: rgb = theme->cyan; break;
			case COLOR_GREEN: rgb = theme->green; break;
			case COLOR_YELLOW: rgb = theme->yellow; break;
			case COLOR_RED: rgb = theme->red; break;
			default: break;
			}
			if (i == AI_STYLE_SYNTAX_NUMBER) rgb = theme->number;
			if (i == AI_STYLE_SYNTAX_FUNCTION) rgb = theme->function;
			colour = theme_nearest(rgb);
		}
		else if (colour == COLOR_BLUE)
			colour = i == AI_STYLE_LINK ? COLOR_CYAN : fg;
		else if (colour == -1) colour = fg;
		if (init_pair(pair_for_tag((AiStyleTag)i), colour, bg) == ERR)
		{
			theme_colour = FALSE;
			return;
		}
    }
	if (init_pair(PAIR_SURFACE, fg, surface) == ERR ||
		init_pair(PAIR_PANEL_ACCENT, native ? COLOR_CYAN : theme_nearest(theme->accent), surface) == ERR ||
		init_pair(PAIR_ACCENT, native ? COLOR_CYAN : theme_nearest(theme->accent), bg) == ERR ||
		init_pair(PAIR_SELECTION, native ? COLOR_BLACK : theme_nearest(theme->background), native ? COLOR_CYAN : theme_nearest(theme->accent)) == ERR)
		theme_colour = FALSE;
}

static attr_t
attr_for_tag(AiStyleTag tag)
{
    attr_t attr = theme_colour ? COLOR_PAIR(pair_for_tag(tag)) : 0;

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
            if (!theme_colour) attr |= A_DIM;
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

/* Short, bounded accents; a quiet session must not repaint indefinitely. */
#define INTRO_DURATION_US (900 * G_TIME_SPAN_MILLISECOND)
#define FEEDBACK_DURATION_US (1600 * G_TIME_SPAN_MILLISECOND)

/*
 * Handing the turn back to the user.
 *
 * A model finishing looks, on a quiet screen, exactly like a model still
 * thinking: the sweep stops, the spinner stops, and nothing says whose
 * move it is. So the input frame breathes for a moment and then *stays*
 * in a different colour with a different title until something is typed.
 *
 * The breathing is bounded and the resting state is static, deliberately.
 * An indefinite pulse would repaint a terminal that somebody left open
 * overnight, which is the rule INTRO_DURATION_US above exists to keep;
 * the colour and the title carry the state for free once it stops.
 */
#define AWAIT_PULSE_STEP_US (420 * G_TIME_SPAN_MILLISECOND)
#define AWAIT_PULSE_CYCLES (3)
#define AWAIT_PULSE_DURATION_US (2 * AWAIT_PULSE_CYCLES * AWAIT_PULSE_STEP_US)

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
#define KEY_PASTE_START (KEY_MAX + 2)
#define KEY_PASTE_END (KEY_MAX + 3)

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
	AiMcpHost *mcp_host;
	AiTuiHerdr *herdr;
    GMainLoop      *loop;
    GCancellable   *cancellable;

    WINDOW         *transcript_win;
    WINDOW         *status_win;
    WINDOW         *input_win;
	gboolean details;
	gboolean tiny;
	gint content_width;
	gint input_first;
	gint row_count;
	GPtrArray *row_cache;
	gint row_cache_width;
	GString *search;
	gboolean searching;
	gint search_row;
	guint search_matches;
	gchar *history_draft;
	gchar *kill_buffer;
	const gchar *approval_prompt;
	gint approval_answer;
	gboolean sending;
	gboolean pasting;
	gboolean skip_permissions;

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

    /* One clock for activity and bounded UI accents, never a permanent idle loop. */
    guint                spinner_id;
    guint                spinner_frame;
	gint64               intro_started;
	gint64               feedback_until;

	/* When the last turn handed control back, or 0 while a turn owns it.
	 * Not cleared by the first keystroke --- app_awaiting_user() reads the
	 * input buffer for that, so emptying the line again says "your turn"
	 * again, which is what is actually true. */
	gint64               awaiting_since;
	gchar               *feedback;
	AiStyleTag           feedback_style;

    /* Set for a one-shot turn (--dump, or a prompt with no terminal),
     * and quit by whichever callback finishes it. Polling the busy flag
     * is not enough: a line that resolves to a built-in never sets it. */
    GMainLoop           *dump_loop;
} App;

/* Publish expansion, provider I/O and approvals through one state mapping.
 * This callback also runs in dump mode, where there are no curses windows. */
static void
app_sync_herdr(App *app)
{
	ai_tui_herdr_update(app->herdr,
		app->sending || ai_conversation_get_busy(app->conversation),
		app->approval_prompt != NULL);
}

/* Dispatch termination on the main loop so normal cleanup can release the
 * herdr identity and restore the terminal, including during an approval. */
static gboolean
on_herdr_shutdown(gpointer data)
{
	App *app = data;

	app->approval_answer = AI_TOOL_APPROVAL_DENY_ALL;
	app->running = FALSE;
	ai_conversation_cancel(app->conversation);
	if (app->loop != NULL) g_main_loop_quit(app->loop);
	return G_SOURCE_CONTINUE;
}

/* One rendered row: which block it came from, and its text. */
typedef struct
{
    AiViewBlock    *block;
    guint           block_index;
    AiRenderedText *rendered;
    guint           line_start;    /* byte offset within rendered */
    guint           line_len;
	const gchar *label;
} Row;

static void app_schedule_redraw(App *app);
static void draw_completion(App *app);
static void completion_advance(App *app);
static void completion_refresh(App *app);
static gboolean completion_select(App *app, gint delta);
static void completion_accept(App *app);
static gboolean completion_is_exact_command(App *app);
static void completion_close(App *app);
static void on_input_sent(GObject *source, GAsyncResult *result,
                          gpointer user_data);
static void say(App *app, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static gchar *fit_to_width(const gchar *text, gint columns);
static gboolean on_resize(gpointer user_data);
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
    GPtrArray *rows;
    AiTranscript *transcript = ai_conversation_get_transcript(app->conversation);
    guint n = ai_transcript_get_n_blocks(transcript);
    guint i;

	/* Activity ticks and cursor movement do not change transcript geometry. */
	if (app->row_cache != NULL && app->row_cache_width == width)
		return g_ptr_array_ref(app->row_cache);
	rows = g_ptr_array_new_with_free_func(row_free);

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
		if (ai_view_block_get_kind(block) == AI_VIEW_BLOCK_TURN ||
			ai_view_block_get_kind(block) == AI_VIEW_BLOCK_TEXT)
		{
			Row *heading = g_new0(Row, 1);
			heading->block = block;
			heading->block_index = i;
			heading->rendered = ai_rendered_text_ref(rendered);
			heading->label = ai_view_block_get_kind(block) == AI_VIEW_BLOCK_TURN ? "YOU" : "ASSISTANT";
			g_ptr_array_add(rows, heading);
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

	g_clear_pointer(&app->row_cache, g_ptr_array_unref);
	app->row_cache = g_ptr_array_ref(rows);
	app->row_cache_width = width;
    return rows;
}

/* A cached row borrows its block; drop the cache synchronously when the
 * transcript changes, before a later draw can encounter a removed block. */
static void
on_transcript_changed(App *app)
{
	g_clear_pointer(&app->row_cache, g_ptr_array_unref);
	app_schedule_redraw(app);
}

/* Enable shared previews as blocks arrive, including in --dump mode.
 * The library keeps summary-only behavior for other embedders by default. */
static void
on_transcript_items_changed(AiTranscript *transcript, guint position,
                            guint removed, guint added, gpointer user_data)
{
	App *app = user_data;
	guint i;
	(void)removed;
	for (i = position; i < position + added; i++)
	{
		AiViewBlock *block = ai_transcript_get_block(transcript, i);
		if (AI_IS_VIEW_TOOL_BLOCK(block)) g_object_set(block, "show-previews", TRUE, NULL);
	}
	on_transcript_changed(app);
}

/* Draw one row, switching attributes as the spans say. */
static void
draw_row(App *app, WINDOW *win, gint y, Row *row, gboolean selected)
{
    const gchar *text = ai_rendered_text_get_text(row->rendered);
    guint offset = row->line_start;
    guint end = row->line_start + row->line_len;
    gint x = 2;

	if (row->label != NULL)
	{
		wattrset(win, theme_attr(ai_view_block_get_kind(row->block) == AI_VIEW_BLOCK_TURN
			? PAIR_SELECTION : PAIR_SURFACE) | A_BOLD);
		mvwprintw(win, y, 2, " %s ", row->label);
		return;
	}
	if (row->line_len > 0 && (ai_view_block_get_kind(row->block) == AI_VIEW_BLOCK_TURN ||
		ai_view_block_get_kind(row->block) == AI_VIEW_BLOCK_TEXT))
	{
		wattrset(win, attr_for_tag(ai_view_block_get_kind(row->block) == AI_VIEW_BLOCK_TURN
			? AI_STYLE_TOOL_TARGET : AI_STYLE_DIM));
		mvwaddstr(win, y, 0, g_get_charset(NULL) ? "│" : "|");
	}
	if (app->searching && app->search->len > 0)
	{
		g_autofree gchar *line = g_strndup(text + offset, row->line_len);
		selected |= strstr(line, app->search->str) != NULL;
	}

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
            attr = theme_attr(PAIR_SELECTION) | A_BOLD;
        }

        if (len >= sizeof buf)
        {
            break;
        }

        memcpy(buf, p, len);
        buf[len] = '\0';

        wattrset(win, attr);
		/* Controls in provider output are data, never cursor movement. */
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			mvwaddch(win, y, x, ' ');
		else
			mvwaddstr(win, y, x, buf);

		x += g_unichar_iszerowidth(g_utf8_get_char(p)) ? 0 : (g_unichar_iswide(g_utf8_get_char(p)) ? 2 : 1);
        offset += (guint)len;
    }

    wattrset(win, A_NORMAL);
}

/*
 * Is the harness idle with the ball in the user's court?
 *
 * Derived rather than stored, so there is no third state to keep in sync:
 * a turn starting clears awaiting_since, and anything in the input buffer
 * means the user has already started answering and no longer needs to be
 * told it is their move. A modal prompt --- an approval, a search --- is
 * its own, louder, request for input and owns the frame while it is up.
 */
static gboolean
app_awaiting_user(App *app)
{
	return app->awaiting_since != 0 && app->input->len == 0 &&
		app->approval_prompt == NULL && !app->searching &&
		!ai_conversation_get_busy(app->conversation);
}

/* The bounded half of the hand-off: a few breaths, then the static frame. */
static gboolean
awaiting_pulse_active(App *app, gint64 now)
{
	return !opt_no_animation && app_awaiting_user(app) &&
		now < app->awaiting_since + AWAIT_PULSE_DURATION_US;
}

/*
 * The colour of a frame that is waiting on a person.
 *
 * Green rather than the accent, because the accent is what the frame
 * wears the rest of the time and a state nobody can see is not a state.
 * Without colour the tag contributes no attributes at all, so bold is
 * what carries it --- a monochrome terminal must not silently lose this.
 */
static attr_t
awaiting_border_attr(void)
{
	attr_t attr = attr_for_tag(AI_STYLE_TOOL_OK);

	return theme_colour ? attr : (attr | A_BOLD);
}

/* Activity and short UI accents share a clock. The transcript row cache
 * stays intact, and the final tick restores the static frame before stopping. */
static gboolean
on_spinner_tick(gpointer user_data)
{
    App *app = user_data;
	gint64 now = g_get_monotonic_time();

	if (!opt_no_animation)
		app->spinner_frame = (app->spinner_frame + 1) % G_N_ELEMENTS(SPINNER_FRAMES);

    app_schedule_redraw(app);
	if (!ai_conversation_get_busy(app->conversation) && now >= app->feedback_until &&
		!awaiting_pulse_active(app, now) &&
		(opt_no_animation || now >= app->intro_started + INTRO_DURATION_US))
	{
		app->spinner_id = 0;
		return G_SOURCE_REMOVE;
	}

    return G_SOURCE_CONTINUE;
}

/*
 * Run the clock while a turn or a short feedback/intro accent is active.
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
	gint64 now = g_get_monotonic_time();
	gboolean active = busy || now < app->feedback_until ||
		awaiting_pulse_active(app, now) ||
		(!opt_no_animation && now < app->intro_started + INTRO_DURATION_US);

    if (active && app->spinner_id == 0 && app->running)
    {
        app->spinner_frame = 0;
		/* Reduced motion still updates elapsed time once per second. */
        app->spinner_id = g_timeout_add(opt_no_animation ? 1000 : SPINNER_INTERVAL_MS, on_spinner_tick,
                                        app);
    }
    else if (!active && app->spinner_id != 0)
    {
        g_source_remove(app->spinner_id);
        app->spinner_id = 0;
    }
}

/* Feedback belongs to the chrome, not the conversation or model context. */
static void
ui_feedback(App *app, const gchar *text, AiStyleTag style)
{
	if (!app->running) return;
	g_free(app->feedback);
	app->feedback = g_strdup(text);
	app->feedback_style = style;
	app->feedback_until = g_get_monotonic_time() + FEEDBACK_DURATION_US;
	sync_spinner(app);
	app_schedule_redraw(app);
}

/* Cancellation is an intentional stop, not a failed operation. */
static void
ui_turn_finished(App *app, const GError *error)
{
	/* However a turn ended --- answered, stopped, or failed --- nothing
	 * further happens until the user says so, so all three hand over. */
	app->awaiting_since = g_get_monotonic_time();

	if (error == NULL)
		ui_feedback(app, "Turn complete", AI_STYLE_TOOL_OK);
	else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
		g_error_matches(error, AI_ERROR, AI_ERROR_CANCELLED))
		ui_feedback(app, "Turn stopped", AI_STYLE_STATUS);
	else
		ui_feedback(app, "Turn failed", AI_STYLE_ERROR);
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

	/* A turn owns the frame again; ui_turn_finished() hands it back. */
	if (ai_conversation_get_busy(app->conversation))
		app->awaiting_since = 0;

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
                                opt_no_animation ? "*" : SPINNER_FRAMES[app->spinner_frame],
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
                                   : app_awaiting_user(app)
                                       ? "your turn · type to reply"
                                       : "ready",
                                app->follow ? "" : "   [scrolled]");
    }
	if (app->feedback != NULL && app->interrupt_id == 0 &&
		!ai_conversation_get_busy(app->conversation) &&
		g_get_monotonic_time() < app->feedback_until)
	{
		g_free(line);
		line = g_strdup_printf(" %s  |  %s%s", app->feedback,
			model != NULL ? model : "ready", app->follow ? "" : "   [scrolled]");
	}

	wbkgd(app->status_win, ' ' | theme_attr(PAIR_SURFACE));
	werase(app->status_win);
	{
		g_autofree gchar *fitted = fit_to_width(line, width - 1);
		wattrset(app->status_win, theme_attr(PAIR_SURFACE));
		mvwaddstr(app->status_win, 0, 0, fitted);
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
	gint first_row;

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

        if (c == '\n')
        {
			pen_note_cursor(pen);
            pen->row++;
            pen->col = 0;
            pen->consumed += (gsize)(next - p);
            p = next;
            continue;
        }

        /* Terminal columns, not characters --- the same rule the view
         * layer wraps by. */
        w = g_unichar_iszerowidth(c) ? 0 : (g_unichar_iswide(c) ? 2 : 1);

        if (pen->col + w > pen->width && pen->width > 0)
        {
            pen->row++;
            pen->col = 0;
        }

		pen_note_cursor(pen);
        if (pen->win != NULL && pen->row >= pen->first_row && pen->row < pen->first_row + pen->max_rows)
        {
            wattrset(pen->win, attr);
			if (g_unichar_iscntrl(c))
				mvwaddch(pen->win, pen->row - pen->first_row + 1, pen->col + INPUT_GUTTER, ' ');
			else
				mvwaddnstr(pen->win, pen->row - pen->first_row + 1, pen->col + INPUT_GUTTER, p,
				           (gint)(next - p));
        }

        pen->col += w;
        pen->consumed += (gsize)(next - p);
        p = next;
    }

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
	/* Only the end of the whole input is an end position. A style boundary
	 * may wrap before the next character, so it must not fix the cursor yet. */
	pen_note_cursor(pen);
}

/* How many rows the input needs at WIDTH. */
static gint
input_rows_for(App *app, gint width)
{
    InputPen pen = { 0 };

    pen.width = MAX(1, width - INPUT_GUTTER - 2);
    pen.max_rows = G_MAXINT;
    pen.cursor = app->cursor;

    input_walk(app, &pen);

    return CLAMP(pen.row + 1, 1, INPUT_MAX_ROWS);
}

/* Explicit UTF-8 borders avoid terminfo ACS mappings turning into q/x
 * under terminal multiplexers. Non-UTF-8 locales get plain ASCII. */
static void
draw_frame(WINDOW *win)
{
	gint height = getmaxy(win), width = getmaxx(win), i;
	gboolean unicode = g_get_charset(NULL);
	const gchar *vertical = unicode ? "│" : "|";
	const gchar *horizontal = unicode ? "─" : "-";

	for (i = 1; i < width - 1; i++)
	{
		mvwaddstr(win, 0, i, horizontal);
		mvwaddstr(win, height - 1, i, horizontal);
	}
	for (i = 1; i < height - 1; i++)
	{
		mvwaddstr(win, i, 0, vertical);
		mvwaddstr(win, i, width - 1, vertical);
	}
	mvwaddstr(win, 0, 0, unicode ? "╭" : "+");
	mvwaddstr(win, 0, width - 1, unicode ? "╮" : "+");
	mvwaddstr(win, height - 1, 0, unicode ? "╰" : "+");
	mvwaddstr(win, height - 1, width - 1, unicode ? "╯" : "+");
}

/* A small two-tone runner lives on a border, never on readable content.
 * Geometry and colors stay fixed; no palette mutation or layout animation. */
static void
draw_sweep(App *app, WINDOW *win, gint y, gint start, gint length)
{
	gint64 now = g_get_monotonic_time();
	gint travel = length - 5;
	gint position, i;
	gboolean intro = now < app->intro_started + INTRO_DURATION_US;

	if (opt_no_animation || travel < 1 || app->approval_prompt != NULL ||
		app->searching || (!intro && !ai_conversation_get_busy(app->conversation)))
		return;
	if (intro)
		position = (gint)((now - app->intro_started) * travel / INTRO_DURATION_US);
	else
	{
		position = (gint)((now / (SPINNER_INTERVAL_MS * G_TIME_SPAN_MILLISECOND)) % (2 * travel));
		if (position > travel) position = 2 * travel - position;
	}
	for (i = 0; i < 5; i++)
	{
		wattrset(win, i == 2 ? theme_attr(PAIR_ACCENT) | A_BOLD : attr_for_tag(AI_STYLE_TOOL_TARGET));
		mvwaddstr(win, y, start + position + i, g_get_charset(NULL) ? "━" : "=");
	}
}

static void
draw_input(App *app)
{
    gint     width = getmaxx(app->input_win);
    gint     rows = getmaxy(app->input_win) - 2;
    InputPen pen = { 0 };
	gboolean busy = ai_conversation_get_busy(app->conversation);
	gint64   now = g_get_monotonic_time();
	gboolean awaiting = app_awaiting_user(app);
	attr_t border = theme_attr(PAIR_ACCENT);

	if (!busy && app->feedback != NULL && now < app->feedback_until)
		border = attr_for_tag(app->feedback_style);
	else if (awaiting)
		border = awaiting_border_attr();

	/*
	 * The breath: the whole frame alternates weight on a slow beat.
	 *
	 * Weight and not geometry or colour --- the frame stays exactly where
	 * it was and stays the colour it settles on, so the motion reads as
	 * one thing asking for attention rather than as the layout moving
	 * under a cursor somebody is about to type at.
	 */
	if (awaiting_pulse_active(app, now) &&
		((now - app->awaiting_since) / AWAIT_PULSE_STEP_US) % 2 == 0)
		border |= A_BOLD;

	wbkgd(app->input_win, ' ' | attr_for_tag(AI_STYLE_DEFAULT));
    werase(app->input_win);
	wattrset(app->input_win, border);
	draw_frame(app->input_win);
	{
		const gchar *label = busy ? " DRAFT / waiting for turn "
			: awaiting ? " YOUR TURN " : " COMPOSE ";
		g_autofree gchar *title = fit_to_width(label, width - 4);
		mvwaddstr(app->input_win, 0, 2, title);
	}
	draw_sweep(app, app->input_win, 0, busy ? 30 : 13, width - (busy ? 32 : 15));
	wattrset(app->input_win, border);
	pen.width = MAX(1, width - INPUT_GUTTER - 2);
    pen.max_rows = rows;
    pen.cursor = app->cursor;
    input_walk(app, &pen);
	app->input_first = CLAMP(app->input_first,
		MAX(0, pen.cursor_row - rows + 1), pen.cursor_row);
	{
		g_autofree gchar *position = g_strdup_printf(" %d/%d ", pen.cursor_row + 1, pen.row + 1);
		if (width > 40) mvwaddstr(app->input_win, getmaxy(app->input_win) - 1,
			width - (gint)strlen(position) - 2, position);
	}
	/* Keep permissions on the stationary lower border, clear of the
	 * activity sweep and row counter. Narrow panes retain the full mode. */
	{
		g_autofree gchar *mode = g_strdup_printf(" %s%s ",
			app->skip_permissions ? "skip-permissions" : "read-only",
			width >= 54 ? " | S-TAB" : "");
		wattrset(app->input_win, attr_for_tag(app->skip_permissions
			? AI_STYLE_TOOL_PENDING : AI_STYLE_TOOL_OK) | A_BOLD);
		mvwaddstr(app->input_win, getmaxy(app->input_win) - 1, 2, mode);
	}
	memset(&pen, 0, sizeof pen);
	pen.win = app->input_win;
	pen.width = MAX(1, width - INPUT_GUTTER - 2);
	pen.max_rows = rows;
	pen.cursor = app->cursor;
	pen.first_row = app->input_first;
	input_walk(app, &pen);
	if (app->input->len == 0)
	{
		g_autofree gchar *hint = fit_to_width(awaiting
			? "Your turn · type a reply, or / commands  @ files"
			: "Ask, build, investigate...  / commands  @ files", width - 5);
		wattrset(app->input_win, attr_for_tag(AI_STYLE_DIM));
		mvwaddstr(app->input_win, 1, 2, hint);
	}
    wmove(app->input_win,
          CLAMP(pen.cursor_row - pen.first_row, 0, rows - 1) + 1,
          MIN(pen.cursor_col + INPUT_GUTTER, width - 1));
    wnoutrefresh(app->input_win);
}

/*
 * Give the input the rows it needs and let the transcript have the rest.
 *
 * Called before every draw rather than only on resize, because the input
 * changes height as it is typed into.
 */
static gboolean
place_window(WINDOW *win, gint y, gint x, gint height, gint width)
{
	if (getbegy(win) == y && getbegx(win) == x &&
		getmaxy(win) == height && getmaxx(win) == width)
		return TRUE;
	/* Free both dimensions before moving after a simultaneous shrink. */
	return wresize(win, 1, 1) != ERR && mvwin(win, y, x) != ERR &&
		wresize(win, height, width) != ERR;
}

static void
app_layout(App *app)
{
    gint height;
    gint width;
    gint rows;

    getmaxyx(stdscr, height, width);
	app->tiny = height < 9 || width < 32;
	app->content_width = width - (app->details && width >= 110 && height >= 20 ? 32 : 0);
	if (app->tiny)
	{
		place_window(app->input_win, 0, 0, 1, 1);
		place_window(app->transcript_win, 0, 0, 1, 1);
		place_window(app->status_win, 0, 0, 1, 1);
		return;
	}
	rows = MIN(input_rows_for(app, app->content_width - 2), MAX(1, height - 8)) + 2;
	if (app->searching || app->approval_prompt != NULL) rows = 3;
	if (!place_window(app->transcript_win, 2, 1, height - rows - 4, app->content_width - 2) ||
		!place_window(app->input_win, height - rows - 1, 1, rows, app->content_width - 2) ||
		!place_window(app->status_win, height - rows - 2, 0, 1, app->content_width))
		app->tiny = TRUE;
}

/* Clipped, single-line chrome: untrusted names cannot move the terminal
 * cursor or leak into the adjacent panel. */
static void
chrome_text(gint y, gint x, gint width, const gchar *text, attr_t attr)
{
	g_autofree gchar *fitted = NULL;
	if (y < 0 || y >= LINES || x < 0 || x >= COLS || width <= 0) return;
	fitted = fit_to_width(text, MIN(width, COLS - x - 1));
	attrset(attr);
	mvaddstr(y, x, fitted);
}

/* The inspector is a projection of live library state, not a second
 * session model. Missing data is named rather than shown as invented zeroes. */
static void
draw_chrome(App *app)
{
	GObject *provider = ai_conversation_get_provider(app->conversation);
	const gchar *model = AI_IS_CLIENT(provider) ? ai_client_get_model(AI_CLIENT(provider)) :
		AI_IS_CLI_CLIENT(provider) ? ai_cli_client_get_model(AI_CLI_CLIENT(provider)) : NULL;
	const gchar *identity = ai_provider_get_name(AI_PROVIDER(provider));
	gint x = app->content_width + 2, y = 3;
	chrome_text(0, 1, 5, " ai ", theme_attr(PAIR_SELECTION) | A_BOLD);
	chrome_text(0, 7, app->content_width - 9, identity, theme_attr(PAIR_ACCENT) | A_BOLD);
	if (COLS >= 65)
		chrome_text(0, COLS - (gint)strlen(THEMES[theme_index].name) - 3,
			(gint)strlen(THEMES[theme_index].name) + 1, THEMES[theme_index].name,
			theme_attr(PAIR_ACCENT));
	chrome_text(1, 2, app->content_width - 3, ai_conversation_get_working_directory(app->conversation), attr_for_tag(AI_STYLE_DIM));
	chrome_text(LINES - 1, 1, COLS - 2,
		app->searching ? "RET next | <up> / S-RET previous | C-u clear | ESC close" :
		app->candidates != NULL ? "TAB / arrows choose | RET accept | ESC dismiss | C-o help" :
		COLS < 65 ? "RET send  C-o help  C-c stop" :
		COLS < 100 ? "RET send | C-f find | C-o help | C-t theme | C-p panel" :
		"RET send  M-RET newline  C-g editor  C-f search  C-o help  C-t theme  C-p panel  C-l latest",
		attr_for_tag(AI_STYLE_DIM));
	if (app->content_width == COLS) return;
	attrset(theme_attr(PAIR_SURFACE));
	for (y = 2; y < LINES - 1; y++) mvhline(y, app->content_width, ' ', COLS - app->content_width);
	y = 3;
	y = panel_text(y, x, 28, "SESSION", theme_attr(PAIR_PANEL_ACCENT) | A_BOLD, FALSE);
	y = panel_text(y, x, 28, ai_provider_get_name(AI_PROVIDER(provider)), theme_attr(PAIR_SURFACE), FALSE);
	y = panel_text(y, x, 28, model != NULL ? model : "Provider default model", theme_attr(PAIR_SURFACE) | A_BOLD, FALSE);
	y = panel_text(y + 1, x, 28, "APPEARANCE", theme_attr(PAIR_PANEL_ACCENT) | A_BOLD, FALSE);
	y = panel_text(y, x, 28, THEMES[theme_index].name, theme_attr(PAIR_SURFACE), FALSE);
	y = panel_text(y, x, 28, theme_colour ? "^T cycle / ^P hide" : "No color / ^P hide", theme_attr(PAIR_SURFACE), FALSE);
	y = panel_text(y + 1, x, 28, "LATEST REPORTED USAGE", theme_attr(PAIR_PANEL_ACCENT) | A_BOLD, FALSE);
	{
		AiTranscript *transcript = ai_conversation_get_transcript(app->conversation);
		gint i;
		const gchar *usage = "Not reported yet";
		for (i = (gint)ai_transcript_get_n_blocks(transcript) - 1; i >= 0; i--)
		{
			AiViewBlock *block = ai_transcript_get_block(transcript, (guint)i);
			if (AI_IS_VIEW_STATUS_BLOCK(block) && ai_view_status_block_get_status_kind(AI_VIEW_STATUS_BLOCK(block)) == AI_VIEW_STATUS_USAGE)
			{
				usage = ai_view_status_block_get_text(AI_VIEW_STATUS_BLOCK(block));
				break;
			}
		}
		y = panel_text(y, x, 28, usage, theme_attr(PAIR_SURFACE), FALSE);
	}
	{
		AiToolExecutor *exec = ai_conversation_get_executor(app->conversation);
		guint n = ai_tool_executor_get_n_todos(exec), i;
		AiBrigade *brigade = ai_conversation_get_brigade(app->conversation);
		g_autoptr(GList) agents = brigade != NULL ? ai_brigade_list(brigade) : NULL;
		g_autofree gchar *todo_heading = g_strdup_printf("TODOS / %u", n);
		g_autofree gchar *agent_heading = g_strdup_printf("AGENTS / %u", g_list_length(agents));
		GList *iter;
		y = panel_text(y + 1, x, 28, todo_heading, theme_attr(PAIR_PANEL_ACCENT) | A_BOLD, FALSE);
		if (n == 0) y = panel_text(y, x, 28, "No todos yet /todos", theme_attr(PAIR_SURFACE), FALSE);
		for (i = 0; i < n && i < 3 && y < LINES - 5; i++)
		{
			const gchar *label = NULL;
			AiTodoState state;
			g_autofree gchar *line = NULL;
			ai_tool_executor_get_todo_fields(exec, i, &label, &state);
			line = g_strdup_printf("%s: %s", ai_todo_state_to_string(state), label);
			y = panel_text(y, x, 28, line, theme_attr(PAIR_SURFACE), TRUE);
		}
		y = panel_text(y + 1, x, 28, agent_heading, theme_attr(PAIR_PANEL_ACCENT) | A_BOLD, FALSE);
		if (agents == NULL) y = panel_text(y, x, 28, brigade == NULL ? "Disabled (--no-agents)" : "No agents running /running", theme_attr(PAIR_SURFACE), FALSE);
		for (iter = agents; iter != NULL && y < LINES - 3; iter = iter->next)
		{
			g_autofree gchar *line = g_strdup_printf("%s: %s", ai_agent_get_id(iter->data), ai_agent_state_to_string(ai_agent_get_state(iter->data)));
			y = panel_text(y, x, 28, line, theme_attr(PAIR_SURFACE), TRUE);
		}
		if (y < LINES - 6)
		{
			gboolean local = ai_conversation_get_local_tools(app->conversation);
			g_autofree gchar *tools = local
				? g_strdup_printf("%u local tools /tools", g_list_length(ai_tool_executor_get_tools(exec)))
				: g_strdup(AI_IS_CLI_CLIENT(provider) ? "Managed by provider CLI" : "Local tools disabled");
			y = panel_text(y + 1, x, 28, "TOOLS", theme_attr(PAIR_PANEL_ACCENT) | A_BOLD, FALSE);
			y = panel_text(y, x, 28, tools, theme_attr(PAIR_SURFACE), FALSE);
			if (local)
				y = panel_text(y, x, 28, app->approve_all || app->skip_permissions ? "Approval: automatic" : "Approval: ask before running", theme_attr(PAIR_SURFACE), FALSE);
		}
	}
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
	bkgd(' ' | attr_for_tag(AI_STYLE_DEFAULT));
	erase();
	if (app->tiny)
	{
		chrome_text(0, 0, COLS, app->approval_prompt != NULL ? "Approval pending: y/n/a/d" : "ai-tui: resize to 32x9", A_BOLD);
		if (LINES > 1) chrome_text(1, 0, COLS, "Draft preserved. ^C ^C quit", A_NORMAL);
		refresh();
		return;
	}
	draw_chrome(app);
	wnoutrefresh(stdscr);

    height = getmaxy(app->transcript_win);
    width = getmaxx(app->transcript_win);

    if (width <= 0 || height <= 0)
    {
        return;
    }

    rows = build_rows(app, MAX(1, width - 3));
	app->row_count = (gint)rows->len;
	app->search_matches = 0;
	if (app->searching && app->search->len > 0)
	{
		for (i = 0; i < (gint)rows->len; i++)
		{
			Row *row = g_ptr_array_index(rows, i);
			g_autofree gchar *text = g_strndup(ai_rendered_text_get_text(row->rendered) + row->line_start, row->line_len);
			if (strstr(text, app->search->str) != NULL) app->search_matches++;
		}
	}

    if (app->follow)
    {
        app->scroll = MAX(0, (gint)rows->len - height);
    }

    app->scroll = CLAMP(app->scroll, 0, MAX(0, (gint)rows->len - height));
    first = app->scroll;

	wbkgd(app->transcript_win, ' ' | attr_for_tag(AI_STYLE_DEFAULT));
    werase(app->transcript_win);
	if (rows->len == 0)
	{
		static const gchar *welcome[] = {
			"MAKE SOMETHING WORTH SHIPPING.",
			"Your terminal. Your models. One conversation.",
			"", "Start with a question or a concrete task.",
			"/help      Explore commands and key bindings",
			"/model     Inspect or change the model",
			"@path      Bring a file into the conversation",
			"/running   Check background agents",
			"", "Ctrl-T change the mood.  Ctrl-G open your editor."
		};
		gboolean logo = width >= 58 && height >= 14;
		gint top = MAX(0, (height - (gint)G_N_ELEMENTS(welcome) - (logo ? 4 : 0)) / 2);
		if (logo)
		{
			wattrset(app->transcript_win, theme_attr(PAIR_ACCENT) | A_BOLD);
			mvwaddstr(app->transcript_win, top, 3, g_get_charset(NULL) ? "▄▀█ █" : " /\\  | ");
			wattrset(app->transcript_win, attr_for_tag(AI_STYLE_TOOL_TARGET) | A_BOLD);
			mvwaddstr(app->transcript_win, top + 1, 3, g_get_charset(NULL) ? "█▀█ █" : "/--\\ | ");
			wattrset(app->transcript_win, theme_attr(PAIR_ACCENT) | A_BOLD);
			mvwaddstr(app->transcript_win, top, 14, "AI / GLIB");
			wattrset(app->transcript_win, attr_for_tag(AI_STYLE_DIM));
			mvwaddstr(app->transcript_win, top + 1, 14, "A workspace for your next idea.");
			top += 4;
		}
		for (i = 0; i < (gint)G_N_ELEMENTS(welcome) && top + i < height; i++)
		{
			g_autofree gchar *text = fit_to_width(welcome[i], width - 5);
			wattrset(app->transcript_win, i == 0 ? theme_attr(PAIR_ACCENT) | A_BOLD : attr_for_tag(i == 1 ? AI_STYLE_DIM : AI_STYLE_DEFAULT));
			mvwaddstr(app->transcript_win, top + i, 3, text);
		}
	}

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
	/* Modal layers are always painted last, including timer-driven redraws. */
	if (app->searching || app->approval_prompt != NULL)
	{
		const gchar *query = app->search->str;
		gint room = getmaxx(app->input_win) - 11;
		g_autofree gchar *line = NULL;
		g_autofree gchar *fitted = NULL;
		/* An append-only search editor follows its tail without hiding the
		 * result count. The complete query remains available for matching. */
		while (*query && (gint)ai_style_text_width(query) > room) query = g_utf8_next_char(query);
		line = app->approval_prompt != NULL ? g_strdup(app->approval_prompt) : g_strdup_printf("Find: %s", query);
		fitted = fit_to_width(line, getmaxx(app->input_win) - 4);
		werase(app->input_win);
		wattrset(app->input_win, theme_attr(PAIR_ACCENT) | A_BOLD);
		draw_frame(app->input_win);
		mvwaddnstr(app->input_win, 0, 2, app->approval_prompt != NULL ? " TOOL APPROVAL " : " SEARCH / case sensitive ", getmaxx(app->input_win) - 4);
		mvwaddstr(app->input_win, 1, 2, fitted);
		if (app->approval_prompt != NULL)
			mvwaddnstr(app->input_win, getmaxy(app->input_win) - 1, 2, " y yes | n no | a always | d deny all ", getmaxx(app->input_win) - 4);
		else
		{
			g_autofree gchar *count = app->search->len == 0 ? g_strdup(" Type to search ") :
				app->search_matches == 0 ? g_strdup(" No matches ") :
				g_strdup_printf(" %u matching rows ", app->search_matches);
			mvwaddnstr(app->input_win, getmaxy(app->input_win) - 1, 2, count, getmaxx(app->input_win) - 4);
		}
		wnoutrefresh(app->input_win);
	}
    doupdate();
}

static gboolean
on_redraw_idle(gpointer user_data)
{
    App *app = user_data;

    app->redraw_id = 0;
    if (app->running) app_redraw(app);

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

/**
 * input_delete:
 * @app: the active composer
 *
 * Delete one complete Unicode character after the cursor. At the end of
 * the draft this is a no-op, including when the draft is empty.
 */
static void
input_delete(App *app)
{
	const gchar *start;
	const gchar *next;

	if (app->cursor >= app->input->len)
		return;
	start = app->input->str + app->cursor;
	next = g_utf8_next_char(start);
	g_string_erase(app->input, (gssize)app->cursor, (gssize)(next - start));
}

/**
 * input_kill:
 * @app: the active composer
 * @word: whether to kill the preceding word instead of the line suffix
 *
 * Keep a single session-local kill buffer. Word deletion consumes trailing
 * Unicode whitespace then non-whitespace characters. Line deletion stops
 * before a newline, or consumes that newline when already at line end.
 * Empty kills preserve the previous buffer so a boundary key cannot lose it.
 */
static void
input_kill(App *app, gboolean word)
{
	const gchar *text = app->input->str;
	gsize start = app->cursor;
	gsize end = app->cursor;
	const gchar *previous;

	if (word)
	{
		while (start > 0)
		{
			previous = g_utf8_prev_char(text + start);
			if (!g_unichar_isspace(g_utf8_get_char(previous)))
				break;
			start = (gsize)(previous - text);
		}
		while (start > 0)
		{
			previous = g_utf8_prev_char(text + start);
			if (g_unichar_isspace(g_utf8_get_char(previous)))
				break;
			start = (gsize)(previous - text);
		}
	}
	else if (text[end] == '\n')
		end++;
	else
	{
		while (text[end] != '\0' && text[end] != '\n')
			end++;
	}

	if (start == end)
		return;
	g_free(app->kill_buffer);
	app->kill_buffer = g_strndup(text + start, end - start);
	g_string_erase(app->input, (gssize)start, (gssize)(end - start));
	app->cursor = (guint)start;
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
		if (direction > 0) return;
		g_free(app->history_draft);
		app->history_draft = g_strdup(app->input->str);
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
        g_string_assign(app->input, app->history_draft != NULL ? app->history_draft : "");
        app->cursor = (guint)app->input->len;
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
					"  ^O help / ^T cycle theme / ^P panel / ^L latest\n"
					"  Shift-Tab    toggle read-only / skip-permissions\n"
					"  ^F search transcript (case-sensitive matching rows)\n"
					"     Enter next, Up or Shift-Enter previous, Esc close\n"
					"  PgUp/PgDn    scroll transcript incrementally\n"
                    "  Enter        send\n"
                    "  Alt-Enter    a new line (Shift-Enter too, where the\n"
                    "               terminal encodes it distinctly)\n"
                    "  ^G           edit the prompt in $EDITOR\n"
					"  Delete       delete the next Unicode character\n"
					"  ^W / ^K      kill previous word / to line end\n"
					"  ^Y           yank the last killed text\n"
                    "  ^C           stop the turn, then clear the line,\n"
                    "               then quit on a second press\n"
                    "  ^D           quit, on an empty line\n"
                    "  Tab          complete /command or @path\n"
                    "  ^N           cycle tool and thinking blocks\n"
                    "  ^B           expand or collapse the selected block\n"
                    "  ^U           clear the line\n");

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

/**
 * resolve_command_path:
 * @app: the active conversation
 * @path: a literal command path
 *
 * Resolve paths without changing the process directory, which may be in
 * use by background work. Only a leading ~/ is expanded; no shell syntax
 * is evaluated and spaces remain part of the filename.
 *
 * Returns: (transfer full): the canonical absolute path
 */
static gchar *
resolve_command_path(App *app, const gchar *path)
{
	g_autofree gchar *expanded = NULL;

	if (g_str_equal(path, "~"))
		expanded = g_strdup(g_get_home_dir());
	else if (g_str_has_prefix(path, "~/"))
		expanded = g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_canonicalize_filename(expanded != NULL ? expanded : path,
		ai_conversation_get_working_directory(app->conversation));
}

static void
save_transcript(App *app, const gchar *path)
{
    g_autofree gchar *text = NULL;
    g_autofree gchar *resolved = NULL;
    g_autoptr(GError) error = NULL;

    if (path == NULL || path[0] == '\0')
    {
        say(app, "/save needs a path.");
        return;
    }

    text = ai_transcript_to_text(
        ai_conversation_get_transcript(app->conversation), 0);

    resolved = resolve_command_path(app, path);
    if (!g_file_set_contents(resolved, text, -1, &error))
    {
        say(app, "Could not write %s: %s", path, error->message);
        return;
    }

    say(app, "Wrote %s", resolved);
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
    g_autofree gchar *resolved = NULL;
    g_autoptr(GError) error = NULL;
    AiExportFormat format;
    const gchar *path;

    if (arguments == NULL || arguments[0] == '\0')
    {
        say(app, "/export needs a format: text, markdown or org.");
        return;
    }

    parts = g_strsplit_set(arguments, " \t\r\n", 2);

    if (!ai_export_format_from_string(parts[0], &format))
    {
        say(app, "Unknown format '%s'. Use text, markdown or org.",
            parts[0]);
        return;
    }

    path = parts[1] != NULL ? g_strchug(parts[1]) : NULL;
    if (path != NULL && path[0] == '\0')
        path = NULL;

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

    resolved = resolve_command_path(app, path);
    if (!g_file_set_contents(resolved, text, -1, &error))
    {
        say(app, "Could not write %s: %s", path, error->message);
        return;
    }

    say(app, "Wrote %s", resolved);
}

static void
change_directory(App *app, const gchar *path)
{
    g_autofree gchar *resolved = NULL;

    if (path == NULL || path[0] == '\0')
    {
        say(app, "%s", ai_conversation_get_working_directory(app->conversation));
        return;
    }

    resolved = resolve_command_path(app, path);
    if (!g_file_test(resolved, G_FILE_TEST_IS_DIR))
    {
        say(app, "No such directory: %s", path);
        return;
    }

    ai_conversation_set_working_directory(app->conversation, resolved);

    if (app->completion != NULL)
    {
        ai_completion_context_set_working_directory(app->completion, resolved);
    }

    say(app, "Working directory: %s", resolved);
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
static void app_reset(App *app);

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
    else if (g_strcmp0(name, "reset") == 0)
    {
        app_reset(app);
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
        GObject          *provider =
            ai_conversation_get_provider(app->conversation);
        const gchar      *current = AI_IS_CLIENT(provider)
            ? ai_client_get_model(AI_CLIENT(provider))
            : ai_cli_client_get_model(AI_CLI_CLIENT(provider));
        g_autofree gchar *requested = NULL;
        g_autofree gchar *previous = g_strdup(current);

        if (arguments != NULL)
        {
            requested = g_strdup(arguments);
            g_strstrip(requested);
        }

        if (requested == NULL || requested[0] == '\0')
        {
            say(app, "Model: %s", current != NULL ? current : "(default)");
        }
        else if (ai_conversation_get_busy(app->conversation))
        {
            /*
             * Same rule as /provider, and for the same reason: half a
             * turn answered by one model and half by another is not a
             * transcript anybody can reason about afterwards.
             */
            say(app, "Model unchanged: a turn is in flight.");
        }
        else if (g_strcmp0(previous, requested) == 0)
        {
            say(app, "Model: %s", requested);
        }
        else
        {
            if (AI_IS_CLIENT(provider))
            {
                ai_client_set_model(AI_CLIENT(provider), requested);
            }
            else
            {
                ai_cli_client_set_model(AI_CLI_CLIENT(provider), requested);
            }

            /*
             * Say it out loud.  A model id that the provider will reject
             * is not detectable here --- no wrapped CLI offers a
             * validating lookup --- so the error arrives on the next
             * turn, and by then the only thing that makes it explicable
             * is having seen the change reported.
             */
            say(app, "Model switched from %s to %s. Context preserved.",
                previous != NULL ? previous : "the default", requested);
        }
    }
    else if (g_strcmp0(name, "context") == 0)
    {
        const gchar      *carried =
            ai_conversation_get_carried_context(app->conversation);
        g_autofree gchar *what = NULL;

        if (arguments != NULL)
        {
            what = g_strdup(arguments);
            g_strstrip(what);
        }

        if (what != NULL && g_strcmp0(what, "clear") == 0)
        {
            if (carried == NULL)
            {
                say(app, "No carried context to drop.");
            }
            else
            {
                ai_conversation_clear_carried_context(app->conversation);
                say(app, "Carried context dropped. Later turns send only "
                    "this session's own history.");
            }
        }
        else if (what != NULL && what[0] != '\0')
        {
            say(app, "/context takes no argument, or `clear`.");
        }
        else if (carried == NULL)
        {
            say(app, "No context carried from another provider.");
        }
        else
        {
            /*
             * The size, not the text.  Printing tens of kilobytes of
             * carried transcript into the transcript that carried it is
             * how a reader loses the thread entirely; the count is what
             * answers "why is my prompt so large".
             */
            say(app, "Carrying %.1f KiB of history from a previous "
                "provider, sent with every turn. /context clear drops it.",
                strlen(carried) / 1024.0);
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
            /* Snapshotted before the switch: the harvest appends to it,
             * so the difference is what this switch actually carried. */
            g_autofree gchar *had_context = g_strdup(
                ai_conversation_get_carried_context(app->conversation));

            g_strstrip(requested);
            replacement = build_provider_named(requested, FALSE, &error);

            if (replacement == NULL
                || !(app->mcp_host != NULL
                    ? ai_mcp_host_set_provider(app->mcp_host, replacement, mcp_executable, !opt_mcp_no_inject, &error)
                    : ai_conversation_set_provider(app->conversation, replacement, &error)))
            {
                say(app, "Provider unchanged: %s",
                    error != NULL ? error->message : "switch failed");
            }
            else
            {
                const gchar *carried = ai_conversation_get_carried_context(
                    app->conversation);

                say(app, "Provider switched to %s. Context preserved.",
                    ai_provider_get_name(AI_PROVIDER(replacement)));

                /*
                 * Say when the wrapped program's own history came along.
                 * That import is the difference between the new provider
                 * knowing what the last one did and guessing, and it is
                 * invisible otherwise --- it lands in the system prompt,
                 * which the transcript never shows.
                 */
                if (carried != NULL && g_strcmp0(carried, had_context) != 0)
                {
                    gsize grew = strlen(carried)
                        - (had_context != NULL ? strlen(had_context) : 0);

                    say(app, "Carried %.1f KiB of that session's own "
                        "history, including its tool calls.", grew / 1024.0);
                }

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
	fputs("\033[?2004l", stdout);
    fflush(stdout);
    endwin();

    if (!g_spawn_sync(NULL, (gchar **)argv->pdata, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                      NULL, NULL, NULL, NULL, &status, &error))
    {
        reset_prog_mode();
		fputs("\033[?2004h", stdout);
		fflush(stdout);
		on_resize(app);
        clearok(curscr, TRUE);
        app_schedule_redraw(app);

        say(app, "Cannot run %s: %s", editor[0], error->message);
        g_unlink(path);
        return;
    }

    reset_prog_mode();
	fputs("\033[?2004h", stdout);
	fflush(stdout);
	on_resize(app);

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
	if (!g_utf8_validate(edited, -1, NULL))
	{
		say(app, "Editor returned invalid UTF-8; draft preserved.");
		app_schedule_redraw(app);
		return;
	}

    /*
     * One trailing newline is an artefact of every editor that ends a
     * file properly, not something the user typed. Interior blank lines
     * are left exactly as written.
     */
    {
        gsize len = strlen(edited);

        if (len > 0 && edited[len - 1] == '\n')
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
	app->pasting = FALSE;
	if (app->approval_prompt != NULL)
	{
		app->approval_answer = AI_TOOL_APPROVAL_DENY_ALL;
		return FALSE;
	}
	if (app->searching)
	{
		app->searching = FALSE;
		return FALSE;
	}
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

/* Search operates on rendered rows, so navigation and highlighting agree
 * with wrapping and expanded blocks. The draft never participates. */
static void
search_step(App *app, gint direction)
{
	g_autoptr(GPtrArray) rows = build_rows(app, MAX(1, getmaxx(app->transcript_win) - 3));
	gint n = (gint)rows->len, i;
	if (n == 0 || app->search->len == 0) return;
	for (i = 1; i <= n; i++)
	{
		gint at = (gint)(((gint64)app->search_row + (gint64)direction * i) % n + n) % n;
		Row *row = g_ptr_array_index(rows, at);
		g_autofree gchar *text = g_strndup(ai_rendered_text_get_text(row->rendered) + row->line_start, row->line_len);
		if (strstr(text, app->search->str) != NULL)
		{
			app->search_row = at;
			app->scroll = at;
			app->follow = FALSE;
			return;
		}
	}
}

static gboolean
drain_keys(App *app)
{
    gint ch;
	wint_t value;
	gint kind;

    while ((kind = wget_wch(app->input_win, &value)) != ERR)
    {
		gchar utf8[8] = { 0 };
		ch = (gint)value;
		/* A bracketed paste is an edit, never a sequence of commands. In
		 * particular its newlines must not submit half a prompt. */
		if (kind == KEY_CODE_YES && ch == KEY_PASTE_START)
		{
			app->pasting = TRUE;
			completion_close(app);
			continue;
		}
		if (app->pasting)
		{
			if (kind == KEY_CODE_YES && ch == KEY_PASTE_END)
			{
				app->pasting = FALSE;
				app->completion_dismissed = TRUE;
			}
			else if (kind == OK && g_unichar_validate((gunichar)value) &&
				(!g_unichar_iscntrl((gunichar)value) || ch == '\n' || ch == '\r' || ch == '\t'))
			{
				g_unichar_to_utf8(ch == '\r' ? '\n' : (gunichar)value, utf8);
				/* Ignore pasted approval answers; require a deliberate key. */
				if (app->approval_prompt == NULL && !app->searching && !app->tiny)
					input_insert(app, utf8);
			}
			app_schedule_redraw(app);
			continue;
		}
		/* Unicode scalar values overlap KEY_* numerically. Only ncurses'
		 * KEY_CODE_YES result authorizes interpreting one as a function key. */
		if (kind == OK && g_unichar_validate((gunichar)value) && !g_unichar_iscntrl((gunichar)value))
			g_unichar_to_utf8((gunichar)value, utf8);
		if (app->approval_prompt != NULL)
		{
			if (kind == OK) switch (ch) {
			case 'y': case 'Y': app->approval_answer = AI_TOOL_APPROVAL_ALLOW; break;
			case 'n': case 'N': case 27: app->approval_answer = AI_TOOL_APPROVAL_DENY; break;
			case 'a': case 'A': app->approval_answer = AI_TOOL_APPROVAL_ALLOW_ALWAYS; break;
			case 'd': case 'D': case 3: app->approval_answer = AI_TOOL_APPROVAL_DENY_ALL; break;
			default: break;
			}
			app_schedule_redraw(app);
			if (app->approval_answer != AI_TOOL_APPROVAL_DEFAULT) return G_SOURCE_CONTINUE;
			continue;
		}
		/* Do not submit or edit a draft while it cannot be seen. */
		if (app->tiny && !((kind == OK && (ch == 3 || ch == 4)) ||
			(kind == KEY_CODE_YES && ch == KEY_RESIZE))) continue;
		if (app->searching && !((kind == KEY_CODE_YES && ch == KEY_RESIZE) ||
			(kind == OK && (ch == 12 || ch == 15 || ch == 16 || ch == 20))))
		{
			if (kind == OK && (ch == 27 || ch == 6 || ch == 3)) app->searching = FALSE;
			else if ((kind == KEY_CODE_YES && (ch == KEY_UP || ch == KEY_SHIFT_ENTER || ch == KEY_LEFT))) search_step(app, -1);
			else if ((kind == OK && (ch == '\n' || ch == '\r')) || (kind == KEY_CODE_YES && (ch == KEY_ENTER || ch == KEY_DOWN || ch == KEY_RIGHT))) search_step(app, 1);
			else
			{
				if ((kind == OK && (ch == 127 || ch == 8)) || (kind == KEY_CODE_YES && ch == KEY_BACKSPACE))
				{
					if (app->search->len) g_string_truncate(app->search, (gsize)(g_utf8_prev_char(app->search->str + app->search->len) - app->search->str));
				}
				else if (kind == OK && ch == 21) g_string_truncate(app->search, 0);
				else if (utf8[0]) g_string_append(app->search, utf8);
				app->search_row = -1;
				search_step(app, 1);
			}
			app_schedule_redraw(app);
			continue;
		}
		if (utf8[0])
		{
			input_insert(app, utf8);
			app->completion_dismissed = FALSE;
			completion_refresh(app);
			app_schedule_redraw(app);
			continue;
		}
		if (kind == OK && ch >= 256) continue;
        switch (ch)
        {
			case KEY_RESIZE:
				clearok(curscr, TRUE);
				break;
			case 15:  /* ^O: open help */
				app->searching = FALSE;
				completion_close(app);
				app->completion_dismissed = TRUE;
				show_help(app);
				app->follow = TRUE;
				break;
			case 20:  /* ^T: theme */
			{
				g_autofree gchar *notice = NULL;
				theme_index = (theme_index + 1) % G_N_ELEMENTS(THEMES);
				theme_explicit = TRUE;
				init_colours();
				clearok(curscr, TRUE);
				notice = g_strdup_printf("Theme: %s", THEMES[theme_index].name);
				ui_feedback(app, notice, AI_STYLE_COMMAND);
				break;
			}
			case 16:  /* ^P: panel */
				app->details = !app->details;
				ui_feedback(app, !app->details ? "Panel hidden" :
					COLS >= 110 && LINES >= 20 ? "Panel shown" : "Panel enabled (widen terminal)", AI_STYLE_COMMAND);
				break;
			case 12:  /* ^L: latest */
				app->searching = FALSE;
				app->follow = TRUE;
				ui_feedback(app, "Following latest output", AI_STYLE_COMMAND);
				break;
			case 6:
				completion_close(app);
				app->searching = TRUE;
				app->search_row = app->scroll - 1;
				search_step(app, 1);
				break;
            case '\n':
            case '\r':
            case KEY_ENTER:
                /* Submit an already complete command on the first Enter.
                 * Partial names and path candidates still need acceptance. */
                if (app->candidates != NULL && !completion_is_exact_command(app))
                {
                    completion_accept(app);
                    break;
                }

                completion_close(app);
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

			case KEY_DC:
				input_delete(app);
				app->completion_dismissed = FALSE;
				completion_refresh(app);
				break;

			case 23:  /* ^W: kill the preceding word */
			case 11:  /* ^K: kill to the end of the logical line */
				input_kill(app, ch == 23);
				app->completion_dismissed = FALSE;
				completion_refresh(app);
				break;

			case 25:  /* ^Y: insert the last killed text literally */
				if (app->kill_buffer != NULL)
					input_insert(app, app->kill_buffer);
				app->completion_dismissed = TRUE;
				completion_close(app);
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
                completion_refresh(app);
                break;

            case KEY_END:
            case 5:   /* ^E */
                app->cursor = (guint)app->input->len;
                completion_refresh(app);
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

                app->scroll += MAX(1, getmaxy(app->transcript_win) / 2);
                app->follow = app->scroll >= MAX(0, app->row_count - getmaxy(app->transcript_win));
                break;

            case '\t':
                completion_advance(app);
                break;

            case KEY_BTAB:  /* Shift-Tab */
            {
				GObject *provider = ai_conversation_get_provider(app->conversation);
				GParamSpec *property = g_object_class_find_property(
					G_OBJECT_GET_CLASS(provider), "skip-permissions");
				gboolean busy = app->sending || ai_conversation_get_busy(app->conversation);
				g_autofree gchar *notice = NULL;

				/* Wrapped tools belong to the child. Changing its property
				 * affects the next invocation, never an existing process. */
				if (AI_IS_CLI_CLIENT(provider) && property == NULL)
				{
					ui_feedback(app, "This provider does not support permission switching", AI_STYLE_ERROR);
					break;
				}
				app->skip_permissions = !app->skip_permissions;
				if (property != NULL)
					g_object_set(provider, "skip-permissions", app->skip_permissions, NULL);
				/* Provider replacement inherits the live choice, not argv's
				 * original value. Session reset reuses the same provider. */
				opt_skip_permissions = app->skip_permissions;
				completion_close(app);
				app->completion_dismissed = TRUE;
				notice = g_strdup_printf("Mode: %s%s",
					app->skip_permissions ? "skip-permissions" : "read-only",
					busy && AI_IS_CLI_CLIENT(provider) ? " (next turn; running tools unchanged)" : "");
				ui_feedback(app, notice, app->skip_permissions
					? AI_STYLE_TOOL_PENDING : AI_STYLE_TOOL_OK);
                break;
            }

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
				wint_t next = 0;
				gint next_kind;
				wtimeout(app->input_win, ESCAPE_SETTLE_MS);
				next_kind = wget_wch(app->input_win, &next);
				nodelay(app->input_win, TRUE);

                if ((next_kind == OK && (next == '\r' || next == '\n')) || (next_kind == KEY_CODE_YES && next == KEY_ENTER))
                {
                    input_insert(app, "\n");
                    app->completion_dismissed = FALSE;
                    completion_refresh(app);
                    break;
                }

                if (next_kind != ERR)
                {
					if (next_kind == KEY_CODE_YES) ungetch((gint)next);
					else unget_wch(next);
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
    if (app->running) drain_keys(app);

    return G_SOURCE_REMOVE;
}

static gboolean
on_key(gint fd, GIOCondition condition, gpointer user_data)
{
    App *app = user_data;

    (void)fd;
	/* Shutdown may drain the main context for provider cancellation. */
	if (!app->running) return G_SOURCE_REMOVE;
	if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
	{
		app->approval_answer = AI_TOOL_APPROVAL_DENY_ALL;
		app->running = FALSE;
		g_main_loop_quit(app->loop);
		return G_SOURCE_REMOVE;
	}

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
	struct winsize size;
	if (!app->running) return G_SOURCE_REMOVE;

	/* GLib owns SIGWINCH, so ncurses' internal resize flag is not set.
	 * Explicitly synchronize its screen geometry with the actual PTY. */
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0 && size.ws_col > 0)
		resizeterm((gint)size.ws_row, (gint)size.ws_col);
	clearok(curscr, TRUE);

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
	app->sending = FALSE;
	app_sync_herdr(app);

    /* The full error is in the transcript; chrome carries only the outcome. */
	ui_turn_finished(app, error);
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
	app->sending = FALSE;
	app_sync_herdr(app);

    if (error != NULL)
    {
        say(app, "%s", error->message);
		ui_turn_finished(app, error);
    }
    else if (command != NULL)
    {
        handle_builtin(app, command);
    }
	else
		ui_turn_finished(app, NULL);

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
    if (app->completion == NULL || app->completion_dismissed || app->searching)
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
        (guint)((((gint64)app->candidate_index + delta) % n + n) % n);

    return TRUE;
}

/* A disk change must update an open slash menu without another keystroke. */
static void
on_resources_changed(AiResourceRegistry *registry, gpointer user_data)
{
	App *app = user_data;

	(void)registry;
	if (app->input->len > 0 && app->input->str[0] == '/')
	{
		completion_refresh(app);
		app_schedule_redraw(app);
	}
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

/**
 * completion_is_exact_command:
 * @app: the active composer
 *
 * Whether accepting the selected command would leave the complete input
 * unchanged. Only a command at the end of the draft qualifies: Enter must
 * still accept partial names, a different selected command, and paths.
 *
 * Returns: %TRUE if Enter should submit instead of accepting completion
 */
static gboolean
completion_is_exact_command(App *app)
{
	const gchar *text = NULL;
	guint start;
	guint end;

	if (app->candidates == NULL ||
		ai_completion_result_get_kind(app->candidates) != AI_COMPLETION_COMMAND ||
		!ai_completion_result_get_item_fields(app->candidates, app->candidate_index,
			&text, NULL, NULL, NULL, NULL))
		return FALSE;

	start = ai_completion_result_get_start(app->candidates);
	end = ai_completion_result_get_end(app->candidates);
	return start == 1 && end == app->input->len && app->cursor == end &&
		app->input->str[0] == '/' && g_str_equal(app->input->str + start, text);
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
	g_autofree gchar *valid = NULL;
	g_autoptr(GString) single_line = NULL;

    if (text == NULL)
    {
        return g_strdup("");
    }

    if (columns <= 1)
    {
        return g_strdup("");
    }

	/* Names and descriptions are untrusted single-line labels. Rendering a
	 * control character literally would move the cursor outside its column. */
	valid = g_utf8_make_valid(text, -1);
	single_line = g_string_new(NULL);
	for (p = valid; *p; p = g_utf8_next_char(p))
	{
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			g_string_append_c(single_line, ' ');
		else
			g_string_append_len(single_line, p, (gssize)(g_utf8_next_char(p) - p));
	}
	text = single_line->str;

    if ((gint)ai_style_text_width(text) <= columns)
    {
        return g_strdup(text);
    }

    /* It does not fit, so one column goes to the ellipsis. Deciding that
     * up front is what stops a name that fits exactly from losing its
     * last character to a truncation that was not needed. */
    for (p = text; *p != '\0'; p = g_utf8_next_char(p))
    {
        gint w = g_unichar_iszerowidth(g_utf8_get_char(p)) ? 0 : (g_unichar_iswide(g_utf8_get_char(p)) ? 2 : 1);

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

    /* Explicit UTF-8 avoids ACS rules becoming literal q's under tmux. */
    {
        gint y = height - rows - 1;
		gint x;
		g_autofree gchar *count = g_strdup_printf(" %u/%u ", app->candidate_index + 1, n);
		gint at = MAX(0, width - (gint)strlen(count) - 2);

		wattrset(app->transcript_win, theme_attr(PAIR_ACCENT));
		for (x = 0; x < width; x++)
			mvwaddstr(app->transcript_win, y, x, g_get_charset(NULL) ? "─" : "-");
		if (at > 14)
			mvwaddstr(app->transcript_win, y, 2, app->input->str[0] == '/' ? " COMMANDS " : " FILES ");
		mvwaddstr(app->transcript_win, y, at, count);
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
        wattrset(app->transcript_win, theme_attr(selected ? PAIR_SELECTION : PAIR_SURFACE));
        mvwhline(app->transcript_win, y, 0, ' ', width);
		if (selected)
			mvwaddstr(app->transcript_win, y, 0, g_get_charset(NULL) ? "›" : ">");

        name = fit_to_width(display, name_column - 3);
        wattrset(app->transcript_win, theme_attr(selected ? PAIR_SELECTION : PAIR_SURFACE) | A_BOLD);
        mvwaddstr(app->transcript_win, y, 2, name);

        if (description != NULL && description[0] != '\0' && text_room > 8)
        {
            g_autofree gchar *summary =
                fit_to_width(description, text_room);

			/* Keep the selection's contrast across every column. */
            wattrset(app->transcript_win, theme_attr(selected ? PAIR_SELECTION : PAIR_SURFACE));
            mvwaddstr(app->transcript_win, y, name_column, summary);
        }

        if (origin != NULL && origin[0] != '\0' && origin_width > 0 &&
            width > name_column + origin_width)
        {
            gint at = width - (gint)ai_style_text_width(origin) - 2;

            wattrset(app->transcript_win, theme_attr(selected ? PAIR_SELECTION : PAIR_SURFACE));
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
	/* Enter while busy leaves the entire draft, cursor and history intact. */
	if (app->sending || ai_conversation_get_busy(app->conversation)) return;

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

    app->follow = TRUE;
	app->sending = TRUE;
	app_sync_herdr(app);
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

/* First turn from leftover argv, once the loop is running. */
static gboolean
on_startup_send(gpointer user_data)
{
	app_send((App *)user_data);
	return G_SOURCE_REMOVE;
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
    g_autofree gchar *prompt = NULL;
    const gchar *target;
	gint answer;

    (void)conversation;

    if (app->approve_all || app->skip_permissions)
    {
        return AI_TOOL_APPROVAL_ALLOW;
    }

    {
        g_autoptr(AiToolCall) call = ai_tool_call_new(tool_use);

        target = ai_tool_call_get_target(call);
        prompt = g_strdup_printf("Run %s%s%s?",
                                 ai_tool_use_get_name(tool_use),
                                 target != NULL ? ": " : "",
                                 target != NULL ? target : "");
    }

	/* One reader owns all input, routing it by modal state. Redraws retain
	 * the prompt while provider I/O continues in this nested context. */
	if (app->approval_prompt != NULL) return AI_TOOL_APPROVAL_DENY;
	app->approval_prompt = prompt;
	app->approval_answer = AI_TOOL_APPROVAL_DEFAULT;
	app_sync_herdr(app);
	app_redraw(app);
	while (app->running && app->approval_answer == AI_TOOL_APPROVAL_DEFAULT &&
		(app->cancellable == NULL || !g_cancellable_is_cancelled(app->cancellable)))
	{
		drain_keys(app);
		if (app->approval_answer != AI_TOOL_APPROVAL_DEFAULT) break;
		g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
	}
	answer = app->approval_answer == AI_TOOL_APPROVAL_DEFAULT ? AI_TOOL_APPROVAL_DENY : app->approval_answer;
	app->approval_prompt = NULL;
	app_sync_herdr(app);

    app_schedule_redraw(app);

    return answer;
}

/**
 * app_reset:
 * @app: the terminal application
 *
 * Replace session-owned state while keeping the user's current settings.
 * Called after built-in resolution, with no foreground turn in flight.
 * Old background callbacks are detached before cancellation so they cannot
 * repopulate the splash screen when their asynchronous cleanup completes.
 */
static void
app_reset(App *app)
{
	g_autoptr(AiConversation) previous = NULL;
	g_autoptr(AiConversation) replacement = NULL;
	g_autoptr(AiBrigade) brigade = NULL;
	GObject *provider;

	previous = g_object_ref(app->conversation);
	provider = ai_conversation_get_provider(previous);
	/* Prepare and bind before discarding any live application state. A
	 * failed config write must leave /new and its MCP clients unchanged. */
	replacement = ai_conversation_new(provider);
	ai_conversation_set_system_prompt(replacement, ai_conversation_get_system_prompt(previous));
	ai_conversation_set_working_directory(replacement, ai_conversation_get_working_directory(previous));
	ai_conversation_set_max_tokens(replacement, ai_conversation_get_max_tokens(previous));
	ai_conversation_set_stream(replacement, ai_conversation_get_stream(previous));
	ai_conversation_set_local_tools(replacement, ai_conversation_get_local_tools(previous));
	ai_conversation_set_command_set(replacement, app->commands);
	if (!opt_no_agents)
		ai_conversation_enable_background_agents(replacement, AGENT_MAX_CONCURRENT);
	if (app->mcp_host != NULL)
	{
		g_autoptr(GError) mcp_error = NULL;
		if (!ai_mcp_host_bind(app->mcp_host, replacement, mcp_executable, !opt_mcp_no_inject, &mcp_error))
		{
			say(app, "Session unchanged: MCP reset failed: %s", mcp_error->message);
			return;
		}
	}
	g_signal_handlers_disconnect_by_data(previous, app);
	g_signal_handlers_disconnect_by_data(ai_conversation_get_transcript(previous), app);
	if (ai_conversation_get_brigade(previous) != NULL)
	{
		brigade = g_object_ref(ai_conversation_get_brigade(previous));
		ai_conversation_set_brigade(previous, NULL);
		ai_brigade_cancel_all(brigade);
	}

	/* A missing ID alone is insufficient when --continue was selected. */
	if (AI_IS_CLI_CLIENT(provider))
	{
		ai_cli_client_set_session_id(AI_CLI_CLIENT(provider), NULL);
		if (g_object_class_find_property(G_OBJECT_GET_CLASS(provider), "continue-session") != NULL)
			g_object_set(provider, "continue-session", FALSE, NULL);
	}

	/* A new executor also forgets tool approvals, todos and agent results. */
	g_set_object(&app->conversation, replacement);
	if (!opt_no_agents)
	{
		g_signal_connect(app->conversation, "agent-finished", G_CALLBACK(on_agent_finished), app);
	}
	g_signal_connect(ai_conversation_get_transcript(app->conversation), "items-changed",
		G_CALLBACK(on_transcript_items_changed), app);
	g_signal_connect_swapped(app->conversation, "notify::busy", G_CALLBACK(app_sync_herdr), app);
	if (app->running)
	{
		g_signal_connect_swapped(ai_conversation_get_transcript(app->conversation), "block-changed",
			G_CALLBACK(on_transcript_changed), app);
		g_signal_connect(app->conversation, "approval-requested", G_CALLBACK(on_approval_requested), app);
		g_signal_connect(app->conversation, "notify::busy", G_CALLBACK(on_busy_changed), app);
		g_signal_connect_swapped(app->conversation, "notify::activity", G_CALLBACK(app_schedule_redraw), app);
	}

	/* Forget drafts and navigation as well as the visible conversation. */
	g_ptr_array_set_size(app->history, 0);
	g_string_truncate(app->input, 0);
	g_clear_pointer(&app->history_draft, g_free);
	g_clear_pointer(&app->kill_buffer, g_free);
	g_clear_pointer(&app->row_cache, g_ptr_array_unref);
	g_clear_pointer(&app->feedback, g_free);
	g_clear_object(&app->cancellable);
	completion_close(app);
	app->completion_dismissed = FALSE;
	if (app->search != NULL) g_string_truncate(app->search, 0);
	app->searching = FALSE;
	app->search_row = -1;
	app->search_matches = 0;
	app->cursor = 0;
	app->input_first = 0;
	app->history_pos = -1;
	app->scroll = 0;
	app->row_count = 0;
	app->selected = -1;
	app->follow = TRUE;
	app->awaiting_since = 0;
	app->feedback_until = 0;
	app->spinner_frame = 0;
	app->intro_started = g_get_monotonic_time();
	app->approve_all = opt_yes;
	app->approval_answer = AI_TOOL_APPROVAL_DEFAULT;
	if (app->interrupt_id != 0)
	{
		g_source_remove(app->interrupt_id);
		app->interrupt_id = 0;
	}
	sync_spinner(app);
	app_sync_herdr(app);
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
    g_autofree gchar *resolved_model = NULL;
    AiProviderType type;
    GObject *provider;

	/* Startup alone consults app defaults; runtime switches start native. */
	if (initial)
	{
		/* Omission must behave exactly like -p default -m default, including
		 * bypassing the library's legacy AI_PROVIDER fallback. */
		if (!ai_provider_factory_resolve_defaults(config, "ai-tui",
		                                          name != NULL ? name : "default",
		                                          opt_model != NULL ? opt_model : "default",
		                                          &type, &resolved_model, error))
			return NULL;
		if (opt_launch || opt_launch_cmd || opt_launch_cmd_print)
			type = ai_launch_provider_type(type);
		provider = ai_provider_factory_new(type, config, error);
	}
	else
		provider = ai_provider_factory_new_from_string(name, config, error);

    if (provider == NULL)
    {
        return NULL;
    }

    if (AI_IS_CLIENT(provider))
    {
        AiClient *c = AI_CLIENT(provider);

		if (resolved_model != NULL) ai_client_set_model(c, resolved_model);
        if (initial && opt_system != NULL)
            ai_client_set_system_prompt(c, opt_system);
        ai_client_set_max_tokens(c, opt_max_tokens);
    }
    else if (AI_IS_CLI_CLIENT(provider))
    {
        AiCliClient *c = AI_CLI_CLIENT(provider);

		/* Also disable the library deadline after runtime provider switches.
		 * Startup --set overrides are applied below. */
		ai_cli_client_set_process_timeout_ms(c, 0);
		if (resolved_model != NULL)
			ai_cli_client_set_model(c, resolved_model);
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

    if ((!initial || opt_skip_permissions) &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
                                     "skip-permissions") != NULL)
    {
        g_object_set(provider, "skip-permissions", opt_skip_permissions, NULL);
    }

    if (initial && !apply_property_overrides(provider, error))
    {
        g_object_unref(provider);
        return NULL;
    }

    /* Wrapped CLI hooks must not replace ai-tui's pane authority or persist
     * a native CLI session that would restore into a different application.
     * Preserve all other child environment and leave native launch modes alone. */
	if (AI_IS_CLI_CLIENT(provider) && !opt_no_herdr && !opt_dry_run &&
		!opt_launch && !opt_launch_cmd && !opt_launch_cmd_print &&
		ai_tui_herdr_detect(g_getenv("HERDR_ENV"),
			g_getenv("HERDR_SOCKET_PATH"), g_getenv("HERDR_PANE_ID")))
	{
		ai_cli_client_set_env(AI_CLI_CLIENT(provider), "HERDR_ENV", "0");
		ai_cli_client_set_env(AI_CLI_CLIENT(provider), "HERDR_PANE_ID", "");
	}

    return provider;
}

static GObject *
build_provider(GError **error)
{
	return build_provider_named(opt_provider, TRUE, error);
}

/*
 * Leftover argv after option parsing, joined with spaces.
 *
 * `--` is skipped if g_option_context_parse() left it in place. Same
 * rule `ai` and the launch modes use, so `ai-tui -- -p is a prompt`
 * does not become a provider flag.
 */
static gchar *
remaining_args_prompt(gint argc, gchar **argv)
{
	gint first;

	first = argc > 1 && g_str_equal(argv[1], "--") ? 2 : 1;
	if (argc <= first)
		return NULL;

	return g_strjoinv(" ", &argv[first]);
}

/*
 * Read all of stdin into a newly-allocated NUL-terminated string.
 * Returns an empty string on EOF with no data. (transfer full)
 */
static gchar *
read_all_stdin(void)
{
	GString *buf = g_string_new(NULL);
	gchar    chunk[4096];
	gsize    n;

	while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
		g_string_append_len(buf, chunk, n);

	return g_string_free(buf, FALSE);
}

/*
 * Prompt from leftover argv, else stdin when stdin is not a terminal.
 * Same rule as `ai`. Empty input is no prompt. (transfer full)
 *
 * Never called for --launch: the native CLI owns stdin.
 */
static gchar *
resolve_prompt(gint argc, gchar **argv)
{
	g_autofree gchar *from_argv = remaining_args_prompt(argc, argv);

	if (from_argv != NULL && from_argv[0] != '\0')
		return (gchar *)g_steal_pointer(&from_argv);

	if (!isatty(STDIN_FILENO))
	{
		gchar *from_stdin = read_all_stdin();

		if (from_stdin != NULL)
			g_strchomp(from_stdin);
		if (from_stdin != NULL && from_stdin[0] == '\0')
		{
			g_free(from_stdin);
			return NULL;
		}
		return from_stdin;
	}

	return NULL;
}

/*
 * Point fd 0 at the controlling terminal after a piped prompt.
 *
 * fread() has already drained the pipe. freopen() closes that fd and
 * opens /dev/tty as stdin, which is what ncurses and
 * g_unix_fd_add(STDIN_FILENO) then read. Fails with no controlling
 * terminal (cron, ssh -T).
 *
 * Callers must also require isatty(STDOUT_FILENO). Otherwise a test or
 * `echo p | ai-tui > out` would steal the developer's terminal via
 * /dev/tty while stdout is still a pipe.
 */
static gboolean
attach_stdin_to_tty(void)
{
	return freopen("/dev/tty", "r", stdin) != NULL
		&& isatty(STDIN_FILENO);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *prompt = NULL;
    g_autoptr(AiTuiHerdr) herdr = NULL;
    GObject *provider;
    App app;
	AiMcpHost *mcp_host __attribute__((cleanup(mcp_cleanup))) = NULL;

    setlocale(LC_ALL, "");

    context = g_option_context_new("[PROMPT] - a terminal agent harness");
    g_option_context_add_main_entries(context, option_entries, NULL);
	g_option_context_add_main_entries(context, mcp_option_entries, NULL);
    g_option_context_set_summary(context,
        "Drives any ai-glib provider from a terminal, showing prose,\n"
        "reasoning and grouped tool calls as they happen.");
	g_option_context_set_description(context,
		"Examples:\n  ai --setup                 # configure ai-tui independently\n"
		"  ai-tui --mcp-tools todo_write,agent_spawn,agent_status,agent_result\n"
		"  ai-tui --mcp-server --mcp-all-tools --mcp-no-inject\n"
		"  ai-tui -p default -m default\n"
		"  ai-tui \"review the diff\"\n"
		"  echo \"review the diff\" | ai-tui\n\n"
		"  ai-tui --launch -p claude -m opus\n"
		"  ai-tui --launch-cmd-print -p default -m default \"hello\"\n\n"
		"Prompt: leftover arguments, else stdin when it is not a terminal.\n"
		"A piped prompt on a terminal is the first turn; without a terminal,\n"
		"one turn is printed as --dump does.\n"
		"Omitted provider: AI_PROVIDER, then ai-tui defaults, then Claude.\n"
		"Explicit default bypasses AI_PROVIDER. Omitted/default model uses the\n"
		"saved ai-tui model only for its matching provider, otherwise native.\n"
		"ai and library defaults do not affect ai-tui; runtime switches start native.");

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_printerr("ai-tui: %s\n", error->message);
        return 1;
    }

	{
		gint mcp_status = mcp_early(argc, argv[0], opt_launch || opt_launch_cmd || opt_launch_cmd_print || (opt_mcp_server && (opt_dump != NULL || opt_dry_run)), &error);
		if (mcp_status >= 0) { if (error != NULL) g_printerr("ai-tui: %s\n", error->message); return mcp_status; }
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
	if (opt_list_themes)
	{
		guint i;
		for (i = 0; i < G_N_ELEMENTS(THEMES); i++)
			g_print("%s%s\n", THEMES[i].name, i == 0 ? " (default)" : "");
		return 0;
	}
	{
		const gchar *name = opt_theme != NULL ? opt_theme : g_getenv("AI_TUI_THEME");
		guint i;
		theme_explicit = opt_theme != NULL;
		if (name != NULL && (theme_explicit || name[0] != '\0'))
		{
			for (i = 0; i < G_N_ELEMENTS(THEMES); i++)
				if (g_str_equal(name, THEMES[i].name)) break;
			if (i == G_N_ELEMENTS(THEMES))
			{
				g_printerr("ai-tui: unknown theme '%s'; use --list-themes\n", name);
				return 2;
			}
			theme_index = i;
		}
	}

	if (opt_launch + opt_launch_cmd + opt_launch_cmd_print > 1 ||
	    ((opt_launch || opt_launch_cmd || opt_launch_cmd_print) &&
	     (opt_dump != NULL || opt_dry_run || opt_local_tools || opt_yes)))
	{
		g_printerr("ai-tui: choose one launch mode; it cannot be combined with dump, dry-run, or local-tool modes\n");
		return 2;
	}
    provider = build_provider(&error);

    if (provider == NULL)
    {
        g_printerr("ai-tui: %s\n", error->message);
        return 1;
    }

	if (opt_launch || opt_launch_cmd || opt_launch_cmd_print)
	{
		g_autofree gchar *launch_prompt = remaining_args_prompt(argc, argv);
		gint status = ai_launch_run(provider, !opt_launch, opt_launch_cmd_print,
		                            launch_prompt);

		g_object_unref(provider);
		return status;
	}

	if (opt_mcp_server)
	{
		gint mcp_status = mcp_headless(provider, "ai-tui", opt_system, opt_max_tokens, !opt_no_stream, opt_no_agents);
		g_object_unref(provider);
		return mcp_status;
	}

	/* Same sources as `ai`: leftover argv, else stdin when it is a pipe. */
	prompt = resolve_prompt(argc, argv);

    memset(&app, 0, sizeof app);
    app.conversation = ai_conversation_new(provider);
	/* --set remains authoritative at startup; show the effective flag. */
	app.skip_permissions = opt_skip_permissions;
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(provider), "skip-permissions") != NULL)
		g_object_get(provider, "skip-permissions", &app.skip_permissions, NULL);
	opt_skip_permissions = app.skip_permissions;
	g_signal_connect(ai_conversation_get_transcript(app.conversation), "items-changed",
		G_CALLBACK(on_transcript_items_changed), &app);
    app.input = g_string_new(prompt);
    app.cursor = (guint)app.input->len;
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
		g_signal_connect(app.registry, "changed",
		                 G_CALLBACK(on_resources_changed), &app);

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

	if (mcp_requested())
	{
		mcp_host = ai_mcp_host_new(app.conversation, "ai-tui", (const gchar * const *)opt_mcp_tools, opt_mcp_all_tools, &error);
		if (mcp_host == NULL || !ai_mcp_host_start(mcp_host, opt_mcp_socket, FALSE, &error) ||
		    !ai_mcp_host_bind(mcp_host, app.conversation, mcp_executable, !opt_mcp_no_inject, &error))
		{
			g_printerr("ai-tui: %s\n", error->message);
			return 2;
		}
		app.mcp_host = mcp_host;
		g_printerr("ai-tui MCP socket: %s\n", ai_mcp_host_get_socket_path(mcp_host));
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
                ai_message_new_user(opt_dump != NULL ? opt_dump :
                                    (prompt != NULL ? prompt : "(prompt)"));
            GList *messages = g_list_append(NULL, message);

            if (klass->build_argv == NULL)
            {
                g_print("%s does not build a command line.\n",
                        G_OBJECT_TYPE_NAME(provider));
            }
            else
            {
                g_auto(GStrv) command = klass->build_argv(
                    AI_CLI_CLIENT(provider), messages, ai_conversation_get_system_prompt(app.conversation),
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

	ai_tui_history_restore(app.conversation, app.history);
	if (!opt_no_herdr)
		herdr = ai_tui_herdr_new(g_getenv("HERDR_ENV"),
			g_getenv("HERDR_SOCKET_PATH"), g_getenv("HERDR_PANE_ID"));
	app.herdr = herdr;
	if (herdr != NULL || mcp_host != NULL)
	{
		g_unix_signal_add(SIGTERM, on_herdr_shutdown, &app);
		g_unix_signal_add(SIGHUP, on_herdr_shutdown, &app);
	}
	g_signal_connect_swapped(app.conversation, "notify::busy",
		G_CALLBACK(app_sync_herdr), &app);

    /*
     * One-shot: --dump, or a prompt given without a terminal.
     *
     * A pipe on stdin is drained first (resolve_prompt). If stdout is
     * still a tty, stdin is then reopened on /dev/tty so the TUI can
     * run. If that is impossible, the prompt is this path --- the same
     * as `ai`. --dump still forces a one-shot on a tty.
     */
    {
        const gchar *one_shot = opt_dump != NULL ? opt_dump : NULL;

        if (one_shot == NULL && !isatty(STDIN_FILENO)
            && !(isatty(STDOUT_FILENO) && attach_stdin_to_tty()))
            one_shot = prompt;

        if (one_shot != NULL)
        {
            g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
            g_autofree gchar *text = NULL;

            app.loop = loop;
            app.dump_loop = loop;
			app.sending = TRUE;
			app_sync_herdr(&app);

            if (opt_no_expand)
            {
                ai_conversation_send_async(app.conversation, one_shot, NULL,
                                           on_sent, &app);
            }
            else
            {
                ai_conversation_send_input_async(app.conversation, one_shot,
                                                 NULL, on_input_sent, &app);
            }

            g_main_loop_run(loop);
            app.dump_loop = NULL;
			/* Cancellation callbacks still reference App and its conversation. */
			if (mcp_host != NULL) ai_mcp_host_stop(mcp_host);

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
    }

    if (!isatty(STDIN_FILENO))
    {
        g_printerr("ai-tui: stdin is not a terminal; pass a prompt as an "
                   "argument, pipe it on stdin, or use --dump PROMPT\n");
        g_clear_object(&app.completion);
        g_clear_object(&app.commands);
        g_clear_object(&app.registry);
        g_object_unref(app.conversation);
        g_object_unref(provider);
        g_string_free(app.input, TRUE);
        g_ptr_array_unref(app.history);
        return 1;
    }

    /* ---- Terminal ---- */
	app.search = g_string_new(NULL);
	app.search_row = -1;
	app.details = TRUE;

	/* Shells may export their own stale LINES/COLUMNS into a new pane. */
	use_env(FALSE);
	use_tioctl(TRUE);
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
	/* tmux and terminals also emit the numeric Home/End forms, which
	 * xterm terminfo may omit in favor of application-cursor sequences. */
	define_key("\033[1~", KEY_HOME);
	define_key("\033[4~", KEY_END);
	define_key("\033[7~", KEY_HOME);
	define_key("\033[8~", KEY_END);
	define_key("\033[Z", KEY_BTAB);
	define_key("\033[9;2u", KEY_BTAB);
	define_key("\033[27;2;9~", KEY_BTAB);
	define_key("\033[200~", KEY_PASTE_START);
	define_key("\033[201~", KEY_PASTE_END);
	/* This terminal input mode has no curses wrapper; colors and drawing
	 * still go exclusively through curses. Balance it on editor handoff. */
	fputs("\033[?2004h", stdout);
	fflush(stdout);

    app.running = TRUE;
    app.loop = g_main_loop_new(NULL, FALSE);
	app.intro_started = g_get_monotonic_time();
	sync_spinner(&app);

    g_signal_connect_swapped(ai_conversation_get_transcript(app.conversation),
                             "block-changed",
                             G_CALLBACK(on_transcript_changed), &app);
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
    if (prompt != NULL && prompt[0] != '\0')
        g_idle_add(on_startup_send, &app);
    g_main_loop_run(app.loop);
	/* MCP stop drains callbacks, including UI and input sources. Do that
	 * while the terminal and App-owned fields are still valid. */
	app.running = FALSE;
	if (mcp_host != NULL) ai_mcp_host_stop(mcp_host);
	if (app.redraw_id != 0)
	{
		g_source_remove(app.redraw_id);
		app.redraw_id = 0;
	}

	fputs("\033[?2004l", stdout);
	fflush(stdout);
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
	g_string_free(app.search, TRUE);
	g_free(app.kill_buffer);
	g_free(app.history_draft);
	g_free(app.feedback);
	g_clear_pointer(&app.row_cache, g_ptr_array_unref);
	delwin(app.input_win);
	delwin(app.status_win);
	delwin(app.transcript_win);

    return 0;
}
