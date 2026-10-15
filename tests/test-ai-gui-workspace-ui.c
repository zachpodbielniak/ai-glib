/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <glib.h>
#ifdef AI_TEST_GTK
#include "ai-gui-queue.h"
#include "ai-gui-dashboard.h"
#include "ai-gui-util.h"
#include "ai-gui.h"
#include "ai-gui-composer.h"
#include <glib/gstdio.h>
#include <unistd.h>

/* The application entry point owns process replacement. No workspace action
 * should ask for an update/restart in this isolated widget harness. */
void
ai_gui_request_restart(const gchar *path)
{
	g_assert_not_reached();
}

static void
collect(GtkWidget *widget, GType type, GPtrArray *found)
{
	GtkWidget *child;
	if (g_type_is_a(G_OBJECT_TYPE(widget), type)) g_ptr_array_add(found, widget);
	for (child = gtk_widget_get_first_child(widget); child != NULL; child = gtk_widget_get_next_sibling(child))
		collect(child, type, found);
}

static GtkWidget *
button_named(GtkWidget *root, const gchar *label)
{
	g_autoptr(GPtrArray) buttons = g_ptr_array_new();
	guint i;
	collect(root, GTK_TYPE_BUTTON, buttons);
	for (i = 0; i < buttons->len; i++) {
		GtkWidget *button = g_ptr_array_index(buttons, i);
		if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(button)), label) == 0) return button;
	}
	return NULL;
}

static void
drain(void)
{
	guint i;
	for (i = 0; i < 50; i++) {
		while (g_main_context_iteration(NULL, FALSE));
		g_usleep(10000);
	}
}

static void
demonstrate(GtkWidget *window, const gchar *name)
{
	const gchar *directory = g_getenv("AI_GUI_DEMONSTRATE");
	g_autoptr(GdkPaintable) paintable = NULL;
	g_autoptr(GskRenderNode) node = NULL;
	g_autoptr(GdkTexture) texture = NULL;
	g_autofree gchar *path = NULL;
	GtkSnapshot *snapshot;
	if (directory == NULL) return;
	paintable = gtk_widget_paintable_new(window);
	gtk_widget_queue_draw(window);
	drain();
	snapshot = gtk_snapshot_new();
	gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(window), gtk_widget_get_height(window));
	node = gtk_snapshot_free_to_node(snapshot);
	g_assert_nonnull(node);
	texture = gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(window)), node, NULL);
	path = g_build_filename(directory, name, NULL);
	g_assert_true(gdk_texture_save_to_png(texture, path));
	g_test_message("DEMONSTRATION: %s", path);
}

