/*
 * ai-gui-session-store.c - The sessions on disk and in the window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <glib/gstdio.h>

#include "core/ai-json-util.h"

#include "ai-gui-session-store.h"

/* A saved session that has grown past this is a corrupt file, not a long
 * conversation: the blocks are summaries, not raw tool output. */
#define AI_GUI_SESSION_MAX_BYTES (32 * 1024 * 1024)

struct _AiGuiSessionStore
{
	GObject parent_instance;

	GPtrArray *sessions;
	gchar     *directory;
};

static void ai_gui_session_store_list_model_init(GListModelInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(AiGuiSessionStore, ai_gui_session_store,
	G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
	                      ai_gui_session_store_list_model_init))

/* ================================================================
 * GListModel
 * ================================================================ */

static GType
store_get_item_type(GListModel *model)
{
	return AI_GUI_TYPE_SESSION;
}

static guint
store_get_n_items(GListModel *model)
{
	return AI_GUI_SESSION_STORE(model)->sessions->len;
}

static gpointer
store_get_item(
	GListModel *model,
	guint       position
){
	AiGuiSessionStore *self = AI_GUI_SESSION_STORE(model);

	if (position >= self->sessions->len)
		return NULL;

	return g_object_ref(g_ptr_array_index(self->sessions, position));
}

static void
ai_gui_session_store_list_model_init(GListModelInterface *iface)
{
	iface->get_item_type = store_get_item_type;
	iface->get_n_items = store_get_n_items;
	iface->get_item = store_get_item;
}

/* ================================================================
 * Construction
 * ================================================================ */

static gchar *
store_default_directory(void)
{
	return g_build_filename(g_get_user_data_dir(), "ai-glib", "gui",
	                        "sessions", NULL);
}

AiGuiSessionStore *
ai_gui_session_store_new(const gchar *directory)
{
	g_autoptr(AiGuiSessionStore) self = g_object_new(AI_GUI_TYPE_SESSION_STORE,
	                                                 NULL);

	self->directory = directory != NULL ? g_strdup(directory)
	                                    : store_default_directory();

	return g_steal_pointer(&self);
}

const gchar *
ai_gui_session_store_get_directory(AiGuiSessionStore *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), NULL);
	return self->directory;
}

/* ================================================================
 * Membership
 * ================================================================ */

void
ai_gui_session_store_add(
	AiGuiSessionStore *self,
	AiGuiSession      *session
){
	g_return_if_fail(AI_GUI_IS_SESSION_STORE(self));
	g_return_if_fail(AI_GUI_IS_SESSION(session));

	/* Newest first: the sidebar reads top to bottom and a session just
	 * created is the one somebody is about to type into. */
	g_ptr_array_insert(self->sessions, 0, g_object_ref(session));
	g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, 1);
}

gboolean
ai_gui_session_store_find(
	AiGuiSessionStore *self,
	AiGuiSession      *session,
	guint             *out_position
){
	guint i;

	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), FALSE);

	for (i = 0; i < self->sessions->len; i++)
	{
		if (g_ptr_array_index(self->sessions, i) == session)
		{
			if (out_position != NULL)
				*out_position = i;

			return TRUE;
		}
	}

	return FALSE;
}

gboolean
ai_gui_session_store_remove(
	AiGuiSessionStore *self,
	AiGuiSession      *session
){
	g_autofree gchar *path = NULL;
	guint position;

	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), FALSE);
	g_return_val_if_fail(AI_GUI_IS_SESSION(session), FALSE);

	if (!ai_gui_session_store_find(self, session, &position))
		return FALSE;

	path = g_build_filename(self->directory,
	                        ai_gui_session_get_id(session), NULL);

	{
		g_autofree gchar *file = g_strconcat(path, ".json", NULL);

		if (g_file_test(file, G_FILE_TEST_EXISTS) && g_unlink(file) != 0)
			g_debug("ai-gui: could not remove %s", file);
	}

	g_ptr_array_remove_index(self->sessions, position);
	g_list_model_items_changed(G_LIST_MODEL(self), position, 1, 0);

	return TRUE;
}

AiGuiSession *
ai_gui_session_store_get(
	AiGuiSessionStore *self,
	guint              position
){
	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), NULL);

	if (position >= self->sessions->len)
		return NULL;

	return g_ptr_array_index(self->sessions, position);
}

guint
ai_gui_session_store_get_n_sessions(AiGuiSessionStore *self)
{
	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), 0);
	return self->sessions->len;
}

/* ================================================================
 * Disk
 * ================================================================ */

gboolean
ai_gui_session_store_save(
	AiGuiSessionStore  *self,
	AiGuiSession       *session,
	GError            **error
){
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *name = NULL;

	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), FALSE);
	g_return_val_if_fail(AI_GUI_IS_SESSION(session), FALSE);

	if (g_mkdir_with_parents(self->directory, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot create %s", self->directory);
		return FALSE;
	}

	root = ai_gui_session_to_json(session);
	generator = json_generator_new();
	json_generator_set_pretty(generator, TRUE);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);

	name = g_strconcat(ai_gui_session_get_id(session), ".json", NULL);
	path = g_build_filename(self->directory, name, NULL);

	/*
	 * Replaced atomically. A session file half-written by a crash during
	 * shutdown is the one case where the next start has no other copy to
	 * fall back on.
	 */
	return g_file_set_contents_full(path, text, -1,
	                                G_FILE_SET_CONTENTS_CONSISTENT |
	                                G_FILE_SET_CONTENTS_DURABLE,
	                                0600, error);
}

