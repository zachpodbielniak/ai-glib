/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voiceprint-identifier.h"
#include "voice/ai-http-speaker-embedder.h"
#include "voice/ai-file-voiceprint-store.h"
#include <string.h>

/*
 * Speaker identification by voiceprint: an AiSpeakerEmbedder turns each
 * utterance into a vector, which is compared with those an
 * AiVoiceprintStore holds.
 *
 * Embeddings come back in any order, but who is "Guest 1" depends on who
 * was heard first. So identifications wait in a queue in the order they
 * were asked for, and are matched from its head only: a fast reply for a
 * later utterance waits for the earlier one. An identification whose
 * cancellable fires leaves the queue at once, so one stuck embedding never
 * holds the rest.
 *
 * Guests are voices nobody enrolled, numbered as first heard and kept in
 * memory for the identifier's lifetime only: they are never saved unless
 * promoted, deliberately, under a real name.
 */
#define MAX_GUEST_WEIGHT 50 /* a guest keeps adapting as the room changes */

typedef struct {
	AiSpeakerEmbedder *embedder;
	AiVoiceprintStore *store;
	gulong store_changed_id;
	GPtrArray *voices; /* AiVoiceprint: enrolled, cached from the store */
	gdouble threshold;
	gboolean track_guests;
	gdouble guest_threshold;
	guint max_guests, min_guest_ms;
	GPtrArray *guests; /* AiVoiceprint "Guest N", in the order first heard */
	guint next_guest;
	GQueue pending; /* Pending*, in the order asked */
	GMainContext *context;
} AiVoiceprintIdentifierPrivate;

enum {
	PROP_0,
	PROP_EMBEDDER,
	PROP_STORE,
	PROP_THRESHOLD,
	PROP_TRACK_GUESTS,
	PROP_GUEST_THRESHOLD,
	PROP_MAX_GUESTS,
	PROP_MIN_GUEST_MS,
	N_PROPS
};
static GParamSpec *props[N_PROPS];
enum { SIGNAL_PROMOTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void identifier_iface_init(AiSpeakerIdentifierInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiVoiceprintIdentifier, ai_voiceprint_identifier, G_TYPE_OBJECT,
						G_ADD_PRIVATE(AiVoiceprintIdentifier)
							G_IMPLEMENT_INTERFACE(AI_TYPE_SPEAKER_IDENTIFIER,
												  identifier_iface_init))

static AiVoiceprintIdentifierPrivate *
private_of(AiVoiceprintIdentifier *self)
{
	return ai_voiceprint_identifier_get_instance_private(self);
}

/* ---- the enrolled voices ---- */

static void
load_voices(AiVoiceprintIdentifier *self)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	g_clear_pointer(&priv->voices, g_ptr_array_unref);
	priv->voices = priv->store != NULL ? ai_voiceprint_store_list(priv->store)
									   : g_ptr_array_new_with_free_func(g_object_unref);
}
static void
store_changed(AiVoiceprintStore *store, gpointer data)
{
	AiVoiceprintIdentifier *self = data;
	load_voices(self);
	g_debug("voiceprints reloaded: %u enrolled", private_of(self)->voices->len);
}
static void
set_store(AiVoiceprintIdentifier *self, AiVoiceprintStore *store)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	if (priv->store != NULL && priv->store_changed_id != 0)
		g_signal_handler_disconnect(priv->store, priv->store_changed_id);
	priv->store_changed_id = 0;
	g_set_object(&priv->store, store);
	if (priv->store != NULL)
		priv->store_changed_id =
			g_signal_connect(priv->store, "changed", G_CALLBACK(store_changed), self);
	load_voices(self);
}

/* The closest voiceprint in LIST comparable with HEARD, and how close;
 * EXCLUDE (nullable) is passed over. */
static AiVoiceprint *
closest_in(AiVoiceprintIdentifier *self, GPtrArray *list, AiVoiceprint *heard,
		   const gchar *exclude, gdouble *score)
{
	AiVoiceprintIdentifierClass *klass = AI_VOICEPRINT_IDENTIFIER_GET_CLASS(self);
	AiVoiceprint *best = NULL;
	guint i;
	*score = -1;
	for (i = 0; i < list->len; i++) {
		AiVoiceprint *v = g_ptr_array_index(list, i);
		gdouble similarity;
		if (!ai_voiceprint_is_comparable(v, heard) ||
			g_strcmp0(ai_voiceprint_get_name(v), exclude) == 0)
			continue;
		similarity = klass->score(self, v, heard);
		if (best == NULL || similarity > *score) {
			best = v;
			*score = similarity;
		}
	}
	return best;
}

