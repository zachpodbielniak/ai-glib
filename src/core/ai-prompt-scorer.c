/*
 * ai-prompt-scorer.c - Prompt complexity scoring for smart routing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * 14-dimension weighted scorer ported from ClawRouter v2.0.
 * All scoring is local — no external API calls.
 */

#include "ai-glib.h"
#include <math.h>
#include <string.h>

/* ================================================================== */
/* Tier enum                                                           */
/* ================================================================== */

static const GEnumValue prompt_tier_values[] = {
    { AI_PROMPT_TIER_SIMPLE,    "AI_PROMPT_TIER_SIMPLE",    "simple"    },
    { AI_PROMPT_TIER_MEDIUM,    "AI_PROMPT_TIER_MEDIUM",    "medium"    },
    { AI_PROMPT_TIER_COMPLEX,   "AI_PROMPT_TIER_COMPLEX",   "complex"   },
    { AI_PROMPT_TIER_REASONING, "AI_PROMPT_TIER_REASONING", "reasoning" },
    { 0, NULL, NULL }
};

GType
ai_prompt_tier_get_type(void)
{
    static GType type = 0;
    if (g_once_init_enter(&type)) {
        GType t = g_enum_register_static("AiPromptTier",
                                         prompt_tier_values);
        g_once_init_leave(&type, t);
    }
    return type;
}

const gchar *
ai_prompt_tier_to_string(AiPromptTier tier)
{
    switch (tier) {
    case AI_PROMPT_TIER_SIMPLE:    return "SIMPLE";
    case AI_PROMPT_TIER_MEDIUM:    return "MEDIUM";
    case AI_PROMPT_TIER_COMPLEX:   return "COMPLEX";
    case AI_PROMPT_TIER_REASONING: return "REASONING";
    default:                       return "UNKNOWN";
    }
}

AiPromptTier
ai_prompt_tier_from_string(const gchar *str)
{
    if (str == NULL) return AI_PROMPT_TIER_MEDIUM;
    if (g_ascii_strcasecmp(str, "simple")    == 0) return AI_PROMPT_TIER_SIMPLE;
    if (g_ascii_strcasecmp(str, "medium")    == 0) return AI_PROMPT_TIER_MEDIUM;
    if (g_ascii_strcasecmp(str, "complex")   == 0) return AI_PROMPT_TIER_COMPLEX;
    if (g_ascii_strcasecmp(str, "reasoning") == 0) return AI_PROMPT_TIER_REASONING;
    return AI_PROMPT_TIER_MEDIUM;
}

/* ================================================================== */
/* AiScoringResult boxed type                                          */
/* ================================================================== */

struct _AiScoringResult {
    gdouble      score;
    AiPromptTier tier;
    gboolean     ambiguous;      /* TRUE when confidence < threshold */
    gdouble      confidence;
    gdouble      agentic_score;
    GPtrArray   *signals;        /* (element-type utf8) */
};

AiScoringResult *
ai_scoring_result_new(void)
{
    AiScoringResult *r;

    r = g_slice_new0(AiScoringResult);
    r->tier      = AI_PROMPT_TIER_MEDIUM;
    r->signals   = g_ptr_array_new_with_free_func(g_free);

    return r;
}

AiScoringResult *
ai_scoring_result_copy(const AiScoringResult *r)
{
    AiScoringResult *c;
    guint i;

    g_return_val_if_fail(r != NULL, NULL);

    c = ai_scoring_result_new();
    c->score         = r->score;
    c->tier          = r->tier;
    c->ambiguous     = r->ambiguous;
    c->confidence    = r->confidence;
    c->agentic_score = r->agentic_score;

    for (i = 0; i < r->signals->len; i++)
        g_ptr_array_add(c->signals,
                        g_strdup(g_ptr_array_index(r->signals, i)));

    return c;
}

void
ai_scoring_result_free(AiScoringResult *r)
{
    if (r == NULL)
        return;
    g_ptr_array_unref(r->signals);
    g_slice_free(AiScoringResult, r);
}

G_DEFINE_BOXED_TYPE(AiScoringResult, ai_scoring_result,
                    ai_scoring_result_copy,
                    ai_scoring_result_free)

