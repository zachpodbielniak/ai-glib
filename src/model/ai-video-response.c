/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include "model/ai-video-response.h"
#include "core/ai-error.h"
#include <libsoup/soup.h>

/**
 * AiVideoResponse:
 *
 * A completed video artifact with provider/model provenance.
 * URLs may expire: download promptly with ai_video_response_save_to_file().
 * Grok Build artifacts use local file URIs.
 */
struct _AiVideoResponse
{
	gchar *url;
	gchar *model;
	gchar *request_id;
	gdouble duration;
};
G_DEFINE_BOXED_TYPE(AiVideoResponse, ai_video_response, ai_video_response_copy, ai_video_response_free)

/**
 * ai_video_response_new:
 * @url: artifact URI
 * @model: (nullable): producing model
 *
 * Creates an owned result. Unknown duration is reported as -1.
 *
 * Returns: (transfer full): a video result
 */
AiVideoResponse *
ai_video_response_new(const gchar *url, const gchar *model)
{
	g_autoptr(AiVideoResponse) self = g_new0(AiVideoResponse, 1);
	self->url = g_strdup(url);
	self->model = g_strdup(model);
	self->duration = -1.0;
	return (AiVideoResponse *)g_steal_pointer(&self);
}

/**
 * ai_video_response_copy:
 * @self: a video result
 *
 * Copies result metadata independently of its source.
 *
 * Returns: (transfer full): a result copy
 */
AiVideoResponse *
ai_video_response_copy(const AiVideoResponse *self)
{
	g_autoptr(AiVideoResponse) copy = NULL;
	g_return_val_if_fail(self != NULL, NULL);
	copy = ai_video_response_new(self->url, self->model);
	copy->request_id = g_strdup(self->request_id);
	copy->duration = self->duration;
	return (AiVideoResponse *)g_steal_pointer(&copy);
}

/**
 * ai_video_response_free:
 * @self: (nullable): a video result
 *
 * Releases owned metadata. The external artifact is not deleted.
 */
void
ai_video_response_free(AiVideoResponse *self)
{
	if (self == NULL)
		return;
	g_free(self->url);
	g_free(self->model);
	g_free(self->request_id);
	g_free(self);
}

/**
 * ai_video_response_get_url:
 * @self: a video result
 *
 * Retrieves result url metadata.
 *
 * Returns: (transfer none) (nullable): the url
 */
const gchar *
ai_video_response_get_url(const AiVideoResponse *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->url;
}

/**
 * ai_video_response_get_model:
 * @self: a video result
 *
 * Retrieves result model metadata.
 *
 * Returns: (transfer none) (nullable): the model
 */
const gchar *
ai_video_response_get_model(const AiVideoResponse *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->model;
}

/**
 * ai_video_response_get_request_id:
 * @self: a video result
 *
 * Retrieves result request id metadata.
 *
 * Returns: (transfer none) (nullable): the request id
 */
const gchar *
ai_video_response_get_request_id(const AiVideoResponse *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->request_id;
}

/**
 * ai_video_response_set_request_id:
 * @self: a video result
 * @request_id: (nullable): provider job identifier
 *
 * Copies the job identifier for diagnostics.
 */
void
ai_video_response_set_request_id(AiVideoResponse *self, const gchar *request_id)
{
	g_autofree gchar *copy = NULL;
	g_return_if_fail(self != NULL);
	copy = g_strdup(request_id);
	g_free(self->request_id);
	self->request_id = (gchar *)g_steal_pointer(&copy);
}

/**
 * ai_video_response_get_duration:
 * @self: a video result
 *
 * Returns the actual duration when supplied by the provider.
 *
 * Returns: duration in seconds, or -1 when unknown
 */
gdouble
ai_video_response_get_duration(const AiVideoResponse *self)
{
	g_return_val_if_fail(self != NULL, -1.0);
	return self->duration;
}

/**
 * ai_video_response_set_duration:
 * @self: a video result
 * @duration: duration in seconds, or -1 when unknown
 *
 * Records the provider's actual output duration.
 */
void
ai_video_response_set_duration(AiVideoResponse *self, gdouble duration)
{
	g_return_if_fail(self != NULL);
	self->duration = duration;
}

/**
 * ai_video_response_save_to_file:
 * @self: a video result
 * @path: destination filename
 * @cancellable: (nullable): cancellation token
 * @error: (out) (optional): return location for an error
 *
 * Streams an HTTP(S) or local file URI into a replacement file with bounded
 * memory use. No API credentials are sent to artifact hosts. Network reads
 * have a 120-second inactivity timeout. Cancelling or failing the copy
 * preserves an existing destination by aborting the replacement stream.
 *
 * Returns: %TRUE when the complete artifact was saved
 */
gboolean
ai_video_response_save_to_file(
	AiVideoResponse *self,
	const gchar *path,
	GCancellable *cancellable,
	GError **error
){
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GFile) source_file = NULL;
	g_autoptr(GFile) destination = NULL;
	g_autoptr(GInputStream) input = NULL;
	g_autoptr(GFileOutputStream) output = NULL;
	g_autoptr(GCancellable) abort_token = NULL;
	guint status;
	gssize count;
	gchar buffer[65536];

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (self->url == NULL || self->url[0] == '\0')
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
		                    "Video result has no artifact URI");
		return FALSE;
	}
	if (g_cancellable_set_error_if_cancelled(cancellable, error))
		return FALSE;

	/* Download through libsoup explicitly; GIO HTTP support is optional. */
	if (g_str_has_prefix(self->url, "https://") ||
	    g_str_has_prefix(self->url, "http://"))
	{
		session = soup_session_new_with_options("timeout", 120, NULL);
		message = soup_message_new("GET", self->url);
		if (message == NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
			                    "Invalid video artifact URL");
			return FALSE;
		}
		input = soup_session_send(session, message, cancellable, error);
		if (input == NULL)
			return FALSE;
		status = soup_message_get_status(message);
		if (!SOUP_STATUS_IS_SUCCESSFUL(status))
		{
			g_set_error(error, AI_ERROR, AI_ERROR_NETWORK_ERROR,
			            "Video download failed: HTTP %u", status);
			return FALSE;
		}
	}
	else if (g_str_has_prefix(self->url, "file://"))
	{
		source_file = g_file_new_for_uri(self->url);
		input = G_INPUT_STREAM(g_file_read(source_file, cancellable, error));
		if (input == NULL)
			return FALSE;
	}
	else
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
		                    "Video artifact must use HTTP(S) or a file URI");
		return FALSE;
	}

	destination = g_file_new_for_path(path);
	output = g_file_replace(destination, NULL, FALSE,
	                        G_FILE_CREATE_REPLACE_DESTINATION,
	                        cancellable, error);
	if (output == NULL)
		return FALSE;

	/* Close with a cancelled token on any failure to discard the temp file. */
	while ((count = g_input_stream_read(input, buffer, sizeof(buffer),
	                                   cancellable, error)) > 0)
	{
		if (!g_output_stream_write_all(G_OUTPUT_STREAM(output), buffer,
		                               (gsize)count, NULL, cancellable, error))
			break;
	}
	if (count != 0)
	{
		abort_token = g_cancellable_new();
		g_cancellable_cancel(abort_token);
		g_output_stream_close(G_OUTPUT_STREAM(output), abort_token, NULL);
		return FALSE;
	}
	return g_output_stream_close(G_OUTPUT_STREAM(output), cancellable, error);
}
