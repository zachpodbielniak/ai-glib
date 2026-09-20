/*
 * ai-gui-prefs.c - Everything the command line can say, as a dialog
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-prefs.h"
#include "ai-gui-style.h"

#include "core/ai-theme.h"

typedef struct
{
	AiGuiSession *session;
	AiGuiWindow  *window;
	AdwDialog    *dialog;
	GtkWidget    *provider_row;
	GtkWidget    *model_row;
	GtkWidget    *directory_row;
	GStrv         provider_names;
} AiGuiPrefs;

static void
prefs_free(
	gpointer  data,
	GClosure *closure
){
	AiGuiPrefs *prefs = data;

	g_clear_object(&prefs->session);
	g_clear_object(&prefs->window);
	g_strfreev(prefs->provider_names);
	g_free(prefs);
}

/* ================================================================
 * Session page
 * ================================================================ */

static GStrv
prefs_provider_names(void)
{
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	GEnumClass *klass = g_type_class_ref(AI_TYPE_PROVIDER_TYPE);
	guint i;

	/*
	 * Straight off the enum rather than a list written out here.
	 *
	 * A provider added to the library appears in this dialog without
	 * anybody remembering to come back, which is the same reason
	 * `ai --list-providers` reads the enum too.
	 */
	for (i = 0; i < klass->n_values; i++)
		g_strv_builder_add(builder, klass->values[i].value_nick);

	g_type_class_unref(klass);

	return g_strv_builder_end(builder);
}

static void
on_apply_provider(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiPrefs *prefs = user_data;
	g_autoptr(GError) error = NULL;
	const gchar *model;
	guint selected;

	selected = adw_combo_row_get_selected(ADW_COMBO_ROW(prefs->provider_row));
	model = gtk_editable_get_text(GTK_EDITABLE(prefs->model_row));

	if (prefs->provider_names[selected] == NULL)
		return;

	if (!ai_gui_session_switch_provider(prefs->session,
		prefs->provider_names[selected],
		model != NULL && *model != '\0' ? model : NULL, &error))
	{
		AdwDialog *alert = adw_alert_dialog_new("Provider unchanged",
			error != NULL ? error->message : "the switch failed");

		adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "Close");
		adw_dialog_present(alert, GTK_WIDGET(prefs->dialog));
		return;
	}

	{
		AdwDialog *alert = adw_alert_dialog_new("Provider switched",
			"Context from the previous provider was carried across where "
			"its transcript could be read.");

		adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "Close");
		adw_dialog_present(alert, GTK_WIDGET(prefs->dialog));
	}
}

static void
on_save_defaults(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiPrefs *prefs = user_data;
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(GError) error = NULL;
	AiProviderType type;
	guint selected;

	selected = adw_combo_row_get_selected(ADW_COMBO_ROW(prefs->provider_row));

	if (prefs->provider_names[selected] == NULL)
		return;

	type = ai_provider_type_from_string(prefs->provider_names[selected]);

	if (!ai_config_save_defaults(config, "ai-gui", type,
		gtk_editable_get_text(GTK_EDITABLE(prefs->model_row)), &error))
	{
		/*
		 * g_message, not g_warning: a config directory that will not
		 * take a write is the caller's machine, not a bug in this
		 * program, and under fatal-warnings a warning would abort.
		 */
		g_message("ai-gui: could not save defaults: %s",
		          error != NULL ? error->message : "unknown");
	}
}