gdouble
ai_scoring_result_get_score(const AiScoringResult *r)
{
    g_return_val_if_fail(r != NULL, 0.0);
    return r->score;
}

AiPromptTier
ai_scoring_result_get_tier(const AiScoringResult *r)
{
    g_return_val_if_fail(r != NULL, AI_PROMPT_TIER_MEDIUM);
    return r->tier;
}

gboolean
ai_scoring_result_get_tier_is_ambiguous(const AiScoringResult *r)
{
    g_return_val_if_fail(r != NULL, TRUE);
    return r->ambiguous;
}

gdouble
ai_scoring_result_get_confidence(const AiScoringResult *r)
{
    g_return_val_if_fail(r != NULL, 0.0);
    return r->confidence;
}

gdouble
ai_scoring_result_get_agentic_score(const AiScoringResult *r)
{
    g_return_val_if_fail(r != NULL, 0.0);
    return r->agentic_score;
}

GPtrArray *
ai_scoring_result_get_signals(const AiScoringResult *r)
{
    g_return_val_if_fail(r != NULL, NULL);
    return r->signals;
}

gchar *
ai_scoring_result_format_debug(const AiScoringResult *r)
{
    GString *s;
    guint i;

    g_return_val_if_fail(r != NULL, g_strdup("(null)"));

    s = g_string_new(NULL);
    g_string_append_printf(s,
        "tier=%s confidence=%.2f score=%.3f agentic=%.2f",
        r->ambiguous ? "AMBIGUOUS" : ai_prompt_tier_to_string(r->tier),
        r->confidence, r->score, r->agentic_score);

    if (r->signals->len > 0) {
        g_string_append(s, " signals=[");
        for (i = 0; i < r->signals->len; i++) {
            if (i > 0) g_string_append(s, ", ");
            g_string_append(s, (const gchar *)g_ptr_array_index(r->signals, i));
        }
        g_string_append_c(s, ']');
    }

    return g_string_free(s, FALSE);
}

/* ================================================================== */
/* AiScorerConfig boxed type                                           */
/* ================================================================== */

struct _AiScorerConfig {
    gdouble simple_medium;
    gdouble medium_complex;
    gdouble complex_reasoning;
    gdouble confidence_threshold;
    gdouble confidence_steepness;
    guint   max_tokens_force_complex;
};

AiScorerConfig *
ai_scorer_config_new_defaults(void)
{
    AiScorerConfig *c;

    c = g_slice_new0(AiScorerConfig);
    c->simple_medium            = 0.0;
    c->medium_complex           = 0.3;
    c->complex_reasoning        = 0.5;
    c->confidence_threshold     = 0.7;
    c->confidence_steepness     = 12.0;
    c->max_tokens_force_complex = 100000;

    return c;
}

AiScorerConfig *
ai_scorer_config_copy(const AiScorerConfig *c)
{
    AiScorerConfig *copy;

    g_return_val_if_fail(c != NULL, NULL);

    copy = g_slice_dup(AiScorerConfig, c);
    return copy;
}

void
ai_scorer_config_free(AiScorerConfig *c)
{
    if (c != NULL)
        g_slice_free(AiScorerConfig, c);
}

G_DEFINE_BOXED_TYPE(AiScorerConfig, ai_scorer_config,
                    ai_scorer_config_copy,
                    ai_scorer_config_free)

void
ai_scorer_config_set_tier_boundaries(AiScorerConfig *c,
                                     gdouble simple_medium,
                                     gdouble medium_complex,
                                     gdouble complex_reasoning)
{
    g_return_if_fail(c != NULL);
    c->simple_medium      = simple_medium;
    c->medium_complex     = medium_complex;
    c->complex_reasoning  = complex_reasoning;
}

void
ai_scorer_config_set_confidence_threshold(AiScorerConfig *c,
                                          gdouble threshold)
{
    g_return_if_fail(c != NULL);
    c->confidence_threshold = threshold;
}

void
ai_scorer_config_set_confidence_steepness(AiScorerConfig *c,
                                          gdouble steepness)
{
    g_return_if_fail(c != NULL);
    c->confidence_steepness = steepness;
}

void
ai_scorer_config_set_max_tokens_force_complex(AiScorerConfig *c,
                                              guint tokens)
{
    g_return_if_fail(c != NULL);
    c->max_tokens_force_complex = tokens;
}

