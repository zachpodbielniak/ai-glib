#### Model Capabilities

# Video Generation

Generate videos from text prompts with Grok video models. The API supports configurable duration, aspect ratio, and resolution, and the SDK handles asynchronous polling automatically. On `grok-imagine-video-1.5`, text-to-video supports native 1080p.

> [!NOTE]
>
> On , text-to-video uses text-to-image then image-to-video under the hood: the model generates a first frame from your prompt, then animates it. You still make a single text-to-video request; the intermediate image is not returned.

## Quick Start

Generate a video with a single API call:

```python customLanguage="pythonXAI"
import os
import xai_sdk

client = xai_sdk.Client(api_key=os.getenv("XAI_API_KEY"))

response = client.video.generate(
    prompt="A glowing crystal-powered rocket launching from the red dunes of Mars, ancient alien ruins lighting up in the background as it soars into a sky full of unfamiliar constellations",
    model="grok-imagine-video-1.5",
    duration=10,
    aspect_ratio="16:9",
    resolution="720p",
)

print(response.url)
```

```python customLanguage="pythonRequests"
import os
import time
import requests

headers = {
    "Content-Type": "application/json",
    "Authorization": f"Bearer {os.environ['XAI_API_KEY']}",
}

response = requests.post(
    "https://api.x.ai/v1/videos/generations",
    headers=headers,
    json={
        "model": "grok-imagine-video-1.5",
        "prompt": "A glowing crystal-powered rocket launching from the red dunes of Mars, ancient alien ruins lighting up in the background as it soars into a sky full of unfamiliar constellations",
        "duration": 10,
        "aspect_ratio": "16:9",
        "resolution": "720p",
    },
)

request_id = response.json()["request_id"]

# Poll until the video is ready
while True:
    result = requests.get(
        f"https://api.x.ai/v1/videos/{request_id}",
        headers={"Authorization": headers["Authorization"]},
    )
    data = result.json()
    if data["status"] == "done":
        print(data["video"]["url"])
        break
    elif data["status"] == "expired":
        print("Request expired")
        break
    time.sleep(5)
```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { experimental_generateVideo as generateVideo } from "ai";

const result = await generateVideo({
    model: xai.video("grok-imagine-video-1.5"),
    prompt: "A glowing crystal-powered rocket launching from the red dunes of Mars, ancient alien ruins lighting up in the background as it soars into a sky full of unfamiliar constellations",
    duration: 10,
    aspectRatio: "16:9",
    providerOptions: {
        xai: { resolution: "720p" },
    },
});

const videoUrl = result.providerMetadata?.xai?.videoUrl;
console.log(videoUrl);
```

```bash
# Start the video generation request
REQUEST_ID=$(curl -s -X POST https://api.x.ai/v1/videos/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-video-1.5",
    "prompt": "A glowing crystal-powered rocket launching from the red dunes of Mars, ancient alien ruins lighting up in the background as it soars into a sky full of unfamiliar constellations",
    "duration": 10,
    "aspect_ratio": "16:9",
    "resolution": "720p"
  }' | jq -r '.request_id')

# Poll until the video is ready
while true; do
  RESULT=$(curl -s https://api.x.ai/v1/videos/$REQUEST_ID \
    -H "Authorization: Bearer $XAI_API_KEY")
  STATUS=$(echo "$RESULT" | jq -r '.status')
  if [ "$STATUS" = "done" ]; then
    echo "$RESULT" | jq -r '.video.url'
    break
  elif [ "$STATUS" = "failed" ] || [ "$STATUS" = "expired" ]; then
    echo "Request $STATUS"; echo "$RESULT" | jq .
    break
  fi
  sleep 5
