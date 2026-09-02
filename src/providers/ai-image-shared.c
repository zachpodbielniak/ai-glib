/*
 * ai-image-shared.c - Shared image-request plumbing for providers (private)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "providers/ai-image-shared.h"
#include "providers/ai-json-util.h"
#include "core/ai-error.h"
#include "model/ai-generated-image.h"

/* Base backoff, doubled per attempt.  Kept small: image generation is
 * already slow, and a long backoff on top of it is worse than failing. */
#define AI_IMAGE_SHARED_BASE_BACKOFF_MS 250
/* Never honour a Retry-After longer than this, defensively. */
#define AI_IMAGE_SHARED_MAX_BACKOFF_MS  10000
/* How much of an error body to quote back to the caller. */
#define AI_IMAGE_SHARED_ERROR_EXCERPT   400

/*
 * ----------------------------------------------------------------------
 * Status mapping
 * ----------------------------------------------------------------------
 */

void
ai_image_shared_status_to_error (
    guint         status,
    const gchar  *body,
    gsize         body_len,
    GError      **error
){
    g_autofree gchar *excerpt = NULL;

    /*
     * The provider's own message is by far the most useful part of a
     * failure -- OpenAI's image_generation_user_error, for instance, is
     * how a content-policy refusal is reported -- so quote a bounded
     * excerpt rather than discarding the body.
     */
    if (body != NULL && body_len > 0)
    {
        gsize len = MIN (body_len, (gsize) AI_IMAGE_SHARED_ERROR_EXCERPT);

        excerpt = g_strndup (body, len);
        g_strstrip (excerpt);
    }

    if (status == 401)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                     "Authentication failed (HTTP %u)%s%s", status,
                     excerpt != NULL ? ": " : "",
                     excerpt != NULL ? excerpt : "");
    }
    else if (status == 403)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_PERMISSION_DENIED,
                     "Permission denied (HTTP %u)%s%s", status,
                     excerpt != NULL ? ": " : "",
                     excerpt != NULL ? excerpt : "");
    }
    else if (status == 429)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_RATE_LIMITED,
                     "Rate limited (HTTP %u)%s%s", status,
                     excerpt != NULL ? ": " : "",
                     excerpt != NULL ? excerpt : "");
    }
    else if (status >= 500)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                     "Server error (HTTP %u)%s%s", status,
                     excerpt != NULL ? ": " : "",
                     excerpt != NULL ? excerpt : "");
    }
    else
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "Request failed (HTTP %u)%s%s", status,
                     excerpt != NULL ? ": " : "",
                     excerpt != NULL ? excerpt : "");
    }
}

/*
 * ----------------------------------------------------------------------
 * Send with retry
 * ----------------------------------------------------------------------
 */

/*
 * A retry has to send a *fresh* SoupMessage: once a message has been
 * sent its request body stream is consumed, and handing the same one back
 * to the session fails with "Source stream is already closed" rather than
 * repeating the request.  So the pieces needed to rebuild it are snapshot
 * up front and a new message is constructed per attempt.
 */
typedef struct
{
    SoupSession        *session;
    gchar              *method;
    GUri               *uri;
    SoupMessageHeaders *headers;
    GBytes             *body;
    gchar              *content_type;
    SoupMessage        *msg;          /* the current attempt */
    GTask              *task;
    guint               attempt;
    guint               max_retries;
} AiImageSendData;

static void
ai_image_send_data_free (AiImageSendData *data)
{
    g_clear_object (&data->session);
    g_clear_pointer (&data->method, g_free);
    g_clear_pointer (&data->uri, g_uri_unref);
    g_clear_pointer (&data->headers, soup_message_headers_unref);
    g_clear_pointer (&data->body, g_bytes_unref);
    g_clear_pointer (&data->content_type, g_free);
    g_clear_object (&data->msg);
    g_clear_object (&data->task);
    g_slice_free (AiImageSendData, data);
}

static void
ai_image_copy_header (const gchar *name, const gchar *value, gpointer user_data)
{
    soup_message_headers_append ((SoupMessageHeaders *) user_data, name, value);
}

/*
 * Construct the message for the next attempt from the snapshot.
 */
