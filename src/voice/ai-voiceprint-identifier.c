/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voiceprint-identifier.h"
#include "model/ai-embedding.h"
#include "core/ai-json-util.h"
#include "core/ai-http-redirect-private.h"
#include <math.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

/*
 * Speaker identification by voiceprint. A service turns audio into a
 * unit-length vector (POST raw 16 kHz mono S16LE, answered with
 * {"model":..., "embedding":[...]}); everything else is here. Voiceprints are
 * files in a directory the caller chooses, one per person, readable only by
 * their owner: they are biometric data, and the service keeps none.
 *
 * The model travels with every voiceprint. Vectors from two models are not
 * comparable -- the cosine between them reads like a weak match rather than
 * like an error -- so a voiceprint from another model is never compared.
 */
#define MIN_BYTES (16000 * 2 / 2) /* half a second */
/* A stranger's first utterance must be this long to start a guest: a short
 * clip's voiceprint is too noisy to tell a new voice from a known one said
 * briefly, and every false guest splits someone in two. */
#define MIN_NEW_GUEST_BYTES (16000 * 2 * 3 / 2)
#define MAX_GUESTS 32
#define MAX_NAME 64

typedef struct {
	gchar *name, *model;
	gfloat *vector;
	gsize dims;
	guint count;
} Voice;

struct _AiVoiceprintIdentifier {
	GObject parent_instance;
	gchar *url, *directory;
	gdouble threshold;
	guint timeout_ms;
	SoupSession *http;
	GPtrArray *voices;
	/* track-guests: voices matching nobody enrolled, numbered as first heard;
	 * kept in memory only, for this identifier's lifetime */
	gboolean track_guests;
	gdouble guest_threshold;
	GPtrArray *guests;
};

enum {
	PROP_0,
	PROP_URL,
	PROP_DIRECTORY,
	PROP_THRESHOLD,
	PROP_TIMEOUT,
	PROP_TRACK_GUESTS,
	PROP_GUEST_THRESHOLD
};

static void identifier_iface_init(AiSpeakerIdentifierInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiVoiceprintIdentifier, ai_voiceprint_identifier, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_SPEAKER_IDENTIFIER, identifier_iface_init))

static void
voice_free(gpointer data)
{
	Voice *v = data;
	g_free(v->name);
	g_free(v->model);
	g_free(v->vector);
	g_free(v);
}
/* A name is a file name here, so it may not leave the directory or hide. */
static gboolean
valid_name(const gchar *name)
{
	return name != NULL && *name != '\0' && *name != '.' && strlen(name) <= MAX_NAME &&
		   g_utf8_validate(name, -1, NULL) && strchr(name, '/') == NULL &&
		   strchr(name, '\\') == NULL;
}
/* Every element a finite number that fits a float, at least one of them, and
 * not all zero; NULL otherwise. A zero or non-finite vector has no direction:
 * it matches nobody, and averaged into a voiceprint it poisons it. */
