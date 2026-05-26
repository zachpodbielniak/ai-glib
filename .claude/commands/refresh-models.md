---
description: Check every AI provider for new upstream models and add #defines + docs for them.
---

# Refresh Provider Model Defines

Goal: bring `src/providers/ai-*-client.h` and `docs/providers/*.md` up to
date with whatever Anthropic, OpenAI, Google, and xAI have shipped since
the last refresh. Ollama is community-curated; skip it unless explicitly
asked.

## Inputs to read first

- `src/providers/ai-claude-client.h`
- `src/providers/ai-openai-client.h`
- `src/providers/ai-gemini-client.h`
- `src/providers/ai-grok-client.h`
- `docs/providers/{claude,openai,gemini,grok}.md`
- `CLAUDE.md` (project conventions)

Enumerate the existing `AI_*_MODEL_*`, `AI_*_DEFAULT_MODEL`,
`AI_*_IMAGE_MODEL_*`, and related defines so you know the baseline.

## Research upstream

Use `WebSearch` + `WebFetch` against the official model-list pages:

- Claude: `https://platform.claude.com/docs/en/about-claude/models/overview`
- OpenAI: `https://developers.openai.com/api/docs/models/all`
- Gemini: `https://ai.google.dev/gemini-api/docs/models`
- Grok:  `https://docs.x.ai/developers/models`

For each provider, build a table of {display name, exact API ID,
status (current/preview/deprecated/retired)}. Note explicit retirement
dates and server-side redirect targets — these matter for the doc
annotations.

## What to add

For each provider, add `#define`s for any current upstream IDs missing
from the header. Group by version family with a `/* Family Models */`
comment block, newest-first, **above** the existing entries — match the
prevailing style.

Scope:
- chat / text-completion models — always
- image-generation models — always
- audio / realtime / TTS / video / embedding — add as **identifier-only**
  constants when upstream offers them, even though the clients don't
  expose those endpoints yet. Prefix each new section with a comment
  that says "Identifiers only. Ai<X>Client does not yet expose
  <modality> endpoints; these constants exist so callers can reference
  upstream IDs by symbolic name once support lands."

Naming follows the existing pattern: `AI_<PROVIDER>_MODEL_<TAG>` for
chat, `AI_<PROVIDER>_IMAGE_MODEL_<TAG>`, `AI_<PROVIDER>_AUDIO_MODEL_*`,
`AI_<PROVIDER>_REALTIME_MODEL_*`, `AI_<PROVIDER>_TTS_MODEL_*`,
`AI_<PROVIDER>_VIDEO_MODEL_*`, `AI_<PROVIDER>_EMBEDDING_MODEL_*`.

## Defaults and aliases

Bump these to the new flagships:

- `AI_<PROVIDER>_DEFAULT_MODEL` → current chat flagship
- `AI_<PROVIDER>_IMAGE_DEFAULT_MODEL` → current stable image model (do
  not point the default at a preview-only model)
- `AI_<PROVIDER>_MODEL_LATEST` / `OPUS` / `SONNET` / `HAIKU` / `FLASH` /
  `PRO` / `FAST` / `REASONING` / `CODE` — retarget to the current
  best-fit constant for that tier

Special case: if a current default ID is server-side retired (e.g. xAI
redirects), the default bump is a forced correctness fix, not a
preference.

## Retired upstream IDs

Keep existing `#define`s for source compatibility — removing them
breaks downstream code that compiled against the symbol. In
`docs/providers/<name>.md`, annotate each retired entry's Description
column with either:
- "Deprecated, retires YYYY-MM-DD" (still works, scheduled removal), or
- "Retired upstream — redirects to <new-id>" (server-side redirect), or
- "Retired upstream — use `AI_<NEW_DEFINE>`" (no redirect)

## Naming-convention drift

Watch for upstream changes in ID format (e.g. xAI switching from
`grok-4-1` to `grok-4.3`, Anthropic moving to dateless IDs at 4.6+).
When the convention changes, add a one-paragraph note in both the
header and the docs explaining the shift.

## Docs sync

Mirror every new `#define` in `docs/providers/<name>.md` in the
corresponding table. Update the "Convenience Aliases" table and the
"Setting the Model" code example to reference the new flagship.

## Verify

Run `make GIR=1 test` and confirm:
- release build succeeds
- GIR scanner stays clean (the `test-gir-clean` gate fails on any
  scanner warning)
- all unit tests pass

If `XAI_API_KEY` / `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` /
`GEMINI_API_KEY` are set in the environment, also smoke-test one new
model per provider via the corresponding `simple-chat-*` example to
prove the new model string round-trips against the live API.

## ABI invariant

This refresh must stay strictly additive:
- no struct fields added/removed/reordered
- no function signatures changed
- no enum integer values changed
- no exported symbols added/removed
- no SONAME change

`#define` retargeting is fine — it's preprocessor-only and existing
binaries already hold the old string literal in `.rodata`. Recompiled
callers pick up the new defaults; that's the intended behavior.

## Stop conditions

- If upstream has shipped nothing new since the existing header content,
  report "all four providers are current" and exit without modifying
  files. Do not invent models to justify a diff.
- If you find a model whose family or naming pattern doesn't fit any
  existing scheme, stop and ask the user how to name the new constant
  rather than guessing.

## Commit

When verification passes, draft a commit message in the project's
`feat(providers): ...` style. Summarize: new flagships per provider,
what aliases got retargeted, what identifier-only sections were added,
which retirement annotations landed, and the ABI invariant. Do not push
unless the user asks.