static SoupMessage *
ai_image_shared_new_message (AiImageSendData *data)
{
    SoupMessage *msg = soup_message_new_from_uri (data->method, data->uri);

    soup_message_headers_foreach (data->headers, ai_image_copy_header,
                                  soup_message_get_request_headers (msg));

    if (data->body != NULL)
    {
        soup_message_set_request_body_from_bytes (msg, data->content_type,
                                                  data->body);
    }

    return msg;
}

static void ai_image_shared_send_attempt (AiImageSendData *data);

/*
 * Whether a failure is worth retrying: transport errors and the statuses
 * that mean "the server is busy", never a 4xx that will fail identically
 * next time.
 */
static gboolean
ai_image_shared_retryable (guint status, const GError *error)
{
    if (error != NULL)
    {
        return !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    }

    return status == 429 || status >= 500;
}

/*
 * Milliseconds to wait before the next attempt, honouring a numeric
 * Retry-After when the server sent one.
 */
static guint
ai_image_shared_backoff_ms (SoupMessage *msg, guint attempt)
{
    guint delay = AI_IMAGE_SHARED_BASE_BACKOFF_MS << attempt;

    if (msg != NULL)
    {
        const gchar *retry_after = soup_message_headers_get_one (
            soup_message_get_response_headers (msg), "Retry-After");

        if (retry_after != NULL)
        {
            gchar *endptr = NULL;
            gint64 secs = g_ascii_strtoll (retry_after, &endptr, 10);

            if (endptr != retry_after && secs > 0)
            {
                delay = (guint) MIN (secs * 1000, (gint64) G_MAXUINT);
            }
        }
    }

    return MIN (delay, (guint) AI_IMAGE_SHARED_MAX_BACKOFF_MS);
}

static gboolean
ai_image_shared_retry_timeout (gpointer user_data)
{
    AiImageSendData *data = user_data;

    ai_image_shared_send_attempt (data);

    return G_SOURCE_REMOVE;
}

static void
ai_image_shared_on_reply (
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    AiImageSendData *data = user_data;
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GError) error = NULL;
    guint status = 0;

    bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result,
                                               &error);

    if (bytes != NULL)
    {
        status = soup_message_get_status (data->msg);

        if (SOUP_STATUS_IS_SUCCESSFUL (status))
        {
            g_task_return_pointer (data->task, g_steal_pointer (&bytes),
                                   (GDestroyNotify) g_bytes_unref);
            ai_image_send_data_free (data);
            return;
        }
    }

    /* Retry if there is budget left and the failure looks transient. */
    if (data->attempt < data->max_retries &&
        ai_image_shared_retryable (status, error) &&
        !g_cancellable_is_cancelled (g_task_get_cancellable (data->task)))
    {
        guint delay = ai_image_shared_backoff_ms (
            bytes != NULL ? data->msg : NULL, data->attempt);

        data->attempt++;

        g_debug ("ai-image: retrying after %ums (attempt %u/%u, status %u)",
                 delay, data->attempt, data->max_retries, status);

        /*
         * A timeout source rather than a sleep: this runs inside the
         * caller's main loop, which must stay responsive while we back
         * off.
         *
         * It must be attached to the *thread-default* context, not the
         * global default that g_timeout_add() would use.  A synchronous
         * caller drives a nested loop on a private context, so a timer on
         * the global default would never be dispatched and the request
         * would hang until it timed out rather than retrying.
         */
        {
            GSource *timer = g_timeout_source_new (delay);

            g_source_set_callback (timer, ai_image_shared_retry_timeout,
                                   data, NULL);
            g_source_attach (timer, g_main_context_get_thread_default ());
            g_source_unref (timer);
        }
        return;
    }

    if (bytes == NULL)
    {
        g_task_return_error (data->task, g_steal_pointer (&error));
    }
    else
    {
        g_autoptr(GError) status_error = NULL;
        gsize len = 0;
        const gchar *body = g_bytes_get_data (bytes, &len);

        ai_image_shared_status_to_error (status, body, len, &status_error);
        g_task_return_error (data->task, g_steal_pointer (&status_error));
    }

    ai_image_send_data_free (data);
}

static void
ai_image_shared_send_attempt (AiImageSendData *data)
{
    g_clear_object (&data->msg);
    data->msg = ai_image_shared_new_message (data);

    soup_session_send_and_read_async (data->session, data->msg,
                                      G_PRIORITY_DEFAULT,
                                      g_task_get_cancellable (data->task),
                                      ai_image_shared_on_reply, data);
}

