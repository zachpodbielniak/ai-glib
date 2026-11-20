/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-relevance-judge.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include <json-glib/json-glib.h>
#include <string.h>

/*
 * Should the assistant speak up about talk it was not addressed in? The
 * interface is one question with a 0-10 answer; AiProviderRelevanceJudge
 * asks a model. A judge that keeps failing (a revoked key, a model that
 * no longer exists) backs off instead of costing one request per line said
 * in the room forever.
 */
G_DEFINE_INTERFACE(AiRelevanceJudge, ai_relevance_judge, G_TYPE_OBJECT)
static void
ai_relevance_judge_default_init(AiRelevanceJudgeInterface *iface)
{
}

/**
 * ai_relevance_judge_judge_async:
 * @self: a judge
 * @assistant: the assistant's name
 * @lines: (array zero-terminated=1): the talk, one "[Name]: text" per line;
 *   "[Assistant]: ..." lines are what the assistant itself said last
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 */
void
ai_relevance_judge_judge_async(AiRelevanceJudge *self, const gchar *assistant,
							   const gchar *const *lines, GCancellable *cancellable,
							   GAsyncReadyCallback callback, gpointer user_data)
{
	AiRelevanceJudgeInterface *iface;
	g_return_if_fail(AI_IS_RELEVANCE_JUDGE(self));
	g_return_if_fail(lines != NULL);
	iface = AI_RELEVANCE_JUDGE_GET_IFACE(self);
	if (iface->judge_async == NULL) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Operation is not implemented");
		return;
	}
	iface->judge_async(self, assistant != NULL ? assistant : "the assistant", lines,
					   cancellable, callback, user_data);
}

/**
 * ai_relevance_judge_judge_finish:
 * @self: a judge
 * @result: asynchronous result
 * @reason: (out) (optional) (transfer full): the judge's one-line reason
 * @error: return location for an error
 *
 * Returns: the score, 0 to 10, or -1 with @error set
 */
gint
ai_relevance_judge_judge_finish(AiRelevanceJudge *self, GAsyncResult *result, gchar **reason,
								GError **error)
{
	AiRelevanceJudgeInterface *iface;
	gchar *why = NULL;
	gint score;
	if (reason != NULL)
		*reason = NULL;
	g_return_val_if_fail(AI_IS_RELEVANCE_JUDGE(self), -1);
	iface = AI_RELEVANCE_JUDGE_GET_IFACE(self);
	if (iface->judge_finish == NULL) {
		if (G_IS_TASK(result))
			g_task_propagate_int(G_TASK(result), error);
		else
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Operation is not implemented");
		return -1;
	}
	score = iface->judge_finish(self, result, &why, error);
	if (score >= 0)
		score = MIN(score, 10);
	if (reason != NULL)
		*reason = why != NULL ? why : g_strdup("");
	else
		g_free(why);
	return score;
}

/* The first line of TEXT from START, stripped. */
static gchar *
rest_of_line(const gchar *start)
{
	const gchar *end = strchr(start, '\n');
	gchar *line = end != NULL ? g_strndup(start, (gsize)(end - start)) : g_strdup(start);
	return g_strstrip(line);
}

/**
 * ai_relevance_judge_parse_reply:
 * @text: (nullable): a model's answer
 * @reason: (out) (transfer full): its reason, possibly empty
 *
 * Reads a relevance answer in any of the shapes a model gives one: a JSON
 * object with "score" and "reason"; "SCORE: 7" then "REASON: ..."; or
 * prose such as "a score of 7 out of 10". The score is clamped to 10. An
 * answer with no score in it is 0: a judge that cannot answer is not a
 * reason to speak.
 *
 * Returns: the score, 0 to 10
 */