static void
test_queue_dialog(void)
{
	g_autoptr(AiGuiOptions) options = g_new0(AiGuiOptions, 1);
	g_autoptr(AiGuiSession) session = NULL;
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(GPtrArray) views = g_ptr_array_new();
	g_autoptr(GPtrArray) checks = g_ptr_array_new();
	g_autofree gchar *sent = NULL;
	AiPromptQueue *queue;
	GtkWidget *window, *save;
	AdwDialog *dialog;
	options->working_directory = g_get_current_dir();
	options->provider = g_strdup("ollama");
	options->model = g_strdup("fixture-model");
	options->max_tokens = 1024;
	session = ai_gui_session_new(options, "ollama", NULL, NULL);
	g_assert_nonnull(session);
	g_assert_true(ai_conversation_set_provider(ai_gui_session_get_conversation(session), G_OBJECT(mock), NULL));
	queue = ai_gui_session_get_queue(session);
	ai_prompt_queue_push(queue, "original", NULL, FALSE, NULL);
	ai_prompt_queue_push(queue, "remove me", NULL, FALSE, NULL);
	window = adw_window_new();
	adw_window_set_content(ADW_WINDOW(window), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	gtk_window_set_default_size(GTK_WINDOW(window), 900, 680);
	gtk_window_present(GTK_WINDOW(window));
	drain();
	ai_gui_queue_present(window, session);
	drain();
	dialog = adw_window_get_visible_dialog(ADW_WINDOW(window));
	g_assert_nonnull(dialog);
	g_assert_true(ai_prompt_queue_get_paused(queue));
	g_assert_null(ai_prompt_queue_pop(queue, NULL));
	collect(GTK_WIDGET(dialog), GTK_TYPE_TEXT_VIEW, views);
	collect(GTK_WIDGET(dialog), GTK_TYPE_CHECK_BUTTON, checks);
	g_assert_cmpuint(views->len, ==, 2);
	g_assert_cmpuint(checks->len, ==, 2);
	demonstrate(window, "queue-editor.png");
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(g_ptr_array_index(views, 0)), "", -1);
	gtk_check_button_set_active(g_ptr_array_index(checks, 1), TRUE);
	save = button_named(GTK_WIDGET(dialog), "Save and resume");
	g_assert_nonnull(save);
	g_signal_emit_by_name(save, "clicked");
	/* Invalid edits are atomic: the removal has not happened either. */
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 2);
	g_assert_true(ai_prompt_queue_get_paused(queue));
	g_assert_cmpstr(ai_prompt_queue_get(queue, 0, NULL), ==, "original");
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(g_ptr_array_index(views, 0)), "edited\nmultiline", -1);
	g_signal_emit_by_name(save, "clicked");
	drain();
	g_assert_false(ai_prompt_queue_get_paused(queue));
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 1);
	sent = ai_message_get_text(g_list_last(ai_mock_provider_get_last_messages(mock))->data);
	g_assert_cmpstr(sent, ==, "edited\nmultiline");
	g_test_message("Submitted after Save and resume: %s", sent);
	g_assert_cmpuint(ai_prompt_queue_get_length(queue), ==, 0);
	/* Cancelling an edit keeps the original entry, then resumes it. */
	ai_prompt_queue_push(queue, "unchanged", NULL, FALSE, NULL);
	ai_gui_queue_present(window, session);
	drain();
	dialog = adw_window_get_visible_dialog(ADW_WINDOW(window));
	g_ptr_array_set_size(views, 0);
	collect(GTK_WIDGET(dialog), GTK_TYPE_TEXT_VIEW, views);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(g_ptr_array_index(views, 0)), "discard this edit", -1);
	g_signal_emit_by_name(button_named(GTK_WIDGET(dialog), "Cancel"), "clicked");
	drain();
	g_clear_pointer(&sent, g_free);
	sent = ai_message_get_text(g_list_last(ai_mock_provider_get_last_messages(mock))->data);
	g_assert_cmpstr(sent, ==, "unchanged");
	gtk_window_destroy(GTK_WINDOW(window));
	drain();
}

static void
project_chosen(AiGuiDashboard *dashboard, const gchar *path, gpointer data)
{
	gchar **selected = data;
	g_free(*selected); *selected = g_strdup(path);
}

