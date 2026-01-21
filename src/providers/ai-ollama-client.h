/*
 * ai-ollama-client.h - Ollama client (local)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "core/ai-client.h"
#include "core/ai-config.h"

G_BEGIN_DECLS

#define AI_TYPE_OLLAMA_CLIENT (ai_ollama_client_get_type())

G_DECLARE_FINAL_TYPE(AiOllamaClient, ai_ollama_client, AI, OLLAMA_CLIENT, AiClient)

/**
 * AI_OLLAMA_DEFAULT_MODEL:
 *
 * The default model for Ollama clients.
 */
#define AI_OLLAMA_DEFAULT_MODEL "gpt-oss:20b"

/*
 * Common Ollama Models
 * Note: Models must be pulled with `ollama pull <model>` before use.
 */

/* DeepSeek Models */
#define AI_OLLAMA_MODEL_DEEPSEEK_R1_32B     "deepseek-r1:32b"
#define AI_OLLAMA_MODEL_DEEPSEEK_R1_14B     "deepseek-r1:14b"
#define AI_OLLAMA_MODEL_DEEPSEEK_R1_8B      "deepseek-r1:8b"
#define AI_OLLAMA_MODEL_DEEPSEEK_R1_1_5B    "deepseek-r1:1.5b"

/* Llama Models */
#define AI_OLLAMA_MODEL_LLAMA3_1_8B         "llama3.1:8b"
#define AI_OLLAMA_MODEL_LLAMA3_2            "llama3.2"

/* Gemma Models */
#define AI_OLLAMA_MODEL_GEMMA3_27B          "gemma3:27b"
#define AI_OLLAMA_MODEL_GEMMA3_12B          "gemma3:12b"
#define AI_OLLAMA_MODEL_GEMMA3_4B           "gemma3:4b"

/* Mixtral / Dolphin Models */
#define AI_OLLAMA_MODEL_DOLPHIN_MIXTRAL     "dolphin-mixtral:8x7b"
#define AI_OLLAMA_MODEL_DOLPHIN3_8B         "dolphin3:8b"

/* Falcon Models */
#define AI_OLLAMA_MODEL_FALCON3_10B         "falcon3:10b"

/* Tiny / Lightweight Models */
#define AI_OLLAMA_MODEL_TINYLLAMA           "tinyllama:1.1b"

/* Embedding Models */
#define AI_OLLAMA_MODEL_NOMIC_EMBED         "nomic-embed-text:v1.5"

/* Custom / Local Models */
#define AI_OLLAMA_MODEL_GPT_OSS_20B         "gpt-oss:20b"
#define AI_OLLAMA_MODEL_NEURALDAREDEVIL     "tarruda/neuraldaredevil-8b-abliterated:fp16"

/**
 * ai_ollama_client_new:
 *
 * Creates a new #AiOllamaClient using the default configuration.
 * The server URL will be read from the OLLAMA_HOST environment variable,
 * or default to http://localhost:11434.
 *
 * Returns: (transfer full): a new #AiOllamaClient
 */
AiOllamaClient *
ai_ollama_client_new(void);

/**
 * ai_ollama_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiOllamaClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiOllamaClient
 */
AiOllamaClient *
ai_ollama_client_new_with_config(AiConfig *config);

/**
 * ai_ollama_client_new_with_host:
 * @host: the Ollama server URL (e.g., "http://localhost:11434")
 *
 * Creates a new #AiOllamaClient connecting to the specified host.
 *
 * Returns: (transfer full): a new #AiOllamaClient
 */
AiOllamaClient *
ai_ollama_client_new_with_host(const gchar *host);

G_END_DECLS
