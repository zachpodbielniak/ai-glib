/*
 * ai-image.c - Binary image payload (input side)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <gio/gio.h>
#include <string.h>

#include "model/ai-image.h"

/*
 * Private structure for the AiImage boxed type.
 *
 * `bytes` is the single source of truth; `base64` is a lazily-populated
 * cache of the same payload, because a reference image is frequently
 * serialised into more than one request (a retry, or a multi-turn edit
 * loop) and base64-encoding a multi-megabyte PNG on every pass is pure
 * waste.  Any mutation of the payload must invalidate the cache -- in
 * practice the payload is immutable after construction, so only the
 * constructors touch it.
 */
struct _AiImage
{
    GBytes *bytes;
    gchar  *base64;
    gchar  *mime_type;
    gchar  *uri;
    gchar  *role;
    gchar  *filename;
    gint    width;
    gint    height;
};

/*
 * ai_image_get_type:
 *
 * Registers the AiImage boxed type with the GLib type system.
 */
G_DEFINE_BOXED_TYPE(AiImage, ai_image, ai_image_copy, ai_image_free)

/*
 * Normalise a caller-supplied MIME type.
 *
 * Providers reject payloads whose declared type is not an image, and a
 * NULL type would serialise as a null JSON member, so fall back to PNG --
 * every image API in scope accepts it and it is the format all three of
 * them return.
 */
static gchar *
ai_image_normalize_mime(const gchar *mime_type)
{
    if (mime_type == NULL || *mime_type == '\0')
    {
        return g_strdup("image/png");
    }

    return g_strdup(mime_type);
}

/**
 * ai_image_new_from_bytes:
 * @bytes: (transfer none): the raw image payload
 * @mime_type: (nullable): the MIME type, or %NULL to assume `image/png`
 *
 * Creates a new #AiImage that shares @bytes.
 *
 * #GBytes is reference-counted and immutable, so no copy of the payload is
 * made; the caller keeps its own reference.
 *
 * Returns: (transfer full): a new #AiImage
 */
AiImage *
ai_image_new_from_bytes(
    GBytes      *bytes,
    const gchar *mime_type
){
    AiImage *self;

    g_return_val_if_fail(bytes != NULL, NULL);

    self = g_slice_new0(AiImage);
    self->bytes = g_bytes_ref(bytes);
    self->mime_type = ai_image_normalize_mime(mime_type);

    return self;
}

/**
 * ai_image_new_from_data:
 * @data: (array length=length) (element-type guint8): the raw image payload
 * @length: length of @data in bytes
 * @mime_type: (nullable): the MIME type, or %NULL to assume `image/png`
 *
 * Creates a new #AiImage from a copy of @data.
 *
 * Convenience wrapper over ai_image_new_from_bytes() for callers holding a
 * plain buffer rather than a #GBytes.
 *
 * Returns: (transfer full): a new #AiImage
 */
AiImage *
ai_image_new_from_data(
    gconstpointer  data,
    gsize          length,
    const gchar   *mime_type
){
    g_autoptr(GBytes) bytes = NULL;

    g_return_val_if_fail(data != NULL, NULL);
    g_return_val_if_fail(length > 0, NULL);

    bytes = g_bytes_new(data, length);

    return ai_image_new_from_bytes(bytes, mime_type);
}

/*
 * Guess a MIME type for a file, given its path and leading bytes.
 *
 * g_content_type_guess() is content-sniffing and authoritative where it
 * works, but on some platforms it returns a non-MIME content type, and it
 * can land on something like application/octet-stream for a file it does
 * not recognise.  Fall back to the extension, then to PNG, so a reference
 * image is never rejected merely because the local content-type database
 * is incomplete.
 */