guint
ai_gui_session_store_save_all(AiGuiSessionStore *self)
{
	guint saved = 0;
	guint i;

	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), 0);

	for (i = 0; i < self->sessions->len; i++)
	{
		g_autoptr(GError) error = NULL;

		if (ai_gui_session_store_save(self,
			g_ptr_array_index(self->sessions, i), &error))
		{
			saved++;
		}
		else
		{
			/*
			 * g_debug, not g_warning: a session that will not write is a
			 * full disk or a read-only home, not a bug in this program --
			 * and under G_DEBUG=fatal-warnings a warning here would abort
			 * the shutdown it is running inside.
			 */
			g_debug("ai-gui: could not save session: %s",
			        error != NULL ? error->message : "unknown");
		}
	}

	return saved;
}

static gint
compare_updated(
	gconstpointer a,
	gconstpointer b
){
	AiGuiSession *left = *(AiGuiSession * const *)a;
	AiGuiSession *right = *(AiGuiSession * const *)b;
	gboolean left_pinned = ai_gui_session_get_pinned(left);
	gboolean right_pinned = ai_gui_session_get_pinned(right);
	gint64 delta;

	if (left_pinned != right_pinned)
		return left_pinned ? -1 : 1;

	delta = ai_gui_session_get_updated_at(right)
		- ai_gui_session_get_updated_at(left);

	if (delta < 0)
		return -1;

	return delta > 0 ? 1 : 0;
}

guint
ai_gui_session_store_load(
	AiGuiSessionStore  *self,
	const AiGuiOptions *options
){
	g_autoptr(GDir) dir = NULL;
	guint added = 0;
	guint before;
	const gchar *name;

	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(self), 0);
	g_return_val_if_fail(options != NULL, 0);

	before = self->sessions->len;
	dir = g_dir_open(self->directory, 0, NULL);

	if (dir == NULL)
		return 0;

	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autoptr(JsonParser) parser = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *path = NULL;
		AiGuiSession *session;
		JsonObject *object;
		GStatBuf info;

		if (!g_str_has_suffix(name, ".json"))
			continue;

		path = g_build_filename(self->directory, name, NULL);

		/* Bounded, like every other file this project reads: a
		 * multi-gigabyte "session" is a corrupt file, and parsing it is
		 * how a start-up hangs with nothing on screen to explain it. */
		if (g_stat(path, &info) != 0 || info.st_size > AI_GUI_SESSION_MAX_BYTES)
		{
			g_debug("ai-gui: skipping %s: not a readable session file", path);
			continue;
		}

		parser = json_parser_new();

		if (!json_parser_load_from_file(parser, path, &error))
		{
			g_debug("ai-gui: skipping %s: %s", path,
			        error != NULL ? error->message : "unparseable");
			continue;
		}

		object = ai_json_root_object(parser);

		if (object == NULL)
		{
			g_debug("ai-gui: skipping %s: not a JSON object", path);
			continue;
		}

		session = ai_gui_session_new_from_json(object, options, &error);

		if (session == NULL)
		{
			/*
			 * A provider that no longer exists, or a model the factory
			 * refuses: worth saying, not worth aborting the start-up
			 * over. The file stays where it is so a later run with the
			 * CLI installed picks it up.
			 */
			g_debug("ai-gui: skipping %s: %s", path,
			        error != NULL ? error->message : "could not be restored");
			continue;
		}

		g_ptr_array_add(self->sessions, session);
		added++;
	}

	g_ptr_array_sort(self->sessions, compare_updated);

	if (added > 0)
		g_list_model_items_changed(G_LIST_MODEL(self), 0, before,
		                           self->sessions->len);

	return added;
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

static void
ai_gui_session_store_dispose(GObject *object)
{
	AiGuiSessionStore *self = AI_GUI_SESSION_STORE(object);

	if (self->sessions != NULL)
		g_ptr_array_set_size(self->sessions, 0);

	G_OBJECT_CLASS(ai_gui_session_store_parent_class)->dispose(object);
}

static void
ai_gui_session_store_finalize(GObject *object)
{
	AiGuiSessionStore *self = AI_GUI_SESSION_STORE(object);

	g_clear_pointer(&self->sessions, g_ptr_array_unref);
	g_free(self->directory);

	G_OBJECT_CLASS(ai_gui_session_store_parent_class)->finalize(object);
}

static void
ai_gui_session_store_class_init(AiGuiSessionStoreClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ai_gui_session_store_dispose;
	object_class->finalize = ai_gui_session_store_finalize;
}

static void
ai_gui_session_store_init(AiGuiSessionStore *self)
{
	self->sessions = g_ptr_array_new_with_free_func(g_object_unref);
}