static void
on_stream_changed(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiSession *session = user_data;

	ai_conversation_set_stream(ai_gui_session_get_conversation(session),
		adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
	ai_gui_session_get_options(session)->stream =
		adw_switch_row_get_active(ADW_SWITCH_ROW(row));
}

static void
on_local_tools_changed(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiSession *session = user_data;
	gboolean wanted = adw_switch_row_get_active(ADW_SWITCH_ROW(row));

	ai_conversation_set_local_tools(
		ai_gui_session_get_conversation(session), wanted);

	/*
	 * The conversation refuses local tools for a wrapped CLI, which runs
	 * its own. Reading the value back rather than trusting the switch is
	 * what keeps the dialog from claiming something that did not happen.
	 */
	adw_switch_row_set_active(ADW_SWITCH_ROW(row),
		ai_conversation_get_local_tools(
			ai_gui_session_get_conversation(session)));
}

static void
on_approve_changed(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	ai_gui_session_set_approve_all(user_data,
		adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
}

static void
on_max_tokens_changed(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiSession *session = user_data;
	gint value = (gint)adw_spin_row_get_value(ADW_SPIN_ROW(row));

	ai_conversation_set_max_tokens(
		ai_gui_session_get_conversation(session), value);
	ai_gui_session_get_options(session)->max_tokens = value;
}

static void
on_directory_chosen(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiPrefs *prefs = user_data;
	g_autoptr(GFile) folder = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
	                                              result, &error);

	if (folder == NULL)
	{
		g_debug("ai-gui: no directory chosen: %s",
		        error != NULL ? error->message : "dismissed");
		return;
	}

	path = g_file_get_path(folder);

	if (path == NULL)
		return;

	ai_gui_session_set_working_directory(prefs->session, path);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(prefs->directory_row), path);
}

static void
on_choose_directory(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiPrefs *prefs = user_data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();

	gtk_file_dialog_set_title(dialog, "Working directory");
	gtk_file_dialog_select_folder(dialog, NULL, NULL, on_directory_chosen,
	                              prefs);
}