void
ai_image_shared_send_async (
    SoupSession         *session,
    SoupMessage         *msg,
    GBytes              *body,
    guint                max_retries,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiImageSendData *data;
    const gchar *content_type;

    g_return_if_fail (SOUP_IS_SESSION (session));
    g_return_if_fail (SOUP_IS_MESSAGE (msg));

    data = g_slice_new0 (AiImageSendData);
    data->session = g_object_ref (session);
    data->task = g_task_new (NULL, cancellable, callback, user_data);
    data->attempt = 0;
    data->max_retries = max_retries;

    /* Snapshot everything needed to rebuild the message on a retry. */
    data->method = g_strdup (soup_message_get_method (msg));
    data->uri = g_uri_ref (soup_message_get_uri (msg));
    data->headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);
    soup_message_headers_foreach (soup_message_get_request_headers (msg),
                                  ai_image_copy_header, data->headers);

    content_type = soup_message_headers_get_content_type (
        soup_message_get_request_headers (msg), NULL);
    data->content_type = g_strdup (content_type);
    data->body = body != NULL ? g_bytes_ref (body) : NULL;

    g_task_set_source_tag (data->task, ai_image_shared_send_async);

    ai_image_shared_send_attempt (data);
}

GBytes *
ai_image_shared_send_finish (
    GAsyncResult  *result,
    GError       **error
){
    g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

    return g_task_propagate_pointer (G_TASK (result), error);
}

/*
 * ----------------------------------------------------------------------
 * Shared request construction
 * ----------------------------------------------------------------------
 */

gchar *
ai_image_shared_prompt_with_roles (AiImageRequest *request)
{
    g_autoptr(GString) out = NULL;
    GList *references;
    GList *iter;
    guint index;
    gboolean any_role = FALSE;

    g_return_val_if_fail (request != NULL, NULL);

    references = ai_image_request_get_reference_images (request);

    for (iter = references; iter != NULL; iter = iter->next)
    {
        if (ai_image_get_role ((AiImage *) iter->data) != NULL)
        {
            any_role = TRUE;
            break;
        }
    }

    if (!any_role)
    {
        return g_strdup (ai_image_request_get_prompt (request));
    }

    /*
     * The wire formats carry reference images as an ordered list with no
     * place to say what each one is for, so the correspondence has to be
     * stated in the prompt -- the only channel the model reads.  Numbering
     * matches the order the parts are emitted in.
     */
    out = g_string_new (NULL);
    g_string_append (out, "Reference images, in order:\n");

    for (iter = references, index = 1; iter != NULL; iter = iter->next, index++)
    {
        const gchar *role = ai_image_get_role ((AiImage *) iter->data);

        g_string_append_printf (out, "  %u. %s\n", index,
                                role != NULL ? role : "(unlabelled)");
    }

    g_string_append_c (out, '\n');
    g_string_append (out, ai_image_request_get_prompt (request));

    return g_strdup (out->str);
}

void
ai_image_shared_apply_extras (
    JsonBuilder    *builder,
    AiImageRequest *request
){
    GHashTable *extras;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_return_if_fail (builder != NULL);
    g_return_if_fail (request != NULL);

    extras = ai_image_request_get_extras (request);
    if (extras == NULL)
    {
        return;
    }

    g_hash_table_iter_init (&iter, extras);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        g_autoptr(JsonNode) node = NULL;

        node = json_gvariant_serialize ((GVariant *) value);
        if (node == NULL)
        {
            g_warning ("ai-image: extra '%s' could not be serialised to JSON",
                       (const gchar *) key);
            continue;
        }

        json_builder_set_member_name (builder, (const gchar *) key);
        json_builder_add_value (builder, g_steal_pointer (&node));
    }
}

