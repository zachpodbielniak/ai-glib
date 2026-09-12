/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Imagine video jobs retain their task through each asynchronous step.
 * All timers use the task context and are destroyed by pointer.
 */
#include "config.h"
#include "providers/ai-grok-video.h"
#include "providers/ai-grok-client.h"
#include "providers/ai-image-shared.h"
#include "core/ai-json-util.h"
#include "core/ai-error.h"

typedef struct {
	AiVideoRequest *request;
	GCancellable *cancel;
	GCancellable *external;
	gulong cancel_id;
	GSource *deadline;
	GSource *poll;
	gchar *id;
	gchar *model;
	gchar *base;
	gboolean timed_out;
} VideoJob;

/* Forward cancellation from any thread; completion stays on the task context. */
static void
forward_cancel(GCancellable *external, gpointer data)
{
	(void)external;
	g_cancellable_cancel(G_CANCELLABLE(data));
}

static void
clear_source(GSource **source)
{
	if (*source != NULL) {
		g_source_destroy(*source);
		g_clear_pointer(source, g_source_unref);
	}
}

static void
job_free(VideoJob *job)
{
	clear_source(&job->deadline);
	clear_source(&job->poll);
	if (job->cancel_id != 0)
		g_cancellable_disconnect(job->external, job->cancel_id);
	g_clear_object(&job->external);
	g_clear_object(&job->cancel);
	ai_video_request_free(job->request);
	g_free(job->id);
	g_free(job->model);
	g_free(job->base);
	g_free(job);
}

static void send_step(GTask *task);

/* A deadline cancels in-flight I/O, or immediately wakes a pending poll. */
static gboolean
deadline_cb(gpointer data)
{
	GTask *task = G_TASK(data);
	VideoJob *job = g_task_get_task_data(task);
	clear_source(&job->deadline);
	job->timed_out = TRUE;
	g_cancellable_cancel(job->cancel);
	return G_SOURCE_REMOVE;
}

static void
finish_error(GTask *task, GError *error)
{
	VideoJob *job = g_task_get_task_data(task);
	clear_source(&job->deadline);
	clear_source(&job->poll);
	if (job->timed_out) {
		g_clear_error(&error);
		error = g_error_new_literal(AI_ERROR, AI_ERROR_TIMEOUT, "Grok video generation timed out");
	}
	g_task_return_error(task, error);
	g_object_unref(task);
}

static gboolean
poll_cb(gpointer data)
{
	GTask *task = G_TASK(data);
	VideoJob *job = g_task_get_task_data(task);
	clear_source(&job->poll);
	send_step(task);
	return G_SOURCE_REMOVE;
}

