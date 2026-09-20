/*
 * ai-gui-style.c - Turning an AiStyleTag into something Pango understands
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <adwaita.h>

#include "core/ai-theme.h"

#include "ai-gui-style.h"

/*
 * The desktop's own colours, for the `terminal` palette.
 *
 * `terminal` means "use what the host already provides": in a terminal
 * that is the sixteen the user configured, and in a window it is the
 * GNOME palette libadwaita itself draws from. Hard-coded rather than
 * read back out of the stylesheet because a Pango attribute needs a
 * concrete RGBA and there is no supported way to resolve `@accent_color`
 * to one --- gtk_style_context_lookup_color() was the way and is gone.
 *
 * Two sets, because a hue that reads on white is not the hue that reads
 * on near-black. These are GNOME's own light and dark variants.
 */
typedef struct
{
	guint muted;
	guint accent;
	guint cyan;
	guint green;
	guint yellow;
	guint red;
	guint number;
	guint function;
} AiGuiNativePalette;

static const AiGuiNativePalette NATIVE_DARK = {
	0x9a9996, 0xc061cb, 0x33c7de, 0x57e389,
	0xf8e45c, 0xff7b63, 0xffbe6f, 0x62a0ea
};

static const AiGuiNativePalette NATIVE_LIGHT = {
	0x77767b, 0x9141ac, 0x1a7f8e, 0x26a269,
	0xb5820a, 0xc01c28, 0xc64600, 0x1c71d8
};

/*
 * Font family and slant, which a terminal cannot express and therefore
 * are not in the shared table. Colour and weight are; see
 * src/core/ai-theme.h.
 */
typedef struct
{
	AiStyleTag tag;
	gboolean   monospace;
	gboolean   italic;
} AiGuiTypeface;

