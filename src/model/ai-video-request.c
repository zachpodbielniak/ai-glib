/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include "model/ai-video-request.h"
/**
 * AiVideoRequest:
 *
 * Owned video generation parameters. Setters copy strings and images.
 * A request may be freed immediately after starting an asynchronous generation.
 * Provider implementations validate model-specific limits before dispatch.
 */
struct _AiVideoRequest
{
	gchar *prompt;
	gchar *model;
	gchar *operation;
	gchar *aspect_ratio;
	gchar *resolution;
	gint duration;
	AiTriState generate_audio;
	guint poll_interval_ms;
	guint timeout_ms;
	AiImage *image;
	GList *reference_images;
	gchar **voices;
};
G_DEFINE_BOXED_TYPE(AiVideoRequest, ai_video_request, ai_video_request_copy, ai_video_request_free)

/**
 * ai_video_request_new:
 * @prompt: prompt text
 *
 * Creates a request with provider defaults and a ten-minute timeout.
 *
 * Returns: (transfer full): a new request
 */
AiVideoRequest *
ai_video_request_new(const gchar *prompt)
{
	g_autoptr(AiVideoRequest) self = g_new0(AiVideoRequest, 1);
	self->prompt = g_strdup(prompt);
	self->duration = -1;
	self->generate_audio = AI_TRI_UNSET;
	self->poll_interval_ms = 1000;
	self->timeout_ms = 600000;
	return (AiVideoRequest *)g_steal_pointer(&self);
}

/**
 * ai_video_request_copy:
 * @self: a request
 *
 * Copies request metadata and retains independent copies of image payloads.
 *
 * Returns: (transfer full): an independent request
 */
AiVideoRequest *
ai_video_request_copy(const AiVideoRequest *self)
{
	g_autoptr(AiVideoRequest) copy = NULL;
	GList *iter;
	g_return_val_if_fail(self != NULL, NULL);
	copy = ai_video_request_new(self->prompt);
	copy->voices = g_strdupv(self->voices);
	copy->model = g_strdup(self->model);
	copy->operation = g_strdup(self->operation);
	copy->aspect_ratio = g_strdup(self->aspect_ratio);
	copy->resolution = g_strdup(self->resolution);
	copy->duration = self->duration;
	copy->generate_audio = self->generate_audio;
	copy->poll_interval_ms = self->poll_interval_ms;
	copy->timeout_ms = self->timeout_ms;
	if (self->image != NULL)
		copy->image = ai_image_copy(self->image);
	for (iter = self->reference_images; iter != NULL; iter = iter->next)
		ai_video_request_add_reference_image(copy, (AiImage *)iter->data);
	return (AiVideoRequest *)g_steal_pointer(&copy);
}

/**
 * ai_video_request_free:
 * @self: (nullable): a request
 *
 * Releases a request and all owned inputs.
 */
void
ai_video_request_free(AiVideoRequest *self)
{
	if (self == NULL)
		return;
	g_free(self->prompt);
	g_free(self->model);
	g_free(self->operation);
	g_free(self->aspect_ratio);
	g_free(self->resolution);
	g_clear_pointer(&self->image, ai_image_free);
	g_list_free_full(self->reference_images, (GDestroyNotify)ai_image_free);
	g_strfreev(self->voices);
	g_free(self);
}

/**
 * ai_video_request_get_prompt:
 * @self: a request
 *
 * Prompt text.
 *
 * Returns: (transfer none) (nullable): the current value
 */
const gchar *
ai_video_request_get_prompt(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->prompt;
}

/**
 * ai_video_request_set_prompt:
 * @self: a request
 * @value: (nullable): prompt text
 *
 * Updates the request parameter, copying the string.
 */
void
ai_video_request_set_prompt(AiVideoRequest *self, const gchar * value)
{
	g_autofree gchar *copy = NULL;
	g_return_if_fail(self != NULL);
	copy = g_strdup(value);
	g_free(self->prompt);
	self->prompt = (gchar *)g_steal_pointer(&copy);
}

