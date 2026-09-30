/*
 * ai-gui-loops.c - See and edit a session's loops and goals
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Every word here comes from the library -- the state names, "next in
 * 4m", "turn 3/20" -- and every change is a /loop or /goal line handed to
 * the session's runner, so the window can neither say something the
 * terminal would not nor accept an edit the slash command would refuse.
 */

#include "ai-gui-loops.h"
#include "ai-gui-util.h"

typedef struct
{
	AiGuiSession *session;
	AiLoopRunner *runner;
	GtkWidget    *toasts;
	GtkWidget    *loops_group;
	GtkWidget    *goals_group;
	GPtrArray    *rows;
	GSource      *tick;
	gulong        changed_id;
} AiGuiLoops;

static void loops_rebuild(AiGuiLoops *view);

static void
loops_free(
	gpointer  data,
	GClosure *closure
){
	AiGuiLoops *view = data;

	if (view->tick != NULL)
	{
		/* By pointer: see the main-context note in AGENTS.md. */
		g_source_destroy(view->tick);
		g_clear_pointer(&view->tick, g_source_unref);
	}

	if (view->runner != NULL)
		g_clear_signal_handler(&view->changed_id, view->runner);

	g_clear_object(&view->runner);
	g_clear_object(&view->session);
	g_clear_pointer(&view->rows, g_ptr_array_unref);
	g_free(view);
}

static void
toast(
	GtkWidget   *overlay,
	const gchar *text
){
	AdwToast *item = adw_toast_new(text);

	adw_toast_set_timeout(item, 6);
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), item);
}

/*
 * Run one /loop or /goal line against the session's schedule. The list
 * redraws from the runner's ::changed, not from here, so an edit dialog
 * that outlives the list has nothing of the list to reach for.
 */
static gboolean
apply_line(
	AiLoopRunner *runner,
	GtkWidget    *overlay,
	const gchar  *kind,
	const gchar  *line
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *notice = NULL;

	notice = ai_loop_runner_command(runner, kind, line, g_get_real_time(), &error);

	if (notice == NULL)
	{
		toast(overlay, error->message);
		return FALSE;
	}

	/* The first line of a notice is the whole answer for a toast. */
	{
		g_auto(GStrv) lines = g_strsplit(notice, "\n", 2);
		toast(overlay, lines[0]);
	}

	return TRUE;
}

static gboolean
loops_apply(
	AiGuiLoops  *view,
	const gchar *kind,
	const gchar *line
){
	return apply_line(view->runner, view->toasts, kind, line);
}

static void
on_verb(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiLoops  *view = user_data;
	const gchar *kind = g_object_get_data(G_OBJECT(button), "ai-loop-kind");
	const gchar *verb = g_object_get_data(G_OBJECT(button), "ai-loop-verb");
	const gchar *id = g_object_get_data(G_OBJECT(button), "ai-loop-id");
	g_autofree gchar *line = g_strdup_printf("%s %s", verb, id);

	loops_apply(view, kind, line);
}

/* ================================================================
 * Editing
 * ================================================================ */

typedef struct
{
	AiLoopRunner *runner;
	GtkWidget    *toasts;
	gchar      *id;
	gboolean    goal;
	GtkWidget  *first;
	GtkWidget  *second;
	GtkWidget  *third;
	gchar      *first_was;
	gchar      *second_was;
	gchar      *third_was;
} LoopsEdit;

static void
loops_edit_free(LoopsEdit *edit)
{
	g_clear_object(&edit->runner);
	g_clear_object(&edit->toasts);
	g_free(edit->id);
	g_free(edit->first_was);
	g_free(edit->second_was);
	g_free(edit->third_was);
	g_free(edit);
}

/* A field somebody did not touch is not part of the edit. */
static const gchar *
changed(
	GtkWidget   *row,
	const gchar *was
){
	const gchar *now = gtk_editable_get_text(GTK_EDITABLE(row));

	return g_strcmp0(now, was) != 0 ? now : NULL;
}