/* ================================================================== */
/* Built-in keyword tables (from ClawRouter v2.0)                      */
/* ================================================================== */

static const gchar * const KW_CODE[] = {
    "function", "class", "import", "def", "select", "async", "await",
    "const", "let", "var", "return", "```",
    /* CJK / Cyrillic / German */
    "\xe5\x87\xbd\xe6\x95\xb0",           /* 函数 */
    "\xe7\xb1\xbb",                         /* 类   */
    "\xe5\xaf\xbc\xe5\x85\xa5",           /* 导入 */
    "\xe5\xbc\x82\xe6\xad\xa5",           /* 异步 */
    "\xe9\x96\xa2\xe6\x95\xb0",           /* 関数 */
    "\xd1\x84\xd1\x83\xd0\xbd\xd0\xba\xd1\x86\xd0\xb8\xd1\x8f", /* функция */
    "\xd0\xba\xd0\xbb\xd0\xb0\xd1\x81\xd1\x81",                   /* класс   */
    "funktion", "klasse", "importieren",
    NULL
};

static const gchar * const KW_REASONING[] = {
    "prove", "theorem", "derive", "step by step", "chain of thought",
    "formally", "mathematical", "proof", "logically",
    "\xe8\xaf\x81\xe6\x98\x8e",   /* 证明 */
    "\xe5\xae\x9a\xe7\x90\x86",   /* 定理 */
    "\xe6\x8e\xa8\xe5\xaf\xbc",   /* 推导 */
    "\xe9\x80\x90\xe6\xad\xa5",   /* 逐步 */
    "\xd0\xb4\xd0\xbe\xd0\xba\xd0\xb0\xd0\xb7\xd0\xb0\xd1\x82\xd1\x8c", /* доказать */
    "\xd1\x82\xd0\xb5\xd0\xbe\xd1\x80\xd0\xb5\xd0\xbc\xd0\xb0",         /* теорема  */
    "\xd1\x88\xd0\xb0\xd0\xb3 \xd0\xb7\xd0\xb0 \xd1\x88\xd0\xb0\xd0\xb3\xd0\xbe\xd0\xbc", /* шаг за шагом */
    "beweisen", "beweis", "schritt f\xc3\xbcr schritt",
    "mathematisch", "logisch",
    NULL
};

static const gchar * const KW_SIMPLE[] = {
    "what is", "define", "translate", "hello", "yes or no",
    "capital of", "how old", "who is", "when was",
    "\xe4\xbb\x80\xe4\xb9\x88\xe6\x98\xaf",   /* 什么是 */
    "\xe4\xbd\xa0\xe5\xa5\xbd",                   /* 你好   */
    "\xd1\x87\xd1\x82\xd0\xbe \xd1\x82\xd0\xb0\xd0\xba\xd0\xbe\xd0\xb5", /* что такое */
    "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82",                   /* привет    */
    "was ist", "hallo", "definiere",
    NULL
};

static const gchar * const KW_TECHNICAL[] = {
    "algorithm", "optimize", "architecture", "distributed",
    "kubernetes", "microservice", "database", "infrastructure",
    "\xe7\xae\x97\xe6\xb3\x95",           /* 算法 */
    "\xe4\xbc\x98\xe5\x8c\x96",           /* 优化 */
    "\xe6\x9e\xb6\xe6\x9e\x84",           /* 架构 */
    "\xd0\xb0\xd0\xbb\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd0\xbc", /* алгоритм    */
    "\xd0\xb0\xd1\x80\xd1\x85\xd0\xb8\xd1\x82\xd0\xb5\xd0\xba\xd1\x82\xd1\x83\xd1\x80\xd0\xb0", /* архитектура */
    "algorithmus", "optimieren", "architektur", "datenbank",
    NULL
};

static const gchar * const KW_CREATIVE[] = {
    "story", "poem", "compose", "brainstorm", "creative",
    "imagine", "write a",
    "\xe6\x95\x85\xe4\xba\x8b",   /* 故事 */
    "\xe8\xaf\x97",               /* 诗   */
    "\xd0\xb8\xd1\x81\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f",             /* история */
    "\xd1\x81\xd1\x82\xd0\xb8\xd1\x85\xd0\xbe\xd1\x82\xd0\xb2\xd0\xbe\xd1\x80\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5", /* стихотворение */
    "geschichte", "gedicht", "kreativ",
    NULL
};