/* ---- default policy ---- */

static gdouble
default_score(AiVoiceprintIdentifier *self, AiVoiceprint *known, AiVoiceprint *heard)
{
	return ai_voiceprint_similarity(known, heard);
}
static AiSpeakerMatch *
matched(AiSpeakerMatchKind kind, const gchar *name, gdouble score, AiVoiceprint *heard)
{
	AiSpeakerMatch *match = ai_speaker_match_new(kind, name, score);
	ai_speaker_match_set_voiceprint(match, heard);
	return match;
}
static AiSpeakerMatch *
default_match(AiVoiceprintIdentifier *self, AiVoiceprint *heard, gsize audio_bytes,
			  GError **error)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	AiVoiceprint *best, *guest;
	gdouble score = -1, guest_score = -1;
	g_autofree gchar *name = NULL;
	best = closest_in(self, priv->voices, heard, NULL, &score);
	if (best != NULL && score >= priv->threshold)
		return matched(AI_SPEAKER_MATCH_ENROLLED, ai_voiceprint_get_name(best), score, heard);
	if (!priv->track_guests)
		return matched(AI_SPEAKER_MATCH_UNKNOWN, NULL, MAX(score, 0.0), heard);
	/* A guest already heard: their voiceprint adapts to what they say. */
	guest = closest_in(self, priv->guests, heard, NULL, &guest_score);
	if (guest != NULL && guest_score >= priv->guest_threshold) {
		ai_voiceprint_absorb(guest, heard, MAX_GUEST_WEIGHT);
		return matched(AI_SPEAKER_MATCH_GUEST, ai_voiceprint_get_name(guest), guest_score,
					   heard);
	}
	/* A new guest needs enough audio: a short clip's voiceprint is too noisy
	 * to tell a new voice from a known one said briefly, and every false
	 * guest splits someone in two. 32 bytes is a millisecond. */
	if (audio_bytes < (gsize)priv->min_guest_ms * 32) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Too little audio to tell a new voice");
		return NULL;
	}
	/* Full: a stranger beyond the cap is unknown, never folded into a guest
	 * it does not sound like. */
	if (priv->guests->len >= priv->max_guests)
		return matched(AI_SPEAKER_MATCH_UNKNOWN, NULL, MAX(guest_score, 0.0), heard);
	name = g_strdup_printf("Guest %u", ++priv->next_guest);
	g_ptr_array_add(priv->guests, ai_voiceprint_copy(heard, name));
	return matched(AI_SPEAKER_MATCH_GUEST, name, 1.0, heard);
}

/* ---- identify, in the order asked ----
 *
 * A Pending lives in the queue until it is answered. When its caller
 * cancels, it is answered at once and leaves the queue, but its embedding
 * still owes a callback: the shell is then owned by that callback, which
 * frees it. */