done
```

Video generation is an **asynchronous process** that typically takes up to several minutes to complete. The exact time varies based on:

* **Prompt complexity** — More detailed scenes require additional processing
* **Duration** — Longer videos take more time to generate
* **Resolution** — Higher resolutions (1080p vs 480p) increase processing time
* **Video editing** — Editing existing videos adds overhead compared to image-to-video or text-to-video

## Video workflows

Use the page that matches the kind of video output you want to create:

* [Image-to-Video](/developers/model-capabilities/video/image-to-video) — Animate a still image.
* [Video Editing](/developers/model-capabilities/video/editing) — Modify an existing video.
* [Reference-to-Video](/developers/model-capabilities/video/reference-to-video) — Guide a generated video with reference images, and on `grok-imagine-video-1.5` pin exact first or last frames.
* [Video Extension](/developers/model-capabilities/video/extension) — Continue an existing video from its last frame.

## How it works

Under the hood, video generation is a two-step process:

1. **Start** — Submit a generation request and receive a `request_id`
2. **Poll** — Repeatedly check the status using the `request_id` until the video is ready

The xAI SDK's `generate()` and `extend()` methods abstract this entirely; they submit your request, poll for the result, and return the completed video response. You don't need to manage request IDs or implement polling logic. For long-running generations, you can [customize the polling behavior](#customize-polling-behavior) with timeout and interval parameters, or [handle polling manually](#handle-polling-manually) for full control over the generation lifecycle.

**REST API users** must implement this two-step flow manually:

**Step 1: Start the generation request**

```bash
curl -X POST https://api.x.ai/v1/videos/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-video-1.5",
    "prompt": "A glowing crystal-powered rocket launching from Mars"
  }'
```

Response:

```json
{"request_id": "d97415a1-5796-b7ec-379f-4e6819e08fdf"}
```

**Step 2: Poll for the result**

Use the `request_id` to check the status. Keep polling every few seconds until the video is ready:

```bash
curl -X GET "https://api.x.ai/v1/videos/{request_id}" \
  -H "Authorization: Bearer $XAI_API_KEY"
```

The response includes a `status` field with one of these values:

| Status | Description |
|--------|-------------|
| `pending` | Video is still being generated |
| `done` | Video is ready |
| `expired` | Request has expired |
| `failed` | Video generation failed |

Response (when complete):

```json
{
  "status": "done",
  "video": {
    "url": "https://vidgen.x.ai/.../video.mp4",
    "duration": 8,
    "respect_moderation": true
  },
  "model": "grok-imagine-video-1.5"
}
```

Videos are returned as temporary URLs. Access the xAI-hosted URL directly when you need it, or download/process it promptly if you need to keep a copy.

## Configuration

The video generation API lets you control the output format of your generated videos. You can specify the duration, aspect ratio, resolution, and (on reference-to-video) a preset voice to match your specific use case.

### Duration

Control video length with the `duration` parameter. The allowed range is 1–15 seconds.

Video editing does not support custom `duration`. The edited video retains the duration of the original, which is capped at 8.7 seconds.

### Aspect Ratio

| Ratio | Use case |
|-------|----------|
| `1:1` | Social media, thumbnails |
| `16:9` / `9:16` | Widescreen, mobile, stories (default: `16:9`) |
| `4:3` / `3:4` | Presentations, portraits |
| `3:2` / `2:3` | Photography |

For image-to-video generation, the output defaults to the input image's aspect ratio. If you specify the `aspect_ratio` parameter, it will override this and stretch the image to the desired aspect ratio.

Video editing does not support custom `aspect_ratio` — the output matches the input video's aspect ratio.

### Resolution

| Resolution | Description |
|------------|-------------|
| `1080p` | Full HD quality |
| `720p` | HD quality |
| `480p` | Standard definition, faster processing (default) |

**Note:** `1080p` is supported on `grok-imagine-video-1.5` for text-to-video and image-to-video. Reference-to-video is capped at 720p.

Video editing does not support custom `resolution`. The output resolution matches the input video's resolution, capped at 720p (e.g., a 1080p input will be downsized to 720p).

### Audio

> [!WARNING]
>
> Preset voices are generally available. Voice references with your own audio files are available to trusted partners, on request. .

On `grok-imagine-video-1.5`, [reference-to-video](/developers/model-capabilities/video/reference-to-video#reference-audio) can carry a voice via `reference_audios`. Voices come from the built-in roster and are named by `voice_id`; voice references with your own audio files are available to trusted partners [on request](https://x.ai/contact-sales?interest=imagine):

| Property | Description |
|----------|-------------|
| Source | A preset `voice_id` (e.g. `{"voice_id": "eve"}`), from the same roster as [Text to Speech](/developers/model-capabilities/audio/text-to-speech#voices). Identifiers are case-insensitive |
| Limit | Max **3** voices per request |
| Prompt | Reference voices by index: `<AUDIO_0>`, `<AUDIO_1>`, `<AUDIO_2>` |

Generated videos include an audio track by default. Pass `generate_audio=False` to request a silent video:

```python customLanguage="pythonXAI"
import os
import xai_sdk