static const gchar * const KW_IMPERATIVE[] = {
    "build", "create", "implement", "design", "develop",
    "construct", "generate", "deploy", "configure", "set up",
    "\xe6\x9e\x84\xe5\xbb\xba",   /* 构建 */
    "\xe5\x88\x9b\xe5\xbb\xba",   /* 创建 */
    "\xe5\xae\x9e\xe7\x8e\xb0",   /* 实现 */
    "\xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd1\x82\xd1\x8c", /* создать */
    "\xd1\x80\xd0\xb5\xd0\xb0\xd0\xbb\xd0\xb8\xd0\xb7\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c", /* реализовать */
    "erstellen", "implementieren", "entwerfen", "entwickeln",
    NULL
};

static const gchar * const KW_CONSTRAINT[] = {
    "under", "at most", "at least", "within", "no more than",
    "o(", "maximum", "minimum", "limit", "budget",
    "\xe4\xb8\x8d\xe8\xb6\x85\xe8\xbf\x87",   /* 不超过 */
    "\xe8\x87\xb3\xe5\xb0\x91",                   /* 至少   */
    "\xd0\xbd\xd0\xb5 \xd0\xb1\xd0\xbe\xd0\xbb\xd0\xb5\xd0\xb5", /* не более */
    "\xd0\xbc\xd0\xb0\xd0\xba\xd1\x81\xd0\xb8\xd0\xbc\xd1\x83\xd0\xbc", /* максимум */
    "h\xc3\xb6\x63hstens", "mindestens",
    NULL
};

static const gchar * const KW_OUTPUT_FORMAT[] = {
    "json", "yaml", "xml", "table", "csv", "markdown",
    "schema", "format as", "structured",
    "\xe8\xa1\xa8\xe6\xa0\xbc",                   /* 表格     */
    "\xe7\xbb\x93\xe6\x9e\x84\xe5\x8c\x96",     /* 结构化   */
    "\xd1\x82\xd0\xb0\xd0\xb1\xd0\xbb\xd0\xb8\xd1\x86\xd0\xb0", /* таблица */
    "tabelle", "strukturiert",
    NULL
};

static const gchar * const KW_REFERENCE[] = {
    "above", "below", "previous", "following", "the docs",
    "the api", "the code", "earlier", "attached",
    "\xe4\xb8\x8a\xe9\x9d\xa2",   /* 上面 */
    "\xe6\x96\x87\xe6\xa1\xa3",   /* 文档 */
    "\xd0\xb4\xd0\xbe\xd0\xba\xd1\x83\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x82\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f", /* документация */
    "dokumentation", "der code",
    NULL
};

static const gchar * const KW_NEGATION[] = {
    "don't", "do not", "avoid", "never", "without",
    "except", "exclude", "no longer",
    "\xe4\xb8\x8d\xe8\xa6\x81",   /* 不要 */
    "\xe9\x81\xbf\xe5\x85\x8d",   /* 避免 */
    "\xd0\xbd\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xb7\xd1\x8f",     /* нельзя  */
    "\xd0\xb8\xd0\xb7\xd0\xb1\xd0\xb5\xd0\xb3\xd0\xb0\xd1\x82\xd1\x8c", /* избегать */
    "vermeide", "niemals", "ohne",
    NULL
};

static const gchar * const KW_DOMAIN[] = {
    "quantum", "fpga", "vlsi", "risc-v", "asic", "photonics",
    "genomics", "proteomics", "topological", "homomorphic",
    "zero-knowledge", "lattice-based",
    "\xe9\x87\x8f\xe5\xad\x90",   /* 量子 */
    "\xd0\xba\xd0\xb2\xd0\xb0\xd0\xbd\xd1\x82\xd0\xbe\xd0\xb2\xd1\x8b\xd0\xb9", /* квантовый */
    "quanten", "photonik", "genomik",
    NULL
};

