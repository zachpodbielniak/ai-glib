/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-file-voiceprint-store.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Voiceprints as files: one NAME.json per person in a directory of the
 * caller's choosing. They are biometric data, so the store is strict about
 * what it reads and writes:
 *
 * - The directory is created 0700, and an existing one that others can
 *   read is tightened to 0700.
 * - A file is opened with O_NOFOLLOW and judged by the descriptor that was
 *   opened: a regular file, owned by this user, at most
 *   AI_FILE_VOICEPRINT_STORE_MAX_FILE bytes. A symlink, a FIFO, someone
 *   else's file or a huge one is skipped. A readable-by-others file is
 *   tightened to 0600.
 * - Writes are atomic (a temporary file renamed into place) and 0600.
 * - A file whose "name" is not its own file name is not a voiceprint.
 *
 * With monitor set, the directory is watched, so an enrolment made by
 * another process (ai-call --enroll while a call runs) reaches a running
 * identifier through AiVoiceprintStore::changed.
 */
typedef struct {
	gchar *directory;
	gboolean monitor;
	GFileMonitor *watch;
	GSource *settle; /* coalesces a burst of file events into one "changed" */
} AiFileVoiceprintStorePrivate;

enum { PROP_0, PROP_DIRECTORY, PROP_MONITOR, N_PROPS };
static GParamSpec *props[N_PROPS];

static void store_iface_init(AiVoiceprintStoreInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiFileVoiceprintStore, ai_file_voiceprint_store, G_TYPE_OBJECT,
						G_ADD_PRIVATE(AiFileVoiceprintStore)
							G_IMPLEMENT_INTERFACE(AI_TYPE_VOICEPRINT_STORE, store_iface_init))

