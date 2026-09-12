#### Model Capabilities

# Multi-Image Editing

Use up to five source images for a single image edit. You can specify images in the order they are sent in the request. By default, the output aspect ratio follows the first input image. You can override this by setting the `aspect_ratio` parameter to a specific ratio, such as `"1:1"` or `"16:9"`.

Each source image can be a public URL, a base64-encoded data URI, or a `file_id` from the [Files API](/developers/files) — and you can mix kinds within a single request. See [Imagine → Files API Integration](/developers/model-capabilities/imagine/files/inputs) for `file_id` details and examples.

## Related

* [Image Generation](/developers/model-capabilities/images/generation) — Generate images from text prompts
* [Image Editing](/developers/model-capabilities/images/editing) — Edit a source image with natural language
* [API Reference](/developers/rest-api-reference) — Full endpoint documentation
* [Imagine API Landing Page](https://x.ai/api/imagine) — Showcase of the Imagine API in action