static void
test_project_dashboard(void)
{
	g_autofree gchar *cwd = g_get_current_dir();
	g_autofree gchar *registry = g_build_filename(cwd, "registry", NULL);
	g_autoptr(AiWorkSession) session = ai_work_session_new(cwd);
	g_autofree gchar *selected = NULL;
	g_autoptr(GPtrArray) entries = g_ptr_array_new();
	GtkWidget *window = adw_window_new();
	GtkWidget *dashboard = ai_gui_dashboard_new();
	g_assert_true(ai_work_session_save(session, registry, FALSE, NULL));
	ai_gui_dashboard_set_registry(AI_GUI_DASHBOARD(dashboard), registry);
	g_signal_connect(dashboard, "project-requested", G_CALLBACK(project_chosen), &selected);
	adw_window_set_content(ADW_WINDOW(window), dashboard);
	gtk_window_set_default_size(GTK_WINDOW(window), 900, 680);
	gtk_window_present(GTK_WINDOW(window));
	ai_gui_dashboard_refresh(AI_GUI_DASHBOARD(dashboard));
	drain();
	g_assert_nonnull(button_named(dashboard, "Open project"));
	{
		GtkWidget *open = button_named(dashboard, "Open project");
		ai_gui_dashboard_refresh(AI_GUI_DASHBOARD(dashboard));
		g_assert_true(button_named(dashboard, "Open project") == open);
	}
	demonstrate(window, "projects-window.png");
	g_signal_emit_by_name(button_named(dashboard, "Open project"), "clicked");
	g_assert_cmpstr(selected, ==, cwd);
	g_test_message("Project opened: %s", selected);
	collect(dashboard, GTK_TYPE_ENTRY, entries);
	g_assert_cmpuint(entries->len, ==, 1);
	gtk_editable_set_text(g_ptr_array_index(entries, 0), cwd);
	g_signal_emit_by_name(g_ptr_array_index(entries, 0), "activate");
	g_assert_cmpstr(selected, ==, cwd);
	g_test_message("Project opened: %s", selected);
	gtk_window_destroy(GTK_WINDOW(window));
	drain();
}
static void
test_window_projects(void)
{
	g_autoptr(AdwApplication) app = adw_application_new("org.copyleft.AiGlib.WorkspaceTest", G_APPLICATION_NON_UNIQUE);
	g_autoptr(AiGuiOptions) options = g_new0(AiGuiOptions, 1);
	g_autofree gchar *cwd = g_get_current_dir();
	g_autofree gchar *other = g_build_filename(cwd, "second project", NULL);
	g_autofree gchar *alias = g_build_filename(cwd, "alias", NULL);
	g_autofree gchar *registry = ai_work_session_default_directory();
	g_autolist(AiWorkSession) rows = NULL;
	AiGuiWindow *window;
	g_assert_cmpint(g_mkdir(other, 0700), ==, 0);
	options->working_directory = g_strdup(cwd);
	options->provider = g_strdup("ollama");
	options->model = g_strdup("fixture-model");
	options->max_tokens = 1024;
	g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
	window = ai_gui_window_new(app, options);
	gtk_window_present(GTK_WINDOW(window));
	drain();
	ai_gui_window_open_project(window, other);
	drain();
	ai_work_session_prune(registry, NULL);
	rows = ai_work_session_list(registry, NULL);
	g_assert_cmpuint(g_list_length(rows), ==, 2);
	g_clear_list(&rows, g_object_unref);
	ai_gui_window_open_project(window, other);
	g_assert_cmpint(symlink(other, alias), ==, 0);
	ai_gui_window_open_project(window, alias);
	ai_gui_window_open_project(window, cwd);
	drain();
	rows = ai_work_session_list(registry, NULL);
	g_assert_cmpuint(g_list_length(rows), ==, 2);
	/* Exercise the composer's real key controller while a draft is present. */
	{
		g_autoptr(GPtrArray) composers = g_ptr_array_new();
		g_autoptr(GPtrArray) views = g_ptr_array_new();
		g_autofree gchar *draft = NULL;
		guint i, j;
		gboolean handled = FALSE;
		AdwDialog *dialog;
		collect(GTK_WIDGET(window), AI_GUI_TYPE_COMPOSER, composers);
		g_assert_cmpuint(composers->len, ==, 1);
		ai_gui_composer_set_text(g_ptr_array_index(composers, 0), "retain this draft");
		collect(g_ptr_array_index(composers, 0), GTK_TYPE_TEXT_VIEW, views);
		for (i = 0; i < views->len; i++) {
			g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(g_ptr_array_index(views, i));
			for (j = 0; j < g_list_model_get_n_items(controllers); j++) {
				g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, j);
				gboolean accepted = FALSE;
				if (!GTK_IS_EVENT_CONTROLLER_KEY(controller)) continue;
				g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_q, 0u, GDK_ALT_MASK, &accepted);
				handled |= accepted;
			}
		}
		g_assert_true(handled);
		drain();
		dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
		g_assert_nonnull(dialog);
		draft = ai_gui_composer_get_text(g_ptr_array_index(composers, 0));
		g_assert_cmpstr(draft, ==, "retain this draft");
		g_signal_emit_by_name(button_named(GTK_WIDGET(dialog), "Cancel"), "clicked");
		drain();
	}
	ai_gui_window_show_dashboard(window, TRUE);
	drain();
	demonstrate(GTK_WIDGET(window), "gui-projects.png");
	g_test_message("Two project conversations loaded; reopening either keeps two conversations.");
	gtk_window_destroy(GTK_WINDOW(window));
	drain();
}
#endif

static void
unavailable(void)
{
	g_test_skip("Set AI_GUI_UI_TEST=1 with a GTK display to run widget integration tests");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#ifdef AI_TEST_GTK
	if (g_getenv("AI_GUI_UI_TEST") != NULL) {
		g_autofree gchar *sandbox = g_dir_make_tmp("ai-workspace-ui-XXXXXX", NULL);
		g_setenv("HOME", sandbox, TRUE);
		g_setenv("XDG_DATA_HOME", sandbox, TRUE);
		g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
		g_setenv("XDG_STATE_HOME", sandbox, TRUE);
		g_setenv("GTK_A11Y", "none", TRUE);
		g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
		g_setenv("OLLAMA_HOST", "http://127.0.0.1:1", TRUE);
		g_setenv("AI_GLIB_NO_UPDATE_CHECK", "1", TRUE);
		g_chdir(sandbox);
		g_assert_true(gtk_init_check());
		adw_init();
		g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
		g_test_add_func("/gui/workspace/queue-editor", test_queue_dialog);
		g_test_add_func("/gui/workspace/projects", test_project_dashboard);
		g_test_add_func("/gui/workspace/window-projects", test_window_projects);
		return g_test_run();
	}
#endif
	g_test_add_func("/gui/workspace/unavailable", unavailable);
	return g_test_run();
}