static AiFileVoiceprintStorePrivate *
private_of(AiVoiceprintStore *store)
{
	return ai_file_voiceprint_store_get_instance_private(AI_FILE_VOICEPRINT_STORE(store));
}
static gchar *
path_for(AiFileVoiceprintStorePrivate *priv, const gchar *name)
{
	g_autofree gchar *file = g_strconcat(name, ".json", NULL);
	return g_build_filename(priv->directory, file, NULL);
}
/* Create the directory, or tighten one that exists, to 0700. */
static gboolean
ensure_directory(AiFileVoiceprintStorePrivate *priv, GError **error)
{
	struct stat st;
	gint fd;
	if (priv->directory == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"No voiceprint directory");
		return FALSE;
	}
	if (g_mkdir_with_parents(priv->directory, 0700) != 0) {
		gint saved = errno;
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Cannot create %s: %s",
					priv->directory, g_strerror(saved));
		return FALSE;
	}
	/* Judged by the descriptor that was opened, never by the path again: a
	 * symlink swapped in after a check must not be what gets chmod'ed. */
	fd = open(priv->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
					"%s is not a directory", priv->directory);
		if (fd >= 0)
			close(fd);
		return FALSE;
	}
	if (st.st_uid != getuid()) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
					"%s belongs to another user", priv->directory);
		close(fd);
		return FALSE;
	}
	if ((st.st_mode & 077) != 0) {
		g_log("ai-glib", G_LOG_LEVEL_INFO,
			  "Voiceprint directory %s was readable by others; now 0700", priv->directory);
		if (fchmod(fd, 0700) != 0) {
			gint saved = errno;
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Cannot protect %s: %s",
						priv->directory, g_strerror(saved));
			close(fd);
			return FALSE;
		}
	}
	close(fd);
	return TRUE;
}
/* One voiceprint file, or NULL (logged at debug) when it is not one. */
static AiVoiceprint *
load_file(const gchar *path, const gchar *stem)
{
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *data = NULL;
	AiVoiceprint *voiceprint;
	struct stat st;
	gssize got, total = 0;
	gint fd;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		g_debug("voiceprint %s: %s; skipped", path, g_strerror(errno));
		return NULL;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
		st.st_size < 0 || (gsize)st.st_size > AI_FILE_VOICEPRINT_STORE_MAX_FILE) {
		g_debug("voiceprint %s: not a regular file of ours within size; skipped", path);
		close(fd);
		return NULL;
	}
	if ((st.st_mode & 077) != 0) {
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Voiceprint %s was readable by others; now 0600",
			  path);
		if (fchmod(fd, 0600) != 0)
			g_debug("voiceprint %s: chmod: %s", path, g_strerror(errno));
	}
	data = g_malloc((gsize)st.st_size + 1);
	while (total < (gssize)st.st_size) {
		got = read(fd, data + total, (gsize)st.st_size - (gsize)total);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		total += got;
	}
	close(fd);
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, data, total, &error)) {
		g_debug("voiceprint %s: %s; skipped", path, error->message);
		return NULL;
	}
	voiceprint = ai_voiceprint_new_from_json(json_parser_get_root(parser), &error);
	if (voiceprint == NULL) {
		g_debug("voiceprint %s: %s; skipped", path, error->message);
		return NULL;
	}
	if (g_strcmp0(ai_voiceprint_get_name(voiceprint), stem) != 0) {
		g_debug("voiceprint %s: its name is not its file name; skipped", path);
		g_object_unref(voiceprint);
		return NULL;
	}
	return voiceprint;
}
static gint
by_name(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(ai_voiceprint_get_name(*(AiVoiceprint *const *)a),
					 ai_voiceprint_get_name(*(AiVoiceprint *const *)b));
}
static GPtrArray *
file_list(AiVoiceprintStore *store)
{
	AiFileVoiceprintStorePrivate *priv = private_of(store);
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	const gchar *entry;
	GDir *dir;
	if (priv->directory == NULL || (dir = g_dir_open(priv->directory, 0, NULL)) == NULL)
		return out;
	while ((entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *stem = NULL, *path = NULL;
		AiVoiceprint *v;
		if (*entry == '.' || !g_str_has_suffix(entry, ".json"))
			continue;
		stem = g_strndup(entry, strlen(entry) - 5);
		if (!ai_voiceprint_name_is_valid(stem))
			continue;
		path = g_build_filename(priv->directory, entry, NULL);
		if ((v = load_file(path, stem)) != NULL)
			g_ptr_array_add(out, v);
	}
	g_dir_close(dir);
	g_ptr_array_sort(out, by_name);
	return out;
}
static AiVoiceprint *
file_lookup(AiVoiceprintStore *store, const gchar *name)
{
	AiFileVoiceprintStorePrivate *priv = private_of(store);
	g_autofree gchar *path = NULL;
	if (priv->directory == NULL)
		return NULL;
	path = path_for(priv, name);
	return load_file(path, name);
}
static gboolean
file_save(AiVoiceprintStore *store, AiVoiceprint *voiceprint, GError **error)
{
	AiFileVoiceprintStorePrivate *priv = private_of(store);
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *json = NULL, *path = NULL;
	if (!ensure_directory(priv, error))
		return FALSE;
	root = ai_voiceprint_to_json(voiceprint);
	json = json_to_string(root, FALSE);
	path = path_for(priv, ai_voiceprint_get_name(voiceprint));
	if (!g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
		return FALSE;
	ai_voiceprint_store_emit_changed(store);
	return TRUE;
}
static gboolean
file_remove(AiVoiceprintStore *store, const gchar *name, GError **error)
{
	AiFileVoiceprintStorePrivate *priv = private_of(store);
	g_autofree gchar *path = NULL;
	if (priv->directory == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Nobody called %s is enrolled",
					name);
		return FALSE;
	}
	path = path_for(priv, name);
	if (g_unlink(path) != 0) {
		gint saved = errno;
		if (saved == ENOENT)
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
						"Nobody called %s is enrolled", name);
		else
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Cannot remove %s: %s",
						path, g_strerror(saved));
		return FALSE;
	}
	ai_voiceprint_store_emit_changed(store);
	return TRUE;
}
static gboolean
file_rename(AiVoiceprintStore *store, const gchar *from, const gchar *to, GError **error)
{
	AiFileVoiceprintStorePrivate *priv = private_of(store);
	g_autoptr(AiVoiceprint) old = file_lookup(store, from);
	g_autoptr(AiVoiceprint) renamed = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *target = NULL, *json = NULL, *temp = NULL;
	gsize len, done;
	gint fd;
	if (old == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Nobody called %s is enrolled",
					from);
		return FALSE;
	}
	if (!ensure_directory(priv, error))
		return FALSE;
	target = path_for(priv, to);
	/* The name lives inside the file too, so this is a write, not a move:
	 * the new file is written in full under a temporary name, then linked
	 * into place. link() refuses an existing target atomically, so a file
	 * that appeared after any check is never replaced. */
	renamed = ai_voiceprint_copy(old, to);
	root = ai_voiceprint_to_json(renamed);
	json = json_to_string(root, FALSE);
	temp = g_build_filename(priv->directory, ".rename-XXXXXX", NULL);
	fd = g_mkstemp_full(temp, O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) {
		gint saved = errno;
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Cannot write in %s: %s",
					priv->directory, g_strerror(saved));
		return FALSE;
	}
	len = strlen(json);
	for (done = 0; done < len;) {
		gssize wrote = write(fd, json + done, len - done);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			break;
		done += (gsize)wrote;
	}
	if (done != len || fsync(fd) != 0) {
		gint saved = errno;
		close(fd);
		g_unlink(temp);
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Cannot write %s: %s",
					temp, g_strerror(saved));
		return FALSE;
	}
	close(fd);
	if (link(temp, target) != 0) {
		gint saved = errno;
		g_unlink(temp);
		if (saved == EEXIST)
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "%s is already enrolled", to);
		else
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Cannot create %s: %s",
						target, g_strerror(saved));
		return FALSE;
	}
	g_unlink(temp);
	/* The new name exists now; if the old one cannot go, say so rather
	 * than leave the caller believing the rename half-failed silently. */
	return file_remove(store, from, error);
}
static void
store_iface_init(AiVoiceprintStoreInterface *iface)
{
	iface->list = file_list;
	iface->lookup = file_lookup;
	iface->save = file_save;
	iface->remove = file_remove;
	iface->rename = file_rename;
}