JsonNode *
ai_image_shared_build_openai_json (
    AiImageRequest         *request,
    const gchar            *model,
    const AiImageModelInfo *info
){
    g_autoptr(JsonBuilder) builder = NULL;
    g_autofree gchar *prompt = NULL;

    g_return_val_if_fail (request != NULL, NULL);
    g_return_val_if_fail (model != NULL, NULL);

    builder = json_builder_new ();
    json_builder_begin_object (builder);

    prompt = ai_image_shared_prompt_with_roles (request);
    json_builder_set_member_name (builder, "prompt");
    json_builder_add_string_value (builder, prompt);

    json_builder_set_member_name (builder, "model");
    json_builder_add_string_value (builder, model);

    /* Size */
    {
        AiImageSize size = ai_image_request_get_size (request);
        const gchar *size_str = NULL;

        if (size == AI_IMAGE_SIZE_CUSTOM)
        {
            size_str = ai_image_request_get_custom_size (request);
        }
        else if (size != AI_IMAGE_SIZE_AUTO)
        {
            size_str = ai_image_size_to_string (size);
        }

        if (size_str != NULL)
        {
            json_builder_set_member_name (builder, "size");
            json_builder_add_string_value (builder, size_str);
        }
    }

    /* Quality, in whichever vocabulary this model speaks. */
    if (info != NULL)
    {
        const gchar *quality = ai_image_model_info_map_quality (
            info, ai_image_request_get_quality (request));

        if (quality != NULL)
        {
            json_builder_set_member_name (builder, "quality");
            json_builder_add_string_value (builder, quality);
        }
    }

    /* Style.  DALL-E 3 only; GPT Image rejects the member outright, which
     * is why this is capability-gated rather than merely value-gated. */
    if (info == NULL ||
        ai_image_model_info_supports (info, AI_IMAGE_CAP_STYLE))
    {
        const gchar *style = ai_image_request_get_style_preset (request);

        if (style == NULL)
        {
            style = ai_image_style_to_string (
                ai_image_request_get_style (request));
        }

        if (style != NULL)
        {
            json_builder_set_member_name (builder, "style");
            json_builder_add_string_value (builder, style);
        }
    }

    /* Count */
    if (ai_image_request_get_count (request) > 1)
    {
        json_builder_set_member_name (builder, "n");
        json_builder_add_int_value (builder,
                                    ai_image_request_get_count (request));
    }

    /*
     * Response format.  The GPT Image family always returns base64 and
     * rejects the member entirely, so its absence from the capability set
     * means "do not send this at all" -- not "send b64_json".  Getting
     * this wrong is the bug that made the default OpenAI image model fail
     * on every request.
     */
    if (info == NULL ||
        ai_image_model_info_supports (info, AI_IMAGE_CAP_URL_RESPONSE))
    {
        json_builder_set_member_name (builder, "response_format");
        json_builder_add_string_value (
            builder,
            ai_image_response_format_to_string (
                ai_image_request_get_response_format (request)));
    }

    /* Background / transparency */
    {
        const gchar *background = ai_image_background_to_string (
            ai_image_request_get_background (request));

        if (background != NULL)
        {
            json_builder_set_member_name (builder, "background");
            json_builder_add_string_value (builder, background);
        }
    }

    /* Output encoding */
    {
        const gchar *format = ai_image_format_to_string (
            ai_image_request_get_output_format (request));

        if (format != NULL)
        {
            json_builder_set_member_name (builder, "output_format");
            json_builder_add_string_value (builder, format);
        }

        if (ai_image_request_get_output_compression (request) >= 0)
        {
            json_builder_set_member_name (builder, "output_compression");
            json_builder_add_int_value (
                builder, ai_image_request_get_output_compression (request));
        }
    }

    /* Moderation */
    {
        const gchar *moderation = ai_image_moderation_to_string (
            ai_image_request_get_moderation (request));

        if (moderation != NULL)
        {
            json_builder_set_member_name (builder, "moderation");
            json_builder_add_string_value (builder, moderation);
        }
    }

    /* Streaming previews */
    if (ai_image_request_get_partial_images (request) >= 0)
    {
        json_builder_set_member_name (builder, "stream");
        json_builder_add_boolean_value (builder, TRUE);
        json_builder_set_member_name (builder, "partial_images");
        json_builder_add_int_value (
            builder, ai_image_request_get_partial_images (request));
    }

    /* User */
    {
        const gchar *user = ai_image_request_get_user (request);

        if (user != NULL)
        {
            json_builder_set_member_name (builder, "user");
            json_builder_add_string_value (builder, user);
        }
    }

    ai_image_shared_apply_extras (builder, request);

    json_builder_end_object (builder);

    return json_builder_get_root (builder);
}

/*
 * Append one AiImage as a multipart file part.
 */