static AdwPreferencesPage *
prefs_build_session_page(AiGuiPrefs *prefs)
{
	AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
	AdwPreferencesGroup *model_group;
	AdwPreferencesGroup *turn_group;
	AdwPreferencesGroup *context_group;
	AiGuiOptions *options = ai_gui_session_get_options(prefs->session);
	AiConversation *conversation =
		ai_gui_session_get_conversation(prefs->session);
	guint i;

	adw_preferences_page_set_title(page, "Session");
	adw_preferences_page_set_icon_name(page, "emblem-system-symbolic");

	/* ---- model ---- */

	model_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(model_group, "Model");
	adw_preferences_group_set_description(model_group,
		"Switching carries what the previous provider's own transcript "
		"could be read from.");

	prefs->provider_names = prefs_provider_names();

	{
		g_autoptr(GtkStringList) names = gtk_string_list_new(
			(const gchar * const *)prefs->provider_names);

		prefs->provider_row = adw_combo_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(prefs->provider_row),
		                              "Provider");
		adw_combo_row_set_model(ADW_COMBO_ROW(prefs->provider_row),
		                        G_LIST_MODEL(g_object_ref(names)));
	}

	for (i = 0; prefs->provider_names[i] != NULL; i++)
	{
		/* Matched on the canonical name, never the display name: the
		 * display name is "Claude Code" and the factory wants
		 * "claude-code". */
		AiProviderType type =
			ai_provider_type_from_string(prefs->provider_names[i]);
		GObject *provider = ai_gui_session_get_provider(prefs->session);

		if (provider != NULL &&
		    g_strcmp0(ai_provider_type_to_string(type),
		              ai_provider_type_to_string(
				ai_provider_get_provider_type(AI_PROVIDER(provider)))) == 0)
		{
			adw_combo_row_set_selected(ADW_COMBO_ROW(prefs->provider_row), i);
			break;
		}
	}

	adw_preferences_group_add(model_group, prefs->provider_row);

	prefs->model_row = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(prefs->model_row),
	                              "Model");
	gtk_editable_set_text(GTK_EDITABLE(prefs->model_row),
		ai_gui_session_get_model(prefs->session) != NULL
			? ai_gui_session_get_model(prefs->session) : "");
	adw_preferences_group_add(model_group, prefs->model_row);

	{
		GtkWidget *apply = gtk_button_new_with_label("Apply provider");
		GtkWidget *save = gtk_button_new_with_label("Save as default");
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

		gtk_widget_add_css_class(apply, "suggested-action");
		gtk_widget_set_halign(row, GTK_ALIGN_END);
		g_signal_connect(apply, "clicked", G_CALLBACK(on_apply_provider),
		                 prefs);
		g_signal_connect(save, "clicked", G_CALLBACK(on_save_defaults), prefs);
		gtk_box_append(GTK_BOX(row), save);
		gtk_box_append(GTK_BOX(row), apply);
		adw_preferences_group_add(model_group, row);
	}

	adw_preferences_page_add(page, model_group);

	/* ---- turn ---- */

	turn_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(turn_group, "Turn");

	{
		GtkWidget *row = adw_spin_row_new_with_range(256.0, 262144.0, 256.0);

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              "Maximum tokens");
		adw_spin_row_set_value(ADW_SPIN_ROW(row),
			(gdouble)ai_conversation_get_max_tokens(conversation));
		g_signal_connect(row, "notify::value",
		                 G_CALLBACK(on_max_tokens_changed), prefs->session);
		adw_preferences_group_add(turn_group, row);
	}

	{
		GtkWidget *row = adw_switch_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Stream");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
			"Show text as it arrives instead of waiting for the whole turn");
		adw_switch_row_set_active(ADW_SWITCH_ROW(row),
			ai_conversation_get_stream(conversation));
		g_signal_connect(row, "notify::active",
		                 G_CALLBACK(on_stream_changed), prefs->session);
		adw_preferences_group_add(turn_group, row);
	}

	{
		GtkWidget *row = adw_switch_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              "Run tools here");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
			"HTTP providers only — a wrapped CLI runs its own");
		adw_switch_row_set_active(ADW_SWITCH_ROW(row),
			ai_conversation_get_local_tools(conversation));
		g_signal_connect(row, "notify::active",
		                 G_CALLBACK(on_local_tools_changed), prefs->session);
		adw_preferences_group_add(turn_group, row);
	}

	{
		GtkWidget *row = adw_switch_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              "Approve every tool call");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
			"Stop asking before each local tool runs");
		adw_switch_row_set_active(ADW_SWITCH_ROW(row),
			ai_gui_session_get_approve_all(prefs->session));
		g_signal_connect(row, "notify::active",
		                 G_CALLBACK(on_approve_changed), prefs->session);
		adw_preferences_group_add(turn_group, row);
	}

	adw_preferences_page_add(page, turn_group);

	/* ---- context ---- */

	context_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(context_group, "Context");

	prefs->directory_row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(prefs->directory_row),
	                              "Working directory");
	adw_action_row_set_subtitle(ADW_ACTION_ROW(prefs->directory_row),
		ai_gui_session_get_working_directory(prefs->session));

	{
		GtkWidget *choose = gtk_button_new_from_icon_name("folder-open-symbolic");

		gtk_widget_add_css_class(choose, "flat");
		gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
		g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_directory),
		                 prefs);
		adw_action_row_add_suffix(ADW_ACTION_ROW(prefs->directory_row), choose);
	}

	adw_preferences_group_add(context_group, prefs->directory_row);

	{
		GtkWidget *row = adw_action_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              "System prompt");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
			options->system != NULL && *options->system != '\0'
				? options->system : "none");
		adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 4);
		adw_preferences_group_add(context_group, row);
	}

	{
		GtkWidget *row = adw_action_row_new();
		const gchar *carried =
			ai_conversation_get_carried_context(conversation);

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              "Carried context");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
			carried != NULL && *carried != '\0'
				? "A previous provider's transcript is being prepended"
				: "none");
		adw_preferences_group_add(context_group, row);
	}

	adw_preferences_page_add(page, context_group);

	return page;
}

/* ================================================================
 * Appearance page
 * ================================================================ */