guint
ai_relevance_judge_parse_reply(const gchar *text, gchar **reason)
{
	g_autoptr(GRegex) score_re = NULL;
	g_autoptr(GRegex) reason_re = NULL;
	g_autoptr(GMatchInfo) info = NULL;
	const gchar *open, *close;
	guint score = 0;
	g_return_val_if_fail(reason != NULL, 0);
	*reason = g_strdup("");
	if (text == NULL)
		return 0;
	/* JSON, possibly inside a code fence or after a preamble. */
	open = strchr(text, '{');
	close = strrchr(text, '}');
	if (open != NULL && close != NULL && close > open) {
		g_autoptr(JsonParser) parser = json_parser_new();
		if (json_parser_load_from_data(parser, open, close - open + 1, NULL) &&
			JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
			JsonObject *o = json_node_get_object(json_parser_get_root(parser));
			JsonNode *s = json_object_get_member(o, "score");
			if (s != NULL && JSON_NODE_HOLDS_VALUE(s) &&
				(json_node_get_value_type(s) == G_TYPE_INT64 ||
				 json_node_get_value_type(s) == G_TYPE_DOUBLE)) {
				gdouble value = json_node_get_value_type(s) == G_TYPE_INT64
									? (gdouble)json_node_get_int(s)
									: json_node_get_double(s);
				if (json_object_has_member(o, "reason") &&
					JSON_NODE_HOLDS_VALUE(json_object_get_member(o, "reason"))) {
					g_free(*reason);
					*reason = g_strstrip(g_strdup(json_object_get_string_member(o, "reason")));
				}
				return value <= 0 ? 0 : value >= 10 ? 10 : (guint)(value + 0.5);
			}
		}
	}
	/* "SCORE: 7", "**Score**: 7/10", "a score of 7". */
	score_re = g_regex_new("\\bscore\\b[^0-9\\n]{0,12}?(\\d{1,3})", G_REGEX_CASELESS, 0, NULL);
	if (g_regex_match(score_re, text, 0, &info)) {
		g_autofree gchar *digits = g_match_info_fetch(info, 1);
		score = (guint)MIN(g_ascii_strtoull(digits, NULL, 10), 10);
	}
	reason_re = g_regex_new("\\breason\\b\\**\\s*[:=-]\\s*", G_REGEX_CASELESS, 0, NULL);
	g_clear_pointer(&info, g_match_info_free);
	if (g_regex_match(reason_re, text, 0, &info)) {
		gint end = 0;
		if (g_match_info_fetch_pos(info, 0, NULL, &end)) {
			g_free(*reason);
			*reason = rest_of_line(text + end);
		}
	}
	return score;
}

/* ---- AiProviderRelevanceJudge ---- */

static const gchar default_prompt[] =
	"You are listening to a live conversation between people in a room. An AI "
	"assistant named %s is present and hears everything, but nobody addressed it. "
	"Decide how much the assistant should speak up right now, from 0 (stay quiet) "
	"to 10 (clearly should speak). Lines marked [Assistant] are what the "
	"assistant itself said last; talk that follows up on them -- a next question, "
	"a correction, another sum -- is usually meant for it.\n"
	"Score high when: a question was asked that nobody answered; someone states "
	"something that is wrong; the group is stuck or looking for information the "
	"assistant is likely to have; someone seems to be waiting for the assistant.\n"
	"Score low when: people are chatting, joking or talking about personal "
	"matters; the conversation is going fine without help; the point is already "
	"settled.\n"
	"Answer with exactly two lines and nothing else:\n"
	"SCORE: <0-10>\n"
	"REASON: <one short sentence>";

typedef struct {
	AiProvider *provider;
	guint max_tokens, failure_limit, backoff_ms;
	guint failures;
	gint64 resume_us; /* backing off until then */
} AiProviderRelevanceJudgePrivate;