static gchar *
ai_image_guess_mime_for_file(
    const gchar   *path,
    gconstpointer  data,
    gsize          length
){
    g_autofree gchar *content_type = NULL;
    g_autofree gchar *mime = NULL;
    g_autofree gchar *lower = NULL;
    gboolean uncertain = FALSE;

    content_type = g_content_type_guess(path, data, length, &uncertain);
    if (content_type != NULL)
    {
        mime = g_content_type_get_mime_type(content_type);
        if (mime != NULL && g_str_has_prefix(mime, "image/"))
        {
            return g_steal_pointer(&mime);
        }
    }

    lower = g_ascii_strdown(path, -1);

    if (g_str_has_suffix(lower, ".png"))
    {
        return g_strdup("image/png");
    }
    if (g_str_has_suffix(lower, ".jpg") || g_str_has_suffix(lower, ".jpeg"))
    {
        return g_strdup("image/jpeg");
    }
    if (g_str_has_suffix(lower, ".webp"))
    {
        return g_strdup("image/webp");
    }
    if (g_str_has_suffix(lower, ".gif"))
    {
        return g_strdup("image/gif");
    }
    if (g_str_has_suffix(lower, ".bmp"))
    {
        return g_strdup("image/bmp");
    }
    if (g_str_has_suffix(lower, ".heic") || g_str_has_suffix(lower, ".heif"))
    {
        return g_strdup("image/heif");
    }

    return g_strdup("image/png");
}

/**
 * ai_image_new_from_file:
 * @path: path to an image file
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a new #AiImage by reading @path.
 *
 * The MIME type is sniffed from the file's contents, falling back to its
 * extension and finally to `image/png`.  The basename is retained as the
 * #AiImage:filename, which multipart-based providers (OpenAI's
 * `/v1/images/edits`) need for the form part.
 *
 * Returns: (transfer full) (nullable): a new #AiImage, or %NULL on error
 */
AiImage *
ai_image_new_from_file(
    const gchar  *path,
    GError      **error
){
    g_autofree gchar *contents = NULL;
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(AiImage) self = NULL;
    gsize length = 0;

    g_return_val_if_fail(path != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    if (!g_file_get_contents(path, &contents, &length, error))
    {
        return NULL;
    }

    if (length == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Image file is empty: %s", path);
        return NULL;
    }

    bytes = g_bytes_new_take(g_steal_pointer(&contents), length);

    self = g_slice_new0(AiImage);
    self->bytes = g_steal_pointer(&bytes);
    self->mime_type = ai_image_guess_mime_for_file(
        path, g_bytes_get_data(self->bytes, NULL), length);
    self->filename = g_path_get_basename(path);
    self->uri = g_filename_to_uri(path, NULL, NULL);

    return (AiImage *)g_steal_pointer(&self);
}

/**
 * ai_image_new_from_base64:
 * @base64_data: base64-encoded image payload
 * @mime_type: (nullable): the MIME type, or %NULL to assume `image/png`
 *
 * Creates a new #AiImage by decoding @base64_data.
 *
 * The encoded form is retained as the cache backing ai_image_dup_base64(),
 * so round-tripping an image straight back into a request costs nothing.
 *
 * Returns: (transfer full) (nullable): a new #AiImage, or %NULL if
 *   @base64_data is not valid base64
 */
AiImage *
ai_image_new_from_base64(
    const gchar *base64_data,
    const gchar *mime_type
){
    AiImage *self;
    guchar *decoded;
    gsize length = 0;

    g_return_val_if_fail(base64_data != NULL, NULL);

    decoded = g_base64_decode(base64_data, &length);
    if (decoded == NULL || length == 0)
    {
        g_free(decoded);
        return NULL;
    }

    self = g_slice_new0(AiImage);
    self->bytes = g_bytes_new_take(decoded, length);
    self->base64 = g_strdup(base64_data);
    self->mime_type = ai_image_normalize_mime(mime_type);

    return self;
}

/**
 * ai_image_copy:
 * @self: (nullable): an #AiImage
 *
 * Creates a copy of @self.
 *
 * The payload is shared rather than duplicated -- #GBytes is immutable and
 * reference-counted -- so copying a reference image is cheap regardless of
 * its size.
 *
 * Returns: (transfer full) (nullable): a copy of @self, or %NULL if @self
 *   is %NULL
 */
AiImage *
ai_image_copy(const AiImage *self)
{
    AiImage *copy;

    if (self == NULL)
    {
        return NULL;
    }

    copy = g_slice_new0(AiImage);
    copy->bytes = self->bytes != NULL ? g_bytes_ref(self->bytes) : NULL;
    copy->base64 = g_strdup(self->base64);
    copy->mime_type = g_strdup(self->mime_type);
    copy->uri = g_strdup(self->uri);
    copy->role = g_strdup(self->role);
    copy->filename = g_strdup(self->filename);
    copy->width = self->width;
    copy->height = self->height;

    return copy;
}

/**
 * ai_image_free:
 * @self: (nullable): an #AiImage
 *
 * Frees @self and everything it owns.  Does nothing if @self is %NULL.
 */
void
ai_image_free(AiImage *self)
{
    if (self == NULL)
    {
        return;
    }

    g_clear_pointer(&self->bytes, g_bytes_unref);
    g_clear_pointer(&self->base64, g_free);
    g_clear_pointer(&self->mime_type, g_free);
    g_clear_pointer(&self->uri, g_free);
    g_clear_pointer(&self->role, g_free);
    g_clear_pointer(&self->filename, g_free);

    g_slice_free(AiImage, self);
}

/**
 * ai_image_get_bytes:
 * @self: an #AiImage
 *
 * Gets the raw image payload.
 *
 * Returns: (transfer none) (nullable): the payload
 */
GBytes *
ai_image_get_bytes(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->bytes;
}

/**
 * ai_image_get_size:
 * @self: an #AiImage
 *
 * Gets the size of the payload in bytes.
 *
 * Useful for pre-flighting a request against a provider's per-image or
 * total-payload limit before spending a round trip on it.
 *
 * Returns: the payload size in bytes, or 0 if there is no payload
 */
gsize
ai_image_get_size(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, 0);

    if (self->bytes == NULL)
    {
        return 0;
    }

    return g_bytes_get_size(self->bytes);
}