static const gchar *const COLOR_SCHEMES[] = { "system", "light", "dark" };
static const gchar *const COLOR_SCHEME_LABELS[] = {
	"Follow the desktop", "Light", "Dark"
};

static void
on_theme_selected(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiPrefs *prefs = user_data;
	guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
	const AiTheme *theme = ai_theme_get(selected);

	if (g_strcmp0(theme->name, ai_gui_style_get_theme()) == 0)
		return;

	ai_gui_window_set_theme(prefs->window, theme->name);
}

static void
on_scheme_selected(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiPrefs *prefs = user_data;
	guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));

	if (selected >= G_N_ELEMENTS(COLOR_SCHEMES))
		return;

	ai_gui_window_set_color_scheme(prefs->window, COLOR_SCHEMES[selected]);
}

static AdwPreferencesPage *
prefs_build_appearance_page(AiGuiPrefs *prefs)
{
	AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
	AdwPreferencesGroup *group =
		ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	GtkWidget *theme_row;
	GtkWidget *scheme_row;
	guint i;

	adw_preferences_page_set_title(page, "Appearance");
	adw_preferences_page_set_icon_name(page, "applications-graphics-symbolic");

	adw_preferences_group_set_title(group, "Theme");
	adw_preferences_group_set_description(group,
		"The same palettes ai-tui draws in a terminal, so one session "
		"looks like the next whichever front-end it is in. Ctrl+T cycles.");

	/* ---- palette ---- */

	{
		g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
		guint selected = 0;

		for (i = 0; i < ai_theme_count(); i++)
		{
			gtk_string_list_append(names, ai_theme_get(i)->name);

			if (g_strcmp0(ai_theme_get(i)->name,
			              ai_gui_style_get_theme()) == 0)
			{
				selected = i;
			}
		}

		theme_row = adw_combo_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme_row), "Palette");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(theme_row),
			"terminal follows the desktop theme; monochrome uses no colour "
			"at all");
		adw_combo_row_set_model(ADW_COMBO_ROW(theme_row),
		                        G_LIST_MODEL(g_object_ref(names)));
		/*
		 * Selected before the handler is connected. Setting it after
		 * would fire a change for the value that was already in force,
		 * writing the settings file on every visit to this page.
		 */
		adw_combo_row_set_selected(ADW_COMBO_ROW(theme_row), selected);
		g_signal_connect(theme_row, "notify::selected",
		                 G_CALLBACK(on_theme_selected), prefs);
		adw_preferences_group_add(group, theme_row);
	}

	/* ---- light or dark ---- */

	{
		g_autoptr(GtkStringList) labels =
			gtk_string_list_new(COLOR_SCHEME_LABELS);
		guint selected = 0;

		for (i = 0; i < G_N_ELEMENTS(COLOR_SCHEMES); i++)
		{
			if (g_strcmp0(COLOR_SCHEMES[i],
			              ai_gui_style_get_color_scheme()) == 0)
			{
				selected = i;
			}
		}

		scheme_row = adw_combo_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scheme_row),
		                              "Light or dark");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(scheme_row),
			"Applies to terminal and monochrome. A named palette carries "
			"its own.");
		adw_combo_row_set_model(ADW_COMBO_ROW(scheme_row),
		                        G_LIST_MODEL(g_object_ref(labels)));
		adw_combo_row_set_selected(ADW_COMBO_ROW(scheme_row), selected);
		g_signal_connect(scheme_row, "notify::selected",
		                 G_CALLBACK(on_scheme_selected), prefs);
		adw_preferences_group_add(group, scheme_row);
	}

	adw_preferences_page_add(page, group);

	return page;
}

/* ================================================================
 * Provider page
 * ================================================================ */

static void
on_property_switch(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	GObject *provider = g_object_get_data(row, "ai-provider");
	const gchar *name = g_object_get_data(row, "ai-property");

	g_object_set(provider, name,
		adw_switch_row_get_active(ADW_SWITCH_ROW(row)), NULL);
}