typedef struct {
	GTask *task; /* NULL once answered early, on cancel */
	AiVoiceprint *heard;
	GError *error;
	gsize bytes;
	gboolean ready;
	GCancellable *cancellable;
	gulong cancel_id;
} Pending;
static void
pending_disconnect(Pending *p)
{
	if (p->cancel_id != 0)
		g_cancellable_disconnect(p->cancellable, p->cancel_id);
	p->cancel_id = 0;
}
static void
pending_free(Pending *p)
{
	pending_disconnect(p);
	g_clear_object(&p->cancellable);
	g_clear_object(&p->task);
	g_clear_object(&p->heard);
	g_clear_error(&p->error);
	g_free(p);
}
/* Match and answer every identification at the head that is ready. */
static void
drain_pending(AiVoiceprintIdentifier *self)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	Pending *p;
	while ((p = g_queue_peek_head(&priv->pending)) != NULL && p->ready) {
		g_queue_pop_head(&priv->pending);
		pending_disconnect(p);
		/* No match for a caller who gave up: matching would grow a guest. */
		if (p->error == NULL && p->cancellable != NULL &&
			g_cancellable_is_cancelled(p->cancellable))
			p->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
										   "Operation was cancelled");
		if (p->error != NULL)
			g_task_return_error(p->task, g_steal_pointer(&p->error));
		else {
			GError *error = NULL;
			AiSpeakerMatch *match =
				AI_VOICEPRINT_IDENTIFIER_GET_CLASS(self)->match(self, p->heard, p->bytes, &error);
			if (match != NULL)
				g_task_return_pointer(p->task, match, g_object_unref);
			else
				g_task_return_error(p->task,
									error != NULL ? error
												  : g_error_new_literal(G_IO_ERROR,
																		G_IO_ERROR_INVALID_DATA,
																		"Too little audio to tell"));
		}
		pending_free(p);
	}
}
typedef struct {
	AiVoiceprintIdentifier *self; /* owned */
	Pending *pending;
} Embedding;
static void
embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	Embedding *e = data;
	AiVoiceprintIdentifier *self = e->self;
	Pending *p = e->pending;
	GError *error = NULL;
	AiVoiceprint *heard = ai_speaker_embedder_embed_finish(AI_SPEAKER_EMBEDDER(source), result,
														   &error);
	g_free(e);
	if (p->task == NULL) {
		/* Answered already, on cancel: drop the late embedding. */
		g_clear_object(&heard);
		g_clear_error(&error);
		pending_free(p);
	} else {
		p->heard = heard;
		p->error = error;
		p->ready = TRUE;
		drain_pending(self);
	}
	g_object_unref(self);
}
/* The caller gave up: answer now and leave the queue, rather than wait for
 * the embedder, so a stuck embedding never holds later utterances. The
 * pending is looked up by pointer before it is touched: it may have been
 * answered and freed in the meantime. */
typedef struct {
	AiVoiceprintIdentifier *self; /* owned */
	Pending *pending;			  /* maybe freed: compare only */
} Cancel;
static gboolean
cancelled_idle(gpointer data)
{
	Cancel *c = data;
	AiVoiceprintIdentifierPrivate *priv = private_of(c->self);
	Pending *p = c->pending;
	if (g_queue_find(&priv->pending, p) == NULL || p->ready)
		return G_SOURCE_REMOVE;
	g_queue_remove(&priv->pending, p);
	pending_disconnect(p);
	g_task_return_new_error(p->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
							"Operation was cancelled");
	g_clear_object(&p->task);
	drain_pending(c->self);
	return G_SOURCE_REMOVE;
}
static void
cancel_free(gpointer data)
{
	Cancel *c = data;
	g_object_unref(c->self);
	g_free(c);
}
/* Any thread: whoever cancelled. Hands the work to the identifier's context. */
static void
on_cancelled(GCancellable *cancellable, gpointer data)
{
	Cancel *c = data;
	Cancel *copy = g_new0(Cancel, 1);
	GSource *idle = g_idle_source_new();
	copy->self = g_object_ref(c->self);
	copy->pending = c->pending;
	g_source_set_callback(idle, cancelled_idle, copy, cancel_free);
	g_source_attach(idle, private_of(c->self)->context);
	g_source_unref(idle);
}
static void
identify_async(AiSpeakerIdentifier *identifier, GBytes *pcm, GCancellable *cancellable,
			   GAsyncReadyCallback callback, gpointer user_data)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(identifier);
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	Pending *p = g_new0(Pending, 1);
	Embedding *e;
	p->task = g_task_new(self, cancellable, callback, user_data);
	p->bytes = g_bytes_get_size(pcm);
	if (priv->embedder == NULL) {
		g_task_return_new_error(p->task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
								"No speaker embedder");
		pending_free(p);
		return;
	}
	g_queue_push_tail(&priv->pending, p);
	if (cancellable != NULL) {
		Cancel *c = g_new0(Cancel, 1);
		c->self = g_object_ref(self);
		c->pending = p;
		p->cancellable = g_object_ref(cancellable);
		p->cancel_id =
			g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled), c, cancel_free);
	}
	e = g_new0(Embedding, 1);
	e->self = g_object_ref(self);
	e->pending = p;
	ai_speaker_embedder_embed_async(priv->embedder, pcm, cancellable, embedded, e);
}
static AiSpeakerMatch *
identify_finish(AiSpeakerIdentifier *identifier, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, identifier), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
static gboolean
has_name(AiSpeakerIdentifier *identifier, const gchar *name)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(AI_VOICEPRINT_IDENTIFIER(identifier));
	g_autoptr(AiVoiceprint) found = NULL;
	if (priv->store == NULL)
		return FALSE;
	found = ai_voiceprint_store_lookup(priv->store, name);
	return found != NULL;
}

