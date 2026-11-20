/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include "voice/ai-speech-features-private.h"
#include <glib/gstdio.h>
#include <signal.h>
#include <math.h>
#include <string.h>

/*
 * In-process voiceprints: the filterbank front-ends, and
 * AiOnnxSpeakerEmbedder against a model this test writes itself -- a
 * one-node graph that takes the features' maximum over time, so its "embedding"
 * is the peak filterbank. That is enough to check loading, metadata,
 * layout, tagging and errors without shipping a model. A real model can be
 * checked too: set AI_TEST_SPEAKER_MODEL to its path.
 */

/* ---- features ---- */

static gfloat *
tone(gdouble hz, gsize n, gdouble amplitude)
{
	gfloat *x = g_new(gfloat, n);
	gsize i;
	for (i = 0; i < n; i++)
		x[i] = (gfloat)(amplitude * sin(2.0 * G_PI * hz * i / 16000.0));
	return x;
}
static void
test_fft(void)
{
	gdouble re[8] = {1, 0, 0, 0, 0, 0, 0, 0}, im[8] = {0};
	gdouble cre[8], cim[8] = {0};
	guint k;
	/* An impulse has a flat spectrum. */
	ai_speech_features_fft(re, im, 8);
	for (k = 0; k < 8; k++) {
		g_assert_cmpfloat_with_epsilon(re[k], 1.0, 1e-12);
		g_assert_cmpfloat_with_epsilon(im[k], 0.0, 1e-12);
	}
	/* A cosine at bin 2 lands in bins 2 and 6. */
	for (k = 0; k < 8; k++)
		cre[k] = cos(2.0 * G_PI * 2 * k / 8);
	ai_speech_features_fft(cre, cim, 8);
	g_assert_cmpfloat_with_epsilon(cre[2], 4.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(cre[6], 4.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(cre[0], 0.0, 1e-9);
}
/* Frames that fit (Kaldi) and frames centred on every hop (NeMo); a tone's
 * energy sits in the bin for its frequency, and higher tones in higher bins. */
static void
test_features_shape(void)
{
	g_autofree gfloat *x = tone(1000, 16000, 0.5);
	g_autofree gfloat *kaldi = NULL, *nemo = NULL;
	gsize frames = 0, f;
	guint b, peak_kaldi = 0, peak_nemo = 0;
	kaldi = ai_speech_features_kaldi_fbank(x, 16000, 80, &frames);
	g_assert_cmpuint(frames, ==, 98);
	for (b = 1; b < 80; b++)
		if (kaldi[50 * 80 + b] > kaldi[50 * 80 + peak_kaldi])
			peak_kaldi = b;
	nemo = ai_speech_features_nemo_mel(x, 16000, 80, &frames);
	g_assert_cmpuint(frames, ==, 101);
	for (b = 1; b < 80; b++)
		if (nemo[50 * 80 + b] > nemo[50 * 80 + peak_nemo])
			peak_nemo = b;
	/* 1 kHz is about a third of the way up the HTK mel scale from 20 Hz
	 * (bin ~23) and a little higher on Slaney's (bin ~24). */
	g_assert_cmpuint(peak_kaldi, >=, 20);
	g_assert_cmpuint(peak_kaldi, <=, 27);
	g_assert_cmpuint(peak_nemo, >=, 20);
	g_assert_cmpuint(peak_nemo, <=, 28);
	{
		g_autofree gfloat *hi = tone(3000, 16000, 0.5);
		g_autofree gfloat *hk = ai_speech_features_kaldi_fbank(hi, 16000, 80, &frames);
		guint peak = 0;
		for (b = 1; b < 80; b++)
			if (hk[50 * 80 + b] > hk[50 * 80 + peak])
				peak = b;
		g_assert_cmpuint(peak, >, peak_kaldi + 10);
	}
	/* Every value is finite, even for silence. */
	{
		g_autofree gfloat *quiet = g_new0(gfloat, 8000);
		g_autofree gfloat *qk = ai_speech_features_kaldi_fbank(quiet, 8000, 80, &frames);
		g_autofree gfloat *qn = NULL;
		for (f = 0; f < frames * 80; f++)
			g_assert_true(isfinite(qk[f]));
		qn = ai_speech_features_nemo_mel(quiet, 8000, 80, &frames);
		for (f = 0; f < frames * 80; f++)
			g_assert_true(isfinite(qn[f]));
	}
	/* Shorter than a frame: nothing. */
	g_assert_null(ai_speech_features_kaldi_fbank(x, 399, 80, &frames));
	g_assert_cmpuint(frames, ==, 0);
	g_assert_null(ai_speech_features_nemo_mel(x, 100, 80, &frames));
}
static void
test_normalisation(void)
{
	g_autofree gfloat *x = tone(440, 16000, 0.3);
	g_autofree gfloat *k = NULL;
	gsize frames = 0, f;
	guint b;
	k = ai_speech_features_kaldi_fbank(x, 16000, 40, &frames);
	ai_speech_features_subtract_mean(k, frames, 40);
	for (b = 0; b < 40; b++) {
		gdouble mean = 0;
		for (f = 0; f < frames; f++)
			mean += k[f * 40 + b];
		g_assert_cmpfloat_with_epsilon(mean / frames, 0.0, 1e-4);
	}
	{
		g_autofree gfloat *n = ai_speech_features_nemo_mel(x, 16000, 40, &frames);
		/* Some variation over time, so the deviation is meaningful. */
		for (f = 0; f < frames; f++)
			n[f * 40 + 5] += (gfloat)(f % 7);
		ai_speech_features_normalize_per_feature(n, frames, 40);
		{
			gdouble mean = 0, var = 0;
			for (f = 0; f < frames; f++)
				mean += n[f * 40 + 5];
			mean /= frames;
			for (f = 0; f < frames; f++)
				var += (n[f * 40 + 5] - mean) * (n[f * 40 + 5] - mean);
			g_assert_cmpfloat_with_epsilon(mean, 0.0, 1e-4);
			g_assert_cmpfloat_with_epsilon(sqrt(var / (frames - 1)), 1.0, 1e-3);
		}
	}
}

/* ---- a model written by hand ----
 *
 * Just enough protobuf for an ONNX ModelProto: varints, length-delimited
 * fields, and the handful of messages a one-node graph needs. */

static void
pb_varint(GByteArray *out, guint64 v)
{
	do {
		guint8 byte = v & 0x7f;
		v >>= 7;
		if (v != 0)
			byte |= 0x80;
		g_byte_array_append(out, &byte, 1);
	} while (v != 0);
}
static void
pb_key(GByteArray *out, guint field, guint wire)
{
	pb_varint(out, ((guint64)field << 3) | wire);
}
static void
pb_int(GByteArray *out, guint field, guint64 v)
{
	pb_key(out, field, 0);
	pb_varint(out, v);
}
static void
pb_bytes(GByteArray *out, guint field, const guint8 *data, gsize len)
{
	pb_key(out, field, 2);
	pb_varint(out, len);
	g_byte_array_append(out, data, (guint)len);
}
static void
pb_string(GByteArray *out, guint field, const gchar *s)
{
	pb_bytes(out, field, (const guint8 *)s, strlen(s));
}
/* Append a sub-message, then free it. */
static void
pb_message(GByteArray *out, guint field, GByteArray *sub)
{
	pb_bytes(out, field, sub->data, sub->len);
	g_byte_array_unref(sub);
}
/* ValueInfoProto for a float tensor; a dim of 0 is symbolic. */
static GByteArray *
value_info(const gchar *name, const gint *dims, guint rank)
{
	GByteArray *vi = g_byte_array_new(), *type = g_byte_array_new(),
			   *tensor = g_byte_array_new(), *shape = g_byte_array_new();
	guint i;
	for (i = 0; i < rank; i++) {
		GByteArray *dim = g_byte_array_new();
		if (dims[i] > 0)
			pb_int(dim, 1, (guint64)dims[i]);
		else
			pb_string(dim, 2, i == 0 ? "N" : "T");
		pb_message(shape, 1, dim);
	}
	pb_int(tensor, 1, 1); /* FLOAT */
	pb_message(tensor, 2, shape);
	pb_message(type, 1, tensor);
	pb_string(vi, 1, name);
	pb_message(vi, 2, type);
	return vi;
}
/* feats [N, T, BINS] -> ReduceMax over T -> embs [N, BINS]. With RANK 2
 * the input is [N, BINS], which no speaker model takes. */
static gchar *
write_model(const gchar *dir, const gchar *file, guint bins, guint rank, const gchar *framework)
{
	GByteArray *model = g_byte_array_new(), *graph = g_byte_array_new(),
			   *node = g_byte_array_new(), *axes = g_byte_array_new(),
			   *keep = g_byte_array_new(), *opset = g_byte_array_new();
	gint in_dims[3], out_dims[2];
	gchar *path = g_build_filename(dir, file, NULL);
	in_dims[0] = 0;
	in_dims[1] = rank == 3 ? 0 : (gint)bins;
	in_dims[2] = (gint)bins;
	out_dims[0] = 0;
	out_dims[1] = (gint)bins;
	pb_string(axes, 1, "axes");
	pb_int(axes, 8, 1);
	pb_int(axes, 20, 7); /* INTS */
	pb_string(keep, 1, "keepdims");
	pb_int(keep, 3, 0);
	pb_int(keep, 20, 2); /* INT */
	pb_string(node, 1, "feats");
	pb_string(node, 2, "embs");
	pb_string(node, 3, "mean");
	pb_string(node, 4, "ReduceMax");
	pb_message(node, 5, axes);
	pb_message(node, 5, keep);
	pb_message(graph, 1, node);
	pb_string(graph, 2, "tiny");
	pb_message(graph, 11, value_info("feats", in_dims, rank));
	pb_message(graph, 12, value_info("embs", out_dims, 2));
	pb_int(model, 1, 7); /* IR version */
	pb_string(model, 2, "ai-glib-test");
	pb_message(model, 7, graph);
	pb_string(opset, 1, "");
	pb_int(opset, 2, 13);
	pb_message(model, 8, opset);
	if (framework != NULL) {
		GByteArray *entry = g_byte_array_new();
		pb_string(entry, 1, "framework");
		pb_string(entry, 2, framework);
		pb_message(model, 14, entry);
	}
	g_assert_true(g_file_set_contents(path, (const gchar *)model->data, model->len, NULL));
	g_byte_array_unref(model);
	return path;
}
static GBytes *
pcm_tone(gdouble hz, gdouble seconds)
{
	gsize n = (gsize)(seconds * 16000), i;
	gint16 *s = g_new(gint16, n);
	for (i = 0; i < n; i++)
		/* Swelling and fading three times a second, so frames differ: a
		 * steady tone is the same frame over and over, which mean
		 * normalisation turns into nothing at all. */
		s[i] = (gint16)GINT16_TO_LE((gint16)(8000 * sin(2.0 * G_PI * hz * i / 16000.0) *
											 (0.6 + 0.4 * sin(2.0 * G_PI * 3.0 * i / 16000.0))));
	return g_bytes_new_take(s, n * 2);
}
typedef struct {
	gboolean done;
	AiVoiceprint *print;
	GError *error;
} Embedded;
static void
embedded(GObject *source, GAsyncResult *result, gpointer data)
{
	Embedded *e = data;
	e->print = ai_speaker_embedder_embed_finish(AI_SPEAKER_EMBEDDER(source), result, &e->error);
	e->done = TRUE;
}
/* Without onnxruntime the type says so, and nothing pretends to work. */
static gboolean
skip_without_runtime(void)
{
	g_autoptr(GError) error = NULL;
	if (ai_onnx_speaker_embedder_is_available())
		return FALSE;
	g_assert_null(ai_onnx_speaker_embedder_new("/nonexistent.onnx", &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_test_skip("built without onnxruntime");
	return TRUE;
}
/* The model loads, its front-end comes from its metadata, and each
 * voiceprint carries the model id; the async path matches the sync one. */
static void
test_onnx_tiny_model(void)
{
	g_autofree gchar *dir = NULL, *path = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiOnnxSpeakerEmbedder) e = NULL;
	g_autoptr(AiVoiceprint) a = NULL, b = NULL, c = NULL;
	g_autoptr(GBytes) low = pcm_tone(300, 1.0), high = pcm_tone(3000, 1.0);
	g_autoptr(GBytes) brief = pcm_tone(300, 0.2);
	Embedded async = {0};
	gint64 limit;
	if (skip_without_runtime())
		return;
	dir = g_dir_make_tmp("onnx-XXXXXX", NULL);
	path = write_model(dir, "tiny.onnx", 8, 3, "wespeaker");
	e = ai_onnx_speaker_embedder_new(path, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_onnx_speaker_embedder_get_frontend(e), ==, AI_SPEAKER_FRONTEND_KALDI);
	g_assert_true(g_str_has_prefix(ai_onnx_speaker_embedder_get_model_id(e), "tiny.onnx:"));
	g_assert_cmpuint(strlen(ai_onnx_speaker_embedder_get_model_id(e)), ==, strlen("tiny.onnx:") + 12);
	a = ai_onnx_speaker_embedder_embed(e, low, NULL, &error);
	g_assert_no_error(error);
	b = ai_onnx_speaker_embedder_embed(e, high, NULL, &error);
	g_assert_no_error(error);
	{
		gsize dims = 0;
		ai_voiceprint_get_embedding(a, &dims);
		g_assert_cmpuint(dims, ==, 8);
	}
	g_assert_cmpstr(ai_voiceprint_get_model(a), ==, ai_onnx_speaker_embedder_get_model_id(e));
	g_assert_cmpfloat(ai_voiceprint_similarity(a, a), >, 0.999);
	g_assert_cmpfloat(ai_voiceprint_similarity(a, b), <, 0.999);
	/* The same through the interface, on a worker thread. */
	ai_speaker_embedder_embed_async(AI_SPEAKER_EMBEDDER(e), low, NULL, embedded, &async);
	limit = g_get_monotonic_time() + 5000000;
	while (!async.done && g_get_monotonic_time() < limit) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	g_assert_true(async.done);
	g_assert_no_error(async.error);
	c = async.print;
	g_assert_cmpfloat(ai_voiceprint_similarity(a, c), >, 0.999);
	/* Too short. */
	g_assert_null(ai_onnx_speaker_embedder_embed(e, brief, NULL, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_unlink(path);
	g_rmdir(dir);
}
/* What cannot be a speaker model is refused at load, not on first use. */
static void
test_onnx_bad_models(void)
{
	g_autofree gchar *dir = NULL, *flat = NULL, *junk = NULL;
	g_autoptr(GError) error = NULL;
	AiOnnxSpeakerEmbedder *e;
	if (skip_without_runtime())
		return;
	dir = g_dir_make_tmp("onnx-XXXXXX", NULL);
	g_assert_null(ai_onnx_speaker_embedder_new("/nonexistent/model.onnx", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	flat = write_model(dir, "flat.onnx", 8, 2, NULL);
	e = ai_onnx_speaker_embedder_new(flat, &error);
	g_assert_null(e);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error(&error);
	junk = g_build_filename(dir, "junk.onnx", NULL);
	g_assert_true(g_file_set_contents(junk, "not a model", -1, NULL));
	g_assert_null(ai_onnx_speaker_embedder_new(junk, &error));
	g_assert_nonnull(error);
	g_unlink(flat);
	g_unlink(junk);
	g_rmdir(dir);
}
/* An identifier over the in-process embedder needs no service at all. */
static void
test_onnx_identifier(void)
{
	g_autofree gchar *dir = NULL, *path = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiOnnxSpeakerEmbedder) e = NULL;
	g_autoptr(AiMemoryVoiceprintStore) store = ai_memory_voiceprint_store_new();
	g_autoptr(AiVoiceprintIdentifier) id = NULL;
	g_autoptr(GBytes) low = pcm_tone(300, 2.0);
	gint64 limit;
	if (skip_without_runtime())
		return;
	dir = g_dir_make_tmp("onnx-XXXXXX", NULL);
	path = write_model(dir, "tiny.onnx", 8, 3, "3d-speaker");
	e = ai_onnx_speaker_embedder_new(path, &error);
	g_assert_no_error(error);
	id = ai_voiceprint_identifier_new_full(AI_SPEAKER_EMBEDDER(e), AI_VOICEPRINT_STORE(store));
	ai_voiceprint_identifier_enroll_async(id, "Tone", low, NULL,
										  (GAsyncReadyCallback)NULL, NULL);
	limit = g_get_monotonic_time() + 5000000;
	while (ai_voiceprint_identifier_get_n_voices(id) == 0 && g_get_monotonic_time() < limit) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	g_assert_cmpuint(ai_voiceprint_identifier_get_n_voices(id), ==, 1);
	g_unlink(path);
	g_rmdir(dir);
}
/* ai-voiceprint-server, beside this test in the build tree, or NULL. */
static gchar *
find_server(void)
{
	g_autofree gchar *self = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = self != NULL ? g_path_get_dirname(self) : NULL;
	gchar *candidate;
	if (dir == NULL)
		return NULL;
	candidate = g_build_filename(dir, "..", "bin", "ai-voiceprint-server", NULL);
	if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
		return candidate;
	g_free(candidate);
	return NULL;
}
/* A port nobody is listening on, right now. */
static guint16
free_port(void)
{
	g_autoptr(GSocket) sock =
		g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL);
	g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autoptr(GSocketAddress) any = g_inet_socket_address_new(loopback, 0);
	g_autoptr(GSocketAddress) bound = NULL;
	g_assert_true(g_socket_bind(sock, any, TRUE, NULL));
	bound = g_socket_get_local_address(sock, NULL);
	return g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
}
/* The server and the in-process embedder agree: the same model behind
 * HTTP gives the same voiceprint, so the two backends are interchangeable. */
static void
test_onnx_server(void)
{
	g_autofree gchar *server = NULL, *dir = NULL, *path = NULL, *port = NULL, *url = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(AiOnnxSpeakerEmbedder) local = NULL;
	g_autoptr(AiHttpSpeakerEmbedder) remote = NULL;
	g_autoptr(AiVoiceprint) here = NULL;
	g_autoptr(GBytes) pcm = pcm_tone(300, 1.0);
	Embedded r = {0};
	gint64 limit;
	if (skip_without_runtime())
		return;
	if ((server = find_server()) == NULL) {
		g_test_skip("ai-voiceprint-server is not built");
		return;
	}
	dir = g_dir_make_tmp("onnx-XXXXXX", NULL);
	path = write_model(dir, "tiny.onnx", 8, 3, "wespeaker");
	port = g_strdup_printf("%u", free_port());
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error, server, "--model", path,
							 "--port", port, NULL);
	g_assert_no_error(error);
	local = ai_onnx_speaker_embedder_new(path, &error);
	g_assert_no_error(error);
	here = ai_onnx_speaker_embedder_embed(local, pcm, NULL, &error);
	g_assert_no_error(error);
	url = g_strdup_printf("http://127.0.0.1:%s/embed", port);
	remote = ai_http_speaker_embedder_new(url);
	/* Until the server is listening, the request is refused: try again. */
	limit = g_get_monotonic_time() + 10000000;
	do {
		memset(&r, 0, sizeof(r));
		ai_speaker_embedder_embed_async(AI_SPEAKER_EMBEDDER(remote), pcm, NULL, embedded, &r);
		while (!r.done)
			g_main_context_iteration(NULL, TRUE);
		if (r.print == NULL) {
			g_clear_error(&r.error);
			g_usleep(50000);
		}
	} while (r.print == NULL && g_get_monotonic_time() < limit);
	g_assert_nonnull(r.print);
	g_assert_cmpstr(ai_voiceprint_get_model(r.print), ==, ai_voiceprint_get_model(here));
	g_assert_cmpfloat(ai_voiceprint_similarity(r.print, here), >, 0.9999);
	g_object_unref(r.print);
	/* Too much audio is refused before the body is read. */
	{
		g_autoptr(GBytes) long_audio = pcm_tone(300, 31.0);
		Embedded big = {0};
		ai_speaker_embedder_embed_async(AI_SPEAKER_EMBEDDER(remote), long_audio, NULL, embedded,
										&big);
		while (!big.done)
			g_main_context_iteration(NULL, TRUE);
		g_assert_null(big.print);
		g_assert_nonnull(big.error);
		g_assert_nonnull(strstr(big.error->message, "413"));
		g_clear_error(&big.error);
	}
	g_subprocess_send_signal(child, SIGTERM);
	g_subprocess_wait(child, NULL, NULL);
	g_unlink(path);
	g_rmdir(dir);
}
/* A real model, when one is at hand: the same audio twice is the same
 * voice, and the embedding has the length the model declares. */
static void
test_onnx_real_model(void)
{
	const gchar *path = g_getenv("AI_TEST_SPEAKER_MODEL");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiOnnxSpeakerEmbedder) e = NULL;
	g_autoptr(AiVoiceprint) a = NULL, b = NULL;
	g_autoptr(GBytes) pcm = pcm_tone(220, 2.0);
	gsize dims = 0;
	if (skip_without_runtime())
		return;
	if (path == NULL || *path == '\0') {
		g_test_skip("set AI_TEST_SPEAKER_MODEL to a speaker-embedding ONNX model");
		return;
	}
	e = ai_onnx_speaker_embedder_new(path, &error);
	g_assert_no_error(error);
	a = ai_onnx_speaker_embedder_embed(e, pcm, NULL, &error);
	g_assert_no_error(error);
	b = ai_onnx_speaker_embedder_embed(e, pcm, NULL, &error);
	g_assert_no_error(error);
	ai_voiceprint_get_embedding(a, &dims);
	if (ai_onnx_speaker_embedder_get_dimensions(e) > 0)
		g_assert_cmpuint(dims, ==, ai_onnx_speaker_embedder_get_dimensions(e));
	g_assert_cmpfloat(ai_voiceprint_similarity(a, b), >, 0.999);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/onnx/fft", test_fft);
	g_test_add_func("/voice/onnx/features-shape", test_features_shape);
	g_test_add_func("/voice/onnx/normalisation", test_normalisation);
	g_test_add_func("/voice/onnx/tiny-model", test_onnx_tiny_model);
	g_test_add_func("/voice/onnx/bad-models", test_onnx_bad_models);
	g_test_add_func("/voice/onnx/identifier", test_onnx_identifier);
	g_test_add_func("/voice/onnx/server", test_onnx_server);
	g_test_add_func("/voice/onnx/real-model", test_onnx_real_model);
	return g_test_run();
}