static void
on_property_entry(
	GtkEditable *row,
	gpointer     user_data
){
	GObject *provider = g_object_get_data(G_OBJECT(row), "ai-provider");
	GParamSpec *pspec = g_object_get_data(G_OBJECT(row), "ai-pspec");
	GValue value = G_VALUE_INIT;

	if (!ai_gui_value_from_string(&value, pspec,
		gtk_editable_get_text(row)))
	{
		/*
		 * A half-typed number is not an error worth shouting about --
		 * somebody is still typing. The row keeps what it has and the
		 * next keystroke gets another go.
		 */
		g_value_unset(&value);
		gtk_widget_add_css_class(GTK_WIDGET(row), "error");
		return;
	}

	gtk_widget_remove_css_class(GTK_WIDGET(row), "error");
	g_object_set_property(provider, g_param_spec_get_name(pspec), &value);
	g_value_unset(&value);
}

static void
on_property_combo(
	GObject    *row,
	GParamSpec *pspec,
	gpointer    user_data
){
	GObject *provider = g_object_get_data(row, "ai-provider");
	GParamSpec *target = g_object_get_data(row, "ai-pspec");
	GEnumClass *klass = g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(target));
	guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));

	if (selected < klass->n_values)
	{
		g_object_set(provider, g_param_spec_get_name(target),
		             klass->values[selected].value, NULL);
	}

	g_type_class_unref(klass);
}

static GtkWidget *
prefs_property_row(
	GObject    *provider,
	GParamSpec *pspec
){
	GType type = G_PARAM_SPEC_VALUE_TYPE(pspec);
	const gchar *name = g_param_spec_get_name(pspec);
	GtkWidget *row;

	if (type == G_TYPE_BOOLEAN)
	{
		gboolean active = FALSE;

		row = adw_switch_row_new();
		g_object_get(provider, name, &active, NULL);
		adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
		g_object_set_data(G_OBJECT(row), "ai-provider", provider);
		g_object_set_data_full(G_OBJECT(row), "ai-property", g_strdup(name),
		                       g_free);
		g_signal_connect(row, "notify::active",
		                 G_CALLBACK(on_property_switch), NULL);
	}
	else if (G_TYPE_IS_ENUM(type))
	{
		GEnumClass *klass = g_type_class_ref(type);
		g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
		GValue value = G_VALUE_INIT;
		guint i;

		row = adw_combo_row_new();

		for (i = 0; i < klass->n_values; i++)
			gtk_string_list_append(names, klass->values[i].value_nick);

		adw_combo_row_set_model(ADW_COMBO_ROW(row),
		                        G_LIST_MODEL(g_object_ref(names)));

		g_value_init(&value, type);
		g_object_get_property(provider, name, &value);

		for (i = 0; i < klass->n_values; i++)
		{
			if (klass->values[i].value == g_value_get_enum(&value))
			{
				adw_combo_row_set_selected(ADW_COMBO_ROW(row), i);
				break;
			}
		}

		g_value_unset(&value);
		g_type_class_unref(klass);

		g_object_set_data(G_OBJECT(row), "ai-provider", provider);
		g_object_set_data(G_OBJECT(row), "ai-pspec", pspec);
		g_signal_connect(row, "notify::selected",
		                 G_CALLBACK(on_property_combo), NULL);
	}
	else
	{
		g_autofree gchar *text = ai_gui_value_to_string(provider, pspec);

		row = adw_entry_row_new();
		gtk_editable_set_text(GTK_EDITABLE(row), text);
		g_object_set_data(G_OBJECT(row), "ai-provider", provider);
		g_object_set_data(G_OBJECT(row), "ai-pspec", pspec);
		g_signal_connect(row, "changed", G_CALLBACK(on_property_entry), NULL);
	}

	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);

	if (g_param_spec_get_blurb(pspec) != NULL && ADW_IS_ACTION_ROW(row))
	{
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
		                            g_param_spec_get_blurb(pspec));
	}

	return row;
}

