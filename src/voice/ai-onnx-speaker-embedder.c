/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-onnx-speaker-embedder.h"
#include "voice/ai-speech-features-private.h"
#include <string.h>
#ifdef AI_HAVE_ONNXRUNTIME
#include <onnxruntime_c_api.h>
#endif

/*
 * A speaker-embedding model run in this process with ONNX Runtime: no
 * service, no network, and the audio never leaves the machine.
 *
 * Models exported for sherpa-onnx carry what is needed to feed them in
 * their metadata ("framework", "normalize_samples", "feat_dim",
 * "output_dim"), so the front-end is chosen from it; without metadata the
 * input shape decides. The model id that tags every voiceprint is the
 * file's name and the start of its SHA-256, so two files that merely share
 * a name never have their voiceprints compared.
 *
 * Built without onnxruntime, the type still exists (so the API and the
 * introspection data do not depend on the build), but creating one fails
 * with G_IO_ERROR_NOT_SUPPORTED and ai_onnx_speaker_embedder_is_available()
 * says so up front.
 */
typedef struct {
	gchar *model_path, *model_id;
	AiSpeakerFrontend frontend;
	guint threads, bins, dims;
	gboolean scale_to_int16; /* wespeaker: samples at 16-bit scale */
	gchar *output_name;
	GPtrArray *input_names; /* the features first; for NeMo, then the length */
#ifdef AI_HAVE_ONNXRUNTIME
	OrtSession *session;
#endif
} AiOnnxSpeakerEmbedderPrivate;

enum { PROP_0, PROP_MODEL_PATH, PROP_FRONTEND, PROP_THREADS, PROP_MODEL_ID, N_PROPS };
static GParamSpec *props[N_PROPS];

static void embedder_iface_init(AiSpeakerEmbedderInterface *iface);
static void initable_iface_init(GInitableIface *iface);
G_DEFINE_TYPE_WITH_CODE(AiOnnxSpeakerEmbedder, ai_onnx_speaker_embedder, G_TYPE_OBJECT,
						G_ADD_PRIVATE(AiOnnxSpeakerEmbedder)
							G_IMPLEMENT_INTERFACE(AI_TYPE_SPEAKER_EMBEDDER, embedder_iface_init)
								G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, initable_iface_init))

/**
 * ai_speaker_frontend_get_type:
 *
 * Returns: the registered #AiSpeakerFrontend type
 */
GType
ai_speaker_frontend_get_type(void)
{
	static gsize type_id;
	if (g_once_init_enter(&type_id)) {
		static const GEnumValue values[] = {
			{AI_SPEAKER_FRONTEND_AUTO, "AI_SPEAKER_FRONTEND_AUTO", "auto"},
			{AI_SPEAKER_FRONTEND_KALDI, "AI_SPEAKER_FRONTEND_KALDI", "kaldi"},
			{AI_SPEAKER_FRONTEND_NEMO, "AI_SPEAKER_FRONTEND_NEMO", "nemo"},
			{0, NULL, NULL}};
		GType registered = g_enum_register_static("AiSpeakerFrontend", values);
		g_once_init_leave(&type_id, registered);
	}
	return type_id;
}

static AiOnnxSpeakerEmbedderPrivate *
private_of(AiOnnxSpeakerEmbedder *self)
{
	return ai_onnx_speaker_embedder_get_instance_private(self);
}

#ifdef AI_HAVE_ONNXRUNTIME
/* "name.onnx:0123456789ab": the file and the start of its SHA-256. */
static gchar *
model_id_of(const gchar *path, GError **error)
{
	g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autoptr(GFileInputStream) in = g_file_read(file, NULL, error);
	g_autofree gchar *base = g_path_get_basename(path);
	guint8 buffer[65536];
	gssize got;
	gchar digest[13];
	if (in == NULL)
		return NULL;
	while ((got = g_input_stream_read(G_INPUT_STREAM(in), buffer, sizeof(buffer), NULL, error)) >
		   0)
		g_checksum_update(sum, buffer, (gsize)got);
	if (got < 0)
		return NULL;
	g_strlcpy(digest, g_checksum_get_string(sum), sizeof(digest));
	return g_strdup_printf("%s:%s", base, digest);
}
#endif

