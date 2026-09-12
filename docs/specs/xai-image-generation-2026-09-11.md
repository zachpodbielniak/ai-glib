#### Model Capabilities

# Image Generation

Generate images from text prompts with Grok Imagine models. The API supports batch generation of multiple images, and control over aspect ratio, resolution, and quality.

## Quick Start

Generate an image with a single API call:

```python customLanguage="pythonXAI"
import xai_sdk

client = xai_sdk.Client()

response = client.image.sample(
    prompt="A collage of London landmarks in a stenciled street‑art style",
    model="grok-imagine-image-2.0",
)

print(response.url)
```

```bash
curl -X POST https://api.x.ai/v1/images/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-image-2.0",
    "prompt": "A collage of London landmarks in a stenciled street‑art style"
  }'
```

```python customLanguage="pythonOpenAISDK"
from openai import OpenAI

client = OpenAI(
    base_url="https://api.x.ai/v1",
    api_key="YOUR_API_KEY",
)

response = client.images.generate(
    model="grok-imagine-image-2.0",
    prompt="A collage of London landmarks in a stenciled street‑art style",
)

print(response.data[0].url)
```

```javascript customLanguage="javascriptOpenAISDK"
import OpenAI from "openai";

const client = new OpenAI({
    apiKey: process.env.XAI_API_KEY,
    baseURL: 'https://api.x.ai/v1',
});

const response = await client.images.generate({
    model: "grok-imagine-image-2.0",
    prompt: "A collage of London landmarks in a stenciled street‑art style",
});

console.log(response.data[0].url);
```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { generateImage } from "ai";

const { image } = await generateImage({
    model: xai.image("grok-imagine-image-2.0"),
    prompt: "A collage of London landmarks in a stenciled street‑art style",
});

console.log(image.base64);
```

Images are returned as URLs by default. URLs are temporary, so download or process promptly. You can also request [base64 output](#base64-output) for embedding images directly.

## Configuration

### Multiple Images

Generate multiple images in a single request with the `n` parameter (`1`–`10`). On the REST API and OpenAI-compatible SDKs, `n` is optional and defaults to `1`. The xAI Python SDK uses `sample()` for a single image and `sample_batch(n=...)` for more than one — `n` is required on `sample_batch()`.

```python customLanguage="pythonXAI"
import xai_sdk

client = xai_sdk.Client()

responses = client.image.sample_batch(
    prompt="A futuristic city skyline at night",
    model="grok-imagine-image-2.0",
    n=4,
)

for i, image in enumerate(responses):
    print(f"Variation {i + 1}: {image.url}")
```

```python customLanguage="pythonOpenAISDK"
from openai import OpenAI

client = OpenAI(
    base_url="https://api.x.ai/v1",
    api_key="YOUR_API_KEY",
)

response = client.images.generate(
    model="grok-imagine-image-2.0",
    prompt="A futuristic city skyline at night",
    n=4,
)

for i, image in enumerate(response.data):
    print(f"Variation {i + 1}: {image.url}")
```

```javascript customLanguage="javascriptOpenAISDK"
import OpenAI from "openai";

const client = new OpenAI({
    apiKey: process.env.XAI_API_KEY,
    baseURL: "https://api.x.ai/v1",
});

const response = await client.images.generate({
    model: "grok-imagine-image-2.0",
    prompt: "A futuristic city skyline at night",
    n: 4,
});

response.data.forEach((image, i) => {
    console.log(`Variation ${i + 1}: ${image.url}`);
});

```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { generateImage } from "ai";

const { images } = await generateImage({
    model: xai.image("grok-imagine-image-2.0"),
    prompt: "A futuristic city skyline at night",
    n: 4,
});

images.forEach((image, i) => {
    console.log(`Variation ${i + 1}: ${image.base64.slice(0, 50)}...`);
});

```

```bash
curl -X POST https://api.x.ai/v1/images/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-image-2.0",
    "prompt": "A futuristic city skyline at night",
    "n": 4
  }'
```

### Aspect Ratio

Control image dimensions with the `aspect_ratio` parameter. When omitted, the default is `auto`, which lets the model pick the best ratio for the prompt.

| Ratio | Use case |
|-------|----------|
| `1:1` | Social media, thumbnails |
| `16:9` / `9:16` | Widescreen, mobile, stories |
| `4:3` / `3:4` | Presentations, portraits |
| `3:2` / `2:3` | Photography |
| `2:1` / `1:2` | Banners, headers |
| `19.5:9` / `9:19.5` | Modern smartphone displays (iPhone) |
| `20:9` / `9:20` | Modern smartphone displays (Android) |
| `21:9` | Cinematic widescreen |
| `5:2` | Wide banners |
| `auto` | Model auto-selects the best ratio for the prompt |