/**
 * ai_video_request_get_model:
 * @self: a request
 *
 * Provider video model; NULL selects the provider default.
 *
 * Returns: (transfer none) (nullable): the current value
 */
const gchar *
ai_video_request_get_model(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->model;
}

/**
 * ai_video_request_set_model:
 * @self: a request
 * @value: (nullable): provider video model; null selects the provider default
 *
 * Updates the request parameter, copying the string.
 */
void
ai_video_request_set_model(AiVideoRequest *self, const gchar * value)
{
	g_autofree gchar *copy = NULL;
	g_return_if_fail(self != NULL);
	copy = g_strdup(value);
	g_free(self->model);
	self->model = (gchar *)g_steal_pointer(&copy);
}

/**
 * ai_video_request_get_operation:
 * @self: a request
 *
 * Operation: generate, image-to-video or reference-to-video; NULL infers from inputs.
 *
 * Returns: (transfer none) (nullable): the current value
 */
const gchar *
ai_video_request_get_operation(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->operation;
}

/**
 * ai_video_request_set_operation:
 * @self: a request
 * @value: (nullable): operation: generate, image-to-video or reference-to-video; null infers from inputs
 *
 * Updates the request parameter, copying the string.
 */
void
ai_video_request_set_operation(AiVideoRequest *self, const gchar * value)
{
	g_autofree gchar *copy = NULL;
	g_return_if_fail(self != NULL);
	copy = g_strdup(value);
	g_free(self->operation);
	self->operation = (gchar *)g_steal_pointer(&copy);
}

/**
 * ai_video_request_get_aspect_ratio:
 * @self: a request
 *
 * Output aspect ratio, or NULL for the provider default.
 *
 * Returns: (transfer none) (nullable): the current value
 */
const gchar *
ai_video_request_get_aspect_ratio(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->aspect_ratio;
}

/**
 * ai_video_request_set_aspect_ratio:
 * @self: a request
 * @value: (nullable): output aspect ratio, or null for the provider default
 *
 * Updates the request parameter, copying the string.
 */
void
ai_video_request_set_aspect_ratio(AiVideoRequest *self, const gchar * value)
{
	g_autofree gchar *copy = NULL;
	g_return_if_fail(self != NULL);
	copy = g_strdup(value);
	g_free(self->aspect_ratio);
	self->aspect_ratio = (gchar *)g_steal_pointer(&copy);
}

/**
 * ai_video_request_get_resolution:
 * @self: a request
 *
 * Output resolution such as 480p, 720p or 1080p.
 *
 * Returns: (transfer none) (nullable): the current value
 */
const gchar *
ai_video_request_get_resolution(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->resolution;
}

/**
 * ai_video_request_set_resolution:
 * @self: a request
 * @value: (nullable): output resolution such as 480p, 720p or 1080p
 *
 * Updates the request parameter, copying the string.
 */
void
ai_video_request_set_resolution(AiVideoRequest *self, const gchar * value)
{
	g_autofree gchar *copy = NULL;
	g_return_if_fail(self != NULL);
	copy = g_strdup(value);
	g_free(self->resolution);
	self->resolution = (gchar *)g_steal_pointer(&copy);
}

/**
 * ai_video_request_get_duration:
 * @self: a request
 *
 * Duration in seconds, or -1 for the provider default.
 *
 * Returns: the current value
 */
gint
ai_video_request_get_duration(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, -1);
	return self->duration;
}

/**
 * ai_video_request_set_duration:
 * @self: a request
 * @value: duration in seconds, or -1 for the provider default
 *
 * Updates the request parameter.
 */
void
ai_video_request_set_duration(AiVideoRequest *self, gint value)
{
	g_return_if_fail(self != NULL);
	self->duration = value;
}

/**
 * ai_video_request_get_generate_audio:
 * @self: a request
 *
 * Whether to generate audio; unspecified uses the provider default.
 *
 * Returns: the current value
 */
AiTriState
ai_video_request_get_generate_audio(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, AI_TRI_UNSET);
	return self->generate_audio;
}

