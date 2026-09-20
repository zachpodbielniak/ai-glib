/*
 * ai-gui-style.c - Turning an AiStyleTag into something Pango understands
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <adwaita.h>

#include "ai-gui-style.h"

/*
 * One row per style role.
 *
 * Two palettes rather than one set of CSS classes because the text being
 * styled lives inside a single #GtkLabel: a span is a run of bytes, not a
 * widget, and only Pango attributes can colour part of a label. The class
 * names would have nothing to attach to.
 */
typedef struct
{
	AiStyleTag   tag;
	const gchar *dark;
	const gchar *light;
	gboolean     bold;
	gboolean     italic;
	gboolean     underline;
	gboolean     monospace;
} AiGuiStyleRow;

static const AiGuiStyleRow STYLE_ROWS[] = {
	{ AI_STYLE_DEFAULT,         NULL,      NULL,      FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_USER_PROMPT,     "#89b4fa", "#1e66f5", TRUE,  FALSE, FALSE, FALSE },
	{ AI_STYLE_HEADING,         "#f9e2af", "#df8e1d", TRUE,  FALSE, FALSE, FALSE },
	{ AI_STYLE_DIM,             "#7f849c", "#8c8fa1", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_TOOL_NAME,       "#94e2d5", "#179299", TRUE,  FALSE, FALSE, FALSE },
	{ AI_STYLE_TOOL_TARGET,     "#89b4fa", "#1e66f5", FALSE, FALSE, FALSE, TRUE  },
	{ AI_STYLE_TOOL_PENDING,    "#f9e2af", "#df8e1d", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_TOOL_OK,         "#a6e3a1", "#40a02b", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_TOOL_FAILED,     "#f38ba8", "#d20f39", TRUE,  FALSE, FALSE, FALSE },
	{ AI_STYLE_ADDED,           "#a6e3a1", "#40a02b", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_REMOVED,         "#f38ba8", "#d20f39", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_CODE,            "#f5c2e7", "#8839ef", FALSE, FALSE, FALSE, TRUE  },
	{ AI_STYLE_THINKING,        "#9399b2", "#7c7f93", FALSE, TRUE,  FALSE, FALSE },
	{ AI_STYLE_ERROR,           "#f38ba8", "#d20f39", TRUE,  FALSE, FALSE, FALSE },
	{ AI_STYLE_STATUS,          "#89dceb", "#04a5e5", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_LINK,            "#89b4fa", "#1e66f5", FALSE, FALSE, TRUE,  FALSE },
	{ AI_STYLE_MARKER,          "#7f849c", "#8c8fa1", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_MENTION,         "#cba6f7", "#8839ef", FALSE, FALSE, FALSE, TRUE  },
	{ AI_STYLE_COMMAND,         "#cba6f7", "#8839ef", TRUE,  FALSE, FALSE, FALSE },
	{ AI_STYLE_TODO_PENDING,    "#7f849c", "#8c8fa1", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_TODO_ACTIVE,     "#f9e2af", "#df8e1d", TRUE,  FALSE, FALSE, FALSE },
	{ AI_STYLE_TODO_DONE,       "#a6e3a1", "#40a02b", FALSE, FALSE, FALSE, FALSE },
	{ AI_STYLE_SYNTAX_KEYWORD,  "#cba6f7", "#8839ef", FALSE, FALSE, FALSE, TRUE  },
	{ AI_STYLE_SYNTAX_STRING,   "#a6e3a1", "#40a02b", FALSE, FALSE, FALSE, TRUE  },
	{ AI_STYLE_SYNTAX_COMMENT,  "#6c7086", "#9ca0b0", FALSE, TRUE,  FALSE, TRUE  },
	{ AI_STYLE_SYNTAX_NUMBER,   "#fab387", "#fe640b", FALSE, FALSE, FALSE, TRUE  },
	{ AI_STYLE_SYNTAX_TYPE,     "#f9e2af", "#df8e1d", FALSE, FALSE, FALSE, TRUE  },
	{ AI_STYLE_SYNTAX_FUNCTION, "#89b4fa", "#1e66f5", FALSE, FALSE, FALSE, TRUE  }
};