/* Completion parses untrusted JSON without triggering json-glib criticals. */
static void
step_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = G_TASK(data);
	VideoJob *job = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	const gchar *body;
	const gchar *status;
	gsize length;
	(void)source;
	bytes = ai_image_shared_send_finish(result, &error);
	if (bytes == NULL) {
		finish_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	if (g_cancellable_is_cancelled(job->cancel)) {
		finish_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Video generation cancelled"));
		return;
	}
	body = g_bytes_get_data(bytes, &length);
	if (!json_parser_load_from_data(parser, body != NULL ? body : "", length, &error)) {
		finish_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	root = ai_json_root_object(parser);
	if (job->id == NULL) {
		const gchar *id = ai_json_get_string(root, "request_id", NULL);
		if (id == NULL || *id == '\0') {
			finish_error(task, g_error_new_literal(AI_ERROR, AI_ERROR_INVALID_RESPONSE, "Missing video request_id"));
			return;
		}
		job->id = g_strdup(id);
	} else {
		status = ai_json_get_string(root, "status", NULL);
		if (g_strcmp0(status, "done") == 0) {
			JsonObject *video = ai_json_get_object(root, "video");
			const gchar *url = ai_json_get_string(video, "url", NULL);
			AiVideoResponse *response;
			if (!ai_json_get_boolean(video, "respect_moderation", TRUE)) {
				finish_error(task, g_error_new_literal(AI_ERROR, AI_ERROR_CONTENT_FILTERED, "Video failed moderation"));
				return;
			}
			if (url == NULL || !(g_str_has_prefix(url, "https://") || g_str_has_prefix(url, "http://"))) {
				finish_error(task, g_error_new_literal(AI_ERROR, AI_ERROR_INVALID_RESPONSE, "Missing or invalid video URL"));
				return;
			}
			response = ai_video_response_new(url, ai_json_get_string(root, "model", job->model));
			ai_video_response_set_request_id(response, job->id);
			ai_video_response_set_duration(response, ai_json_get_double(video, "duration", -1));
			clear_source(&job->deadline);
			g_task_return_pointer(task, response, (GDestroyNotify)ai_video_response_free);
			g_object_unref(task);
			return;
		}
		if (g_strcmp0(status, "pending") != 0) {
			JsonObject *detail = ai_json_get_object(root, "error");
			finish_error(task, g_error_new(AI_ERROR,
				g_strcmp0(status, "expired") == 0 ? AI_ERROR_TIMEOUT : AI_ERROR_INVALID_RESPONSE,
				"Video status %s: %s", status != NULL ? status : "missing",
				ai_json_get_string(detail, "message", "generation did not complete")));
			return;
		}
	}
	/* A cancellable child source wakes the same poll immediately on cancellation. */
	job->poll = g_timeout_source_new(ai_video_request_get_poll_interval_ms(job->request));
	{
		GSource *cancel_source = g_cancellable_source_new(job->cancel);
		g_source_set_dummy_callback(cancel_source);
		g_source_add_child_source(job->poll, cancel_source);
		g_source_unref(cancel_source);
	}
	g_source_set_callback(job->poll, poll_cb, task, NULL);
	g_source_attach(job->poll, g_task_get_context(task));
}

/* Binary images become documented data URLs, never filesystem URLs. */
static void
add_image(JsonBuilder *builder, AiImage *image)
{
	g_autofree gchar *base64 = ai_image_dup_base64(image);
	g_autofree gchar *url = g_strdup_printf("data:%s;base64,%s", ai_image_get_mime_type(image), base64);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "url");
	json_builder_add_string_value(builder, url);
	json_builder_end_object(builder);
}

/* Role metadata has no wire member; name each input in the prompt instead. */
static gchar *
video_prompt_with_roles(AiVideoRequest *request)
{
	g_autoptr(GString) prompt = g_string_new(ai_video_request_get_prompt(request));
	AiImage *image = ai_video_request_get_image(request);
	GList *iter;
	guint index = 1;
	const gchar *role;

	if (image != NULL) {
		role = ai_image_get_role(image);
		if (role != NULL && *role != '\0')
			g_string_append_printf(prompt, "\nStarting image role: %s", role);
	}
	for (iter = ai_video_request_get_reference_images(request); iter != NULL; iter = iter->next, index++) {
		role = ai_image_get_role((AiImage *)iter->data);
		if (role != NULL && *role != '\0')
			g_string_append_printf(prompt, "\nReference image %u role: %s", index, role);
	}
	return g_string_free((GString *)g_steal_pointer(&prompt), FALSE);
}