static void
ai_image_shared_append_image_part (
    SoupMultipart *multipart,
    const gchar   *control_name,
    AiImage       *image
){
    GBytes *bytes = ai_image_get_bytes (image);
    const gchar *filename = ai_image_get_filename (image);
    g_autofree gchar *fallback = NULL;

    /*
     * OpenAI infers the image format from the part's filename, so a part
     * built from raw bytes needs a name whose extension matches its MIME
     * type -- otherwise a WebP arrives claiming to be a PNG and is
     * rejected.
     */
    if (filename == NULL)
    {
        const gchar *mime = ai_image_get_mime_type (image);
        const gchar *ext = "png";

        if (g_strcmp0 (mime, "image/jpeg") == 0)
        {
            ext = "jpg";
        }
        else if (g_strcmp0 (mime, "image/webp") == 0)
        {
            ext = "webp";
        }

        fallback = g_strdup_printf ("image.%s", ext);
        filename = fallback;
    }

    soup_multipart_append_form_file (multipart, control_name, filename,
                                     ai_image_get_mime_type (image), bytes);
}

SoupMultipart *
ai_image_shared_build_openai_multipart (
    AiImageRequest          *request,
    const gchar             *model,
    const AiImageModelInfo  *info,
    GError                 **error
){
    g_autoptr(SoupMultipart) multipart = NULL;
    g_autofree gchar *prompt = NULL;
    GList *references;
    GList *iter;
    AiImage *mask;
    gboolean multi;

    g_return_val_if_fail (request != NULL, NULL);
    g_return_val_if_fail (model != NULL, NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    references = ai_image_request_get_reference_images (request);
    if (references == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "This operation requires at least one reference image");
        return NULL;
    }

    multipart = soup_multipart_new (SOUP_FORM_MIME_TYPE_MULTIPART);

    /*
     * Models taking several references expect a repeated `image[]`
     * control; single-reference models expect a bare `image`.  Sending the
     * wrong spelling is rejected, so key it off the declared capability.
     */
    multi = info == NULL ||
            ai_image_model_info_supports (info, AI_IMAGE_CAP_MULTI_REFERENCE);

    for (iter = references; iter != NULL; iter = iter->next)
    {
        ai_image_shared_append_image_part (multipart,
                                           multi ? "image[]" : "image",
                                           (AiImage *) iter->data);
    }

    mask = ai_image_request_get_mask (request);
    if (mask != NULL)
    {
        ai_image_shared_append_image_part (multipart, "mask", mask);
    }

    soup_multipart_append_form_string (multipart, "model", model);

    /* Variations take no prompt at all; everything else does. */
    if (ai_image_request_get_operation (request) != AI_IMAGE_OPERATION_VARIATION)
    {
        prompt = ai_image_shared_prompt_with_roles (request);
        soup_multipart_append_form_string (multipart, "prompt", prompt);
    }

    {
        AiImageSize size = ai_image_request_get_size (request);
        const gchar *size_str = NULL;

        if (size == AI_IMAGE_SIZE_CUSTOM)
        {
            size_str = ai_image_request_get_custom_size (request);
        }
        else if (size != AI_IMAGE_SIZE_AUTO)
        {
            size_str = ai_image_size_to_string (size);
        }

        if (size_str != NULL)
        {
            soup_multipart_append_form_string (multipart, "size", size_str);
        }
    }

    if (ai_image_request_get_count (request) > 1)
    {
        g_autofree gchar *n = g_strdup_printf (
            "%d", ai_image_request_get_count (request));

        soup_multipart_append_form_string (multipart, "n", n);
    }

    if (info != NULL)
    {
        const gchar *quality = ai_image_model_info_map_quality (
            info, ai_image_request_get_quality (request));

        if (quality != NULL)
        {
            soup_multipart_append_form_string (multipart, "quality", quality);
        }
    }

    {
        const gchar *fidelity = ai_image_fidelity_to_string (
            ai_image_request_get_input_fidelity (request));

        if (fidelity != NULL)
        {
            soup_multipart_append_form_string (multipart, "input_fidelity",
                                               fidelity);
        }
    }

    {
        const gchar *background = ai_image_background_to_string (
            ai_image_request_get_background (request));

        if (background != NULL)
        {
            soup_multipart_append_form_string (multipart, "background",
                                               background);
        }
    }

    {
        const gchar *format = ai_image_format_to_string (
            ai_image_request_get_output_format (request));

        if (format != NULL)
        {
            soup_multipart_append_form_string (multipart, "output_format",
                                               format);
        }
    }

    if (info == NULL ||
        ai_image_model_info_supports (info, AI_IMAGE_CAP_URL_RESPONSE))
    {
        soup_multipart_append_form_string (
            multipart, "response_format",
            ai_image_response_format_to_string (
                ai_image_request_get_response_format (request)));
    }

    return (SoupMultipart *) g_steal_pointer (&multipart);
}