/*
 * Skipped because they are set elsewhere in this dialog, or because a
 * free-text row is the wrong shape for them. Everything else the
 * provider exposes appears, including knobs added after this file was
 * written -- which is the point.
 */
static gboolean
prefs_property_hidden(const gchar *name)
{
	static const gchar *hidden[] = {
		"model", "system-prompt", "max-tokens", "config", NULL
	};
	gsize i;

	for (i = 0; hidden[i] != NULL; i++)
	{
		if (g_strcmp0(name, hidden[i]) == 0)
			return TRUE;
	}

	return FALSE;
}

static AdwPreferencesPage *
prefs_build_provider_page(AiGuiPrefs *prefs)
{
	AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
	AdwPreferencesGroup *group =
		ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	GObject *provider = ai_gui_session_get_provider(prefs->session);
	g_autofree GParamSpec **specs = NULL;
	guint n_specs = 0;
	guint i;
	guint shown = 0;

	adw_preferences_page_set_title(page, "Provider");
	adw_preferences_page_set_icon_name(page, "preferences-other-symbolic");
	adw_preferences_group_set_title(group,
		provider != NULL
			? ai_provider_get_name(AI_PROVIDER(provider)) : "Provider");
	adw_preferences_group_set_description(group,
		"Every writable property this provider exposes, the same set "
		"`ai --set` reaches.");

	if (provider != NULL)
	{
		specs = g_object_class_list_properties(G_OBJECT_GET_CLASS(provider),
		                                       &n_specs);
	}

	for (i = 0; i < n_specs; i++)
	{
		GParamSpec *pspec = specs[i];
		const gchar *name = g_param_spec_get_name(pspec);

		if ((pspec->flags & G_PARAM_WRITABLE) == 0)
			continue;

		if ((pspec->flags & G_PARAM_READABLE) == 0)
			continue;

		if (prefs_property_hidden(name))
			continue;

		adw_preferences_group_add(group,
		                          prefs_property_row(provider, pspec));
		shown++;
	}

	if (shown == 0)
	{
		GtkWidget *row = adw_action_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
			"This provider has no settable properties");
		adw_preferences_group_add(group, row);
	}

	adw_preferences_page_add(page, group);

	return page;
}

/* ================================================================
 * Entry point
 * ================================================================ */

static void
on_prefs_closed(
	AdwDialog *dialog,
	gpointer   user_data
){
}

void
ai_gui_prefs_present(
	GtkWidget    *parent,
	AiGuiSession *session
){
	AiGuiPrefs *prefs;
	AdwPreferencesDialog *dialog;

	g_return_if_fail(AI_GUI_IS_SESSION(session));

	prefs = g_new0(AiGuiPrefs, 1);
	prefs->session = g_object_ref(session);
	prefs->window = GTK_IS_WIDGET(parent)
		? AI_GUI_WINDOW(gtk_widget_get_root(parent)) : NULL;

	if (prefs->window != NULL)
		g_object_ref(prefs->window);

	dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
	prefs->dialog = ADW_DIALOG(dialog);
	adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");

	adw_preferences_dialog_add(dialog, prefs_build_session_page(prefs));
	adw_preferences_dialog_add(dialog, prefs_build_provider_page(prefs));

	if (prefs->window != NULL)
		adw_preferences_dialog_add(dialog, prefs_build_appearance_page(prefs));

	/*
	 * The dialog owns the state for exactly as long as it exists.
	 *
	 * The handler does nothing; the closure's destroy notify is the
	 * point. Freeing from the handler instead would leave the closure
	 * holding a pointer to freed memory until GTK got round to
	 * disconnecting it.
	 */
	g_signal_connect_data(dialog, "closed", G_CALLBACK(on_prefs_closed),
	                      prefs, prefs_free, 0);

	adw_dialog_present(ADW_DIALOG(dialog), parent);
}