static const AiGuiTypeface TYPEFACES[] = {
	{ AI_STYLE_TOOL_TARGET,     TRUE,  FALSE },
	{ AI_STYLE_CODE,            TRUE,  FALSE },
	{ AI_STYLE_MENTION,         TRUE,  FALSE },
	{ AI_STYLE_THINKING,        FALSE, TRUE  },
	{ AI_STYLE_SYNTAX_KEYWORD,  TRUE,  FALSE },
	{ AI_STYLE_SYNTAX_STRING,   TRUE,  FALSE },
	{ AI_STYLE_SYNTAX_COMMENT,  TRUE,  TRUE  },
	{ AI_STYLE_SYNTAX_NUMBER,   TRUE,  FALSE },
	{ AI_STYLE_SYNTAX_TYPE,     TRUE,  FALSE },
	{ AI_STYLE_SYNTAX_FUNCTION, TRUE,  FALSE }
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
	".ai-status-disconnected { opacity: 0.45; }\n"
	/* The quota indicator turns colour as the allowance runs down; the
	 * palette's own warning and error entries, so it matches whichever
	 * theme is loaded rather than naming colours here. */
	".ai-quota-low { color: @warning_color; }\n"
	".ai-quota-spent { color: @error_color; font-weight: bold; }\n";

typedef struct
{
	AiGuiStyleChangedFunc func;
	gpointer              data;
} AiGuiStyleListener;

static const AiTheme    *style_theme;
static GtkCssProvider   *style_palette_provider;
static gchar            *style_color_scheme;
static gboolean          style_dark;
static GArray           *style_listeners;

void
ai_gui_style_add_changed(
	AiGuiStyleChangedFunc func,
	gpointer              user_data
){
	AiGuiStyleListener listener;

	g_return_if_fail(func != NULL);

	if (style_listeners == NULL)
		style_listeners = g_array_new(FALSE, FALSE, sizeof(AiGuiStyleListener));

	listener.func = func;
	listener.data = user_data;
	g_array_append_val(style_listeners, listener);
}

void
ai_gui_style_remove_changed(
	AiGuiStyleChangedFunc func,
	gpointer              user_data
){
	guint i;

	if (style_listeners == NULL)
		return;

	for (i = 0; i < style_listeners->len; i++)
	{
		AiGuiStyleListener *listener =
			&g_array_index(style_listeners, AiGuiStyleListener, i);

		if (listener->func == func && listener->data == user_data)
		{
			g_array_remove_index_fast(style_listeners, i);
			return;
		}
	}
}

static void
style_notify(void)
{
	guint i;

	/* Backwards, so a listener that removes itself while being called
	 * does not make the loop skip its neighbour. */
	for (i = style_listeners != NULL ? style_listeners->len : 0; i > 0; i--)
	{
		AiGuiStyleListener *listener =
			&g_array_index(style_listeners, AiGuiStyleListener, i - 1);

		listener->func(listener->data);
	}
}

/* ================================================================
 * Colour resolution
 * ================================================================ */

static void
style_rgba_from_rgb(
	guint    rgb,
	GdkRGBA *out
){
	out->red = (gdouble)((rgb >> 16) & 255) / 255.0;
	out->green = (gdouble)((rgb >> 8) & 255) / 255.0;
	out->blue = (gdouble)(rgb & 255) / 255.0;
	out->alpha = 1.0;
}

static guint
style_native_colour(AiThemeRole role)
{
	const AiGuiNativePalette *palette = style_dark ? &NATIVE_DARK
	                                              : &NATIVE_LIGHT;

	switch (role)
	{
		case AI_THEME_ROLE_MUTED:    return palette->muted;
		case AI_THEME_ROLE_ACCENT:   return palette->accent;
		case AI_THEME_ROLE_CYAN:     return palette->cyan;
		case AI_THEME_ROLE_GREEN:    return palette->green;
		case AI_THEME_ROLE_YELLOW:   return palette->yellow;
		case AI_THEME_ROLE_RED:      return palette->red;
		case AI_THEME_ROLE_NUMBER:   return palette->number;
		case AI_THEME_ROLE_FUNCTION: return palette->function;
		case AI_THEME_ROLE_DEFAULT:
		case AI_THEME_ROLE_TEXT:
		default:                     return 0;
	}
}

gboolean
ai_gui_style_tag_colour(
	AiStyleTag  tag,
	GdkRGBA    *out_rgba
){
	AiThemeRole role = ai_theme_role_for_tag(tag);

	g_return_val_if_fail(out_rgba != NULL, FALSE);

	/* Nothing has a colour under monochrome, not even ordinary text:
	 * the point of it is the label's own foreground throughout. */
	if (ai_theme_is_monochrome(style_theme))
		return FALSE;

	if (ai_theme_is_native(style_theme))
	{
		guint rgb = style_native_colour(role);

		/*
		 * DEFAULT and TEXT inherit here. Under `terminal` the window has
		 * not recoloured itself, so prose must be whatever the GTK theme
		 * draws labels in -- including a high-contrast or a third-party
		 * stylesheet this file has never heard of.
		 */
		if (rgb == 0)
			return FALSE;

		style_rgba_from_rgb(rgb, out_rgba);
		return TRUE;
	}

	style_rgba_from_rgb(ai_theme_colour(style_theme, role), out_rgba);
	return TRUE;
}

/* ================================================================
 * Applying a palette
 * ================================================================ */

static void
style_append_colour(
	GString     *css,
	const gchar *name,
	guint        rgb
){
	g_string_append_printf(css, "@define-color %s #%06x;\n", name, rgb);
}

/*
 * A palette is applied by redefining libadwaita's own named colours.
 *
 * That is the documented way to recolour a libadwaita application, and
 * it is what makes the header bar, the popovers, the dialogs and the
 * scrollbars part of the same theme. Styling the transcript alone would
 * have produced catppuccin text inside Adwaita-blue chrome.
 */
static gchar *
style_build_palette_css(const AiTheme *theme)
{
	g_autoptr(GString) css = g_string_new(NULL);

	style_append_colour(css, "window_bg_color", theme->background);
	style_append_colour(css, "window_fg_color", theme->text);
	style_append_colour(css, "view_bg_color", theme->background);
	style_append_colour(css, "view_fg_color", theme->text);
	style_append_colour(css, "headerbar_bg_color", theme->surface);
	style_append_colour(css, "headerbar_fg_color", theme->text);
	style_append_colour(css, "headerbar_border_color", theme->text);
	style_append_colour(css, "sidebar_bg_color", theme->surface);
	style_append_colour(css, "sidebar_fg_color", theme->text);
	style_append_colour(css, "secondary_sidebar_bg_color", theme->surface);
	style_append_colour(css, "secondary_sidebar_fg_color", theme->text);
	style_append_colour(css, "card_bg_color", theme->surface);
	style_append_colour(css, "card_fg_color", theme->text);
	style_append_colour(css, "dialog_bg_color", theme->surface);
	style_append_colour(css, "dialog_fg_color", theme->text);
	style_append_colour(css, "popover_bg_color", theme->surface);
	style_append_colour(css, "popover_fg_color", theme->text);

	style_append_colour(css, "accent_color", theme->accent);
	style_append_colour(css, "accent_bg_color", theme->accent);
	/*
	 * Text *on* the accent, which is the background rather than the
	 * palette's foreground: an accent is chosen to contrast with the
	 * page, so the page is what reads on top of it.
	 */
	style_append_colour(css, "accent_fg_color", theme->background);

	style_append_colour(css, "success_color", theme->green);
	style_append_colour(css, "success_bg_color", theme->green);
	style_append_colour(css, "success_fg_color", theme->background);
	style_append_colour(css, "warning_color", theme->yellow);
	style_append_colour(css, "warning_bg_color", theme->yellow);
	style_append_colour(css, "warning_fg_color", theme->background);
	style_append_colour(css, "error_color", theme->red);
	style_append_colour(css, "error_bg_color", theme->red);
	style_append_colour(css, "error_fg_color", theme->background);
	style_append_colour(css, "destructive_color", theme->red);
	style_append_colour(css, "destructive_bg_color", theme->red);
	style_append_colour(css, "destructive_fg_color", theme->background);

	return g_string_free(g_steal_pointer(&css), FALSE);
}

static AdwColorScheme
style_requested_scheme(void)
{
	if (g_strcmp0(style_color_scheme, "light") == 0)
		return ADW_COLOR_SCHEME_FORCE_LIGHT;

	if (g_strcmp0(style_color_scheme, "dark") == 0)
		return ADW_COLOR_SCHEME_FORCE_DARK;

	return ADW_COLOR_SCHEME_DEFAULT;
}

static void
style_apply(void)
{
	AdwStyleManager *manager = adw_style_manager_get_default();
	GdkDisplay *display = gdk_display_get_default();

	if (ai_theme_is_native(style_theme))
	{
		/*
		 * No palette of our own: drop the overrides so the GTK theme
		 * shows through, and honour the light/dark preference.
		 */
		if (style_palette_provider != NULL && display != NULL)
		{
			gtk_style_context_remove_provider_for_display(display,
				GTK_STYLE_PROVIDER(style_palette_provider));
		}

		g_clear_object(&style_palette_provider);
		adw_style_manager_set_color_scheme(manager, style_requested_scheme());
		style_dark = adw_style_manager_get_dark(manager);
		style_notify();
		return;
	}

	/*
	 * A palette states its own light or dark. Forcing the scheme to
	 * match is not overriding the person's preference so much as
	 * refusing to render a near-black page with light-theme widget
	 * internals, which looks like a bug rather than like a choice.
	 */
	adw_style_manager_set_color_scheme(manager,
		ai_theme_is_dark(style_theme) ? ADW_COLOR_SCHEME_FORCE_DARK
		                              : ADW_COLOR_SCHEME_FORCE_LIGHT);
	style_dark = ai_theme_is_dark(style_theme);

	if (display == NULL)
	{
		style_notify();
		return;
	}

	if (style_palette_provider == NULL)
	{
		style_palette_provider = gtk_css_provider_new();
		/*
		 * Above the application stylesheet, so a palette's named
		 * colours are in force by the time APP_CSS's `@error_color`
		 * references are resolved.
		 */
		gtk_style_context_add_provider_for_display(display,
			GTK_STYLE_PROVIDER(style_palette_provider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}

	{
		g_autofree gchar *css = style_build_palette_css(style_theme);

		gtk_css_provider_load_from_string(style_palette_provider, css);
	}

	style_notify();
}

gboolean
ai_gui_style_set_theme(const gchar *name)
{
	const AiTheme *theme = ai_theme_find(name, NULL);

	if (theme == NULL)
		return FALSE;

	style_theme = theme;
	style_apply();

	return TRUE;
}

const gchar *
ai_gui_style_get_theme(void)
{
	return style_theme != NULL ? style_theme->name : ai_theme_get(0)->name;
}

const gchar *
ai_gui_style_cycle_theme(void)
{
	guint index = 0;

	if (style_theme != NULL)
		ai_theme_find(style_theme->name, &index);

	style_theme = ai_theme_get((index + 1) % ai_theme_count());
	style_apply();

	return style_theme->name;
}

gboolean
ai_gui_style_set_color_scheme(const gchar *name)
{
	if (g_strcmp0(name, "system") != 0 && g_strcmp0(name, "light") != 0 &&
	    g_strcmp0(name, "dark") != 0)
	{
		return FALSE;
	}

	g_free(style_color_scheme);
	style_color_scheme = g_strdup(name);
	style_apply();

	return TRUE;
}

const gchar *
ai_gui_style_get_color_scheme(void)
{
	return style_color_scheme != NULL ? style_color_scheme : "system";
}

static void
on_scheme_changed(
	GObject    *manager,
	GParamSpec *pspec,
	gpointer    user_data
){
	/*
	 * Only meaningful while no palette is forcing the scheme. When one
	 * is, this fires because *we* forced it and the answer is already
	 * what style_apply() recorded.
	 */
	if (!ai_theme_is_native(style_theme))
		return;

	style_dark = adw_style_manager_get_dark(ADW_STYLE_MANAGER(manager));
	style_notify();
}

void
ai_gui_style_init(
	const gchar *theme,
	const gchar *color_scheme
){
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

	if (!ai_gui_style_set_color_scheme(color_scheme))
		ai_gui_style_set_color_scheme("system");

	if (!ai_gui_style_set_theme(theme))
		ai_gui_style_set_theme(ai_theme_get(0)->name);
}

/* ================================================================
 * Pango attributes
 * ================================================================ */

static const AiGuiTypeface *
style_typeface(AiStyleTag tag)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(TYPEFACES); i++)
	{
		if (TYPEFACES[i].tag == tag)
			return &TYPEFACES[i];
	}

	return NULL;
}

static void
style_add(
	PangoAttrList  *attributes,
	PangoAttribute *attribute,
	guint           start,
	guint           len
){
	attribute->start_index = start;
	attribute->end_index = start + len;
	pango_attr_list_insert(attributes, attribute);
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
		const AiGuiTypeface *typeface;
		AiThemeEmphasis emphasis;
		AiStyleTag tag = AI_STYLE_DEFAULT;
		GdkRGBA rgba;
		guint start = 0;
		guint len = 0;

		if (!ai_rendered_text_get_span(rendered, i, &start, &len, &tag))
			continue;

		if (len == 0)
			continue;

		if (ai_gui_style_tag_colour(tag, &rgba))
		{
			style_add(attributes, pango_attr_foreground_new(
				(guint16)(rgba.red * 65535.0),
				(guint16)(rgba.green * 65535.0),
				(guint16)(rgba.blue * 65535.0)), start, len);
		}

		emphasis = ai_theme_emphasis_for_tag(tag);

		if (emphasis & AI_THEME_EMPHASIS_BOLD)
		{
			style_add(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD),
			          start, len);
		}

		if (emphasis & AI_THEME_EMPHASIS_UNDERLINE)
		{
			style_add(attributes,
			          pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
			          start, len);
		}

		/*
		 * Faint only where there is no colour, which is the rule ai-tui
		 * has always followed: a palette's muted entry has already said
		 * "quiet", and saying it twice would make the same tag look
		 * different in the two front-ends.
		 */
		if ((emphasis & AI_THEME_EMPHASIS_FAINT) &&
		    ai_theme_is_monochrome(style_theme))
		{
			style_add(attributes, pango_attr_foreground_alpha_new(0xaaaa),
			          start, len);
		}

		typeface = style_typeface(tag);

		if (typeface != NULL && typeface->monospace)
		{
			style_add(attributes, pango_attr_family_new("monospace"),
			          start, len);
		}

		if (typeface != NULL && typeface->italic)
		{
			style_add(attributes, pango_attr_style_new(PANGO_STYLE_ITALIC),
			          start, len);
		}
	}

	return attributes;
}
