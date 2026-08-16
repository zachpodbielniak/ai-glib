/*
 * transcript-render.c - A frontend in a hundred lines, without ncurses
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The same transcript ai-tui shows, rendered with ANSI escapes and nothing
 * else. It exists to demonstrate that the view layer is genuinely
 * toolkit-agnostic: the only thing this file knows that the library does
 * not is which escape sequence means "green".
 *
 * Read this before writing a frontend of your own -- an Emacs one replaces
 * ansi_for_tag() with a face lookup and the write loop with `insert` plus
 * `put-text-property`, and shares everything else.
 *
 *   ./transcript-render "what is 2 + 2?"
 */

#include <stdio.h>
#include <string.h>

#include <ai-glib.h>

/*
 * The one decision a frontend makes that the library does not: what a
 * style role looks like on this display.
 */
static const gchar *
ansi_for_tag(AiStyleTag tag)
{
    switch (tag)
    {
        case AI_STYLE_USER_PROMPT:  return "\033[1m";
        case AI_STYLE_HEADING:      return "\033[1m";
        case AI_STYLE_DIM:          return "\033[2m";
        case AI_STYLE_TOOL_NAME:    return "\033[1;35m";
        case AI_STYLE_TOOL_TARGET:  return "\033[36m";
        case AI_STYLE_TOOL_PENDING: return "\033[33m";
        case AI_STYLE_TOOL_OK:      return "\033[32m";
        case AI_STYLE_TOOL_FAILED:  return "\033[31m";
        case AI_STYLE_ADDED:        return "\033[32m";
        case AI_STYLE_REMOVED:      return "\033[31m";
        case AI_STYLE_CODE:         return "\033[36m";
        case AI_STYLE_THINKING:     return "\033[2;34m";
        case AI_STYLE_ERROR:        return "\033[1;31m";
        case AI_STYLE_STATUS:       return "\033[33m";
        case AI_STYLE_LINK:         return "\033[4;34m";
        case AI_STYLE_MARKER:       return "\033[2m";
        default:                    return "";
    }
}

/*
 * Print one block.
 *
 * Walk the spans, not the characters: each says "these bytes are a tool
 * target", and the escape goes around them. Offsets are byte offsets, which
 * is exactly what fwrite() wants.
 */
static void
print_block(AiViewBlock *block, guint width)
{
    g_autoptr(AiRenderedText) rendered = ai_view_block_render(block, width);
    const gchar *text = ai_rendered_text_get_text(rendered);
    guint length = ai_rendered_text_get_length(rendered);
    guint offset = 0;
    guint i;

    for (i = 0; i < ai_rendered_text_get_n_spans(rendered); i++)
    {
        guint start = 0;
        guint len = 0;
        AiStyleTag tag = AI_STYLE_DEFAULT;

        ai_rendered_text_get_span(rendered, i, &start, &len, &tag);

        /* Whatever lies between the last span and this one is unstyled. */
        if (start > offset)
        {
            fwrite(text + offset, 1, start - offset, stdout);
        }

        fputs(ansi_for_tag(tag), stdout);
        fwrite(text + start, 1, len, stdout);
        fputs("\033[0m", stdout);

        offset = start + len;
    }

    if (offset < length)
    {
        fwrite(text + offset, 1, length - offset, stdout);
    }

    fputs("\n\n", stdout);
}

static void
on_block_changed(
    AiTranscript *transcript,
    guint         position,
    AiViewBlock  *block,
    gpointer      user_data
){
    (void)transcript;
    (void)position;
    (void)block;
    (void)user_data;

    /*
     * A real frontend redraws the one region that changed. This one prints
     * blocks as they complete instead, which needs no cursor addressing.
     */
}

static void
on_items_changed(
    GListModel *model,
    guint       position,
    guint       removed,
    guint       added,
    gpointer    user_data
){
    guint i;

    (void)removed;
    (void)user_data;

    for (i = 0; i < added; i++)
    {
        g_autoptr(AiViewBlock) block =
            g_list_model_get_item(model, position + i);

        if (block != NULL)
        {
            print_block(block, 80);
        }
    }
}

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;

    ai_conversation_send_finish(AI_CONVERSATION(source), result, &error);
    g_main_loop_quit(loop);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(AiClaudeClient) client = NULL;
    g_autoptr(AiConversation) conversation = NULL;
    AiTranscript *transcript;

    if (argc < 2)
    {
        g_printerr("usage: %s PROMPT\n", argv[0]);
        return 1;
    }

    client = ai_claude_client_new();
    conversation = ai_conversation_new(G_OBJECT(client));
    transcript = ai_conversation_get_transcript(conversation);

    g_signal_connect(transcript, "items-changed",
                     G_CALLBACK(on_items_changed), NULL);
    g_signal_connect(transcript, "block-changed",
                     G_CALLBACK(on_block_changed), NULL);

    loop = g_main_loop_new(NULL, FALSE);
    ai_conversation_send_async(conversation, argv[1], NULL, on_sent, loop);
    g_main_loop_run(loop);

    return 0;
}