enum { PROP_0, PROP_PROVIDER, PROP_MAX_TOKENS, PROP_FAILURE_LIMIT, PROP_BACKOFF, N_PROPS };
static GParamSpec *props[N_PROPS];
enum { SIGNAL_FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void judge_iface_init(AiRelevanceJudgeInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiProviderRelevanceJudge, ai_provider_relevance_judge, G_TYPE_OBJECT,
						G_ADD_PRIVATE(AiProviderRelevanceJudge)
							G_IMPLEMENT_INTERFACE(AI_TYPE_RELEVANCE_JUDGE, judge_iface_init))

static gchar *
default_build_system_prompt(AiProviderRelevanceJudge *self, const gchar *assistant)
{
	return g_strdup_printf(default_prompt, assistant);
}
static gchar *
default_build_message(AiProviderRelevanceJudge *self, const gchar *const *lines)
{
	GString *talk = g_string_new(NULL);
	guint i;
	for (i = 0; lines[i] != NULL; i++) {
		g_string_append(talk, lines[i]);
		g_string_append_c(talk, '\n');
	}
	return g_string_free(talk, FALSE);
}
static guint
default_parse(AiProviderRelevanceJudge *self, const gchar *text, gchar **reason)
{
	return ai_relevance_judge_parse_reply(text, reason);
}

typedef struct {
	GList *messages;
} Asked;
static void
asked_free(gpointer data)
{
	Asked *a = data;
	g_list_free_full(a->messages, g_object_unref);
	g_free(a);
}
static void
note_failure(AiProviderRelevanceJudge *self, const GError *error)
{
	AiProviderRelevanceJudgePrivate *priv = ai_provider_relevance_judge_get_instance_private(self);
	priv->failures++;
	g_log("ai-glib", G_LOG_LEVEL_INFO, "Relevance judge failed (%u in a row): %s",
		  priv->failures, error->message);
	if (priv->failure_limit > 0 && priv->failures >= priv->failure_limit) {
		/* Doubling from backoff-ms, at most eight times as long. */
		guint shift = MIN(priv->failures - priv->failure_limit, 3u);
		priv->resume_us =
			g_get_monotonic_time() + (gint64)priv->backoff_ms * 1000 * (gint64)(1u << shift);
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Relevance judge paused for %u s",
			  (priv->backoff_ms << shift) / 1000);
	}
	g_signal_emit(self, signals[SIGNAL_FAILED], 0, error);
}
static void
answered(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	AiProviderRelevanceJudge *self = g_task_get_source_object(task);
	AiProviderRelevanceJudgePrivate *priv = ai_provider_relevance_judge_get_instance_private(self);
	GError *error = NULL;
	g_autoptr(AiResponse) response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);
	g_autofree gchar *text = NULL;
	gchar *reason = NULL;
	guint score;
	if (response == NULL) {
		if (error == NULL)
			error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
										"The judge returned nothing");
		/* Being superseded is not the judge's failure. */
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			note_failure(self, error);
		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}
	priv->failures = 0;
	priv->resume_us = 0;
	text = ai_response_get_text(response);
	score = AI_PROVIDER_RELEVANCE_JUDGE_GET_CLASS(self)->parse(self, text, &reason);
	g_task_set_task_data(task, reason, g_free);
	g_task_return_int(task, (gssize)MIN(score, 10u));
	g_object_unref(task);
}
static void
judge_async(AiRelevanceJudge *judge, const gchar *assistant, const gchar *const *lines,
			GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	AiProviderRelevanceJudge *self = AI_PROVIDER_RELEVANCE_JUDGE(judge);
	AiProviderRelevanceJudgePrivate *priv = ai_provider_relevance_judge_get_instance_private(self);
	AiProviderRelevanceJudgeClass *klass = AI_PROVIDER_RELEVANCE_JUDGE_GET_CLASS(self);
	GTask *task = g_task_new(self, cancellable, callback, user_data);
	g_autofree gchar *system = NULL, *message = NULL;
	Asked *asked;
	if (priv->provider == NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
								"The relevance judge has no provider");
		g_object_unref(task);
		return;
	}
	if (priv->resume_us != 0 && g_get_monotonic_time() < priv->resume_us) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
								"The relevance judge is backing off after failures");
		g_object_unref(task);
		return;
	}
	system = klass->build_system_prompt(self, assistant);
	message = klass->build_message(self, lines);
	asked = g_new0(Asked, 1);
	asked->messages = g_list_append(NULL, ai_message_new_user(message));
	/* The messages must outlive the request. The task's own data slot is
	 * kept for the reason, so they ride on the task as object data. */
	g_object_set_data_full(G_OBJECT(task), "ai-relevance-messages", asked, asked_free);
	ai_provider_chat_async(priv->provider, asked->messages, system, (gint)priv->max_tokens, NULL,
						   cancellable, answered, task);
}
static gint
judge_finish(AiRelevanceJudge *judge, GAsyncResult *result, gchar **reason, GError **error)
{
	gssize score;
	g_return_val_if_fail(g_task_is_valid(result, judge), -1);
	score = g_task_propagate_int(G_TASK(result), error);
	if (score < 0)
		return -1;
	if (reason != NULL)
		*reason = g_strdup(g_task_get_task_data(G_TASK(result)));
	return (gint)score;
}
static void
judge_iface_init(AiRelevanceJudgeInterface *iface)
{
	iface->judge_async = judge_async;
	iface->judge_finish = judge_finish;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiProviderRelevanceJudgePrivate *priv =
		ai_provider_relevance_judge_get_instance_private(AI_PROVIDER_RELEVANCE_JUDGE(object));
	switch (id) {
	case PROP_PROVIDER:
		g_value_set_object(value, priv->provider);
		break;
	case PROP_MAX_TOKENS:
		g_value_set_uint(value, priv->max_tokens);
		break;
	case PROP_FAILURE_LIMIT:
		g_value_set_uint(value, priv->failure_limit);
		break;
	case PROP_BACKOFF:
		g_value_set_uint(value, priv->backoff_ms);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiProviderRelevanceJudgePrivate *priv =
		ai_provider_relevance_judge_get_instance_private(AI_PROVIDER_RELEVANCE_JUDGE(object));
	switch (id) {
	case PROP_PROVIDER:
		g_set_object(&priv->provider, g_value_get_object(value));
		priv->failures = 0;
		priv->resume_us = 0;
		break;
	case PROP_MAX_TOKENS:
		priv->max_tokens = g_value_get_uint(value);
		break;
	case PROP_FAILURE_LIMIT:
		priv->failure_limit = g_value_get_uint(value);
		break;
	case PROP_BACKOFF:
		priv->backoff_ms = g_value_get_uint(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
dispose(GObject *object)
{
	AiProviderRelevanceJudgePrivate *priv =
		ai_provider_relevance_judge_get_instance_private(AI_PROVIDER_RELEVANCE_JUDGE(object));
	g_clear_object(&priv->provider);
	G_OBJECT_CLASS(ai_provider_relevance_judge_parent_class)->dispose(object);
}
static void
ai_provider_relevance_judge_class_init(AiProviderRelevanceJudgeClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->dispose = dispose;
	klass->build_system_prompt = default_build_system_prompt;
	klass->build_message = default_build_message;
	klass->parse = default_parse;
	/**
	 * AiProviderRelevanceJudge:provider:
	 *
	 * The model that scores. A small fast one is enough. It must not be a
	 * provider object another thread is using, such as a voice session's
	 * conversation's.
	 */
	props[PROP_PROVIDER] =
		g_param_spec_object("provider", "Provider", "The model that scores", AI_TYPE_PROVIDER,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiProviderRelevanceJudge:max-tokens:
	 *
	 * The longest answer asked for.
	 */
	props[PROP_MAX_TOKENS] = g_param_spec_uint("max-tokens", "Maximum tokens",
											   "Longest answer asked for", 16, 8192, 200,
											   G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
												   G_PARAM_STATIC_STRINGS);
	/**
	 * AiProviderRelevanceJudge:failure-limit:
	 *
	 * After this many failures in a row, stop asking for
	 * #AiProviderRelevanceJudge:backoff-ms (doubling with each further
	 * failure, to eight times). 0 never backs off.
	 */
	props[PROP_FAILURE_LIMIT] = g_param_spec_uint(
		"failure-limit", "Failure limit", "Failures in a row before backing off", 0, 1000, 3,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiProviderRelevanceJudge:backoff-ms:
	 *
	 * How long the first pause after #AiProviderRelevanceJudge:failure-limit
	 * failures lasts.
	 */
	props[PROP_BACKOFF] = g_param_spec_uint("backoff-ms", "Back-off", "First pause after failures",
											0, 3600000, 60000,
											G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
												G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
	/**
	 * AiProviderRelevanceJudge::failed:
	 * @self: the judge
	 * @error: why
	 *
	 * The model could not be asked or did not answer. Cancellation is not
	 * a failure.
	 */
	signals[SIGNAL_FAILED] = g_signal_new(
		"failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET(AiProviderRelevanceJudgeClass, failed), NULL, NULL, NULL, G_TYPE_NONE,
		1, G_TYPE_ERROR);
}
static void
ai_provider_relevance_judge_init(AiProviderRelevanceJudge *self)
{
}

/**
 * ai_provider_relevance_judge_new:
 * @provider: (nullable): the model that scores
 *
 * Returns: (transfer full): a new judge
 */
AiProviderRelevanceJudge *
ai_provider_relevance_judge_new(AiProvider *provider)
{
	return g_object_new(AI_TYPE_PROVIDER_RELEVANCE_JUDGE, "provider", provider, NULL);
}