```python customLanguage="pythonXAI"
import xai_sdk

client = xai_sdk.Client()

response = client.image.sample(
    prompt="Mountain landscape at sunrise",
    model="grok-imagine-image-2.0",
    aspect_ratio="16:9",
)

print(response.url)
```

```python customLanguage="pythonOpenAISDK"
from openai import OpenAI

client = OpenAI(
    base_url="https://api.x.ai/v1",
    api_key="YOUR_API_KEY",
)

response = client.images.generate(
    model="grok-imagine-image-2.0",
    prompt="Mountain landscape at sunrise",
    extra_body={"aspect_ratio": "16:9"},
)

print(response.data[0].url)
```

```javascript customLanguage="javascriptOpenAISDK"
import OpenAI from "openai";

const client = new OpenAI({
    apiKey: process.env.XAI_API_KEY,
    baseURL: "https://api.x.ai/v1",
});

const response = await client.images.generate({
    model: "grok-imagine-image-2.0",
    prompt: "Mountain landscape at sunrise",

    aspect_ratio: "16:9",
});

console.log(response.data[0].url);
```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { generateImage } from "ai";

const { image } = await generateImage({
    model: xai.image("grok-imagine-image-2.0"),
    prompt: "Mountain landscape at sunrise",
    aspectRatio: "16:9",
});

console.log(image.base64);
```

```bash
curl -X POST https://api.x.ai/v1/images/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-image-2.0",
    "prompt": "Mountain landscape at sunrise",
    "aspect_ratio": "16:9"
  }'
```

### Resolution

You can specify different resolutions of the output image with the `resolution` parameter. Currently supported image resolutions are:

* `1k` (default when omitted)
* `2k`

```python customLanguage="pythonXAI"
import xai_sdk

client = xai_sdk.Client()

response = client.image.sample(
    prompt="An astronaut performing EVA in LEO.",
    model="grok-imagine-image-2.0",
    resolution="2k"
)

print(response.url)
```

```python customLanguage="pythonOpenAISDK"
from openai import OpenAI

client = OpenAI(
    base_url="https://api.x.ai/v1",
    api_key="YOUR_API_KEY",
)

response = client.images.generate(
    model="grok-imagine-image-2.0",
    prompt="An astronaut performing EVA in LEO.",
    extra_body={"resolution": "2k"},
)

print(response.data[0].url)
```

```javascript customLanguage="javascriptOpenAISDK"
import OpenAI from "openai";

const client = new OpenAI({
    apiKey: process.env.XAI_API_KEY,
    baseURL: "https://api.x.ai/v1",
});

const response = await client.images.generate({
    model: "grok-imagine-image-2.0",
    prompt: "An astronaut performing EVA in LEO.",

    resolution: "2k",
});

console.log(response.data[0].url);
```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { generateImage } from "ai";

const { image } = await generateImage({
    model: xai.image("grok-imagine-image-2.0"),
    prompt: "An astronaut performing EVA in LEO.",
    providerOptions: {
        xai: { resolution: "2k" },
    },
});

console.log(image.base64);
```

```bash
curl -X POST https://api.x.ai/v1/images/generations \
-H "Content-Type: application/json" \
-H "Authorization: Bearer $XAI_API_KEY" \
-d '{
    "model": "grok-imagine-image-2.0",
    "prompt": "An astronaut performing EVA in LEO.",
    "resolution": "2k"
}'
```

### Quality

Control generation quality with the optional `quality` parameter. Allowed values are `low`, `medium`, and `auto`. When omitted, the default is `auto`, which lets the service choose the quality for each request. Auto currently uses `low` for image generation and `medium` for [image editing](/developers/model-capabilities/images/editing). Images are billed at the quality they are served at (see [Pricing](/developers/pricing)). Pass `low` or `medium` to pin a specific quality. The parameter is only supported for `grok-imagine-image-2.0`.

```python customLanguage="pythonXAI"
import xai_sdk

client = xai_sdk.Client()

response = client.image.sample(
    prompt="A watercolor painting of a lighthouse at dawn",
    model="grok-imagine-image-2.0",
    quality="low",
)

print(response.url)
```

```bash
curl -X POST https://api.x.ai/v1/images/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-image-2.0",
    "prompt": "A watercolor painting of a lighthouse at dawn",
    "quality": "low"
  }'
```