static gfloat *
default_features(AiOnnxSpeakerEmbedder *self, const gfloat *samples, gsize n, guint bins,
				 gsize *frames)
{
	AiOnnxSpeakerEmbedderPrivate *priv = private_of(self);
	gfloat *features;
	if (priv->frontend == AI_SPEAKER_FRONTEND_NEMO) {
		features = ai_speech_features_nemo_mel(samples, n, bins, frames);
		ai_speech_features_normalize_per_feature(features, *frames, bins);
		return features;
	}
	if (priv->scale_to_int16) {
		g_autofree gfloat *scaled = g_new(gfloat, n);
		gsize i;
		for (i = 0; i < n; i++)
			scaled[i] = samples[i] * 32768.0f;
		features = ai_speech_features_kaldi_fbank(scaled, n, bins, frames);
	} else
		features = ai_speech_features_kaldi_fbank(samples, n, bins, frames);
	/* Cepstral mean normalisation, as both wespeaker and 3D-Speaker train. */
	ai_speech_features_subtract_mean(features, *frames, bins);
	return features;
}

#ifdef AI_HAVE_ONNXRUNTIME

/* ---- ONNX Runtime ---- */

static const OrtApi *
ort(void)
{
	static const OrtApi *api;
	static gsize once;
	if (g_once_init_enter(&once)) {
		const OrtApiBase *base = OrtGetApiBase();
		api = base != NULL ? base->GetApi(ORT_API_VERSION) : NULL;
		g_once_init_leave(&once, 1);
	}
	return api;
}
/* Consumes STATUS; FALSE with ERROR set when it was a failure. */
static gboolean
ort_ok(OrtStatus *status, const gchar *what, GError **error)
{
	if (status == NULL)
		return TRUE;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ONNX Runtime: %s: %s", what,
				ort()->GetErrorMessage(status));
	ort()->ReleaseStatus(status);
	return FALSE;
}
/* One environment per process, as ONNX Runtime wants. */
static OrtEnv *
ort_env(GError **error)
{
	static OrtEnv *env;
	static gchar *failure;
	static gsize once;
	if (g_once_init_enter(&once)) {
		GError *local = NULL;
		if (ort() == NULL)
			failure = g_strdup("the installed onnxruntime is older than ai-glib was built for");
		else if (!ort_ok(ort()->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ai-glib", &env), "environment",
						 &local)) {
			failure = g_strdup(local->message);
			g_error_free(local);
		}
		g_once_init_leave(&once, 1);
	}
	if (env == NULL)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, failure);
	return env;
}
/* Copy an ONNX Runtime string into GLib's allocator and free the original. */
static gchar *
ort_take_string(OrtAllocator *allocator, char *value)
{
	gchar *copy = g_strdup(value);
	OrtStatus *st = ort()->AllocatorFree(allocator, value);
	if (st != NULL)
		ort()->ReleaseStatus(st);
	return copy;
}
static gchar *
metadata(OrtModelMetadata *md, OrtAllocator *allocator, const gchar *key)
{
	char *value = NULL;
	OrtStatus *st = ort()->ModelMetadataLookupCustomMetadataMap(md, allocator, key, &value);
	if (st != NULL) {
		ort()->ReleaseStatus(st);
		return NULL;
	}
	return value != NULL ? ort_take_string(allocator, value) : NULL;
}
/* The shape of input or output I, into DIMS (at most 8); returns the rank. */
static gsize
io_shape(OrtSession *session, gboolean input, gsize i, gint64 *dims, GError **error)
{
	OrtTypeInfo *info = NULL;
	const OrtTensorTypeAndShapeInfo *tensor = NULL;
	size_t rank = 0;
	OrtStatus *st = input ? ort()->SessionGetInputTypeInfo(session, i, &info)
						  : ort()->SessionGetOutputTypeInfo(session, i, &info);
	if (!ort_ok(st, "type info", error))
		return 0;
	if (ort_ok(ort()->CastTypeInfoToTensorInfo(info, &tensor), "tensor info", error) &&
		tensor != NULL && ort_ok(ort()->GetDimensionsCount(tensor, &rank), "rank", error) &&
		rank <= 8)
		ort_ok(ort()->GetDimensions(tensor, (int64_t *)dims, rank), "shape", error);
	ort()->ReleaseTypeInfo(info);
	return rank <= 8 ? rank : 0;
}
static gchar *
io_name(OrtSession *session, OrtAllocator *allocator, gboolean input, gsize i)
{
	char *name = NULL;
	OrtStatus *st = input ? ort()->SessionGetInputName(session, i, allocator, &name)
						  : ort()->SessionGetOutputName(session, i, allocator, &name);
	if (st != NULL) {
		ort()->ReleaseStatus(st);
		return NULL;
	}
	return ort_take_string(allocator, name);
}
/* Load the model and work out how to feed it. */
static gboolean
load_model(AiOnnxSpeakerEmbedder *self, GError **error)
{
	AiOnnxSpeakerEmbedderPrivate *priv = private_of(self);
	OrtEnv *env = ort_env(error);
	OrtSessionOptions *options = NULL;
	OrtAllocator *allocator = NULL;
	OrtModelMetadata *md = NULL;
	g_autofree gchar *framework = NULL, *normalize = NULL, *feat_dim = NULL, *output_dim = NULL;
	gint64 dims[8];
	size_t n_in = 0, n_out = 0, i;
	gsize rank;
	gboolean ok;
	if (env == NULL)
		return FALSE;
	if (!ort_ok(ort()->CreateSessionOptions(&options), "options", error))
		return FALSE;
	ok = ort_ok(ort()->SetIntraOpNumThreads(options, (int)MAX(1u, priv->threads)), "threads",
				error) &&
		 ort_ok(ort()->SetInterOpNumThreads(options, 1), "threads", error) &&
		 ort_ok(ort()->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL), "options",
				error) &&
		 ort_ok(ort()->CreateSession(env, priv->model_path, options, &priv->session),
				priv->model_path, error);
	ort()->ReleaseSessionOptions(options);
	if (!ok)
		return FALSE;
	if (!ort_ok(ort()->GetAllocatorWithDefaultOptions(&allocator), "allocator", error) ||
		!ort_ok(ort()->SessionGetInputCount(priv->session, &n_in), "inputs", error) ||
		!ort_ok(ort()->SessionGetOutputCount(priv->session, &n_out), "outputs", error))
		return FALSE;
	if (n_in == 0 || n_out == 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"%s has no inputs or no outputs", priv->model_path);
		return FALSE;
	}
	/* What sherpa-onnx's exports say about themselves. */
	if (ort_ok(ort()->SessionGetModelMetadata(priv->session, &md), "metadata", NULL)) {
		framework = metadata(md, allocator, "framework");
		normalize = metadata(md, allocator, "normalize_samples");
		feat_dim = metadata(md, allocator, "feat_dim");
		output_dim = metadata(md, allocator, "output_dim");
		ort()->ReleaseModelMetadata(md);
	}
	for (i = 0; i < n_in; i++)
		g_ptr_array_add(priv->input_names, io_name(priv->session, allocator, TRUE, i));
	rank = io_shape(priv->session, TRUE, 0, dims, error);
	if (rank != 3) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"%s takes a rank-%" G_GSIZE_FORMAT " input; a speaker model takes "
					"[batch, frames, bins] or [batch, bins, frames]",
					priv->model_path, rank);
		return FALSE;
	}
	if (priv->frontend == AI_SPEAKER_FRONTEND_AUTO) {
		if (g_strcmp0(framework, "nemo") == 0)
			priv->frontend = AI_SPEAKER_FRONTEND_NEMO;
		else if (framework != NULL)
			priv->frontend = AI_SPEAKER_FRONTEND_KALDI;
		else
			/* No metadata: bins fixed in the middle and a length input is NeMo. */
			priv->frontend = dims[1] > 0 && dims[2] < 0 && n_in >= 2 ? AI_SPEAKER_FRONTEND_NEMO
																	   : AI_SPEAKER_FRONTEND_KALDI;
	}
	priv->bins = (guint)(priv->frontend == AI_SPEAKER_FRONTEND_NEMO ? dims[1] : dims[2]);
	if (feat_dim != NULL && g_ascii_strtoull(feat_dim, NULL, 10) > 0)
		priv->bins = (guint)g_ascii_strtoull(feat_dim, NULL, 10);
	if (priv->bins == 0 || priv->bins > 512) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"%s does not say how many filterbank bins it takes", priv->model_path);
		return FALSE;
	}
	if (priv->frontend == AI_SPEAKER_FRONTEND_NEMO && n_in < 2) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"%s: a NeMo model takes the features and their length", priv->model_path);
		return FALSE;
	}
	/* wespeaker trains on 16-bit-scale samples, and says normalize_samples=0. */
	priv->scale_to_int16 = priv->frontend == AI_SPEAKER_FRONTEND_KALDI &&
						   (normalize != NULL ? g_str_equal(normalize, "0")
											  : g_strcmp0(framework, "wespeaker") == 0);
	/* The embedding: "embs" or "embedding" by name, else the first rank-2
	 * output, else the first. */
	for (i = 0; i < n_out && priv->output_name == NULL; i++) {
		g_autofree gchar *name = io_name(priv->session, allocator, FALSE, i);
		if (g_strcmp0(name, "embs") == 0 || g_strcmp0(name, "embedding") == 0 ||
			g_strcmp0(name, "embeddings") == 0)
			priv->output_name = g_steal_pointer(&name);
	}
	for (i = 0; i < n_out && priv->output_name == NULL; i++)
		if (io_shape(priv->session, FALSE, i, dims, NULL) == 2)
			priv->output_name = io_name(priv->session, allocator, FALSE, i);
	if (priv->output_name == NULL)
		priv->output_name = io_name(priv->session, allocator, FALSE, 0);
	priv->dims = output_dim != NULL ? (guint)g_ascii_strtoull(output_dim, NULL, 10) : 0;
	g_debug("speaker model %s: %s front-end, %u bins, %s%s", priv->model_id,
			priv->frontend == AI_SPEAKER_FRONTEND_NEMO ? "NeMo" : "Kaldi", priv->bins,
			priv->scale_to_int16 ? "16-bit samples, " : "", priv->output_name);
	return TRUE;
}
/* The caller gave up: tell a run in progress to stop. Any thread. */
static void
terminate_run(GCancellable *cancellable, gpointer data)
{
	OrtStatus *st = ort()->RunOptionsSetTerminate((OrtRunOptions *)data);
	if (st != NULL)
		ort()->ReleaseStatus(st);
}
static AiVoiceprint *
run_model(AiOnnxSpeakerEmbedder *self, const gfloat *features, gsize frames,
		  GCancellable *cancellable, GError **error)
{
	AiOnnxSpeakerEmbedderPrivate *priv = private_of(self);
	OrtMemoryInfo *memory = NULL;
	OrtRunOptions *run = NULL;
	OrtValue *inputs[2] = {NULL, NULL}, *output = NULL;
	OrtTensorTypeAndShapeInfo *shape = NULL;
	g_autofree gfloat *layout = NULL;
	const char *in_names[2], *out_names[1];
	gint64 feat_shape[3], length_shape[1], length[1];
	size_t count = 0;
	gfloat *data = NULL;
	AiVoiceprint *print = NULL;
	gulong cancel_id = 0;
	gsize n_inputs = 1, f;
	guint b;
	if (!ort_ok(ort()->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory),
				"memory", error))
		return NULL;
	/* Kaldi models take [1, frames, bins], which is how features come;
	 * NeMo takes [1, bins, frames] and how many frames there are. */
	if (priv->frontend == AI_SPEAKER_FRONTEND_NEMO) {
		layout = g_new(gfloat, frames * priv->bins);
		for (f = 0; f < frames; f++)
			for (b = 0; b < priv->bins; b++)
				layout[b * frames + f] = features[f * priv->bins + b];
		feat_shape[0] = 1;
		feat_shape[1] = priv->bins;
		feat_shape[2] = (gint64)frames;
		length_shape[0] = 1;
		length[0] = (gint64)frames;
		n_inputs = 2;
	} else {
		layout = g_memdup2(features, frames * priv->bins * sizeof(gfloat));
		feat_shape[0] = 1;
		feat_shape[1] = (gint64)frames;
		feat_shape[2] = priv->bins;
	}
	in_names[0] = g_ptr_array_index(priv->input_names, 0);
	in_names[1] = n_inputs > 1 ? g_ptr_array_index(priv->input_names, 1) : NULL;
	out_names[0] = priv->output_name;
	if (!ort_ok(ort()->CreateTensorWithDataAsOrtValue(memory, layout,
													  frames * priv->bins * sizeof(gfloat),
													  feat_shape, 3,
													  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
													  &inputs[0]),
				"features", error) ||
		(n_inputs > 1 &&
		 !ort_ok(ort()->CreateTensorWithDataAsOrtValue(memory, length, sizeof(length),
													   length_shape, 1,
													   ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
													   &inputs[1]),
				 "length", error)) ||
		!ort_ok(ort()->CreateRunOptions(&run), "run options", error))
		goto out;
	if (cancellable != NULL)
		cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(terminate_run), run, NULL);
	if (!ort_ok(ort()->Run(priv->session, run, in_names, (const OrtValue *const *)inputs,
						   n_inputs, out_names, 1, &output),
				"inference", error)) {
		if (g_cancellable_is_cancelled(cancellable)) {
			g_clear_error(error);
			g_cancellable_set_error_if_cancelled(cancellable, error);
		}
		goto out;
	}
	if (!ort_ok(ort()->GetTensorTypeAndShape(output, &shape), "output", error) ||
		!ort_ok(ort()->GetTensorShapeElementCount(shape, &count), "output", error) ||
		!ort_ok(ort()->GetTensorMutableData(output, (void **)&data), "output", error))
		goto out;
	if (priv->dims > 0 && count != priv->dims) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"The model answered with %" G_GSIZE_FORMAT " values, not %u", (gsize)count,
					priv->dims);
		goto out;
	}
	print = ai_voiceprint_new(NULL, priv->model_id, data, count, error);