static gfloat *
read_vector(JsonArray *array, gsize *dims)
{
	guint i, n = array != NULL ? json_array_get_length(array) : 0;
	gfloat *vector;
	gdouble norm = 0;
	if (n == 0 || n > 8192)
		return NULL;
	vector = g_new(gfloat, n);
	for (i = 0; i < n; i++) {
		JsonNode *node = json_array_get_element(array, i);
		GType type = JSON_NODE_HOLDS_VALUE(node) ? json_node_get_value_type(node) : G_TYPE_INVALID;
		gdouble value;
		if (type == G_TYPE_DOUBLE)
			value = json_node_get_double(node);
		else if (type == G_TYPE_INT64)
			value = (gdouble)json_node_get_int(node);
		else {
			g_free(vector);
			return NULL;
		}
		if (!isfinite(value) || fabs(value) > 1e30) {
			g_free(vector);
			return NULL;
		}
		vector[i] = (gfloat)value;
		norm += value * value;
	}
	if (!(norm > 0) || !isfinite(norm)) {
		g_free(vector);
		return NULL;
	}
	*dims = n;
	return vector;
}
static void
load(AiVoiceprintIdentifier *self)
{
	GDir *dir;
	const gchar *entry;
	g_ptr_array_set_size(self->voices, 0);
	if (self->directory == NULL || (dir = g_dir_open(self->directory, 0, NULL)) == NULL)
		return;
	while ((entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL, *stem = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(GError) error = NULL;
		JsonObject *o;
		const gchar *name, *model;
		Voice *v;
		if (!g_str_has_suffix(entry, ".json") || *entry == '.')
			continue;
		path = g_build_filename(self->directory, entry, NULL);
		if (!json_parser_load_from_file(parser, path, &error)) {
			g_debug("voiceprint %s: %s", path, error->message);
			continue;
		}
		o = ai_json_root_object(parser);
		name = ai_json_get_string(o, "name", NULL);
		model = ai_json_get_string(o, "model", NULL);
		stem = g_strndup(entry, strlen(entry) - 5);
		if (!valid_name(name) || g_strcmp0(name, stem) != 0 || model == NULL) {
			g_debug("voiceprint %s: not a voiceprint; skipped", path);
			continue;
		}
		v = g_new0(Voice, 1);
		v->vector = read_vector(ai_json_get_array(o, "embedding"), &v->dims);
		if (v->vector == NULL) {
			g_debug("voiceprint %s: no usable embedding; skipped", path);
			g_free(v);
			continue;
		}
		ai_embedding_normalize(v->vector, v->dims);
		v->name = g_strdup(name);
		v->model = g_strdup(model);
		v->count = (guint)CLAMP(ai_json_get_int(o, "count", 1), 1, G_MAXINT);
		g_ptr_array_add(self->voices, v);
	}
	g_dir_close(dir);
}

/* ---- the service ---- */

typedef struct {
	gchar *model;
	gfloat *vector;
	gsize dims;
	gchar *name; /* enrolling: whose */
	gsize bytes; /* how much audio it came from */
} Print;
static void
print_free(gpointer data)
{
	Print *p = data;
	g_free(p->model);
	g_free(p->vector);
	g_free(p->name);
	g_free(p);
}
static void
embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	g_autoptr(JsonParser) parser = json_parser_new();
	Print *print = g_task_get_task_data(task);
	JsonObject *o = NULL;
	gsize size = 0;
	const gchar *raw;
	guint status;
	if (body == NULL || ai_http_redirect_refused(message, &error)) {
		g_task_return_error(task, g_steal_pointer(&error));
		g_object_unref(task);
		return;
	}
	raw = g_bytes_get_data(body, &size);
	status = soup_message_get_status(message);
	if (size > 0 && size < 1024 * 1024 && json_parser_load_from_data(parser, raw, size, NULL))
		o = ai_json_root_object(parser);
	if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		const gchar *why = ai_json_get_string(o, "error", NULL);
		g_task_return_new_error(task, G_IO_ERROR,
								status == 422 ? G_IO_ERROR_INVALID_DATA : G_IO_ERROR_FAILED,
								"Voiceprint service: HTTP %u%s%s", status, why ? ": " : "",
								why ? why : "");
		g_object_unref(task);
		return;
	}
	print->model = g_strdup(ai_json_get_string(o, "model", NULL));
	print->vector = read_vector(ai_json_get_array(o, "embedding"), &print->dims);
	if (print->model == NULL || print->vector == NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
								"Voiceprint service answered without a model and embedding");
		g_object_unref(task);
		return;
	}
	ai_embedding_normalize(print->vector, print->dims);
	g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}
/* Takes TASK; its data is a Print, filled in once the service has answered. */
static void
embed(AiVoiceprintIdentifier *self, GBytes *pcm, GTask *task)
{
	g_autoptr(SoupMessage) message = NULL;
	if (g_bytes_get_size(pcm) < MIN_BYTES || g_bytes_get_size(pcm) % 2 != 0) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
								"Too little audio to tell who spoke");
		g_object_unref(task);
		return;
	}
	if (self->url == NULL ||
		(message = ai_http_message_new("POST", self->url, AI_HTTP_NO_CREDENTIALS)) == NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"No usable voiceprint service URL");
		g_object_unref(task);
		return;
	}
	((Print *)g_task_get_task_data(task))->bytes = g_bytes_get_size(pcm);
	soup_message_set_request_body_from_bytes(message, "application/octet-stream", pcm);
	soup_session_send_and_read_async(self->http, message, G_PRIORITY_DEFAULT,
									 g_task_get_cancellable(task), embedded, task);
}

/* ---- identify ---- */