AiImageResponse *
ai_image_shared_parse_openai_response (
    JsonNode     *root,
    const gchar  *model,
    GError      **error
){
    g_autoptr(AiImageResponse) response = NULL;
    JsonObject *obj;
    JsonArray *data;
    guint length;
    guint i;
    gint64 created;

    g_return_val_if_fail (root != NULL, NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!JSON_NODE_HOLDS_OBJECT (root))
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                     "Expected a JSON object in the image response");
        return NULL;
    }

    obj = json_node_get_object (root);

    if (ai_json_get_node (obj, "error") != NULL)
    {
        JsonObject *err = ai_json_get_object (obj, "error");
        const gchar *message = ai_json_get_string (err, "message",
                                                   "Unknown error");

        g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", message);
        return NULL;
    }

    /* An array, not merely a member: a "data" holding an object passed
       the old has_member check and then criticalled on the way to a
       NULL, which the caller read as an empty image list. */
    data = ai_json_get_array (obj, "data");

    if (data == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                     "Image response has no 'data' array");
        return NULL;
    }

    created = ai_json_get_int (obj, "created",
                               g_get_real_time () / G_USEC_PER_SEC);

    response = ai_image_response_new (NULL, created);
    if (model != NULL)
    {
        ai_image_response_set_model (response, model);
    }

    length = json_array_get_length (data);

    for (i = 0; i < length; i++)
    {
        JsonObject *item = ai_json_array_get_object (data, i);
        g_autoptr(AiGeneratedImage) image = NULL;
        const gchar *b64;
        const gchar *url;

        if (item == NULL)
        {
            continue;
        }

        b64 = ai_json_get_string (item, "b64_json", NULL);
        url = ai_json_get_string (item, "url", NULL);

        /* Prefer inline bytes: they are already in hand, whereas a URL is
         * a second round trip against a link that expires. */
        if (b64 != NULL)
        {
            image = ai_generated_image_new_from_base64 (b64, "image/png");
        }
        else if (url != NULL)
        {
            image = ai_generated_image_new_from_url (url);
        }
        else
        {
            continue;
        }

        {
            const gchar *revised = ai_json_get_string (item, "revised_prompt",
                                                       NULL);

            if (revised != NULL)
            {
                ai_generated_image_set_revised_prompt (image, revised);
            }
        }

        ai_image_response_add_image (response,
                                     (AiGeneratedImage *) g_steal_pointer (&image));
    }

    return (AiImageResponse *) g_steal_pointer (&response);
}

void
ai_image_shared_build_gemini_parts (
    JsonBuilder    *builder,
    AiImageRequest *request
){
    g_autofree gchar *prompt = NULL;
    GList *iter;

    g_return_if_fail (builder != NULL);
    g_return_if_fail (request != NULL);

    json_builder_begin_array (builder);

    /* Text part first: the reference parts that follow are interpreted
     * relative to it. */
    prompt = ai_image_shared_prompt_with_roles (request);

    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "text");
    json_builder_add_string_value (builder, prompt);
    json_builder_end_object (builder);

    /*
     * One inline_data part per reference.  Note the asymmetry in Gemini's
     * wire format: requests spell this snake_case `inline_data` while
     * responses come back camelCase `inlineData`.
     */
    for (iter = ai_image_request_get_reference_images (request);
         iter != NULL;
         iter = iter->next)
    {
        AiImage *image = iter->data;
        g_autofree gchar *b64 = ai_image_dup_base64 (image);

        if (b64 == NULL)
        {
            continue;
        }

        json_builder_begin_object (builder);
        json_builder_set_member_name (builder, "inline_data");
        json_builder_begin_object (builder);

        json_builder_set_member_name (builder, "mime_type");
        json_builder_add_string_value (builder,
                                       ai_image_get_mime_type (image));

        json_builder_set_member_name (builder, "data");
        json_builder_add_string_value (builder, b64);

        json_builder_end_object (builder);
        json_builder_end_object (builder);
    }

    json_builder_end_array (builder);
}