out:
	if (cancel_id != 0)
		g_cancellable_disconnect(cancellable, cancel_id);
	if (shape != NULL)
		ort()->ReleaseTensorTypeAndShapeInfo(shape);
	if (output != NULL)
		ort()->ReleaseValue(output);
	if (inputs[0] != NULL)
		ort()->ReleaseValue(inputs[0]);
	if (inputs[1] != NULL)
		ort()->ReleaseValue(inputs[1]);
	if (run != NULL)
		ort()->ReleaseRunOptions(run);
	ort()->ReleaseMemoryInfo(memory);
	return print;
}
#endif /* AI_HAVE_ONNXRUNTIME */

/**
 * ai_onnx_speaker_embedder_embed:
 * @self: an embedder
 * @pcm: 16 kHz mono S16LE audio
 * @cancellable: (nullable): stops a run in progress
 * @error: return location for an error
 *
 * Embeds synchronously, on the calling thread. The asynchronous interface
 * method runs this on a worker thread.
 *
 * Returns: (transfer full) (nullable): the utterance's voiceprint, unnamed
 */
AiVoiceprint *
ai_onnx_speaker_embedder_embed(AiOnnxSpeakerEmbedder *self, GBytes *pcm,
							   GCancellable *cancellable, GError **error)
{
#ifdef AI_HAVE_ONNXRUNTIME
	AiOnnxSpeakerEmbedderPrivate *priv;
	g_autofree gfloat *samples = NULL;
	g_autofree gfloat *features = NULL;
	const guint8 *raw;
	gsize size, n, i, frames = 0;
	g_return_val_if_fail(AI_IS_ONNX_SPEAKER_EMBEDDER(self), NULL);
	g_return_val_if_fail(pcm != NULL, NULL);
	priv = private_of(self);
	if (priv->session == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
							"The speaker model is not loaded");
		return NULL;
	}
	raw = g_bytes_get_data(pcm, &size);
	n = size / 2;
	if (n < AI_SPEAKER_EMBEDDER_DEFAULT_MIN_BYTES / 2) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Too little audio to tell who spoke");
		return NULL;
	}
	samples = g_new(gfloat, n);
	for (i = 0; i < n; i++) {
		gint16 s;
		memcpy(&s, raw + 2 * i, 2);
		samples[i] = (gfloat)GINT16_FROM_LE(s) / 32768.0f;
	}
	features = AI_ONNX_SPEAKER_EMBEDDER_GET_CLASS(self)->features(self, samples, n, priv->bins,
																  &frames);
	if (features == NULL || frames == 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Too little audio to tell who spoke");
		return NULL;
	}
	if (g_cancellable_set_error_if_cancelled(cancellable, error))
		return NULL;
	return run_model(self, features, frames, cancellable, error);
