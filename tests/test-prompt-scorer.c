/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026
 *
 * test-prompt-scorer.c - Unit tests for AiPromptScorer
 */

#include "ai-glib.h"
#include <string.h>

/* ================================================================== */
/* Tier classification tests                                           */
/* ================================================================== */

static void
test_scorer_simple_greeting(void)
{
    g_autoptr(AiScoringResult) r = NULL;

    r = ai_prompt_scorer_classify("hello", NULL, NULL);
    g_assert_nonnull(r);
    g_assert_cmpint(ai_scoring_result_get_tier(r), ==,
                    AI_PROMPT_TIER_SIMPLE);
}

static void
test_scorer_simple_question(void)
{
    g_autoptr(AiScoringResult) r = NULL;

    r = ai_prompt_scorer_classify("what time is it?", NULL, NULL);
    g_assert_nonnull(r);
    g_assert_cmpint(ai_scoring_result_get_tier(r), ==,
                    AI_PROMPT_TIER_SIMPLE);
}

static void
test_scorer_medium_code(void)
{
    g_autoptr(AiScoringResult) r = NULL;

    r = ai_prompt_scorer_classify(
        "write a python function that sorts a list of integers "
        "using quicksort", NULL, NULL);
    g_assert_nonnull(r);
    /* Should be at least MEDIUM */
    g_assert_cmpint(ai_scoring_result_get_tier(r), >=,
                    AI_PROMPT_TIER_MEDIUM);
}

static void
test_scorer_complex_scores_higher_than_simple(void)
{
    g_autoptr(AiScoringResult) simple  = NULL;
    g_autoptr(AiScoringResult) complex = NULL;

    simple = ai_prompt_scorer_classify("hello", NULL, NULL);

    complex = ai_prompt_scorer_classify(
        "Design a microservices architecture for a real-time "
        "trading platform. Include API gateway, event sourcing, "
        "database sharding strategy, and explain the trade-offs "
        "between consistency and availability. Provide code examples "
        "in Go for the order matching engine with proper error "
        "handling, concurrent access patterns, mutex locks, and "
        "implement the full CQRS pattern. Write comprehensive unit "
        "tests, integration tests, benchmark tests, implement "
        "graceful shutdown, circuit breaker, retry with exponential "
        "backoff, distributed tracing with OpenTelemetry, and "
        "Kubernetes deployment manifests with horizontal pod "
        "autoscaling.", NULL, NULL);

    g_assert_nonnull(simple);
    g_assert_nonnull(complex);

    /* Complex prompt must score strictly higher */
    g_assert_cmpfloat(ai_scoring_result_get_score(complex), >,
                      ai_scoring_result_get_score(simple));
    /* And tier must be at least MEDIUM */
    g_assert_cmpint(ai_scoring_result_get_tier(complex), >=,
                    AI_PROMPT_TIER_MEDIUM);
}

static void
test_scorer_reasoning_keywords(void)
{
    g_autoptr(AiScoringResult) r = NULL;

    r = ai_prompt_scorer_classify(
        "Prove by induction that the sum of the first n natural "
        "numbers equals n(n+1)/2. Then derive the formula using "
        "mathematical reasoning and chain of thought step by step.",
        NULL, NULL);
    g_assert_nonnull(r);
    g_assert_cmpint(ai_scoring_result_get_tier(r), ==,
                    AI_PROMPT_TIER_REASONING);
}

/* ================================================================== */
/* Score range tests                                                   */
/* ================================================================== */

static void
test_scorer_score_monotonic(void)
{
    g_autoptr(AiScoringResult) r_short = NULL;
    g_autoptr(AiScoringResult) r_long  = NULL;

    r_short = ai_prompt_scorer_classify("hi", NULL, NULL);
    r_long  = ai_prompt_scorer_classify(
        "implement a distributed hash table in rust with full "
        "error handling, unit tests, benchmarks, documentation, "
        "and a CLI interface for debugging. Use async I/O.",
        NULL, NULL);

    g_assert_nonnull(r_short);
    g_assert_nonnull(r_long);

    /* Longer, more complex prompt should score higher */
    g_assert_cmpfloat(ai_scoring_result_get_score(r_long), >,
                      ai_scoring_result_get_score(r_short));
}

static void
test_scorer_confidence_range(void)
{
    g_autoptr(AiScoringResult) r = NULL;

    r = ai_prompt_scorer_classify(
        "explain how a b-tree works with examples", NULL, NULL);
    g_assert_nonnull(r);
    g_assert_cmpfloat(ai_scoring_result_get_confidence(r), >=, 0.0);
    g_assert_cmpfloat(ai_scoring_result_get_confidence(r), <=, 1.0);
}