static const gchar * const KW_AGENTIC[] = {
    "read file", "read the file", "look at", "check the", "open the",
    "edit", "modify", "update the", "change the", "write to",
    "create file", "execute", "deploy", "install", "npm", "pip",
    "compile", "after that", "and also", "once done", "step 1",
    "step 2", "fix", "debug", "until it works", "keep trying",
    "iterate", "make sure", "verify", "confirm",
    "\xe8\xaf\xbb\xe5\x8f\x96\xe6\x96\x87\xe4\xbb\xb6",   /* 读取文件 */
    "\xe7\xbc\x96\xe8\xbe\x91",                               /* 编辑     */
    "\xe4\xbf\xae\xe6\x94\xb9",                               /* 修改     */
    "\xe9\x83\xa8\xe7\xbd\xb2",                               /* 部署     */
    "\xe4\xbf\xae\xe5\xa4\x8d",                               /* 修复     */
    "\xe8\xb0\x83\xe8\xaf\x95",                               /* 调试     */
    NULL
};

/* ================================================================== */
/* Dimension weights (sum to ~1.0)                                     */
/* ================================================================== */

typedef struct {
    const gchar *name;
    gdouble      weight;
} DimensionWeight;

static const DimensionWeight WEIGHTS[] = {
    { "tokenCount",          0.08 },
    { "codePresence",        0.15 },
    { "reasoningMarkers",    0.18 },
    { "technicalTerms",      0.10 },
    { "creativeMarkers",     0.05 },
    { "simpleIndicators",    0.02 },
    { "multiStepPatterns",   0.12 },
    { "questionComplexity",  0.05 },
    { "imperativeVerbs",     0.03 },
    { "constraintCount",     0.04 },
    { "outputFormat",        0.03 },
    { "referenceComplexity", 0.02 },
    { "negationComplexity",  0.01 },
    { "domainSpecificity",   0.02 },
    { "agenticTask",         0.04 },
    { NULL, 0.0 }
};

/* ================================================================== */
/* Internal scoring helpers                                            */
/* ================================================================== */

typedef struct {
    const gchar *name;
    gdouble      score;
    const gchar *signal;   /* static string, or NULL */
} DimensionScore;

/**
 * count_keyword_matches:
 *
 * Count how many keywords from a NULL-terminated list appear
 * (case-insensitive substring) in @text.
 */
static guint
count_keyword_matches(const gchar        *text,
                      const gchar *const *keywords)
{
    guint count = 0;
    guint i;

    for (i = 0; keywords[i] != NULL; i++) {
        if (strstr(text, keywords[i]) != NULL)
            count++;
    }

    return count;
}

static DimensionScore
score_token_count(guint estimated_tokens)
{
    DimensionScore ds = { "tokenCount", 0.0, NULL };

    if (estimated_tokens < 50) {
        ds.score  = -1.0;
        ds.signal = "short";
    } else if (estimated_tokens > 500) {
        ds.score  = 1.0;
        ds.signal = "long";
    }

    return ds;
}

static DimensionScore
score_keyword(const gchar        *text,
              const gchar *const *keywords,
              const gchar        *name,
              const gchar        *signal_label,
              guint               low_thresh,
              guint               high_thresh,
              gdouble             score_none,
              gdouble             score_low,
              gdouble             score_high)
{
    DimensionScore ds = { name, score_none, NULL };
    guint matches;

    matches = count_keyword_matches(text, keywords);

    if (matches >= high_thresh) {
        ds.score  = score_high;
        ds.signal = signal_label;
    } else if (matches >= low_thresh) {
        ds.score  = score_low;
        ds.signal = signal_label;
    }

    return ds;
}

static DimensionScore
score_multi_step(const gchar *text)
{
    DimensionScore ds = { "multiStepPatterns", 0.0, NULL };
    gboolean hit = FALSE;

    /* "first...then" pattern */
    if (strstr(text, "first") != NULL && strstr(text, "then") != NULL)
        hit = TRUE;

    /* "step N" pattern */
    if (!hit) {
        const gchar *p = text;
        while ((p = strstr(p, "step ")) != NULL) {
            p += 5;
            if (*p >= '0' && *p <= '9') {
                hit = TRUE;
                break;
            }
        }
    }

    /* numbered list "N. " */
    if (!hit) {
        const gchar *p = text;
        while (*p != '\0') {
            if (*p >= '1' && *p <= '9' && *(p + 1) == '.' && *(p + 2) == ' ') {
                hit = TRUE;
                break;
            }
            p++;
        }
    }

    if (hit) {
        ds.score  = 0.5;
        ds.signal = "multi-step";
    }

    return ds;
}