/* ---- promotion ---- */

static gboolean
promote(AiSpeakerIdentifier *identifier, const gchar *guest_name, const gchar *name,
		gboolean merge, GError **error)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(identifier);
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	g_autoptr(AiVoiceprint) existing = NULL;
	g_autoptr(AiVoiceprint) saved = NULL;
	g_autofree gchar *was = NULL;
	AiVoiceprint *guest = NULL;
	guint i;
	if (priv->store == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"No voiceprint store to save a voice in");
		return FALSE;
	}
	if (!ai_voiceprint_name_is_valid(name)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
					"Not a usable voiceprint name: \"%s\"", name);
		return FALSE;
	}
	for (i = 0; i < priv->guests->len && guest == NULL; i++)
		if (g_ascii_strcasecmp(ai_voiceprint_get_name(g_ptr_array_index(priv->guests, i)),
							   guest_name) == 0)
			guest = g_ptr_array_index(priv->guests, i);
	if (guest == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No voice called %s was heard",
					guest_name);
		return FALSE;
	}
	/* One person must never silently take over another's voiceprint. */
	existing = ai_voiceprint_store_lookup(priv->store, name);
	if (existing != NULL && !merge) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "%s is already enrolled", name);
		return FALSE;
	}
	if (existing != NULL && ai_voiceprint_is_comparable(existing, guest)) {
		ai_voiceprint_absorb(existing, guest, 0);
		saved = g_object_ref(existing);
	} else
		saved = ai_voiceprint_copy(guest, name);
	if (!ai_voiceprint_store_save(priv->store, saved, error))
		return FALSE;
	/* The guest's name goes with them; the number is not reused. */
	was = g_strdup(ai_voiceprint_get_name(guest));
	g_ptr_array_remove(priv->guests, guest);
	load_voices(self);
	g_signal_emit(self, signals[SIGNAL_PROMOTED], 0, was, name);
	return TRUE;
}
static void
identifier_iface_init(AiSpeakerIdentifierInterface *iface)
{
	iface->identify_async = identify_async;
	iface->identify_finish = identify_finish;
	iface->promote = promote;
	iface->has_name = has_name;
}

/* ---- enroll ---- */

static void
enroll_embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	AiVoiceprintIdentifier *self = g_task_get_source_object(task);
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	const gchar *name = g_task_get_task_data(task);
	g_autoptr(AiVoiceprint) heard = NULL;
	g_autoptr(AiVoiceprint) existing = NULL;
	g_autoptr(AiVoiceprint) fresh = NULL;
	GError *error = NULL;
	heard = ai_speaker_embedder_embed_finish(AI_SPEAKER_EMBEDDER(source), result, &error);
	if (heard == NULL) {
		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}
	/* More of the same voice sharpens it; another model starts it over. */
	existing = ai_voiceprint_store_lookup(priv->store, name);
	if (existing != NULL && ai_voiceprint_is_comparable(existing, heard)) {
		ai_voiceprint_absorb(existing, heard, 0);
		fresh = g_object_ref(existing);
	} else
		fresh = ai_voiceprint_copy(heard, name);
	if (!ai_voiceprint_store_save(priv->store, fresh, &error)) {
		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}
	load_voices(self);
	g_task_return_pointer(task, g_steal_pointer(&fresh), g_object_unref);
	g_object_unref(task);
}

/**
 * ai_voiceprint_identifier_enroll_async:
 * @self: an #AiVoiceprintIdentifier
 * @name: who is speaking; see ai_voiceprint_name_is_valid()
 * @pcm: some seconds of them talking, 16 kHz mono S16LE
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 *
 * Saves @name's voiceprint in the store. Enrolling a name again averages
 * the new audio into it, which makes it steadier; a voiceprint from another
 * model is replaced.
 */