### Base64 Output

Control the output format with the `response_format` parameter. When omitted, the default is `url`, which returns temporary hosted URLs. For embedding images directly without downloading, request base64:

```python customLanguage="pythonXAI"
import xai_sdk

client = xai_sdk.Client()

response = client.image.sample(
    prompt="A serene Japanese garden",
    model="grok-imagine-image-2.0",
    image_format="base64",
)

# Save to file
with open("garden.jpg", "wb") as f:
    f.write(response.image)
```

```python customLanguage="pythonOpenAISDK"
import base64
from openai import OpenAI

client = OpenAI(
    base_url="https://api.x.ai/v1",
    api_key="YOUR_API_KEY",
)

response = client.images.generate(
    model="grok-imagine-image-2.0",
    prompt="A serene Japanese garden",
    response_format="b64_json",
)

# Save to file
image_bytes = base64.b64decode(response.data[0].b64_json)
with open("garden.jpg", "wb") as f:
    f.write(image_bytes)
```

```javascript customLanguage="javascriptOpenAISDK"
import OpenAI from "openai";
import fs from "fs";

const client = new OpenAI({
    apiKey: process.env.XAI_API_KEY,
    baseURL: "https://api.x.ai/v1",
});

const response = await client.images.generate({
    model: "grok-imagine-image-2.0",
    prompt: "A serene Japanese garden",
    response_format: "b64_json",
});

// Save to file
const imageBuffer = Buffer.from(response.data[0].b64_json, "base64");
fs.writeFileSync("garden.jpg", imageBuffer);
```

```javascript customLanguage="javascriptAISDK"
import { xai } from "@ai-sdk/xai";
import { generateImage } from "ai";
import fs from "fs";

const { image } = await generateImage({
    model: xai.image("grok-imagine-image-2.0"),
    prompt: "A serene Japanese garden",
});

// Save to file (AI SDK returns base64 by default)
const imageBuffer = Buffer.from(image.base64, "base64");
fs.writeFileSync("garden.jpg", imageBuffer);
```

```bash
curl -X POST https://api.x.ai/v1/images/generations \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $XAI_API_KEY" \
  -d '{
    "model": "grok-imagine-image-2.0",
    "prompt": "A serene Japanese garden",
    "response_format": "b64_json"
  }'
```

### Response Details

The xAI SDK exposes additional metadata on the response object beyond the image URL or base64 data.

**Moderation** — Check whether the generated image passed content moderation:

```python customLanguage="pythonXAI"
if response.respect_moderation:
    print(response.url)
else:
    print("Image filtered by moderation")
```

**Model** — Get the actual model used (resolving any aliases):

```python customLanguage="pythonXAI"
print(f"Model: {response.model}")
```

## Concurrent Requests

When you need to generate multiple images with **different prompts**, such as generating unrelated images in parallel, use `AsyncClient` with `asyncio.gather` to fire requests concurrently. This is significantly faster than issuing them one at a time.

> [!TIP]
>
> If you want multiple variations from the **same prompt**, use [`sample_batch()` with the `n` parameter\`](#multiple-images) instead. That generates all images in a single request and is the most efficient approach for same-prompt generation.

```python customLanguage="pythonXAI"
import asyncio
import xai_sdk

async def generate_concurrently():
    client = xai_sdk.AsyncClient()

    # Each request uses a different prompt
    prompts = [
        "A futuristic city skyline at sunset",
        "A serene Japanese garden in winter",
        "An astronaut floating above Earth",
        "A medieval castle on a misty mountain",
    ]

    # Fire all requests concurrently
    tasks = [
        client.image.sample(
            prompt=prompt,
            model="grok-imagine-image-2.0",
        )
        for prompt in prompts
    ]

    results = await asyncio.gather(*tasks)

    for prompt, result in zip(prompts, results):
        print(f"{prompt}: {result.url}")

asyncio.run(generate_concurrently())
```

## Related

* [Models](/developers/models) — Available image models
* [Image Editing](/developers/model-capabilities/images/editing) — Edit images with natural language
* [Image Generation Tool](/developers/tools/image-generation) — Let Grok generate and edit images inside a conversation
* [Video Generation](/developers/model-capabilities/video/generation) — Generate videos from text prompts
* [API Reference](/developers/rest-api-reference) — Full endpoint documentation
* [Imagine API Landing Page](https://x.ai/api/imagine) — Showcase of the Imagine API in action