static DimensionScore
score_question_complexity(const gchar *text)
{
    DimensionScore ds = { "questionComplexity", 0.0, NULL };
    guint count = 0;
    const gchar *p;

    for (p = text; *p != '\0'; p++) {
        if (*p == '?')
            count++;
    }

    if (count > 3) {
        ds.score  = 0.5;
        ds.signal = "multi-question";
    }

    return ds;
}

static gdouble
score_agentic(const gchar *text,
              gdouble     *out_agentic_score,
              const gchar **out_signal)
{
    guint matches;

    matches = count_keyword_matches(text, KW_AGENTIC);

    if (matches >= 4) {
        *out_agentic_score = 1.0;
        *out_signal = "agentic";
        return 1.0;
    }
    if (matches >= 3) {
        *out_agentic_score = 0.6;
        *out_signal = "agentic";
        return 0.6;
    }
    if (matches >= 1) {
        *out_agentic_score = 0.2;
        *out_signal = "agentic-light";
        return 0.2;
    }

    *out_agentic_score = 0.0;
    *out_signal = NULL;
    return 0.0;
}

static gdouble
calibrate_confidence(gdouble distance, gdouble steepness)
{
    return 1.0 / (1.0 + exp(-steepness * distance));
}

/* ================================================================== */
/* Main classify function                                              */
/* ================================================================== */