client = xai_sdk.Client(api_key=os.getenv("XAI_API_KEY"))

response = client.video.generate(
    prompt="A paper boat drifting down a rain-soaked street",
    model="grok-imagine-video-1.5",
    generate_audio=False,
)

print(response.url)
```

```bash
curl -X POST https://api.x.ai/v1/videos/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-video-1.5",
    "prompt": "A paper boat drifting down a rain-soaked street",
    "generate_audio": false
  }'
```

### Example

```python customLanguage="pythonXAI"
import os
import xai_sdk

client = xai_sdk.Client(api_key=os.getenv("XAI_API_KEY"))

response = client.video.generate(
    prompt="Timelapse of a flower blooming in a sunlit garden",
    model="grok-imagine-video-1.5",
    duration=10,
    aspect_ratio="16:9",
    resolution="720p",
)

print(f"Video URL: {response.url}")
print(f"Duration: {response.duration}s")
```

```python customLanguage="pythonRequests"
import os
import time
import requests

headers = {
    "Content-Type": "application/json",
    "Authorization": f"Bearer {os.environ['XAI_API_KEY']}",
}

response = requests.post(
    "https://api.x.ai/v1/videos/generations",
    headers=headers,
    json={
        "model": "grok-imagine-video-1.5",
        "prompt": "Timelapse of a flower blooming in a sunlit garden",
        "duration": 10,
        "aspect_ratio": "16:9",
        "resolution": "720p",
    },
)

request_id = response.json()["request_id"]

while True:
    result = requests.get(
        f"https://api.x.ai/v1/videos/{request_id}",
        headers={"Authorization": headers["Authorization"]},
    )
    data = result.json()
    if data["status"] == "done":
        print(f"Video URL: {data['video']['url']}")
        print(f"Duration: {data['video']['duration']}s")
        break
    elif data["status"] == "expired":
        print("Request expired")
        break
    time.sleep(5)
```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { experimental_generateVideo as generateVideo } from "ai";

const result = await generateVideo({
    model: xai.video("grok-imagine-video-1.5"),
    prompt: "Timelapse of a flower blooming in a sunlit garden",
    duration: 10,
    aspectRatio: "16:9",
    providerOptions: {
        xai: { resolution: "720p" },
    },
});

const videoUrl = result.providerMetadata?.xai?.videoUrl;
console.log(videoUrl);
```

```bash
# Start the video generation request
REQUEST_ID=$(curl -s -X POST https://api.x.ai/v1/videos/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-video-1.5",
    "prompt": "Timelapse of a flower blooming in a sunlit garden",
    "duration": 10,
    "aspect_ratio": "16:9",
    "resolution": "720p"
  }' | jq -r '.request_id')

# Poll until the video is ready
while true; do
  RESULT=$(curl -s https://api.x.ai/v1/videos/$REQUEST_ID \
    -H "Authorization: Bearer $XAI_API_KEY")
  STATUS=$(echo "$RESULT" | jq -r '.status')
  if [ "$STATUS" = "done" ]; then
    echo "$RESULT" | jq -r '.video.url'
    break
  elif [ "$STATUS" = "failed" ] || [ "$STATUS" = "expired" ]; then
    echo "Request $STATUS"; echo "$RESULT" | jq .
    break
  fi
  sleep 5
done
```

### Request Modes

The video generation endpoint supports multiple modes, determined by which fields are set. Edit and extend use dedicated endpoints; generation modes share `/v1/videos/generations`:

| Mode | REST API fields | AI SDK shape | Description |
|------|-----------------|--------------|-------------|
| Text-to-video | `prompt` only | `prompt: "..."` | Generates video from a text prompt alone. |
| Image-to-video | `prompt` + `image` | `prompt: { image, text }` | Generates video with the provided image as the starting frame. |
| Reference-to-video | `prompt` + `reference_images` or `reference_audios` | `prompt: "..."` + `providerOptions.xai.{ mode: "reference-to-video", referenceImageUrls }` | Generates video guided by reference images and/or a preset voice on `grok-imagine-video-1.5`. |
| First & Last frame | `last_frame`, optionally with `image` and/or `prompt` | REST body `last_frame` (no dedicated AI SDK field) | On `grok-imagine-video-1.5`, pins the exact last frame. Add `image` to also pin the first frame and interpolate between the two. `prompt` is optional whenever a frame is pinned. Can be combined with `reference_images` / `reference_audios`. |
| Edit-video | `/v1/videos/edits` + `video` | `prompt: "..."` + `providerOptions.xai.{ mode: "edit-video", videoUrl }` | Modifies an existing video based on the prompt. |
| Extend-video | `/v1/videos/extensions` + `video` | `prompt: "..."` + `providerOptions.xai.{ mode: "extend-video", videoUrl }` | Extends an existing video from its last frame. |