/**
 * ai_video_request_set_generate_audio:
 * @self: a request
 * @value: whether to generate audio; unspecified uses the provider default
 *
 * Updates the request parameter.
 */
void
ai_video_request_set_generate_audio(AiVideoRequest *self, AiTriState value)
{
	g_return_if_fail(self != NULL);
	self->generate_audio = value;
}

/**
 * ai_video_request_get_poll_interval_ms:
 * @self: a request
 *
 * Polling interval in milliseconds; must be positive.
 *
 * Returns: the current value
 */
guint
ai_video_request_get_poll_interval_ms(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, 1000);
	return self->poll_interval_ms;
}

/**
 * ai_video_request_set_poll_interval_ms:
 * @self: a request
 * @value: polling interval in milliseconds; must be positive
 *
 * Updates the request parameter.
 */
void
ai_video_request_set_poll_interval_ms(AiVideoRequest *self, guint value)
{
	g_return_if_fail(self != NULL);
	self->poll_interval_ms = value;
}

/**
 * ai_video_request_get_timeout_ms:
 * @self: a request
 *
 * Total generation timeout in milliseconds; must be positive.
 *
 * Returns: the current value
 */
guint
ai_video_request_get_timeout_ms(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, 600000);
	return self->timeout_ms;
}

/**
 * ai_video_request_set_timeout_ms:
 * @self: a request
 * @value: total generation timeout in milliseconds; must be positive
 *
 * Updates the request parameter.
 */
void
ai_video_request_set_timeout_ms(AiVideoRequest *self, guint value)
{
	g_return_if_fail(self != NULL);
	self->timeout_ms = value;
}

/**
 * ai_video_request_get_image:
 * @self: a request
 *
 * Returns the pinned first frame.
 *
 * Returns: (transfer none) (nullable): the source image
 */
AiImage *
ai_video_request_get_image(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->image;
}

/**
 * ai_video_request_set_image:
 * @self: a request
 * @image: (nullable): source image
 *
 * Copies the pinned first frame, or clears it.
 */
void
ai_video_request_set_image(AiVideoRequest *self, AiImage *image)
{
	g_autoptr(AiImage) copy = NULL;
	g_return_if_fail(self != NULL);
	if (image != NULL)
		copy = ai_image_copy(image);
	g_clear_pointer(&self->image, ai_image_free);
	self->image = (AiImage *)g_steal_pointer(&copy);
}

/**
 * ai_video_request_add_reference_image:
 * @self: a request
 * @image: reference image
 *
 * Appends a copy of the image, preserving its role.
 */
void
ai_video_request_add_reference_image(AiVideoRequest *self, AiImage *image)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail(image != NULL);
	self->reference_images = g_list_append(self->reference_images, ai_image_copy(image));
}

/**
 * ai_video_request_get_reference_images:
 * @self: a request
 *
 * Returns the ordered reference images.
 *
 * Returns: (transfer none) (element-type AiImage): borrowed references
 */
GList *
ai_video_request_get_reference_images(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->reference_images;
}

/**
 * ai_video_request_get_reference_image_count:
 * @self: a request
 *
 * Counts conditioning images, excluding the pinned first frame.
 *
 * Returns: the number of reference images
 */
guint
ai_video_request_get_reference_image_count(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return g_list_length(self->reference_images);
}

/**
 * ai_video_request_get_voices:
 * @self: a request
 *
 * Returns preset voice identifiers for reference-to-video.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): voice names
 */
const gchar * const *
ai_video_request_get_voices(const AiVideoRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return (const gchar * const *)self->voices;
}

/**
 * ai_video_request_set_voices:
 * @self: a request
 * @voices: (nullable) (array zero-terminated=1): preset voice identifiers
 *
 * Copies the ordered voice names; providers validate their own limits.
 */
void
ai_video_request_set_voices(AiVideoRequest *self, const gchar * const *voices)
{
	g_auto(GStrv) copy = NULL;
	g_return_if_fail(self != NULL);
	copy = g_strdupv((gchar **)voices);
	g_strfreev(self->voices);
	self->voices = (gchar **)g_steal_pointer(&copy);
}
