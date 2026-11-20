/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-speech-features-private.h"
#include <float.h>
#include <math.h>
#include <string.h>

/*
 * Filterbank front-ends for speaker-embedding models. See the header for
 * what each computes; the comments here say how.
 */
#define FFT_SIZE 512
#define FRAME_LENGTH 400 /* 25 ms */
#define FRAME_SHIFT 160	 /* 10 ms */
#define PREEMPH 0.97

/* ---- FFT ---- */

void
ai_speech_features_fft(gdouble *re, gdouble *im, gsize n)
{
	gsize i, j, len;
	/* Bit-reversal permutation. */
	for (i = 1, j = 0; i < n; i++) {
		gsize bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			gdouble t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	/* Butterflies, doubling the transform length each pass. */
	for (len = 2; len <= n; len <<= 1) {
		gdouble angle = -2.0 * G_PI / (gdouble)len;
		gdouble wr = cos(angle), wi = sin(angle);
		for (i = 0; i < n; i += len) {
			gdouble cr = 1.0, ci = 0.0;
			gsize k;
			for (k = 0; k < len / 2; k++) {
				gsize a = i + k, b = i + k + len / 2;
				gdouble tr = re[b] * cr - im[b] * ci;
				gdouble ti = re[b] * ci + im[b] * cr;
				gdouble next;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				next = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = next;
			}
		}
	}
}
/* |X[k]|^2 for k = 0 .. FFT_SIZE/2 of a real FFT_SIZE frame. */
static void
power_spectrum(const gdouble *frame, gdouble *power)
{
	gdouble re[FFT_SIZE], im[FFT_SIZE];
	gsize k;
	memcpy(re, frame, sizeof(re));
	memset(im, 0, sizeof(im));
	ai_speech_features_fft(re, im, FFT_SIZE);
	for (k = 0; k <= FFT_SIZE / 2; k++)
		power[k] = re[k] * re[k] + im[k] * im[k];
}

/* ---- Kaldi filterbanks ---- */

static gdouble
htk_mel(gdouble hz)
{
	return 1127.0 * log(1.0 + hz / 700.0);
}
/* Triangles equally spaced in HTK mel, evaluated at each FFT bin's centre
 * frequency: weights[bin * (FFT_SIZE/2 + 1) + k]. */
static gdouble *
kaldi_mel_banks(guint num_bins)
{
	gsize nfreq = FFT_SIZE / 2 + 1;
	gdouble *weights = g_new0(gdouble, (gsize)num_bins * nfreq);
	gdouble low = htk_mel(20.0), high = htk_mel(AI_SPEECH_FEATURES_RATE / 2.0);
	gdouble delta = (high - low) / (num_bins + 1);
	guint b;
	gsize k;
	for (b = 0; b < num_bins; b++) {
		gdouble left = low + b * delta, centre = left + delta, right = centre + delta;
		/* Kaldi weighs bins 0 .. FFT_SIZE/2 - 1; the Nyquist bin stays 0. */
		for (k = 0; k < FFT_SIZE / 2; k++) {
			gdouble mel = htk_mel((gdouble)k * AI_SPEECH_FEATURES_RATE / FFT_SIZE);
			if (mel > left && mel < right)
				weights[b * nfreq + k] =
					mel <= centre ? (mel - left) / (centre - left) : (right - mel) / (right - centre);
		}
	}
	return weights;
}
gfloat *
ai_speech_features_kaldi_fbank(const gfloat *samples, gsize n, guint num_bins, gsize *frames)
{
	gsize nfreq = FFT_SIZE / 2 + 1, count, f, i, k;
	g_autofree gdouble *banks = NULL;
	gdouble window[FRAME_LENGTH], frame[FFT_SIZE], power[FFT_SIZE / 2 + 1];
	gfloat *out;
	*frames = 0;
	if (samples == NULL || n < FRAME_LENGTH || num_bins == 0)
		return NULL;
	/* snip_edges: only frames that fit inside the signal. */
	count = 1 + (n - FRAME_LENGTH) / FRAME_SHIFT;
	banks = kaldi_mel_banks(num_bins);
	for (i = 0; i < FRAME_LENGTH; i++)
		window[i] = pow(0.5 - 0.5 * cos(2.0 * G_PI * i / (FRAME_LENGTH - 1)), 0.85);
	out = g_new(gfloat, count * num_bins);
	for (f = 0; f < count; f++) {
		const gfloat *x = samples + f * FRAME_SHIFT;
		gdouble mean = 0;
		guint b;
		/* Remove DC, then pre-emphasise within the frame, back to front. */
		for (i = 0; i < FRAME_LENGTH; i++)
			mean += x[i];
		mean /= FRAME_LENGTH;
		for (i = 0; i < FRAME_LENGTH; i++)
			frame[i] = x[i] - mean;
		for (i = FRAME_LENGTH - 1; i > 0; i--)
			frame[i] -= PREEMPH * frame[i - 1];
		frame[0] -= PREEMPH * frame[0];
		for (i = 0; i < FRAME_LENGTH; i++)
			frame[i] *= window[i];
		memset(frame + FRAME_LENGTH, 0, (FFT_SIZE - FRAME_LENGTH) * sizeof(gdouble));
		power_spectrum(frame, power);
		for (b = 0; b < num_bins; b++) {
			gdouble energy = 0;
			for (k = 0; k < nfreq; k++)
				energy += banks[b * nfreq + k] * power[k];
			out[f * num_bins + b] = (gfloat)log(MAX(energy, (gdouble)FLT_EPSILON));
		}
	}
	*frames = count;
	return out;
}

/* ---- NeMo mel spectrogram ---- */

/* Slaney's mel scale: linear below 1 kHz, logarithmic above. */
static gdouble
slaney_mel(gdouble hz)
{
	const gdouble f_sp = 200.0 / 3.0, min_log_hz = 1000.0, min_log_mel = min_log_hz / f_sp;
	const gdouble logstep = log(6.4) / 27.0;
	return hz < min_log_hz ? hz / f_sp : min_log_mel + log(hz / min_log_hz) / logstep;
}
static gdouble
slaney_hz(gdouble mel)
{
	const gdouble f_sp = 200.0 / 3.0, min_log_hz = 1000.0, min_log_mel = min_log_hz / f_sp;
	const gdouble logstep = log(6.4) / 27.0;
	return mel < min_log_mel ? mel * f_sp : min_log_hz * exp(logstep * (mel - min_log_mel));
}
/* librosa.filters.mel(sr=16000, n_fft=512, n_mels=NUM_BINS, fmin=0, fmax=8000,
 * htk=False, norm="slaney"): triangles in Hz between Slaney-mel-spaced
 * points, each scaled to unit area. */
static gdouble *
slaney_mel_banks(guint num_bins)
{
	gsize nfreq = FFT_SIZE / 2 + 1, k;
	gdouble *weights = g_new0(gdouble, (gsize)num_bins * nfreq);
	g_autofree gdouble *points = g_new(gdouble, num_bins + 2);
	gdouble low = slaney_mel(0.0), high = slaney_mel(AI_SPEECH_FEATURES_RATE / 2.0);
	guint b;
	for (b = 0; b < num_bins + 2; b++)
		points[b] = slaney_hz(low + (high - low) * b / (num_bins + 1));
	for (b = 0; b < num_bins; b++) {
		gdouble enorm = 2.0 / (points[b + 2] - points[b]);
		for (k = 0; k < nfreq; k++) {
			gdouble hz = (gdouble)k * AI_SPEECH_FEATURES_RATE / FFT_SIZE;
			gdouble lower = (hz - points[b]) / (points[b + 1] - points[b]);
			gdouble upper = (points[b + 2] - hz) / (points[b + 2] - points[b + 1]);
			gdouble w = MAX(0.0, MIN(lower, upper));
			weights[b * nfreq + k] = w * enorm;
		}
	}
	return weights;
}
gfloat *
ai_speech_features_nemo_mel(const gfloat *samples, gsize n, guint num_bins, gsize *frames)
{
	const gsize pad = FFT_SIZE / 2, nfreq = FFT_SIZE / 2 + 1;
	const gsize offset = (FFT_SIZE - FRAME_LENGTH) / 2; /* the window, centred in the FFT */
	g_autofree gdouble *emph = NULL;
	g_autofree gdouble *padded = NULL;
	g_autofree gdouble *banks = NULL;
	gdouble window[FRAME_LENGTH], frame[FFT_SIZE], power[FFT_SIZE / 2 + 1];
	gsize count, f, i, k;
	gfloat *out;
	*frames = 0;
	/* Reflection needs more samples than the pad. */
	if (samples == NULL || n <= pad || num_bins == 0)
		return NULL;
	emph = g_new(gdouble, n);
	emph[0] = samples[0];
	for (i = 1; i < n; i++)
		emph[i] = samples[i] - PREEMPH * samples[i - 1];
	/* centre=True: reflect PAD samples at each end (not repeating the edge). */
	padded = g_new(gdouble, n + 2 * pad);
	for (i = 0; i < pad; i++) {
		padded[i] = emph[pad - i];
		padded[pad + n + i] = emph[n - 2 - i];
	}
	memcpy(padded + pad, emph, n * sizeof(gdouble));
	count = 1 + n / FRAME_SHIFT;
	/* torch.hann_window(400, periodic=False). */
	for (i = 0; i < FRAME_LENGTH; i++)
		window[i] = 0.5 - 0.5 * cos(2.0 * G_PI * i / (FRAME_LENGTH - 1));
	banks = slaney_mel_banks(num_bins);
	out = g_new(gfloat, count * num_bins);
	for (f = 0; f < count; f++) {
		const gdouble *x = padded + f * FRAME_SHIFT;
		guint b;
		memset(frame, 0, sizeof(frame));
		for (i = 0; i < FRAME_LENGTH; i++)
			frame[offset + i] = x[offset + i] * window[i];
		power_spectrum(frame, power);
		for (b = 0; b < num_bins; b++) {
			gdouble energy = 0;
			for (k = 0; k < nfreq; k++)
				energy += banks[b * nfreq + k] * power[k];
			out[f * num_bins + b] = (gfloat)log(energy + 5.960464477539063e-08);
		}
	}
	*frames = count;
	return out;
}

/* ---- normalisation ---- */

void
ai_speech_features_subtract_mean(gfloat *features, gsize frames, guint dim)
{
	gsize f;
	guint d;
	if (features == NULL || frames == 0)
		return;
	for (d = 0; d < dim; d++) {
		gdouble mean = 0;
		for (f = 0; f < frames; f++)
			mean += features[f * dim + d];
		mean /= (gdouble)frames;
		for (f = 0; f < frames; f++)
			features[f * dim + d] -= (gfloat)mean;
	}
}
void
ai_speech_features_normalize_per_feature(gfloat *features, gsize frames, guint dim)
{
	gsize f;
	guint d;
	if (features == NULL || frames == 0)
		return;
	for (d = 0; d < dim; d++) {
		gdouble mean = 0, var = 0, sd;
		for (f = 0; f < frames; f++)
			mean += features[f * dim + d];
		mean /= (gdouble)frames;
		for (f = 0; f < frames; f++) {
			gdouble dv = features[f * dim + d] - mean;
			var += dv * dv;
		}
		sd = sqrt(frames > 1 ? var / (gdouble)(frames - 1) : 0.0) + 1e-5;
		for (f = 0; f < frames; f++)
			features[f * dim + d] = (gfloat)((features[f * dim + d] - mean) / sd);
	}
}