static void
on_edit_response(
	AdwAlertDialog *dialog,
	const gchar    *response,
	gpointer        user_data
){
	LoopsEdit        *edit = user_data;
	g_autofree gchar *line = NULL;

	if (g_strcmp0(response, "save") == 0)
	{
		if (edit->goal)
			line = ai_gui_loops_edit_line(edit->id, TRUE, NULL, NULL,
				changed(edit->second, edit->second_was),
				changed(edit->third, edit->third_was),
				changed(edit->first, edit->first_was));
		else
			line = ai_gui_loops_edit_line(edit->id, FALSE,
				changed(edit->first, edit->first_was),
				changed(edit->second, edit->second_was),
				NULL, NULL, NULL);

		if (line != NULL)
			apply_line(edit->runner, edit->toasts, edit->goal ? "goal" : "loop", line);
	}

	loops_edit_free(edit);
}

static GtkWidget *
edit_row(
	GtkWidget   *list,
	const gchar *title,
	const gchar *text
){
	GtkWidget *row = adw_entry_row_new();

	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), text != NULL ? text : "");
	gtk_list_box_append(GTK_LIST_BOX(list), row);
	return row;
}

static void
on_edit(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiLoops     *view = user_data;
	const gchar    *id = g_object_get_data(G_OBJECT(button), "ai-loop-id");
	AiLoopSchedule *schedule = ai_loop_runner_get_schedule(view->runner);
	gint            index = ai_loop_schedule_find(schedule, id);
	LoopsEdit      *edit;
	AdwDialog      *dialog;
	GtkWidget      *list;

	if (index < 0)
	{
		loops_rebuild(view);
		return;
	}

	edit = g_new0(LoopsEdit, 1);
	edit->runner = g_object_ref(view->runner);
	edit->toasts = g_object_ref(view->toasts);
	edit->id = g_strdup(id);
	edit->goal = ai_loop_schedule_get_kind(schedule, (guint)index) == AI_LOOP_KIND_GOAL;

	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");

	if (edit->goal)
	{
		edit->first_was = g_strdup(ai_loop_schedule_get_condition(schedule, (guint)index));
		edit->second_was = g_strdup_printf("%u", ai_loop_schedule_get_max_turns(schedule, (guint)index));
		edit->third_was = ai_loop_format_duration(ai_loop_schedule_get_max_duration_us(schedule, (guint)index));
		edit->first = edit_row(list, "Condition", edit->first_was);
		edit->second = edit_row(list, "Turn bound (at most 200)", edit->second_was);
		edit->third = edit_row(list, "Time bound, from the start (30m, 4h, 2d)", edit->third_was);
	}
	else
	{
		const gchar *prompt = ai_loop_schedule_get_prompt(schedule, (guint)index);

		edit->first_was = ai_gui_loops_interval_text(ai_loop_schedule_get_interval_us(schedule, (guint)index));
		edit->second_was = g_strdup(prompt != NULL ? prompt : "");
		edit->first = edit_row(list, "Every (10m, 2h, 1d, or self-paced)", edit->first_was);
		edit->second = edit_row(list, "Prompt or /command (empty: maintenance prompt)", edit->second_was);
	}

	dialog = adw_alert_dialog_new(edit->goal ? "Edit goal" : "Edit loop", id);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), list);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
		"cancel", "Cancel", "save", "Save", NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
		"save", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	g_signal_connect(dialog, "response", G_CALLBACK(on_edit_response), edit);
	adw_dialog_present(dialog, GTK_WIDGET(button));
}

/* ================================================================
 * The list
 * ================================================================ */

static GtkWidget *
verb_button(
	AiGuiLoops  *view,
	const gchar *icon,
	const gchar *tooltip,
	const gchar *kind,
	const gchar *verb,
	const gchar *id
){
	GtkWidget *button = gtk_button_new_from_icon_name(icon);

	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(button, tooltip);
	g_object_set_data(G_OBJECT(button), "ai-loop-kind", (gpointer)kind);
	g_object_set_data(G_OBJECT(button), "ai-loop-verb", (gpointer)verb);
	g_object_set_data_full(G_OBJECT(button), "ai-loop-id", g_strdup(id), g_free);
	g_signal_connect(button, "clicked",
	                 G_CALLBACK(g_strcmp0(verb, "edit") == 0 ? G_CALLBACK(on_edit) : G_CALLBACK(on_verb)),
	                 view);
	return button;
}