/* ================================================================== */
/* Scoring result accessors                                            */
/* ================================================================== */

static void
test_scorer_result_signals(void)
{
    g_autoptr(AiScoringResult) r = NULL;
    GPtrArray *signals;

    r = ai_prompt_scorer_classify(
        "write a recursive function in C to traverse a linked list",
        NULL, NULL);
    g_assert_nonnull(r);

    signals = ai_scoring_result_get_signals(r);
    g_assert_nonnull(signals);
    /* Should have at least one signal for a code-related prompt */
    g_assert_cmpuint(signals->len, >, 0);
}

static void
test_scorer_result_copy(void)
{
    g_autoptr(AiScoringResult) r = NULL;
    g_autoptr(AiScoringResult) c = NULL;

    r = ai_prompt_scorer_classify("hello there", NULL, NULL);
    g_assert_nonnull(r);

    c = ai_scoring_result_copy(r);
    g_assert_nonnull(c);
    g_assert_cmpfloat(ai_scoring_result_get_score(r), ==,
                      ai_scoring_result_get_score(c));
    g_assert_cmpint(ai_scoring_result_get_tier(r), ==,
                    ai_scoring_result_get_tier(c));
    g_assert_cmpfloat(ai_scoring_result_get_confidence(r), ==,
                      ai_scoring_result_get_confidence(c));
}

static void
test_scorer_result_format_debug(void)
{
    g_autoptr(AiScoringResult) r = NULL;
    g_autofree gchar *debug = NULL;

    r = ai_prompt_scorer_classify("debug this segfault in my code",
                                  NULL, NULL);
    g_assert_nonnull(r);

    debug = ai_scoring_result_format_debug(r);
    g_assert_nonnull(debug);
    g_assert_true(strlen(debug) > 0);
}

/* ================================================================== */
/* Config tests                                                        */
/* ================================================================== */

static void
test_scorer_config_defaults(void)
{
    g_autoptr(AiScorerConfig) cfg = NULL;

    cfg = ai_scorer_config_new_defaults();
    g_assert_nonnull(cfg);
}

static void
test_scorer_config_custom_boundaries(void)
{
    g_autoptr(AiScorerConfig)  cfg_default = NULL;
    g_autoptr(AiScorerConfig)  cfg_loose   = NULL;
    g_autoptr(AiScoringResult) r_default   = NULL;
    g_autoptr(AiScoringResult) r_loose     = NULL;
    const gchar *prompt;

    /* A moderately complex prompt */
    prompt = "write a python function that implements binary search "
             "with error handling and type annotations";

    cfg_default = ai_scorer_config_new_defaults();
    r_default = ai_prompt_scorer_classify(prompt, NULL, cfg_default);

    /* Very low boundaries should elevate the tier */
    cfg_loose = ai_scorer_config_new_defaults();
    ai_scorer_config_set_tier_boundaries(cfg_loose, -1.0, -0.5, 0.0);

    r_loose = ai_prompt_scorer_classify(prompt, NULL, cfg_loose);

    g_assert_nonnull(r_default);
    g_assert_nonnull(r_loose);

    /* With much lower boundaries, tier should be >= the default */
    g_assert_cmpint(ai_scoring_result_get_tier(r_loose), >=,
                    ai_scoring_result_get_tier(r_default));
}

static void
test_scorer_config_copy(void)
{
    g_autoptr(AiScorerConfig) cfg = NULL;
    g_autoptr(AiScorerConfig) c   = NULL;

    cfg = ai_scorer_config_new_defaults();
    ai_scorer_config_set_tier_boundaries(cfg, 0.1, 0.4, 0.7);
    ai_scorer_config_set_confidence_threshold(cfg, 0.15);

    c = ai_scorer_config_copy(cfg);
    g_assert_nonnull(c);

    /* Verify copy produces same results */
    {
        g_autoptr(AiScoringResult) r1 = NULL;
        g_autoptr(AiScoringResult) r2 = NULL;

        r1 = ai_prompt_scorer_classify("test prompt", NULL, cfg);
        r2 = ai_prompt_scorer_classify("test prompt", NULL, c);

        g_assert_cmpfloat(ai_scoring_result_get_score(r1), ==,
                          ai_scoring_result_get_score(r2));
        g_assert_cmpint(ai_scoring_result_get_tier(r1), ==,
                        ai_scoring_result_get_tier(r2));
    }
}

/* ================================================================== */
/* Tier enum conversion tests                                          */
/* ================================================================== */