/* The closest voice in LIST made by the same model, and how close. */
static Voice *
closest(GPtrArray *list, const Print *print, gdouble *score)
{
	Voice *best = NULL;
	guint i;
	*score = 0;
	for (i = 0; i < list->len; i++) {
		Voice *v = g_ptr_array_index(list, i);
		gdouble similarity;
		if (v->dims != print->dims || g_strcmp0(v->model, print->model) != 0)
			continue;
		similarity = ai_embedding_cosine(v->vector, print->vector, v->dims);
		if (best == NULL || similarity > *score) {
			best = v;
			*score = similarity;
		}
	}
	return best;
}
/* A guest's voiceprint is the running mean of everything heard from them. */
static void
absorb(Voice *v, const Print *print)
{
	gsize k;
	for (k = 0; k < v->dims; k++)
		v->vector[k] = (v->vector[k] * v->count + print->vector[k]) / (v->count + 1);
	ai_embedding_normalize(v->vector, v->dims);
	if (v->count < 50) /* keep adapting as the room changes */
		v->count++;
}
/* Who a voice that matches nobody enrolled is: a guest already heard, a new
 * one, or -- too short to start a guest on -- nobody yet (NULL). */
static const gchar *
guest(AiVoiceprintIdentifier *self, const Print *print, gdouble *score)
{
	Voice *v = closest(self->guests, print, score);
	if (v != NULL && (*score >= self->guest_threshold || self->guests->len >= MAX_GUESTS)) {
		absorb(v, print);
		return v->name;
	}
	if (print->bytes < MIN_NEW_GUEST_BYTES)
		return NULL;
	v = g_new0(Voice, 1);
	v->name = g_strdup_printf("Guest %u", self->guests->len + 1);
	v->model = g_strdup(print->model);
	v->dims = print->dims;
	v->vector = g_memdup2(print->vector, print->dims * sizeof(gfloat));
	v->count = 1;
	g_ptr_array_add(self->guests, v);
	*score = 1.0;
	return v->name;
}

static void
identify_embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *outer = data;
	g_autoptr(GError) error = NULL;
	AiVoiceprintIdentifier *self = g_task_get_source_object(outer);
	Print *print = g_task_get_task_data(G_TASK(result));
	const Voice *best;
	const gchar *name = NULL;
	gdouble best_score = 0;
	if (!g_task_propagate_boolean(G_TASK(result), &error)) {
		g_task_return_error(outer, g_steal_pointer(&error));
		g_object_unref(outer);
		return;
	}
	best = closest(self->voices, print, &best_score);
	if (best != NULL && best_score >= self->threshold)
		name = best->name;
	else if (self->track_guests) {
		name = guest(self, print, &best_score);
		if (name == NULL) {
			g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
									"Too little audio to tell a new voice");
			g_object_unref(outer);
			return;
		}
	}
	g_task_set_task_data(outer, g_memdup2(&best_score, sizeof(best_score)), g_free);
	g_task_return_pointer(outer, g_strdup(name), g_free);
	g_object_unref(outer);
}
static void
identify_async(AiSpeakerIdentifier *identifier, GBytes *pcm, GCancellable *cancellable,
			   GAsyncReadyCallback callback, gpointer user_data)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(identifier);
	GTask *outer = g_task_new(self, cancellable, callback, user_data);
	GTask *inner = g_task_new(self, cancellable, identify_embedded, outer);
	g_task_set_task_data(inner, g_new0(Print, 1), print_free);
	embed(self, pcm, inner);
}
static gchar *
identify_finish(AiSpeakerIdentifier *identifier, GAsyncResult *result, gdouble *score,
				GError **error)
{
	gdouble *best;
	gchar *name;
	g_return_val_if_fail(g_task_is_valid(result, identifier), NULL);
	best = g_task_get_task_data(G_TASK(result));
	name = g_task_propagate_pointer(G_TASK(result), error);
	if (score != NULL)
		*score = best != NULL ? *best : 0;
	return name;
}
static void
identifier_iface_init(AiSpeakerIdentifierInterface *iface)
{
	iface->identify_async = identify_async;
	iface->identify_finish = identify_finish;
}

/* ---- enroll ---- */