On `grok-imagine-video-1.5`, `image` combined with `reference_images`, `reference_audios`, or `last_frame` is reference-to-video with a pinned first frame. The clip starts on that image rather than treating it as a style reference. `last_frame` on its own (no `image`, no references) is valid: the model generates the opening and lands on the pinned frame. `prompt` is optional for any request that includes `image`, `reference_images`, or `last_frame`; it is required only for text-to-video. Classic `grok-imagine-video` still rejects `last_frame` and rejects combining `image` with reference inputs.

Do not mix AI SDK `mode` values. Each request supports exactly one of `"edit-video"`, `"extend-video"`, or `"reference-to-video"`. When you omit `mode`, the AI SDK uses standard generation.

See [First & Last frame](/developers/model-capabilities/video/reference-to-video#first--last-frame) for `last_frame` examples.

## Customize Polling Behavior

When using the SDK's `generate()` or `extend()` methods, you can control how long to wait and how frequently to check for results:

| Python SDK | AI SDK (`providerOptions.xai`) | Description | Default |
|-----------|-------------|-------------|---------|
| `timeout` | `pollTimeoutMs` | Maximum time to wait for the video to complete | 10 minutes |
| `interval` | `pollIntervalMs` | Time between status checks | 100 milliseconds |

```python customLanguage="pythonXAI"
import os
from datetime import timedelta
import xai_sdk

client = xai_sdk.Client(api_key=os.getenv("XAI_API_KEY"))

response = client.video.generate(
    prompt="Epic cinematic drone shot flying through mountain peaks",
    model="grok-imagine-video-1.5",
    duration=15,
    timeout=timedelta(minutes=15),  # Wait up to 15 minutes
    interval=timedelta(seconds=5),  # Check every 5 seconds
)

print(response.url)
```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { experimental_generateVideo as generateVideo } from "ai";

const result = await generateVideo({
    model: xai.video("grok-imagine-video-1.5"),
    prompt: "Epic cinematic drone shot flying through mountain peaks",
    duration: 15,
    providerOptions: {
        xai: {
            pollTimeoutMs: 15 * 60 * 1000,  // Wait up to 15 minutes
            pollIntervalMs: 5 * 1000,        // Check every 5 seconds
        },
    },
});

const videoUrl = result.providerMetadata?.xai?.videoUrl;
console.log(videoUrl);
```

If the video isn't ready within the timeout period, the Python SDK raises a `TimeoutError` and the AI SDK aborts via its `AbortSignal`. For even finer control, use the [manual polling approach](#handle-polling-manually); the Python SDK provides `start()` and `get()` methods, while the AI SDK supports a custom `abortSignal` for cancellation.

## Handle Polling Manually

For fine-grained control over the generation lifecycle, use `start()` or `extend_start()` to initiate generation/extension requests respectively and `get()` to check status.

The `get()` method returns a response with a `status` field. Import the status enum from the SDK:

```python customLanguage="pythonXAI"
import os
import time
import xai_sdk
from xai_sdk.proto import deferred_pb2

client = xai_sdk.Client(api_key=os.getenv("XAI_API_KEY"))

# Start the generation request
start_response = client.video.start(
    prompt="A cat lounging in a sunbeam, tail gently swishing",
    model="grok-imagine-video-1.5",
    duration=5,
)

print(f"Request ID: {start_response.request_id}")

# Poll for results
while True:
    result = client.video.get(start_response.request_id)
    
    if result.status == deferred_pb2.DeferredStatus.DONE:
        print(f"Video URL: {result.response.video.url}")
        break
    elif result.status == deferred_pb2.DeferredStatus.EXPIRED:
        print("Request expired")
        break
    elif result.status == deferred_pb2.DeferredStatus.FAILED:
        print("Video generation failed")
        break
    elif result.status == deferred_pb2.DeferredStatus.PENDING:
        print("Still processing...")
        time.sleep(5)
```

```python customLanguage="pythonRequests"
import os
import time
import requests

headers = {
    "Content-Type": "application/json",
    "Authorization": f"Bearer {os.environ['XAI_API_KEY']}",
}

# Step 1: Start generation
response = requests.post(
    "https://api.x.ai/v1/videos/generations",
    headers=headers,
    json={
        "model": "grok-imagine-video-1.5",
        "prompt": "A cat lounging in a sunbeam, tail gently swishing",
        "duration": 5,
    },
)

request_id = response.json()["request_id"]
print(f"Request ID: {request_id}")

# Step 2: Poll for results
while True:
    result = requests.get(
        f"https://api.x.ai/v1/videos/{request_id}",
        headers={"Authorization": headers["Authorization"]},
    )
    data = result.json()

    if data["status"] == "done":
        print(f"Video URL: {data['video']['url']}")
        break
    elif data["status"] == "expired":
        print("Request expired")
        break
    elif data["status"] == "failed":
        print("Video generation failed")
        break
    else:
        print("Still processing...")
        time.sleep(5)
```

```javascript customLanguage="javascriptWithoutSDK"
// Step 1: Start generation
const response = await fetch("https://api.x.ai/v1/videos/generations", {
    method: "POST",
    headers: {
        "Content-Type": "application/json",
        "Authorization": `Bearer ${process.env.XAI_API_KEY}`,
    },
    body: JSON.stringify({
        model: "grok-imagine-video-1.5",
        prompt: "A cat lounging in a sunbeam, tail gently swishing",
        duration: 5,
    }),
});

const { request_id } = await response.json();
console.log(`Request ID: ${request_id}`);

// Step 2: Poll for results
while (true) {
    const result = await fetch(`https://api.x.ai/v1/videos/${request_id}`, {
        headers: { "Authorization": `Bearer ${process.env.XAI_API_KEY}` },
    });

    const data = await result.json();

    if (data.status === "done") {
        console.log(`Video URL: ${data.video.url}`);
        break;
    } else if (data.status === "expired") {
        console.log("Request expired");
        break;
    } else if (data.status === "failed") {
        console.log("Video generation failed");
        break;
    } else {
        console.log("Still processing...");
        await new Promise(resolve => setTimeout(resolve, 5000));
    }
}
```

```bash
# Step 1: Start generation
REQUEST_ID=$(curl -s -X POST https://api.x.ai/v1/videos/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-video-1.5",
    "prompt": "A cat lounging in a sunbeam, tail gently swishing",
    "duration": 5
  }' | jq -r '.request_id')

echo "Request ID: $REQUEST_ID"

# Step 2: Poll for results
while true; do
  RESULT=$(curl -s https://api.x.ai/v1/videos/$REQUEST_ID \
    -H "Authorization: Bearer $XAI_API_KEY")
  STATUS=$(echo "$RESULT" | jq -r '.status')
  if [ "$STATUS" = "done" ]; then
    echo "$RESULT" | jq -r '.video.url'
    break
  elif [ "$STATUS" = "failed" ] || [ "$STATUS" = "expired" ]; then
    echo "Request $STATUS"; echo "$RESULT" | jq .
    break
  fi
  echo "Still processing..."
  sleep 5
done
```

The available status values are:

| Proto Value | Description |
|-------------|-------------|
| `deferred_pb2.DeferredStatus.PENDING` | Video is still being generated |
| `deferred_pb2.DeferredStatus.DONE` | Video is ready |
| `deferred_pb2.DeferredStatus.EXPIRED` | Request has expired |
| `deferred_pb2.DeferredStatus.FAILED` | Video generation failed |

## Error Handling

When using the SDK's `generate()` or `extend()` methods, video generation failures are raised as a `VideoGenerationError` exception. This exception includes a `code` and `message` describing what went wrong. Import it from `xai_sdk.video`:

```python customLanguage="pythonXAI"
import os
import xai_sdk
from xai_sdk.video import VideoGenerationError

client = xai_sdk.Client(api_key=os.getenv("XAI_API_KEY"))

try:
    response = client.video.generate(
        prompt="A cat lounging in a sunbeam, tail gently swishing",
        model="grok-imagine-video-1.5",
        duration=5,
    )
    print(response.url)
except VideoGenerationError as e:
    print(f"Error code: {e.code}")
    print(f"Error message: {e.message}")
```

The `VideoGenerationError` exception has the following attributes:

| Attribute | Type | Description |
|-----------|------|-------------|
| `code` | `str` | An error code identifying the failure reason |
| `message` | `str` | A human-readable message describing the failure |

When polling manually, a failed generation returns `status: "failed"` with an `error` object:

```json
{
  "status": "failed",
  "error": {
    "code": "invalid_argument",
    "message": "Prompt cannot be empty. Please provide a prompt."
  }
}
```

The possible `error.code` values are:

| Code | Meaning | What to do |
|------|---------|------------|
| `invalid_argument` | The request input is invalid, such as an unsupported duration, an invalid image or video input, a prompt that is too long, conflicting request modes, or content blocked by moderation. | Fix the request parameters or input media, then submit a new request. |
| `permission_denied` | The API key or team does not have permission for the requested video operation. | Confirm the API key belongs to the right team and that the team has access to the requested capability. |
| `failed_precondition` | The requested operation is not available for the selected model or settings, such as video editing, video extension, or a requested resolution that the model cannot process. | Change the model, mode, resolution, or other request settings. |
| `service_unavailable` | Video generation is temporarily overloaded. | Retry the request later. |
| `internal_error` | The service could not complete the generation because of an internal failure. | Retry the request. If the error persists, contact xAI support with the `request_id`. |

Authentication errors, missing models, and rate limits are returned synchronously as standard API errors before a video job is created, so they do not appear in the `error.code` field of a failed video result.

You can combine this with `TimeoutError` handling for comprehensive error coverage:

```python customLanguage="pythonXAI"
import os
import xai_sdk
from xai_sdk.video import VideoGenerationError

client = xai_sdk.Client(api_key=os.getenv("XAI_API_KEY"))

try:
    response = client.video.generate(
        prompt="A cat lounging in a sunbeam, tail gently swishing",
        model="grok-imagine-video-1.5",
        duration=5,
    )
    print(response.url)
except VideoGenerationError as e:
    print(f"Generation failed [{e.code}]: {e.message}")
except TimeoutError:
    print("Generation timed out — try increasing the timeout or simplifying the prompt")
```

## Response Details

The SDK response includes the generated video and provider-specific metadata. In the AI SDK, the xAI-hosted output URL is available at `providerMetadata.xai.videoUrl`.

```python customLanguage="pythonXAI"
if response.respect_moderation:
    print(response.url)
else:
    print("Video filtered by moderation")

print(f"Duration: {response.duration} seconds")
print(f"Model: {response.model}")
```

```javascript customLanguage="javascriptAISDK"
const result = await generateVideo({
    model: xai.video("grok-imagine-video-1.5"),
    prompt: "A futuristic city skyline at dusk",
    duration: 5,
});

console.log(result.providerMetadata?.xai?.videoUrl);
```

## Concurrent Requests

When you need to generate multiple videos, run requests concurrently. This is especially useful for comparing prompts or creating multiple variations.

```python customLanguage="pythonXAI"
import os
import asyncio
import xai_sdk

async def generate_concurrently():
    client = xai_sdk.AsyncClient(api_key=os.getenv("XAI_API_KEY"))

    prompts = [
        "A cat sitting on a sunlit windowsill, tail gently swishing.",
        "A dog sprinting through a field of tall grass at golden hour.",
        "A hummingbird hovering near a red flower in slow motion.",
    ]

    tasks = [
        client.video.generate(
            prompt=prompt,
            model="grok-imagine-video-1.5",
            duration=5,
        )
        for prompt in prompts
    ]

    results = await asyncio.gather(*tasks)

    for prompt, result in zip(prompts, results):
        print(f"{prompt}: {result.url}")

asyncio.run(generate_concurrently())
```

## Related

* [Models](/developers/models) — Available video models and pricing
* [Image-to-Video](/developers/model-capabilities/video/image-to-video) — Animate a still image
* [Reference-to-Video](/developers/model-capabilities/video/reference-to-video) — Guide a video with reference images, or pin the first & last frame
* [Video Editing](/developers/model-capabilities/video/editing) — Edit existing videos
* [Video Extension](/developers/model-capabilities/video/extension) — Extend existing videos
* [Image Generation](/developers/model-capabilities/images/generation) — Generate still images from text
* [API Reference](/developers/rest-api-reference) — Full endpoint documentation
* [Imagine API Landing Page](https://x.ai/api/imagine) — Showcase of the Imagine API in action