static void
test_scorer_tier_to_string(void)
{
    /* to_string returns uppercase */
    g_assert_cmpstr(ai_prompt_tier_to_string(AI_PROMPT_TIER_SIMPLE),
                    ==, "SIMPLE");
    g_assert_cmpstr(ai_prompt_tier_to_string(AI_PROMPT_TIER_MEDIUM),
                    ==, "MEDIUM");
    g_assert_cmpstr(ai_prompt_tier_to_string(AI_PROMPT_TIER_COMPLEX),
                    ==, "COMPLEX");
    g_assert_cmpstr(ai_prompt_tier_to_string(AI_PROMPT_TIER_REASONING),
                    ==, "REASONING");
}

static void
test_scorer_tier_from_string(void)
{
    g_assert_cmpint(ai_prompt_tier_from_string("simple"), ==,
                    AI_PROMPT_TIER_SIMPLE);
    g_assert_cmpint(ai_prompt_tier_from_string("medium"), ==,
                    AI_PROMPT_TIER_MEDIUM);
    g_assert_cmpint(ai_prompt_tier_from_string("complex"), ==,
                    AI_PROMPT_TIER_COMPLEX);
    g_assert_cmpint(ai_prompt_tier_from_string("reasoning"), ==,
                    AI_PROMPT_TIER_REASONING);
    /* Unknown string should default to MEDIUM */
    g_assert_cmpint(ai_prompt_tier_from_string("bogus"), ==,
                    AI_PROMPT_TIER_MEDIUM);
}

/* ================================================================== */
/* Agentic detection tests                                             */
/* ================================================================== */

static void
test_scorer_agentic_detection(void)
{
    g_autoptr(AiScoringResult) r = NULL;

    r = ai_prompt_scorer_classify(
        "Use the filesystem tool to read the config file, then "
        "execute a shell command to deploy the application and "
        "finally search the codebase for any remaining TODOs.",
        NULL, NULL);
    g_assert_nonnull(r);
    g_assert_cmpfloat(ai_scoring_result_get_agentic_score(r), >, 0.0);
}

/* ================================================================== */
/* System prompt inclusion test                                        */
/* ================================================================== */

static void
test_scorer_with_system_prompt(void)
{
    g_autoptr(AiScoringResult) r_no_sys = NULL;
    g_autoptr(AiScoringResult) r_sys    = NULL;

    /* Same user prompt, but system prompt adds token weight */
    r_no_sys = ai_prompt_scorer_classify("hi", NULL, NULL);
    r_sys    = ai_prompt_scorer_classify("hi",
        "You are an expert systems architect with deep knowledge of "
        "distributed computing, microservices, and cloud infrastructure.",
        NULL);

    g_assert_nonnull(r_no_sys);
    g_assert_nonnull(r_sys);

    /* System prompt adds to total token count, may shift score up */
    g_assert_cmpfloat(ai_scoring_result_get_score(r_sys), >=,
                      ai_scoring_result_get_score(r_no_sys));
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Tier classification */
    g_test_add_func("/scorer/simple-greeting",
                    test_scorer_simple_greeting);
    g_test_add_func("/scorer/simple-question",
                    test_scorer_simple_question);
    g_test_add_func("/scorer/medium-code",
                    test_scorer_medium_code);
    g_test_add_func("/scorer/complex-scores-higher-than-simple",
                    test_scorer_complex_scores_higher_than_simple);
    g_test_add_func("/scorer/reasoning-keywords",
                    test_scorer_reasoning_keywords);

    /* Score range */
    g_test_add_func("/scorer/score-monotonic",
                    test_scorer_score_monotonic);
    g_test_add_func("/scorer/confidence-range",
                    test_scorer_confidence_range);

    /* Result accessors */
    g_test_add_func("/scorer/result-signals",
                    test_scorer_result_signals);
    g_test_add_func("/scorer/result-copy",
                    test_scorer_result_copy);
    g_test_add_func("/scorer/result-format-debug",
                    test_scorer_result_format_debug);

    /* Config */
    g_test_add_func("/scorer/config-defaults",
                    test_scorer_config_defaults);
    g_test_add_func("/scorer/config-custom-boundaries",
                    test_scorer_config_custom_boundaries);
    g_test_add_func("/scorer/config-copy",
                    test_scorer_config_copy);

    /* Tier enum */
    g_test_add_func("/scorer/tier-to-string",
                    test_scorer_tier_to_string);
    g_test_add_func("/scorer/tier-from-string",
                    test_scorer_tier_from_string);

    /* Agentic */
    g_test_add_func("/scorer/agentic-detection",
                    test_scorer_agentic_detection);

    /* System prompt */
    g_test_add_func("/scorer/with-system-prompt",
                    test_scorer_with_system_prompt);

    return g_test_run();
}