static gboolean
save(AiVoiceprintIdentifier *self, const Voice *v, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *json = NULL, *file = NULL, *path = NULL;
	gsize i;
	if (g_mkdir_with_parents(self->directory, 0700) != 0) {
		gint saved = errno;
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Cannot create %s: %s",
					self->directory, g_strerror(saved));
		return FALSE;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, v->name);
	json_builder_set_member_name(builder, "model");
	json_builder_add_string_value(builder, v->model);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, v->count);
	json_builder_set_member_name(builder, "embedding");
	json_builder_begin_array(builder);
	for (i = 0; i < v->dims; i++)
		json_builder_add_double_value(builder, v->vector[i]);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json = json_to_string(root, FALSE);
	file = g_strconcat(v->name, ".json", NULL);
	path = g_build_filename(self->directory, file, NULL);
	return g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}
static void
enroll_embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *outer = data;
	AiVoiceprintIdentifier *self = g_task_get_source_object(outer);
	Print *print = g_task_get_task_data(G_TASK(result));
	g_autoptr(GError) error = NULL;
	Voice *existing = NULL, *fresh;
	guint i;
	if (!g_task_propagate_boolean(G_TASK(result), &error)) {
		g_task_return_error(outer, g_steal_pointer(&error));
		g_object_unref(outer);
		return;
	}
	for (i = 0; i < self->voices->len; i++) {
		Voice *v = g_ptr_array_index(self->voices, i);
		if (g_str_equal(v->name, print->name))
			existing = v;
	}
	fresh = g_new0(Voice, 1);
	fresh->name = g_strdup(print->name);
	fresh->model = g_strdup(print->model);
	fresh->dims = print->dims;
	fresh->vector = g_memdup2(print->vector, print->dims * sizeof(gfloat));
	fresh->count = 1;
	/* More of the same voice sharpens it; another model starts it over. */
	if (existing != NULL && existing->dims == print->dims &&
		g_str_equal(existing->model, print->model)) {
		gsize k;
		for (k = 0; k < fresh->dims; k++)
			fresh->vector[k] = (existing->vector[k] * existing->count + print->vector[k]) /
							   (existing->count + 1);
		ai_embedding_normalize(fresh->vector, fresh->dims);
		fresh->count = existing->count + 1;
	}
	if (!save(self, fresh, &error)) {
		voice_free(fresh);
		g_task_return_error(outer, g_steal_pointer(&error));
		g_object_unref(outer);
		return;
	}
	if (existing != NULL)
		g_ptr_array_remove(self->voices, existing);
	g_ptr_array_add(self->voices, fresh);
	g_task_return_boolean(outer, TRUE);
	g_object_unref(outer);
}

/**
 * ai_voiceprint_identifier_enroll_async:
 * @self: an #AiVoiceprintIdentifier
 * @name: who is speaking; also the voiceprint's file name
 * @pcm: some seconds of them talking, 16 kHz mono S16LE
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 *
 * Saves @name's voiceprint in #AiVoiceprintIdentifier:directory, mode 0600.
 * Enrolling a name again averages the new audio into it.
 */
void
ai_voiceprint_identifier_enroll_async(AiVoiceprintIdentifier *self, const gchar *name,
									  GBytes *pcm, GCancellable *cancellable,
									  GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *outer, *inner;
	Print *print;
	g_return_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self));
	g_return_if_fail(pcm != NULL);
	outer = g_task_new(self, cancellable, callback, user_data);
	if (self->directory == NULL) {
		g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"No voiceprint directory");
		g_object_unref(outer);
		return;
	}
	if (!valid_name(name)) {
		g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"Not a usable name: \"%s\"", name != NULL ? name : "");
		g_object_unref(outer);
		return;
	}
	inner = g_task_new(self, cancellable, enroll_embedded, outer);
	print = g_new0(Print, 1);
	print->name = g_strdup(name);
	g_task_set_task_data(inner, print, print_free);
	embed(self, pcm, inner);
}

/**
 * ai_voiceprint_identifier_enroll_finish:
 * @self: an #AiVoiceprintIdentifier
 * @result: asynchronous result
 * @error: return location for an error
 *
 * Returns: whether the voiceprint was saved
 */
gboolean
ai_voiceprint_identifier_enroll_finish(AiVoiceprintIdentifier *self, GAsyncResult *result,
									   GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
	return g_task_propagate_boolean(G_TASK(result), error);
}

/**
 * ai_voiceprint_identifier_get_n_voices:
 * @self: an #AiVoiceprintIdentifier
 *
 * Returns: how many people are enrolled
 */
guint
ai_voiceprint_identifier_get_n_voices(AiVoiceprintIdentifier *self)
{
	g_return_val_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self), 0);
	return self->voices->len;
}