AiScoringResult *
ai_prompt_scorer_classify(const gchar          *prompt,
                          const gchar          *system_prompt,
                          const AiScorerConfig *config)
{
    g_autoptr(AiScorerConfig)  default_config = NULL;
    g_autofree gchar          *combined       = NULL;
    g_autofree gchar          *user_lower     = NULL;
    const gchar               *text;
    AiScoringResult           *result;
    DimensionScore             dims[15];
    guint                      ndims = 0;
    guint                      estimated_tokens;
    gdouble                    weighted_score;
    gdouble                    agentic_score;
    const gchar               *agentic_signal;
    guint                      reasoning_matches;
    gdouble                    distance;
    guint                      i;

    g_return_val_if_fail(prompt != NULL, ai_scoring_result_new());

    /* Use defaults if no config provided */
    if (config == NULL) {
        default_config = ai_scorer_config_new_defaults();
        config = default_config;
    }

    /*
     * Build combined lowercase text for most dimensions.
     * Reasoning markers use user_lower only (system prompt shouldn't
     * inflate complexity for simple user queries).
     */
    user_lower = g_utf8_strdown(prompt, -1);
    if (system_prompt != NULL) {
        g_autofree gchar *sys_lower = g_utf8_strdown(system_prompt, -1);
        combined = g_strdup_printf("%s %s", sys_lower, user_lower);
    } else {
        combined = g_strdup(user_lower);
    }
    text = combined;

    /* Rough token estimate: ~4 chars per token */
    estimated_tokens = (guint)(strlen(prompt) / 4);
    if (system_prompt != NULL)
        estimated_tokens += (guint)(strlen(system_prompt) / 4);

    /* Score all 14 dimensions */
    dims[ndims++] = score_token_count(estimated_tokens);
    dims[ndims++] = score_keyword(text, KW_CODE, "codePresence", "code",
                                  1, 2, 0.0, 0.5, 1.0);
    /* Reasoning: user prompt only */
    dims[ndims++] = score_keyword(user_lower, KW_REASONING,
                                  "reasoningMarkers", "reasoning",
                                  1, 2, 0.0, 0.7, 1.0);
    dims[ndims++] = score_keyword(text, KW_TECHNICAL, "technicalTerms",
                                  "technical", 2, 4, 0.0, 0.5, 1.0);
    dims[ndims++] = score_keyword(text, KW_CREATIVE, "creativeMarkers",
                                  "creative", 1, 2, 0.0, 0.5, 0.7);
    dims[ndims++] = score_keyword(text, KW_SIMPLE, "simpleIndicators",
                                  "simple", 1, 2, 0.0, -1.0, -1.0);
    dims[ndims++] = score_multi_step(text);
    dims[ndims++] = score_question_complexity(prompt);
    dims[ndims++] = score_keyword(text, KW_IMPERATIVE, "imperativeVerbs",
                                  "imperative", 1, 2, 0.0, 0.3, 0.5);
    dims[ndims++] = score_keyword(text, KW_CONSTRAINT, "constraintCount",
                                  "constraints", 1, 3, 0.0, 0.3, 0.7);
    dims[ndims++] = score_keyword(text, KW_OUTPUT_FORMAT, "outputFormat",
                                  "format", 1, 2, 0.0, 0.4, 0.7);
    dims[ndims++] = score_keyword(text, KW_REFERENCE, "referenceComplexity",
                                  "references", 1, 2, 0.0, 0.3, 0.5);
    dims[ndims++] = score_keyword(text, KW_NEGATION, "negationComplexity",
                                  "negation", 2, 3, 0.0, 0.3, 0.5);
    dims[ndims++] = score_keyword(text, KW_DOMAIN, "domainSpecificity",
                                  "domain-specific", 1, 2, 0.0, 0.5, 0.8);

    /* Agentic dimension (special scoring) */
    {
        DimensionScore ads = { "agenticTask", 0.0, NULL };
        ads.score = score_agentic(text, &agentic_score, &agentic_signal);
        ads.signal = agentic_signal;
        dims[ndims++] = ads;
    }

    /* Compute weighted score */
    weighted_score = 0.0;
    for (i = 0; i < ndims; i++) {
        guint w;
        for (w = 0; WEIGHTS[w].name != NULL; w++) {
            if (strcmp(dims[i].name, WEIGHTS[w].name) == 0) {
                weighted_score += dims[i].score * WEIGHTS[w].weight;
                break;
            }
        }
    }

    /* Build result */
    result = ai_scoring_result_new();
    result->score         = weighted_score;
    result->agentic_score = agentic_score;

    /* Collect signals */
    for (i = 0; i < ndims; i++) {
        if (dims[i].signal != NULL)
            g_ptr_array_add(result->signals, g_strdup(dims[i].signal));
    }

    /* Reasoning override: 2+ reasoning keywords → force REASONING */
    reasoning_matches = count_keyword_matches(user_lower, KW_REASONING);
    if (reasoning_matches >= 2) {
        gdouble conf;
        conf = calibrate_confidence(
            fmax(weighted_score, 0.3),
            config->confidence_steepness);
        result->tier       = AI_PROMPT_TIER_REASONING;
        result->confidence = fmax(conf, 0.85);
        result->ambiguous  = FALSE;
        return result;
    }

    /* Max-tokens override */
    if (estimated_tokens > config->max_tokens_force_complex
        && weighted_score < config->medium_complex)
    {
        result->tier       = AI_PROMPT_TIER_COMPLEX;
        result->confidence = 0.9;
        result->ambiguous  = FALSE;
        return result;
    }

    /* Map weighted score to tier via boundaries */
    if (weighted_score < config->simple_medium) {
        result->tier = AI_PROMPT_TIER_SIMPLE;
        distance = config->simple_medium - weighted_score;
    } else if (weighted_score < config->medium_complex) {
        result->tier = AI_PROMPT_TIER_MEDIUM;
        distance = fmin(weighted_score - config->simple_medium,
                        config->medium_complex - weighted_score);
    } else if (weighted_score < config->complex_reasoning) {
        result->tier = AI_PROMPT_TIER_COMPLEX;
        distance = fmin(weighted_score - config->medium_complex,
                        config->complex_reasoning - weighted_score);
    } else {
        result->tier = AI_PROMPT_TIER_REASONING;
        distance = weighted_score - config->complex_reasoning;
    }

    result->confidence = calibrate_confidence(distance,
                                              config->confidence_steepness);

    /* Low confidence → ambiguous */
    if (result->confidence < config->confidence_threshold) {
        result->ambiguous = TRUE;
        /* Default ambiguous to MEDIUM */
        result->tier = AI_PROMPT_TIER_MEDIUM;
    }

    return result;
}