static const gchar *APP_CSS =
	".ai-transcript { background: transparent; }\n"
	".ai-block { padding: 6px 10px; }\n"
	".ai-block-body { font-size: 1.0em; }\n"
	".ai-turn {\n"
	"  border-radius: 12px;\n"
	"  padding: 10px 14px;\n"
	"  background: alpha(currentColor, 0.07);\n"
	"}\n"
	".ai-thinking { opacity: 0.75; }\n"
	".ai-error { background: alpha(@error_color, 0.12); border-radius: 8px; }\n"
	".ai-tool { background: alpha(currentColor, 0.04); border-radius: 8px; }\n"
	".ai-composer { padding: 6px; }\n"
	".ai-composer textview { background: transparent; font-size: 1.0em; }\n"
	".ai-attachment { padding: 2px 6px; }\n"
	".ai-monospace { font-family: monospace; }\n"
	".ai-session-subtitle { font-size: 0.82em; opacity: 0.7; }\n"
	".ai-activity { font-size: 0.85em; opacity: 0.8; }\n"
	/*
	 * Dashboard status pills. One class per state, because the state is
	 * a role and what it looks like is this file's decision -- the same
	 * split AiStyleTag draws between the library and the frontend.
	 */
	".ai-status {\n"
	"  font-size: 0.78em;\n"
	"  font-weight: bold;\n"
	"  letter-spacing: 0.06em;\n"
	"  padding: 3px 8px;\n"
	"  border-radius: 6px;\n"
	"  background: alpha(currentColor, 0.10);\n"
	"}\n"
	".ai-status-input { color: @warning_color; background: alpha(@warning_color, 0.16); }\n"
	".ai-status-error { color: @error_color; background: alpha(@error_color, 0.16); }\n"
	".ai-status-work { color: @accent_color; background: alpha(@accent_color, 0.16); }\n"
	".ai-status-done { color: @success_color; background: alpha(@success_color, 0.16); }\n"
	".ai-status-stopped { opacity: 0.8; }\n"
	".ai-status-idle { opacity: 0.6; }\n"
	".ai-status-disconnected { opacity: 0.45; }\n";

static gboolean style_dark = TRUE;

static const AiGuiStyleRow *
style_row(AiStyleTag tag)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(STYLE_ROWS); i++)
	{
		if (STYLE_ROWS[i].tag == tag)
			return &STYLE_ROWS[i];
	}

	/*
	 * An unknown tag renders as ordinary text rather than not at all.
	 * The library can grow a role without this file having to know about
	 * it first, which is the point of the tags being roles.
	 */
	return &STYLE_ROWS[0];
}

static void
on_scheme_changed(
	GObject    *manager,
	GParamSpec *pspec,
	gpointer    user_data
){
	ai_gui_style_set_dark(adw_style_manager_get_dark(ADW_STYLE_MANAGER(manager)));
}

void
ai_gui_style_init(void)
{
	AdwStyleManager *manager = adw_style_manager_get_default();
	g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
	GdkDisplay *display = gdk_display_get_default();

	gtk_css_provider_load_from_string(provider, APP_CSS);

	if (display != NULL)
	{
		gtk_style_context_add_provider_for_display(display,
			GTK_STYLE_PROVIDER(provider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}

	style_dark = adw_style_manager_get_dark(manager);
	g_signal_connect(manager, "notify::dark", G_CALLBACK(on_scheme_changed),
	                 NULL);
}

void
ai_gui_style_set_dark(gboolean dark)
{
	style_dark = dark;
}

gboolean
ai_gui_style_get_dark(void)
{
	return style_dark;
}

const gchar *
ai_gui_style_tag_colour(AiStyleTag tag)
{
	const AiGuiStyleRow *row = style_row(tag);

	return style_dark ? row->dark : row->light;
}

PangoAttrList *
ai_gui_style_attributes(AiRenderedText *rendered)
{
	PangoAttrList *attributes = pango_attr_list_new();
	guint n_spans;
	guint i;

	g_return_val_if_fail(rendered != NULL, attributes);

	n_spans = ai_rendered_text_get_n_spans(rendered);

	for (i = 0; i < n_spans; i++)
	{
		const AiGuiStyleRow *row;
		const gchar *colour;
		AiStyleTag tag = AI_STYLE_DEFAULT;
		guint start = 0;
		guint len = 0;

		if (!ai_rendered_text_get_span(rendered, i, &start, &len, &tag))
			continue;

		if (len == 0)
			continue;

		row = style_row(tag);
		colour = style_dark ? row->dark : row->light;

		if (colour != NULL)
		{
			GdkRGBA rgba;

			if (gdk_rgba_parse(&rgba, colour))
			{
				PangoAttribute *attribute = pango_attr_foreground_new(
					(guint16)(rgba.red * 65535.0),
					(guint16)(rgba.green * 65535.0),
					(guint16)(rgba.blue * 65535.0));

				attribute->start_index = start;
				attribute->end_index = start + len;
				pango_attr_list_insert(attributes, attribute);
			}
		}

		if (row->bold)
		{
			PangoAttribute *attribute = pango_attr_weight_new(PANGO_WEIGHT_BOLD);

			attribute->start_index = start;
			attribute->end_index = start + len;
			pango_attr_list_insert(attributes, attribute);
		}

		if (row->italic)
		{
			PangoAttribute *attribute = pango_attr_style_new(PANGO_STYLE_ITALIC);

			attribute->start_index = start;
			attribute->end_index = start + len;
			pango_attr_list_insert(attributes, attribute);
		}

		if (row->underline)
		{
			PangoAttribute *attribute =
				pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);

			attribute->start_index = start;
			attribute->end_index = start + len;
			pango_attr_list_insert(attributes, attribute);
		}

		if (row->monospace)
		{
			PangoAttribute *attribute = pango_attr_family_new("monospace");

			attribute->start_index = start;
			attribute->end_index = start + len;
			pango_attr_list_insert(attributes, attribute);
		}
	}

	return attributes;
}