#else
	g_return_val_if_fail(AI_IS_ONNX_SPEAKER_EMBEDDER(self), NULL);
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
						"ai-glib was built without onnxruntime");
	return NULL;
#endif
}

static void
embed_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
	GError *error = NULL;
	AiVoiceprint *print =
		ai_onnx_speaker_embedder_embed(AI_ONNX_SPEAKER_EMBEDDER(source), data, cancellable, &error);
	if (print == NULL)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, print, g_object_unref);
}
static void
embed_async(AiSpeakerEmbedder *embedder, GBytes *pcm, GCancellable *cancellable,
			GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(embedder, cancellable, callback, user_data);
	g_task_set_task_data(task, g_bytes_ref(pcm), (GDestroyNotify)g_bytes_unref);
	g_task_run_in_thread(task, embed_thread);
}
static AiVoiceprint *
embed_finish(AiSpeakerEmbedder *embedder, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, embedder), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
static void
embedder_iface_init(AiSpeakerEmbedderInterface *iface)
{
	iface->embed_async = embed_async;
	iface->embed_finish = embed_finish;
}

static gboolean
initable_init(GInitable *initable, GCancellable *cancellable, GError **error)
{
	AiOnnxSpeakerEmbedder *self = AI_ONNX_SPEAKER_EMBEDDER(initable);
	AiOnnxSpeakerEmbedderPrivate *priv = private_of(self);
	if (priv->model_path == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"No speaker model path");
		return FALSE;
	}
