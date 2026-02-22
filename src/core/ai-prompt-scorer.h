/*
 * ai-prompt-scorer.h - Prompt complexity scoring for smart routing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Scores prompts across 14 weighted dimensions (code, reasoning,
 * technical, creative, constraints, etc.) and maps the aggregate
 * score to a tier with sigmoid-calibrated confidence.  Runs entirely
 * locally in < 1 ms with zero external API calls.
 *
 * Ported from ClawRouter's rule-based classifier.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Tier enum                                                           */
/* ------------------------------------------------------------------ */

/**
 * AiPromptTier:
 * @AI_PROMPT_TIER_SIMPLE: trivial / lookup / greeting
 * @AI_PROMPT_TIER_MEDIUM: moderate code, summaries
 * @AI_PROMPT_TIER_COMPLEX: architecture, debugging, multi-file
 * @AI_PROMPT_TIER_REASONING: proofs, formal logic, chain-of-thought
 *
 * Complexity tier assigned to a prompt by the scorer.
 */
typedef enum {
    AI_PROMPT_TIER_SIMPLE = 0,
    AI_PROMPT_TIER_MEDIUM,
    AI_PROMPT_TIER_COMPLEX,
    AI_PROMPT_TIER_REASONING
} AiPromptTier;

GType ai_prompt_tier_get_type(void) G_GNUC_CONST;
#define AI_TYPE_PROMPT_TIER (ai_prompt_tier_get_type())

const gchar * ai_prompt_tier_to_string(AiPromptTier tier);
AiPromptTier  ai_prompt_tier_from_string(const gchar *str);

/* ------------------------------------------------------------------ */
/* Scoring result (boxed)                                              */
/* ------------------------------------------------------------------ */

/**
 * AiScoringResult:
 *
 * Result of prompt classification.  Contains the weighted score,
 * assigned tier, sigmoid-calibrated confidence, agentic score,
 * and a list of human-readable signal strings.
 */
typedef struct _AiScoringResult AiScoringResult;

#define AI_TYPE_SCORING_RESULT (ai_scoring_result_get_type())

GType ai_scoring_result_get_type(void) G_GNUC_CONST;

AiScoringResult * ai_scoring_result_new(void);
AiScoringResult * ai_scoring_result_copy(const AiScoringResult *r);
void              ai_scoring_result_free(AiScoringResult *r);

gdouble      ai_scoring_result_get_score(const AiScoringResult *r);
AiPromptTier ai_scoring_result_get_tier(const AiScoringResult *r);
gboolean     ai_scoring_result_get_tier_is_ambiguous(const AiScoringResult *r);
gdouble      ai_scoring_result_get_confidence(const AiScoringResult *r);
gdouble      ai_scoring_result_get_agentic_score(const AiScoringResult *r);
guint        ai_scoring_result_get_estimated_tokens(const AiScoringResult *r);

/**
 * ai_scoring_result_get_signals:
 * @r: an #AiScoringResult
 *
 * Returns the human-readable signal strings (e.g. "code", "reasoning").
 *
 * Returns: (element-type utf8) (transfer none): the signals array
 */
GPtrArray * ai_scoring_result_get_signals(const AiScoringResult *r);

/**
 * ai_scoring_result_format_debug:
 * @r: an #AiScoringResult
 *
 * Returns a single-line human-readable summary of the scoring result
 * suitable for debug output.
 *
 * Returns: (transfer full): a debug string.  Free with g_free().
 */
gchar * ai_scoring_result_format_debug(const AiScoringResult *r);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiScoringResult, ai_scoring_result_free)

/* ------------------------------------------------------------------ */
/* Scorer config (boxed)                                               */
/* ------------------------------------------------------------------ */

/**
 * AiScorerConfig:
 *
 * Optional configuration for the scorer.  If %NULL is passed to
 * ai_prompt_scorer_classify(), built-in defaults (ported from
 * ClawRouter v2.0) are used.
 */
typedef struct _AiScorerConfig AiScorerConfig;

#define AI_TYPE_SCORER_CONFIG (ai_scorer_config_get_type())

GType ai_scorer_config_get_type(void) G_GNUC_CONST;

AiScorerConfig * ai_scorer_config_new_defaults(void);
AiScorerConfig * ai_scorer_config_copy(const AiScorerConfig *c);
void             ai_scorer_config_free(AiScorerConfig *c);

void ai_scorer_config_set_tier_boundaries(AiScorerConfig *c,
                                          gdouble simple_medium,
                                          gdouble medium_complex,
                                          gdouble complex_reasoning);

void ai_scorer_config_set_confidence_threshold(AiScorerConfig *c,
                                               gdouble threshold);

void ai_scorer_config_set_confidence_steepness(AiScorerConfig *c,
                                               gdouble steepness);

void ai_scorer_config_set_max_tokens_force_complex(AiScorerConfig *c,
                                                   guint tokens);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiScorerConfig, ai_scorer_config_free)

/* ------------------------------------------------------------------ */
/* Main classify function                                              */
/* ------------------------------------------------------------------ */

/**
 * ai_prompt_scorer_classify:
 * @prompt: the user prompt text
 * @system_prompt: (nullable): the system prompt, or %NULL
 * @config: (nullable): scorer config, or %NULL for defaults
 *
 * Scores @prompt across 14 weighted dimensions and maps the result
 * to a tier with sigmoid-calibrated confidence.  Runs entirely
 * in-process with no external calls.
 *
 * Returns: (transfer full): a new #AiScoringResult
 */
AiScoringResult *
ai_prompt_scorer_classify(const gchar          *prompt,
                          const gchar          *system_prompt,
                          const AiScorerConfig *config);

G_END_DECLS
