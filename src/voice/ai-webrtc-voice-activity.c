/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-webrtc-voice-activity.h"
#include "fvad.h"

typedef struct {
	Fvad *vad;
	guint silent_samples;
	gboolean speaking;
} Activity;
struct _AiWebrtcVoiceActivity {
	GObject parent_instance;
	GHashTable *participants;
	guint silence_ms;
	guint mode;
};
static void
activity_iface(AiVoiceActivityInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiWebrtcVoiceActivity, ai_webrtc_voice_activity, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_VOICE_ACTIVITY, activity_iface))

static void
activity_free(gpointer data)
{
	Activity *a = data;
	fvad_free(a->vad);
	g_free(a);
}

static gint
process(AiVoiceActivity *vad, const gchar *speaker, GBytes *pcm, GError **error)
{
	AiWebrtcVoiceActivity *self = AI_WEBRTC_VOICE_ACTIVITY(vad);
	Activity *a;
	gsize bytes, i;
	const guint8 *raw = g_bytes_get_data(pcm, &bytes);
	gint16 samples[480];
	gint detected;
	if (bytes != 320 && bytes != 640 && bytes != 960) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"VAD requires 10, 20 or 30 ms of 16 kHz mono S16LE PCM");
		return -1;
	}
	a = g_hash_table_lookup(self->participants, speaker);
	if (a == NULL) {
		a = g_new0(Activity, 1);
		a->vad = fvad_new();
		if (a->vad == NULL) {
			g_free(a);
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
								"Cannot allocate WebRTC VAD");
			return -1;
		}
		fvad_set_sample_rate(a->vad, 16000);
		fvad_set_mode(a->vad, self->mode);
		g_hash_table_insert(self->participants, g_strdup(speaker), a);
	}
	for (i = 0; i < bytes / 2; i++)
		samples[i] = (gint16)(raw[2 * i] | ((guint16)raw[2 * i + 1] << 8));
	detected = fvad_process(a->vad, samples, bytes / 2);
	if (detected > 0) {
		a->speaking = TRUE;
		a->silent_samples = 0;
		return AI_VOICE_ACTIVITY_SPEECH;
	}
	if (a->speaking) {
		a->silent_samples += bytes / 2;
		if (a->silent_samples >= self->silence_ms * 16) {
			a->speaking = FALSE;
			a->silent_samples = 0;
			return AI_VOICE_ACTIVITY_END;
		}
	}
	return 0;
}

static void
reset(AiVoiceActivity *vad, const gchar *speaker)
{
	g_hash_table_remove(AI_WEBRTC_VOICE_ACTIVITY(vad)->participants, speaker);
}

static void
activity_iface(AiVoiceActivityInterface *iface)
{
	iface->process = process;
	iface->reset = reset;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiWebrtcVoiceActivity *self = AI_WEBRTC_VOICE_ACTIVITY(object);
	if (id == 1)
		g_value_set_uint(value, self->silence_ms);
	else if (id == 2)
		g_value_set_uint(value, self->mode);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiWebrtcVoiceActivity *self = AI_WEBRTC_VOICE_ACTIVITY(object);
	if (id == 1)
		self->silence_ms = g_value_get_uint(value);
	else if (id == 2) {
		self->mode = g_value_get_uint(value);
		g_hash_table_remove_all(self->participants);
	} else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
finalize(GObject *object)
{
	g_hash_table_unref(AI_WEBRTC_VOICE_ACTIVITY(object)->participants);
	G_OBJECT_CLASS(ai_webrtc_voice_activity_parent_class)->finalize(object);
}

static void
ai_webrtc_voice_activity_class_init(AiWebrtcVoiceActivityClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->finalize = finalize;
	oc->get_property = get_property;
	oc->set_property = set_property;
	g_object_class_install_property(
		oc, 1,
		g_param_spec_uint("trailing-silence-ms", "Trailing silence",
						  "Silence ending an utterance", 10, 10000, 600,
						  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, 2,
		g_param_spec_uint("mode", "Mode", "WebRTC aggressiveness", 0, 3, 2,
						  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
ai_webrtc_voice_activity_init(AiWebrtcVoiceActivity *self)
{
	self->silence_ms = 600;
	self->mode = 2;
	self->participants =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, activity_free);
}

/**
 * ai_webrtc_voice_activity_new:
 *
 * Returns: (transfer full): a VAD with independent participant state
 */
AiWebrtcVoiceActivity *
ai_webrtc_voice_activity_new(void)
{
	g_autoptr(AiWebrtcVoiceActivity) self =
		g_object_new(AI_TYPE_WEBRTC_VOICE_ACTIVITY, NULL);
	return g_steal_pointer(&self);
}