/**
 * ai_voiceprint_identifier_new:
 * @url: the voiceprint service's embedding endpoint
 * @directory: (nullable): where voiceprints are kept
 *
 * Returns: (transfer full): a new identifier, with the voiceprints in
 *   @directory already loaded
 */
AiVoiceprintIdentifier *
ai_voiceprint_identifier_new(const gchar *url, const gchar *directory)
{
	return g_object_new(AI_TYPE_VOICEPRINT_IDENTIFIER, "url", url, "directory", directory,
						NULL);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(object);
	if (id == PROP_URL)
		g_value_set_string(value, self->url);
	else if (id == PROP_DIRECTORY)
		g_value_set_string(value, self->directory);
	else if (id == PROP_THRESHOLD)
		g_value_set_double(value, self->threshold);
	else if (id == PROP_TIMEOUT)
		g_value_set_uint(value, self->timeout_ms);
	else if (id == PROP_TRACK_GUESTS)
		g_value_set_boolean(value, self->track_guests);
	else if (id == PROP_GUEST_THRESHOLD)
		g_value_set_double(value, self->guest_threshold);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(object);
	if (id == PROP_URL) {
		g_free(self->url);
		self->url = g_value_dup_string(value);
	} else if (id == PROP_DIRECTORY) {
		g_free(self->directory);
		self->directory = g_value_dup_string(value);
		load(self);
	} else if (id == PROP_THRESHOLD)
		self->threshold = g_value_get_double(value);
	else if (id == PROP_TIMEOUT) {
		self->timeout_ms = g_value_get_uint(value);
		g_object_set(self->http, "timeout", MAX(1u, (self->timeout_ms + 999) / 1000), NULL);
	} else if (id == PROP_TRACK_GUESTS)
		self->track_guests = g_value_get_boolean(value);
	else if (id == PROP_GUEST_THRESHOLD)
		self->guest_threshold = g_value_get_double(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
finalize(GObject *object)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(object);
	g_free(self->url);
	g_free(self->directory);
	g_ptr_array_unref(self->voices);
	g_ptr_array_unref(self->guests);
	g_object_unref(self->http);
	G_OBJECT_CLASS(ai_voiceprint_identifier_parent_class)->finalize(object);
}
static void
ai_voiceprint_identifier_class_init(AiVoiceprintIdentifierClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	g_object_class_install_property(
		oc, PROP_URL,
		g_param_spec_string("url", "URL", "Voiceprint service embedding endpoint", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_DIRECTORY,
		g_param_spec_string("directory", "Directory", "Where voiceprints are kept", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceprintIdentifier:threshold:
	 *
	 * The cosine similarity a voice must reach to be named. Below it, the
	 * speaker is nobody enrolled.
	 */
	g_object_class_install_property(
		oc, PROP_THRESHOLD,
		g_param_spec_double("threshold", "Threshold", "Similarity needed to name a speaker",
							0.0, 1.0, 0.55,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceprintIdentifier:track-guests:
	 *
	 * Name every voice, not only enrolled ones. A voice that matches nobody
	 * enrolled becomes "Guest 1", "Guest 2", ... in the order first heard, and
	 * later utterances in that voice are the same guest. Guests live in memory
	 * for this identifier's lifetime and are never saved. A new guest needs
	 * at least a second and a half of audio; a shorter unknown voice is
	 * %G_IO_ERROR_INVALID_DATA, too short to tell.
	 */
	g_object_class_install_property(
		oc, PROP_TRACK_GUESTS,
		g_param_spec_boolean("track-guests", "Track guests",
							 "Number voices that match nobody enrolled", FALSE,
							 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceprintIdentifier:guest-threshold:
	 *
	 * The similarity a voice needs to be a guest already heard rather than a
	 * new one.
	 */
	g_object_class_install_property(
		oc, PROP_GUEST_THRESHOLD,
		g_param_spec_double("guest-threshold", "Guest threshold",
							"Similarity needed to be a guest already heard", 0.0, 1.0, 0.35,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_TIMEOUT,
		g_param_spec_uint("timeout-ms", "Timeout", "Voiceprint service request timeout", 1,
						  600000, 5000,
						  G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
}
static void
ai_voiceprint_identifier_init(AiVoiceprintIdentifier *self)
{
	self->voices = g_ptr_array_new_with_free_func(voice_free);
	self->guests = g_ptr_array_new_with_free_func(voice_free);
	self->http = ai_http_session_new();
}