/**
 * ai_image_get_mime_type:
 * @self: an #AiImage
 *
 * Gets the MIME type of the payload.
 *
 * Returns: (transfer none): the MIME type
 */
const gchar *
ai_image_get_mime_type(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->mime_type;
}

/**
 * ai_image_set_mime_type:
 * @self: an #AiImage
 * @mime_type: (nullable): the MIME type, or %NULL to reset to `image/png`
 *
 * Overrides the MIME type of the payload.
 */
void
ai_image_set_mime_type(
    AiImage     *self,
    const gchar *mime_type
){
    g_return_if_fail(self != NULL);

    g_free(self->mime_type);
    self->mime_type = ai_image_normalize_mime(mime_type);
}

/**
 * ai_image_dup_base64:
 * @self: an #AiImage
 *
 * Gets the payload base64-encoded, as the JSON-based provider wire formats
 * require.
 *
 * The encoding is computed on first use and cached, so repeatedly
 * serialising the same reference image -- across a retry, or across the
 * turns of an edit loop -- costs one encode, not one per request.
 *
 * Returns: (transfer full) (nullable): the base64 form, or %NULL if there
 *   is no payload
 */
gchar *
ai_image_dup_base64(AiImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    if (self->bytes == NULL)
    {
        return NULL;
    }

    if (self->base64 == NULL)
    {
        gconstpointer data;
        gsize length = 0;

        data = g_bytes_get_data(self->bytes, &length);
        self->base64 = g_base64_encode((const guchar *)data, length);
    }

    return g_strdup(self->base64);
}

/**
 * ai_image_get_width:
 * @self: an #AiImage
 *
 * Gets the pixel width, if known.
 *
 * Returns: the width in pixels, or 0 when unknown
 */
gint
ai_image_get_width(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->width;
}

/**
 * ai_image_get_height:
 * @self: an #AiImage
 *
 * Gets the pixel height, if known.
 *
 * Returns: the height in pixels, or 0 when unknown
 */