void
ai_voiceprint_identifier_enroll_async(AiVoiceprintIdentifier *self, const gchar *name,
									  GBytes *pcm, GCancellable *cancellable,
									  GAsyncReadyCallback callback, gpointer user_data)
{
	AiVoiceprintIdentifierPrivate *priv;
	GTask *task;
	g_return_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self));
	g_return_if_fail(pcm != NULL);
	priv = private_of(self);
	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, ai_voiceprint_identifier_enroll_async);
	if (priv->store == NULL || priv->embedder == NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
								"Enrolling needs an embedder and a store");
		g_object_unref(task);
		return;
	}
	if (!ai_voiceprint_name_is_valid(name)) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"Not a usable name: \"%s\"", name != NULL ? name : "");
		g_object_unref(task);
		return;
	}
	g_task_set_task_data(task, g_strdup(name), g_free);
	ai_speaker_embedder_embed_async(priv->embedder, pcm, cancellable, enroll_embedded, task);
}

/**
 * ai_voiceprint_identifier_enroll_finish:
 * @self: an #AiVoiceprintIdentifier
 * @result: asynchronous result
 * @error: return location for an error
 *
 * Returns: (transfer full) (nullable): the voiceprint as saved
 */
AiVoiceprint *
ai_voiceprint_identifier_enroll_finish(AiVoiceprintIdentifier *self, GAsyncResult *result,
									   GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- the rest of the API ---- */

/**
 * ai_voiceprint_identifier_get_embedder:
 * @self: an #AiVoiceprintIdentifier
 *
 * Returns: (transfer none) (nullable): the embedder
 */
AiSpeakerEmbedder *
ai_voiceprint_identifier_get_embedder(AiVoiceprintIdentifier *self)
{
	g_return_val_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self), NULL);
	return private_of(self)->embedder;
}

/**
 * ai_voiceprint_identifier_get_store:
 * @self: an #AiVoiceprintIdentifier
 *
 * Returns: (transfer none) (nullable): the store
 */
AiVoiceprintStore *
ai_voiceprint_identifier_get_store(AiVoiceprintIdentifier *self)
{
	g_return_val_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self), NULL);
	return private_of(self)->store;
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
	return private_of(self)->voices->len;
}

/**
 * ai_voiceprint_identifier_reload:
 * @self: an #AiVoiceprintIdentifier
 *
 * Reads the enrolled voices from the store again. A store that emits
 * #AiVoiceprintStore::changed needs no call.
 */
void
ai_voiceprint_identifier_reload(AiVoiceprintIdentifier *self)
{
	g_return_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self));
	load_voices(self);
}

/**
 * ai_voiceprint_identifier_closest:
 * @self: an #AiVoiceprintIdentifier
 * @heard: a voiceprint
 * @exclude: (nullable): a name to pass over, such as the one being enrolled
 * @score: (out) (optional): its similarity
 *
 * The enrolled voice most like @heard, at any similarity: enrolment uses it
 * to warn that two people sound alike to the model.
 *
 * Returns: (transfer full) (nullable): the name, or %NULL with nobody
 *   comparable enrolled
 */
gchar *
ai_voiceprint_identifier_closest(AiVoiceprintIdentifier *self, AiVoiceprint *heard,
								 const gchar *exclude, gdouble *score)
{
	AiVoiceprint *best;
	gdouble s = -1;
	g_return_val_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self), NULL);
	g_return_val_if_fail(AI_IS_VOICEPRINT(heard), NULL);
	best = closest_in(self, private_of(self)->voices, heard, exclude, &s);
	if (score != NULL)
		*score = s;
	return best != NULL ? g_strdup(ai_voiceprint_get_name(best)) : NULL;
}

/**
 * ai_voiceprint_identifier_list_guests:
 * @self: an #AiVoiceprintIdentifier
 *
 * Returns: (transfer container) (element-type AiVoiceprint): copies of the
 *   guests heard so far, in the order first heard
 */
GPtrArray *
ai_voiceprint_identifier_list_guests(AiVoiceprintIdentifier *self)
{
	AiVoiceprintIdentifierPrivate *priv;
	GPtrArray *out;
	guint i;
	g_return_val_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self), NULL);
	priv = private_of(self);
	out = g_ptr_array_new_with_free_func(g_object_unref);
	for (i = 0; i < priv->guests->len; i++)
		g_ptr_array_add(out, ai_voiceprint_copy(g_ptr_array_index(priv->guests, i), NULL));
	return out;
}

