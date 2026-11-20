/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_COMPILATION)
#error "ai-speech-features-private.h is an internal header"
#endif
#include <glib.h>
G_BEGIN_DECLS
/*
 * Speech features for speaker-embedding models, in plain C: what the model
 * was trained on, computed the same way, or its embeddings mean nothing.
 *
 * Kaldi filterbanks (wespeaker, 3D-Speaker): 25 ms frames every 10 ms that
 * end inside the signal, DC removed, pre-emphasis 0.97 per frame, a Povey
 * window, a 512-point power spectrum, NUM_BINS HTK-mel triangles from 20 Hz
 * to Nyquist, natural log floored at FLT_EPSILON. Samples are taken as
 * given: wespeaker expects them at 16-bit scale, 3D-Speaker in [-1, 1].
 *
 * NeMo mel spectrograms (TitaNet): pre-emphasis 0.97 over the signal, frames
 * centred by reflecting 256 samples at each end, a 400-sample Hann window
 * in a 512-point power spectrum, NUM_BINS Slaney mel bins from 0 to 8 kHz
 * normalised by area, log(x + 2^-24). Samples in [-1, 1].
 *
 * Both return frames * NUM_BINS floats, frame-major, and set *FRAMES; NULL
 * (and 0) when the audio is shorter than one frame.
 */
#define AI_SPEECH_FEATURES_RATE (16000)

gfloat *
ai_speech_features_kaldi_fbank(const gfloat *samples, gsize n, guint num_bins, gsize *frames);
gfloat *
ai_speech_features_nemo_mel(const gfloat *samples, gsize n, guint num_bins, gsize *frames);
/* Subtract each bin's mean over time (cepstral mean normalisation). */
void
ai_speech_features_subtract_mean(gfloat *features, gsize frames, guint dim);
/* Each bin to zero mean and unit standard deviation over time, as NeMo's
 * per_feature normalisation does (unbiased deviation, plus 1e-5). */
void
ai_speech_features_normalize_per_feature(gfloat *features, gsize frames, guint dim);
/* In-place radix-2 FFT of N (a power of two) complex values. */
void
ai_speech_features_fft(gdouble *re, gdouble *im, gsize n);
G_END_DECLS