gint
ai_image_get_height(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->height;
}

/**
 * ai_image_set_dimensions:
 * @self: an #AiImage
 * @width: the width in pixels, or 0 for unknown
 * @height: the height in pixels, or 0 for unknown
 *
 * Records the pixel dimensions of the payload.
 *
 * ai-glib never decodes image data, so dimensions are only ever populated
 * by a caller that already knows them.
 */
void
ai_image_set_dimensions(
    AiImage *self,
    gint     width,
    gint     height
){
    g_return_if_fail(self != NULL);

    self->width = width;
    self->height = height;
}

/**
 * ai_image_get_uri:
 * @self: an #AiImage
 *
 * Gets the source URI, if the image came from one.
 *
 * This is provenance for diagnostics only; it is never sent to a provider.
 *
 * Returns: (transfer none) (nullable): the source URI, or %NULL
 */
const gchar *
ai_image_get_uri(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->uri;
}

/**
 * ai_image_set_uri:
 * @self: an #AiImage
 * @uri: (nullable): the source URI, or %NULL to clear
 *
 * Records where the image came from.
 */
void
ai_image_set_uri(
    AiImage     *self,
    const gchar *uri
){
    g_return_if_fail(self != NULL);

    g_free(self->uri);
    self->uri = g_strdup(uri);
}

/**
 * ai_image_get_role:
 * @self: an #AiImage
 *
 * Gets the role label, if any.
 *
 * Returns: (transfer none) (nullable): the role, or %NULL
 */
const gchar *
ai_image_get_role(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->role;
}

/**
 * ai_image_set_role:
 * @self: an #AiImage
 * @role: (nullable): a short label such as `"style"` or `"subject"`, or
 *   %NULL to clear
 *
 * Labels what this image is for, when it is used as a reference.
 *
 * Providers that accept several reference images have no positional
 * convention for distinguishing them, so ai-glib folds the role labels
 * into the prompt text -- the only channel the model actually reads.
 * Unlabelled references are simply passed through in order.
 */
void
ai_image_set_role(
    AiImage     *self,
    const gchar *role
){
    g_return_if_fail(self != NULL);

    g_free(self->role);
    self->role = g_strdup(role);
}

/**
 * ai_image_get_filename:
 * @self: an #AiImage
 *
 * Gets the filename used for multipart form parts.
 *
 * Returns: (transfer none) (nullable): the filename, or %NULL
 */
const gchar *
ai_image_get_filename(const AiImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->filename;
}

/**
 * ai_image_set_filename:
 * @self: an #AiImage
 * @filename: (nullable): the filename, or %NULL to clear
 *
 * Sets the filename advertised in a multipart form part.
 *
 * OpenAI's `/v1/images/edits` infers the image format from the part's
 * filename, so an image built from bytes rather than from a file needs one
 * set explicitly if it is not a PNG.
 */
void
ai_image_set_filename(
    AiImage     *self,
    const gchar *filename
){
    g_return_if_fail(self != NULL);

    g_free(self->filename);
    self->filename = g_strdup(filename);
}

/**
 * ai_image_save_to_file:
 * @self: an #AiImage
 * @path: destination path
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the payload to @path, replacing any existing file.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_image_save_to_file(
    AiImage      *self,
    const gchar  *path,
    GError      **error
){
    g_autoptr(GFile) file = NULL;
    g_autoptr(GFileOutputStream) stream = NULL;
    gconstpointer data;
    gsize length = 0;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (self->bytes == NULL)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "AiImage has no payload to save");
        return FALSE;
    }

    file = g_file_new_for_path(path);
    stream = g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
    if (stream == NULL)
    {
        return FALSE;
    }

    data = g_bytes_get_data(self->bytes, &length);

    if (!g_output_stream_write_all(G_OUTPUT_STREAM(stream), data, length,
                                   NULL, NULL, error))
    {
        return FALSE;
    }

    return g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, error);
}