#ifdef AI_HAVE_ONNXRUNTIME
	if (priv->session != NULL)
		return TRUE;
	g_free(priv->model_id);
	if ((priv->model_id = model_id_of(priv->model_path, error)) == NULL)
		return FALSE;
	return load_model(self, error);
#else
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"Cannot load %s: ai-glib was built without onnxruntime", priv->model_path);
	return FALSE;
#endif
}
static void
initable_iface_init(GInitableIface *iface)
{
	iface->init = initable_init;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiOnnxSpeakerEmbedderPrivate *priv = private_of(AI_ONNX_SPEAKER_EMBEDDER(object));
	switch (id) {
	case PROP_MODEL_PATH:
		g_value_set_string(value, priv->model_path);
		break;
	case PROP_FRONTEND:
		g_value_set_enum(value, priv->frontend);
		break;
	case PROP_THREADS:
		g_value_set_uint(value, priv->threads);
		break;
	case PROP_MODEL_ID:
		g_value_set_string(value, priv->model_id);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiOnnxSpeakerEmbedderPrivate *priv = private_of(AI_ONNX_SPEAKER_EMBEDDER(object));
	switch (id) {
	case PROP_MODEL_PATH:
		g_free(priv->model_path);
		priv->model_path = g_value_dup_string(value);
		break;
	case PROP_FRONTEND:
		priv->frontend = g_value_get_enum(value);
		break;
	case PROP_THREADS:
		priv->threads = g_value_get_uint(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
finalize(GObject *object)
{
	AiOnnxSpeakerEmbedderPrivate *priv = private_of(AI_ONNX_SPEAKER_EMBEDDER(object));
#ifdef AI_HAVE_ONNXRUNTIME
	if (priv->session != NULL)
		ort()->ReleaseSession(priv->session);
#endif
	g_free(priv->model_path);
	g_free(priv->model_id);
	g_free(priv->output_name);
	g_ptr_array_unref(priv->input_names);
	G_OBJECT_CLASS(ai_onnx_speaker_embedder_parent_class)->finalize(object);
}
static void
ai_onnx_speaker_embedder_class_init(AiOnnxSpeakerEmbedderClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	klass->features = default_features;
	/**
	 * AiOnnxSpeakerEmbedder:model-path:
	 *
	 * The speaker-embedding model, an ONNX file.
	 */
	props[PROP_MODEL_PATH] =
		g_param_spec_string("model-path", "Model path", "Speaker-embedding ONNX model", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * AiOnnxSpeakerEmbedder:frontend:
	 *
	 * The features the model takes. Automatic reads the model's metadata,
	 * then its input shape.
	 */
	props[PROP_FRONTEND] = g_param_spec_enum(
		"frontend", "Front-end", "Features the model takes", AI_TYPE_SPEAKER_FRONTEND,
		AI_SPEAKER_FRONTEND_AUTO,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * AiOnnxSpeakerEmbedder:threads:
	 *
	 * Threads one embedding may use.
	 */
	props[PROP_THREADS] = g_param_spec_uint(
		"threads", "Threads", "Threads one embedding may use", 1, 64, 1,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * AiOnnxSpeakerEmbedder:model-id:
	 *
	 * What every voiceprint it makes is tagged with: the file name and the
	 * start of its SHA-256.
	 */
	props[PROP_MODEL_ID] = g_param_spec_string("model-id", "Model id", "Tag on its voiceprints",
											   NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
}
static void
ai_onnx_speaker_embedder_init(AiOnnxSpeakerEmbedder *self)
{
	private_of(self)->input_names = g_ptr_array_new_with_free_func(g_free);
}

/**
 * ai_onnx_speaker_embedder_is_available:
 *
 * Returns: whether ai-glib was built with onnxruntime, so a model can be
 *   loaded at all
 */
gboolean
ai_onnx_speaker_embedder_is_available(void)
{
#ifdef AI_HAVE_ONNXRUNTIME
	return TRUE;
#else
	return FALSE;
#endif
}

/**
 * ai_onnx_speaker_embedder_new:
 * @model_path: a speaker-embedding ONNX model
 * @error: return location for an error
 *
 * Loads the model now, so a bad path or an unusable model is an error here
 * rather than on the first utterance.
 *
 * Returns: (transfer full) (nullable): a new embedder
 */
AiOnnxSpeakerEmbedder *
ai_onnx_speaker_embedder_new(const gchar *model_path, GError **error)
{
	return g_initable_new(AI_TYPE_ONNX_SPEAKER_EMBEDDER, NULL, error, "model-path", model_path,
						  NULL);
}

/**
 * ai_onnx_speaker_embedder_get_model_id:
 * @self: an embedder
 *
 * Returns: (nullable): the tag on its voiceprints
 */
const gchar *
ai_onnx_speaker_embedder_get_model_id(AiOnnxSpeakerEmbedder *self)
{
	g_return_val_if_fail(AI_IS_ONNX_SPEAKER_EMBEDDER(self), NULL);
	return private_of(self)->model_id;
}

/**
 * ai_onnx_speaker_embedder_get_dimensions:
 * @self: an embedder
 *
 * Returns: the embedding length the model declares, 0 when it does not say
 */
guint
ai_onnx_speaker_embedder_get_dimensions(AiOnnxSpeakerEmbedder *self)
{
	g_return_val_if_fail(AI_IS_ONNX_SPEAKER_EMBEDDER(self), 0);
	return private_of(self)->dims;
}

/**
 * ai_onnx_speaker_embedder_get_frontend:
 * @self: an embedder
 *
 * Returns: the front-end in use, decided when the model was loaded
 */
AiSpeakerFrontend
ai_onnx_speaker_embedder_get_frontend(AiOnnxSpeakerEmbedder *self)
{
	g_return_val_if_fail(AI_IS_ONNX_SPEAKER_EMBEDDER(self), AI_SPEAKER_FRONTEND_AUTO);
	return private_of(self)->frontend;
}
