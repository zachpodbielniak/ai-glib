/*
 * multi-provider.c - Multi-provider example using ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates using multiple AI providers with ai-glib.
 * It shows how to create clients for different providers and use them
 * through the common AiProvider interface.
 *
 * Usage:
 *   export ANTHROPIC_API_KEY="your-claude-key"
 *   export OPENAI_API_KEY="your-openai-key"
 *   ./multi-provider
 */

#include <stdio.h>
#include <stdlib.h>

#include "ai-glib.h"

/*
 * Print provider information.
 */
static void
print_provider_info(AiProvider *provider)
{
	const gchar *name;
	const gchar *model;
	AiProviderType type;

	type = ai_provider_get_provider_type(provider);
	name = ai_provider_get_name(provider);
	model = ai_provider_get_default_model(provider);

	printf("Provider: %s (type: %s)\n", name, ai_provider_type_to_string(type));
	printf("Default model: %s\n\n", model);
}

/*
 * Demonstrate creating different provider clients.
 */
int
main(
	int   argc,
	char *argv[]
){
	g_autoptr(AiClaudeClient) claude = NULL;
	g_autoptr(AiOpenAIClient) openai = NULL;
	g_autoptr(AiGrokClient) grok = NULL;
	g_autoptr(AiGeminiClient) gemini = NULL;
	g_autoptr(AiOllamaClient) ollama = NULL;

	(void)argc;
	(void)argv;

	printf("=== ai-glib Multi-Provider Example ===\n\n");

	/* Create Claude client */
	printf("--- Claude ---\n");
	claude = ai_claude_client_new();
	print_provider_info(AI_PROVIDER(claude));

	/* Create OpenAI client */
	printf("--- OpenAI ---\n");
	openai = ai_openai_client_new();
	print_provider_info(AI_PROVIDER(openai));

	/* Create Grok client */
	printf("--- Grok (xAI) ---\n");
	grok = ai_grok_client_new();
	print_provider_info(AI_PROVIDER(grok));

	/* Create Gemini client */
	printf("--- Gemini (Google) ---\n");
	gemini = ai_gemini_client_new();
	print_provider_info(AI_PROVIDER(gemini));

	/* Create Ollama client (local) */
	printf("--- Ollama (Local) ---\n");
	ollama = ai_ollama_client_new();
	print_provider_info(AI_PROVIDER(ollama));

	printf("All providers initialized successfully!\n");
	printf("\nNote: To actually make API calls, set the appropriate\n");
	printf("environment variables for each provider:\n");
	printf("  - ANTHROPIC_API_KEY for Claude\n");
	printf("  - OPENAI_API_KEY for OpenAI\n");
	printf("  - XAI_API_KEY for Grok\n");
	printf("  - GEMINI_API_KEY for Gemini\n");
	printf("  - OLLAMA_HOST for Ollama (default: http://localhost:11434)\n");

	return 0;
}