/**
 * ai_voiceprint_identifier_forget_guests:
 * @self: an #AiVoiceprintIdentifier
 *
 * Drops every guest and starts numbering from 1 again, as for a new call.
 */
void
ai_voiceprint_identifier_forget_guests(AiVoiceprintIdentifier *self)
{
	AiVoiceprintIdentifierPrivate *priv;
	g_return_if_fail(AI_IS_VOICEPRINT_IDENTIFIER(self));
	priv = private_of(self);
	g_ptr_array_set_size(priv->guests, 0);
	priv->next_guest = 0;
}

/**
 * ai_voiceprint_identifier_new:
 * @url: a voiceprint service's embedding endpoint
 * @directory: (nullable): where voiceprints are kept
 *
 * An identifier over an #AiHttpSpeakerEmbedder and a watched
 * #AiFileVoiceprintStore.
 *
 * Returns: (transfer full): a new identifier, with the voiceprints in
 *   @directory already loaded
 */
AiVoiceprintIdentifier *
ai_voiceprint_identifier_new(const gchar *url, const gchar *directory)
{
	g_autoptr(AiHttpSpeakerEmbedder) embedder = ai_http_speaker_embedder_new(url);
	g_autoptr(AiFileVoiceprintStore) store = NULL;
	if (directory != NULL)
		store = g_object_new(AI_TYPE_FILE_VOICEPRINT_STORE, "directory", directory, "monitor",
							 TRUE, NULL);
	return ai_voiceprint_identifier_new_full(AI_SPEAKER_EMBEDDER(embedder),
											 store != NULL ? AI_VOICEPRINT_STORE(store) : NULL);
}

/**
 * ai_voiceprint_identifier_new_full:
 * @embedder: what turns audio into voiceprints
 * @store: (nullable): where enrolled voiceprints are kept; %NULL: none
 *   enrolled, guests only
 *
 * Returns: (transfer full): a new identifier
 */