static void
send_step(GTask *task)
{
	VideoJob *job = g_task_get_task_data(task);
	AiClient *client = AI_CLIENT(g_task_get_source_object(task));
	g_autoptr(SoupMessage) msg = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *url = NULL;
	if (g_cancellable_is_cancelled(job->cancel)) {
		finish_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Video generation cancelled"));
		return;
	}
	if (job->id != NULL) {
		g_autofree gchar *escaped = g_uri_escape_string(job->id, NULL, FALSE);
		url = g_strconcat(job->base, "/v1/videos/", escaped, NULL);
	} else {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autoptr(JsonNode) root = NULL;
		g_autofree gchar *body = NULL;
		g_autofree gchar *prompt = video_prompt_with_roles(job->request);
		const gchar *value;
		const gchar * const *voices;
		guint voice_index;
		GList *iter;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "model");
		json_builder_add_string_value(builder, job->model);
		json_builder_set_member_name(builder, "prompt");
		json_builder_add_string_value(builder, prompt);
		if (ai_video_request_get_duration(job->request) != -1) {
			json_builder_set_member_name(builder, "duration");
			json_builder_add_int_value(builder, ai_video_request_get_duration(job->request));
		}
		value = ai_video_request_get_aspect_ratio(job->request);
		if (value != NULL) {
			json_builder_set_member_name(builder, "aspect_ratio");
			json_builder_add_string_value(builder, value);
		}
		value = ai_video_request_get_resolution(job->request);
		if (value != NULL) {
			json_builder_set_member_name(builder, "resolution");
			json_builder_add_string_value(builder, value);
		}
		if (ai_video_request_get_image(job->request) != NULL) {
			json_builder_set_member_name(builder, "image");
			add_image(builder, ai_video_request_get_image(job->request));
		}
		iter = ai_video_request_get_reference_images(job->request);
		if (iter != NULL) {
			json_builder_set_member_name(builder, "reference_images");
			json_builder_begin_array(builder);
			for (; iter != NULL; iter = iter->next)
				add_image(builder, (AiImage *)iter->data);
			json_builder_end_array(builder);
		}
		if (ai_video_request_get_generate_audio(job->request) != AI_TRI_UNSET) {
			json_builder_set_member_name(builder, "generate_audio");
			json_builder_add_boolean_value(builder, ai_video_request_get_generate_audio(job->request) == AI_TRI_TRUE);
		}
		voices = ai_video_request_get_voices(job->request);
		if (voices != NULL && voices[0] != NULL) {
			json_builder_set_member_name(builder, "reference_audios");
			json_builder_begin_array(builder);
			for (voice_index = 0; voices[voice_index] != NULL; voice_index++) {
				json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "voice_id");
				json_builder_add_string_value(builder, voices[voice_index]);
				json_builder_end_object(builder);
			}
			json_builder_end_array(builder);
		}
		json_builder_end_object(builder);
		root = json_builder_get_root(builder);
		body = json_to_string(root, FALSE);
		bytes = g_bytes_new(body, strlen(body));
		url = g_strconcat(job->base, "/v1/videos/generations", NULL);
	}
	msg = soup_message_new(job->id != NULL ? "GET" : "POST", url);
	if (msg == NULL) {
		finish_error(task, g_error_new_literal(AI_ERROR, AI_ERROR_INVALID_REQUEST, "Invalid Grok base URL"));
		return;
	}
	if (bytes != NULL)
		soup_message_set_request_body_from_bytes(msg, "application/json", bytes);
	AI_CLIENT_GET_CLASS(client)->add_auth_headers(client, msg);
	/* Creation is not idempotent: retry only GET, never risk duplicate paid jobs. */
	g_main_context_push_thread_default(g_task_get_context(task));
	ai_image_shared_send_async(ai_client_get_soup_session(client), msg, bytes,
		job->id != NULL ? ai_config_get_max_retries(ai_client_get_config(client)) : 0,
		job->cancel, step_done, task);
	g_main_context_pop_thread_default(g_task_get_context(task));
}