static void
loops_clear_rows(AiGuiLoops *view)
{
	guint i;

	/* Remembered rather than rediscovered: an AdwPreferencesGroup's
	 * children are its internal box, not its rows (see ai-gui-agents.c). */
	for (i = 0; i < view->rows->len; i++)
	{
		GtkWidget *row = g_ptr_array_index(view->rows, i);
		GtkWidget *group = g_object_get_data(G_OBJECT(row), "ai-loop-group");

		adw_preferences_group_remove(ADW_PREFERENCES_GROUP(group), row);
	}

	g_ptr_array_set_size(view->rows, 0);
}

static void
loops_add_row(
	AiGuiLoops *view,
	GtkWidget  *group,
	GtkWidget  *row
){
	g_object_set_data(G_OBJECT(row), "ai-loop-group", group);
	g_ptr_array_add(view->rows, row);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
}

static void
loops_rebuild(AiGuiLoops *view)
{
	AiLoopSchedule *schedule = ai_loop_runner_get_schedule(view->runner);
	gint64          now = g_get_real_time();
	guint           n = ai_loop_schedule_get_n_tasks(schedule);
	guint           loops = 0;
	guint           goals = 0;
	guint           i;

	loops_clear_rows(view);

	for (i = 0; i < n; i++)
	{
		AiLoopKind        kind = ai_loop_schedule_get_kind(schedule, i);
		AiLoopState       state = ai_loop_schedule_get_state(schedule, i);
		const gchar      *id = ai_loop_schedule_get_id(schedule, i);
		const gchar      *kind_name = ai_loop_kind_to_string(kind);
		const gchar      *reason = ai_loop_schedule_get_reason(schedule, i);
		g_autofree gchar *excerpt = ai_loop_schedule_dup_excerpt(schedule, i);
		g_autofree gchar *status = ai_loop_schedule_dup_status(schedule, i, now);
		g_autofree gchar *subtitle = NULL;
		GtkWidget        *row = adw_action_row_new();

		/* A prompt is text, not markup: "<b>" in one is literal. */
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), excerpt);

		if (kind == AI_LOOP_KIND_GOAL && reason != NULL)
			subtitle = g_strdup_printf("%s · %s · %s", id, status, reason);
		else
			subtitle = g_strdup_printf("%s · %s", id, status);

		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
		adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);

		if (!ai_loop_state_is_final(state))
		{
			if (state == AI_LOOP_STATE_PAUSED)
				adw_action_row_add_suffix(ADW_ACTION_ROW(row),
					verb_button(view, "media-playback-start-symbolic", "Resume", kind_name, "resume", id));
			else
				adw_action_row_add_suffix(ADW_ACTION_ROW(row),
					verb_button(view, "media-playback-pause-symbolic", "Pause", kind_name, "pause", id));

			adw_action_row_add_suffix(ADW_ACTION_ROW(row),
				verb_button(view, "media-skip-forward-symbolic",
				            "Run now (after any turn in progress)", kind_name, "run", id));
			adw_action_row_add_suffix(ADW_ACTION_ROW(row),
				verb_button(view, "document-edit-symbolic", "Edit", kind_name, "edit", id));

			if (kind == AI_LOOP_KIND_GOAL)
				adw_action_row_add_suffix(ADW_ACTION_ROW(row),
					verb_button(view, "process-stop-symbolic", "Stop: end it as not met", kind_name, "stop", id));
		}
		else if (state == AI_LOOP_STATE_EXPIRED)
		{
			/* Raising the bounds is how an expired goal gets more room. */
			adw_action_row_add_suffix(ADW_ACTION_ROW(row),
				verb_button(view, "document-edit-symbolic", "Raise the bounds", kind_name, "edit", id));
		}

		adw_action_row_add_suffix(ADW_ACTION_ROW(row),
			verb_button(view, "user-trash-symbolic", "Delete", kind_name, "delete", id));

		if (kind == AI_LOOP_KIND_GOAL)
		{
			loops_add_row(view, view->goals_group, row);
			goals++;
		}
		else
		{
			loops_add_row(view, view->loops_group, row);
			loops++;
		}
	}

	if (loops == 0)
	{
		GtkWidget *row = adw_action_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No loops");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "/loop 10m check the deploy");
		loops_add_row(view, view->loops_group, row);
	}

	if (goals == 0)
	{
		GtkWidget *row = adw_action_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No goals");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "/goal the tests pass --turns 10");
		loops_add_row(view, view->goals_group, row);
	}
}