AiVoiceprintIdentifier *
ai_voiceprint_identifier_new_full(AiSpeakerEmbedder *embedder, AiVoiceprintStore *store)
{
	g_return_val_if_fail(AI_IS_SPEAKER_EMBEDDER(embedder), NULL);
	g_return_val_if_fail(store == NULL || AI_IS_VOICEPRINT_STORE(store), NULL);
	return g_object_new(AI_TYPE_VOICEPRINT_IDENTIFIER, "embedder", embedder, "store", store,
						NULL);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(AI_VOICEPRINT_IDENTIFIER(object));
	switch (id) {
	case PROP_EMBEDDER:
		g_value_set_object(value, priv->embedder);
		break;
	case PROP_STORE:
		g_value_set_object(value, priv->store);
		break;
	case PROP_THRESHOLD:
		g_value_set_double(value, priv->threshold);
		break;
	case PROP_TRACK_GUESTS:
		g_value_set_boolean(value, priv->track_guests);
		break;
	case PROP_GUEST_THRESHOLD:
		g_value_set_double(value, priv->guest_threshold);
		break;
	case PROP_MAX_GUESTS:
		g_value_set_uint(value, priv->max_guests);
		break;
	case PROP_MIN_GUEST_MS:
		g_value_set_uint(value, priv->min_guest_ms);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(object);
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	switch (id) {
	case PROP_EMBEDDER:
		g_set_object(&priv->embedder, g_value_get_object(value));
		break;
	case PROP_STORE:
		set_store(self, g_value_get_object(value));
		break;
	case PROP_THRESHOLD:
		priv->threshold = g_value_get_double(value);
		break;
	case PROP_TRACK_GUESTS:
		priv->track_guests = g_value_get_boolean(value);
		break;
	case PROP_GUEST_THRESHOLD:
		priv->guest_threshold = g_value_get_double(value);
		break;
	case PROP_MAX_GUESTS:
		priv->max_guests = g_value_get_uint(value);
		break;
	case PROP_MIN_GUEST_MS:
		priv->min_guest_ms = g_value_get_uint(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
dispose(GObject *object)
{
	AiVoiceprintIdentifier *self = AI_VOICEPRINT_IDENTIFIER(object);
	set_store(self, NULL);
	g_clear_object(&private_of(self)->embedder);
	G_OBJECT_CLASS(ai_voiceprint_identifier_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(AI_VOICEPRINT_IDENTIFIER(object));
	/* Every embedding in flight holds a reference, so no Pending is left. */
	g_ptr_array_unref(priv->voices);
	g_ptr_array_unref(priv->guests);
	g_main_context_unref(priv->context);
	G_OBJECT_CLASS(ai_voiceprint_identifier_parent_class)->finalize(object);
}
static void
ai_voiceprint_identifier_class_init(AiVoiceprintIdentifierClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->dispose = dispose;
	oc->finalize = finalize;
	klass->score = default_score;
	klass->match = default_match;
	/**
	 * AiVoiceprintIdentifier:embedder:
	 *
	 * What turns audio into voiceprints.
	 */
	props[PROP_EMBEDDER] = g_param_spec_object(
		"embedder", "Embedder", "Turns audio into voiceprints", AI_TYPE_SPEAKER_EMBEDDER,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprintIdentifier:store:
	 *
	 * Where enrolled voiceprints are kept. Reloaded whenever it emits
	 * #AiVoiceprintStore::changed.
	 */
	props[PROP_STORE] =
		g_param_spec_object("store", "Store", "Where enrolled voiceprints are kept",
							AI_TYPE_VOICEPRINT_STORE,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprintIdentifier:threshold:
	 *
	 * The similarity a voice must reach to be named as someone enrolled.
	 * The right value belongs to the embedding model: measure it with the
	 * device's own audio.
	 */
	props[PROP_THRESHOLD] = g_param_spec_double(
		"threshold", "Threshold", "Similarity needed to name a speaker", 0.0, 1.0, 0.55,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprintIdentifier:track-guests:
	 *
	 * Name every voice, not only enrolled ones. A voice that matches nobody
	 * enrolled becomes "Guest 1", "Guest 2", ... in the order first heard,
	 * and later utterances in that voice are the same guest. Guests live in
	 * memory and are never saved unless promoted. A new guest needs
	 * #AiVoiceprintIdentifier:min-guest-ms of audio; a shorter unknown voice
	 * is %G_IO_ERROR_INVALID_DATA, too short to tell.
	 */
	props[PROP_TRACK_GUESTS] =
		g_param_spec_boolean("track-guests", "Track guests",
							 "Number voices that match nobody enrolled", FALSE,
							 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprintIdentifier:guest-threshold:
	 *
	 * The similarity a voice needs to be a guest already heard rather than
	 * a new one.
	 */
	props[PROP_GUEST_THRESHOLD] = g_param_spec_double(
		"guest-threshold", "Guest threshold", "Similarity needed to be a guest already heard",
		0.0, 1.0, 0.35, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprintIdentifier:max-guests:
	 *
	 * The most guests tracked at once. A stranger beyond it is unknown.
	 */
	props[PROP_MAX_GUESTS] = g_param_spec_uint(
		"max-guests", "Maximum guests", "Most guests tracked at once", 1, 1024, 32,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprintIdentifier:min-guest-ms:
	 *
	 * The least audio that may start a new guest.
	 */
	props[PROP_MIN_GUEST_MS] = g_param_spec_uint(
		"min-guest-ms", "Minimum guest audio", "Least audio that starts a new guest", 0,
		60000, 1500, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
	/**
	 * AiVoiceprintIdentifier::promoted:
	 * @self: the identifier
	 * @guest: the guest's name, as it was ("Guest 2")
	 * @name: the name the voice is now enrolled as
	 *
	 * A guest's voice was saved under a lasting name.
	 */
	signals[SIGNAL_PROMOTED] = g_signal_new(
		"promoted", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET(AiVoiceprintIdentifierClass, promoted), NULL, NULL, NULL, G_TYPE_NONE,
		2, G_TYPE_STRING, G_TYPE_STRING);
}
static void
ai_voiceprint_identifier_init(AiVoiceprintIdentifier *self)
{
	AiVoiceprintIdentifierPrivate *priv = private_of(self);
	priv->voices = g_ptr_array_new_with_free_func(g_object_unref);
	priv->guests = g_ptr_array_new_with_free_func(g_object_unref);
	priv->context = g_main_context_ref_thread_default();
	g_queue_init(&priv->pending);
}