/* Enforce the documented HTTP subset before any network traffic. */
static const gchar *
validate_request(AiVideoRequest *request)
{
	const gchar *op = ai_video_request_get_operation(request);
	const gchar *resolution = ai_video_request_get_resolution(request);
	const gchar *aspect = ai_video_request_get_aspect_ratio(request);
	const gchar *model = ai_video_request_get_model(request);
	const gchar *ratios[] = { "1:1", "16:9", "9:16", "4:3", "3:4", "3:2", "2:3", NULL };
	gint duration = ai_video_request_get_duration(request);
	guint count = ai_video_request_get_reference_image_count(request);
	AiImage *image = ai_video_request_get_image(request);
	GList *iter;
	const gchar * const *voices = ai_video_request_get_voices(request);
	guint voice_count = 0;
	if (voices != NULL) {
		while (voices[voice_count] != NULL) {
			if (*voices[voice_count] == '\0') return "Empty voice identifier";
			voice_count++;
		}
	}
	if (op == NULL)
		op = count > 0 || voice_count > 0 ? "reference-to-video" : (image != NULL ? "image-to-video" : "generate");
	if (voice_count > 3 || (voice_count > 0 && (g_strcmp0(op, "reference-to-video") != 0 || (model != NULL && strcmp(model, AI_GROK_VIDEO_MODEL_GROK_IMAGINE_1_5) != 0))))
		return "Up to three preset voices require video 1.5 reference-to-video";
	if (ai_video_request_get_prompt(request) == NULL || *ai_video_request_get_prompt(request) == '\0')
		return "A video prompt is required";
	if (duration != -1 && (duration < 1 || duration > 15))
		return "Video duration must be 1 through 15 seconds";
	if (aspect != NULL && !g_strv_contains(ratios, aspect))
		return "Unsupported video aspect ratio";
	if (resolution != NULL && strcmp(resolution, "480p") != 0 && strcmp(resolution, "720p") != 0 && strcmp(resolution, "1080p") != 0)
		return "Unsupported video resolution";
	if (g_strcmp0(resolution, "1080p") == 0 && ((count > 0 || voice_count > 0) || (model != NULL && strcmp(model, AI_GROK_VIDEO_MODEL_GROK_IMAGINE_1_5) != 0)))
		return "1080p requires Imagine video 1.5 and no reference images";
	if (g_strcmp0(op, "generate") == 0) {
		if (image != NULL || count > 0) return "generate does not accept input images";
	} else if (g_strcmp0(op, "image-to-video") == 0) {
		if (image == NULL || count > 0) return "image-to-video requires one image and no references";
	} else if (g_strcmp0(op, "reference-to-video") == 0) {
		if (image != NULL || (count == 0 && voice_count == 0) || count > 7) return "reference-to-video requires 1 through 7 references and no starting image";
	} else return "Unsupported video operation";
	if (image != NULL && (ai_image_get_size(image) == 0 || !(g_strcmp0(ai_image_get_mime_type(image), "image/png") == 0 || g_strcmp0(ai_image_get_mime_type(image), "image/jpeg") == 0 || g_strcmp0(ai_image_get_mime_type(image), "image/webp") == 0)))
		return "Video input must be a nonempty PNG, JPEG or WebP";
	for (iter = ai_video_request_get_reference_images(request); iter != NULL; iter = iter->next) {
		AiImage *reference = (AiImage *)iter->data;
		if (ai_image_get_size(reference) == 0 || !(g_strcmp0(ai_image_get_mime_type(reference), "image/png") == 0 || g_strcmp0(ai_image_get_mime_type(reference), "image/jpeg") == 0 || g_strcmp0(ai_image_get_mime_type(reference), "image/webp") == 0))
			return "Video references must be nonempty PNG, JPEG or WebP";
	}
	return NULL;
}

static void
generate_async(AiVideoGenerator *generator, AiVideoRequest *request,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task = g_task_new(generator, cancellable, callback, user_data);
	VideoJob *job;
	const gchar *invalid = validate_request(request);
	const gchar *model;
	if (invalid != NULL) {
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST, "%s", invalid);
		g_object_unref(task);
		return;
	}
	job = g_new0(VideoJob, 1);
	job->request = ai_video_request_copy(request);
	job->cancel = g_cancellable_new();
	if (cancellable != NULL) {
		job->external = g_object_ref(cancellable);
		job->cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(forward_cancel), g_object_ref(job->cancel), g_object_unref);
	}
	model = ai_video_request_get_model(request);
	job->model = g_strdup(model != NULL ? model : AI_GROK_VIDEO_MODEL_GROK_IMAGINE_1_5);
	job->base = g_strdup(ai_config_get_base_url(ai_client_get_config(AI_CLIENT(generator)), AI_PROVIDER_GROK));
	g_task_set_task_data(task, job, (GDestroyNotify)job_free);
	job->deadline = g_timeout_source_new(ai_video_request_get_timeout_ms(request));
	g_source_set_callback(job->deadline, deadline_cb, task, NULL);
	g_source_attach(job->deadline, g_task_get_context(task));
	send_step(task);
}

static AiVideoResponse *
generate_finish(AiVideoGenerator *generator, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, generator), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}

static const gchar *
default_model(AiVideoGenerator *generator)
{
	(void)generator;
	return AI_GROK_VIDEO_MODEL_GROK_IMAGINE_1_5;
}

/* Private interface registration shared with the client type definition. */
void
ai_grok_video_generator_init(AiVideoGeneratorInterface *iface)
{
	iface->generate_video_async = generate_async;
	iface->generate_video_finish = generate_finish;
	iface->get_default_model = default_model;
}
