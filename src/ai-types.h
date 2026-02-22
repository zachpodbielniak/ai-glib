/*
 * ai-types.h - Forward type declarations for ai-glib
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

#include <glib.h>

G_BEGIN_DECLS

/*
 * Forward declarations for all public types in ai-glib.
 * This allows headers to reference types without circular dependencies.
 *
 * Note: For final types (G_DECLARE_FINAL_TYPE), we only declare the instance
 * struct, not the class, because G_DECLARE_FINAL_TYPE creates an anonymous
 * struct for the class which would conflict with a named forward declaration.
 */

/* Core types */
typedef struct _AiConfig  AiConfig;
/* AiConfig is final - no class forward declaration */

/* Model types */
typedef struct _AiUsage  AiUsage;

/* AiContentBlock is derivable - needs class forward declaration */
typedef struct _AiContentBlock       AiContentBlock;
typedef struct _AiContentBlockClass  AiContentBlockClass;

typedef struct _AiTextContent  AiTextContent;
/* AiTextContent is final */

typedef struct _AiToolUse  AiToolUse;
/* AiToolUse is final */

typedef struct _AiToolResult  AiToolResult;
/* AiToolResult is final */

typedef struct _AiTool  AiTool;
/* AiTool is final */

typedef struct _AiMessage  AiMessage;
/* AiMessage is final */

typedef struct _AiResponse  AiResponse;
/* AiResponse is final */

/* AiClient is derivable - needs class forward declaration */
typedef struct _AiClient       AiClient;
typedef struct _AiClientClass  AiClientClass;

typedef struct _AiClaudeClient  AiClaudeClient;
/* AiClaudeClient is final */

typedef struct _AiOpenAIClient  AiOpenAIClient;
/* AiOpenAIClient is final */

typedef struct _AiGeminiClient  AiGeminiClient;
/* AiGeminiClient is final */

typedef struct _AiGrokClient  AiGrokClient;
/* AiGrokClient is final */

typedef struct _AiOllamaClient  AiOllamaClient;
/* AiOllamaClient is final */

/* Convenience types */
typedef struct _AiSimple  AiSimple;
/* AiSimple is final */

typedef struct _AiToolExecutor  AiToolExecutor;
/* AiToolExecutor is final */

/* Search provider interface and implementations */
typedef struct _AiSearchProvider           AiSearchProvider;
typedef struct _AiSearchProviderInterface  AiSearchProviderInterface;

typedef struct _AiBingSearch  AiBingSearch;
/* AiBingSearch is final */

typedef struct _AiBraveSearch  AiBraveSearch;
/* AiBraveSearch is final */

/* Interface types */
typedef struct _AiProvider           AiProvider;
typedef struct _AiProviderInterface  AiProviderInterface;

typedef struct _AiStreamable           AiStreamable;
typedef struct _AiStreamableInterface  AiStreamableInterface;

G_END_DECLS