/* ---- watching the directory ---- */

static gboolean
settled(gpointer data)
{
	AiFileVoiceprintStore *self = data;
	AiFileVoiceprintStorePrivate *priv = ai_file_voiceprint_store_get_instance_private(self);
	g_clear_pointer(&priv->settle, g_source_unref);
	ai_voiceprint_store_emit_changed(AI_VOICEPRINT_STORE(self));
	return G_SOURCE_REMOVE;
}
static void
directory_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
				  gpointer data)
{
	AiFileVoiceprintStore *self = data;
	AiFileVoiceprintStorePrivate *priv = ai_file_voiceprint_store_get_instance_private(self);
	g_autofree gchar *base = file != NULL ? g_file_get_basename(file) : NULL;
	g_autofree gchar *moved = other != NULL ? g_file_get_basename(other) : NULL;
	/* An atomic write is a temporary file renamed into place: the event
	 * names the temporary file, and the voiceprint is the other one. */
	if (!(base != NULL && *base != '.' && g_str_has_suffix(base, ".json")) &&
		!(moved != NULL && *moved != '.' && g_str_has_suffix(moved, ".json")))
		return;
	if (priv->settle != NULL)
		return;
	priv->settle = g_timeout_source_new(200);
	g_source_set_callback(priv->settle, settled, self, NULL);
	g_source_attach(priv->settle, g_main_context_get_thread_default());
}
static void
update_watch(AiFileVoiceprintStore *self)
{
	AiFileVoiceprintStorePrivate *priv = ai_file_voiceprint_store_get_instance_private(self);
	g_autoptr(GFile) dir = NULL;
	g_autoptr(GError) error = NULL;
	if (priv->watch != NULL) {
		g_signal_handlers_disconnect_by_data(priv->watch, self);
		g_file_monitor_cancel(priv->watch);
		g_clear_object(&priv->watch);
	}
	if (!priv->monitor || priv->directory == NULL)
		return;
	dir = g_file_new_for_path(priv->directory);
	priv->watch = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
	if (priv->watch == NULL) {
		g_debug("voiceprint directory %s not watched: %s", priv->directory, error->message);
		return;
	}
	g_signal_connect(priv->watch, "changed", G_CALLBACK(directory_changed), self);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiFileVoiceprintStorePrivate *priv = private_of(AI_VOICEPRINT_STORE(object));
	if (id == PROP_DIRECTORY)
		g_value_set_string(value, priv->directory);
	else if (id == PROP_MONITOR)
		g_value_set_boolean(value, priv->monitor);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiFileVoiceprintStorePrivate *priv = private_of(AI_VOICEPRINT_STORE(object));
	if (id == PROP_DIRECTORY) {
		g_free(priv->directory);
		priv->directory = g_value_dup_string(value);
	} else if (id == PROP_MONITOR)
		priv->monitor = g_value_get_boolean(value);
	else {
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
		return;
	}
	update_watch(AI_FILE_VOICEPRINT_STORE(object));
}
static void
dispose(GObject *object)
{
	AiFileVoiceprintStorePrivate *priv = private_of(AI_VOICEPRINT_STORE(object));
	priv->monitor = FALSE;
	update_watch(AI_FILE_VOICEPRINT_STORE(object));
	if (priv->settle != NULL) {
		g_source_destroy(priv->settle);
		g_clear_pointer(&priv->settle, g_source_unref);
	}
	G_OBJECT_CLASS(ai_file_voiceprint_store_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiFileVoiceprintStorePrivate *priv = private_of(AI_VOICEPRINT_STORE(object));
	g_free(priv->directory);
	G_OBJECT_CLASS(ai_file_voiceprint_store_parent_class)->finalize(object);
}
static void
ai_file_voiceprint_store_class_init(AiFileVoiceprintStoreClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->dispose = dispose;
	oc->finalize = finalize;
	/**
	 * AiFileVoiceprintStore:directory:
	 *
	 * Where the voiceprint files are kept. Created 0700 on the first save.
	 */
	props[PROP_DIRECTORY] =
		g_param_spec_string("directory", "Directory", "Where voiceprints are kept", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiFileVoiceprintStore:monitor:
	 *
	 * Watch the directory and emit #AiVoiceprintStore::changed when another
	 * process changes it.
	 */
	props[PROP_MONITOR] =
		g_param_spec_boolean("monitor", "Monitor", "Watch the directory for changes", FALSE,
							 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
}
static void
ai_file_voiceprint_store_init(AiFileVoiceprintStore *self)
{
}

/**
 * ai_file_voiceprint_store_new:
 * @directory: where voiceprints are kept
 *
 * Returns: (transfer full): a store over @directory
 */
AiFileVoiceprintStore *
ai_file_voiceprint_store_new(const gchar *directory)
{
	return g_object_new(AI_TYPE_FILE_VOICEPRINT_STORE, "directory", directory, NULL);
}

/**
 * ai_file_voiceprint_store_get_directory:
 * @self: a store
 *
 * Returns: (nullable): the directory
 */
const gchar *
ai_file_voiceprint_store_get_directory(AiFileVoiceprintStore *self)
{
	g_return_val_if_fail(AI_IS_FILE_VOICEPRINT_STORE(self), NULL);
	return private_of(AI_VOICEPRINT_STORE(self))->directory;
}