static void
on_changed(
	AiLoopRunner *runner,
	gpointer      user_data
){
	loops_rebuild(user_data);
}

static gboolean
on_tick(gpointer user_data)
{
	/* "next in 4m" moves on its own; nobody needs it finer than this. */
	loops_rebuild(user_data);
	return G_SOURCE_CONTINUE;
}

static void
on_add(
	AdwEntryRow *entry,
	gpointer     user_data
){
	AiGuiLoops       *view = user_data;
	g_autofree gchar *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));

	if (g_str_has_prefix(text, "/loop ") || g_str_has_prefix(text, "/goal "))
	{
		if (loops_apply(view, g_str_has_prefix(text, "/goal ") ? "goal" : "loop", text + 6))
			gtk_editable_set_text(GTK_EDITABLE(entry), "");
	}
	else
	{
		toast(view->toasts, "Start with /loop or /goal, as in the message box.");
	}
}

static void
on_closed(
	AdwDialog *dialog,
	gpointer   user_data
){
}

void
ai_gui_loops_present(
	GtkWidget    *parent,
	AiGuiSession *session
){
	AiGuiLoops         *view;
	AdwDialog          *dialog;
	AdwPreferencesPage *page;
	GtkWidget          *toolbar;
	GtkWidget          *add_group;
	GtkWidget          *add;

	g_return_if_fail(AI_GUI_IS_SESSION(session));

	view = g_new0(AiGuiLoops, 1);
	view->session = g_object_ref(session);
	view->runner = g_object_ref(ai_gui_session_get_loops(session));
	view->rows = g_ptr_array_new();

	dialog = adw_dialog_new();
	adw_dialog_set_title(dialog, "Loops and goals");
	adw_dialog_set_content_width(dialog, 640);
	adw_dialog_set_content_height(dialog, 560);

	page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

	view->loops_group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(view->loops_group), "Loops");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(view->loops_group),
		"A prompt or /command on an interval, or at a pace the model picks. "
		"Never sent mid-turn: a loop that comes due waits, then runs once. "
		"Loops end 7 days after they are created.");
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(view->loops_group));

	view->goals_group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(view->goals_group), "Goals");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(view->goals_group),
		"Turns that continue until the condition holds, judged after each "
		"turn by the model's own verdict. Every goal has a turn and a time "
		"bound; one that reaches either is expired, not met.");
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(view->goals_group));

	add_group = adw_preferences_group_new();
	add = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add), "Add: /loop 10m check CI, or /goal the tests pass");
	adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(add), TRUE);
	g_signal_connect(add, "apply", G_CALLBACK(on_add), view);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group), add);
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(add_group));

	view->toasts = adw_toast_overlay_new();
	adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(view->toasts), GTK_WIDGET(page));

	toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), view->toasts);
	adw_dialog_set_child(dialog, toolbar);

	loops_rebuild(view);
	view->changed_id = g_signal_connect(view->runner, "changed", G_CALLBACK(on_changed), view);

	view->tick = g_timeout_source_new_seconds(20);
	g_source_set_callback(view->tick, on_tick, view, NULL);
	g_source_attach(view->tick, g_main_context_get_thread_default());

	g_signal_connect_data(dialog, "closed", G_CALLBACK(on_closed), view,
	                      loops_free, 0);

	adw_dialog_present(dialog, parent);
}
