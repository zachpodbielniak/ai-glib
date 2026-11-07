/*
 * ai-libresearch-search.c - libresearch metasearch provider
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include <string.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "convenience/ai-libresearch-search.h"
#include "convenience/ai-search-provider.h"
#include "convenience/ai-search-http.h"
#include "core/ai-error.h"
#include "core/ai-json-util.h"

#define LIBRESEARCH_DEFAULT_COUNT     10
#define LIBRESEARCH_MAX_COUNT         100
#define LIBRESEARCH_TIMEOUT_S         30
#define LIBRESEARCH_RESEARCH_TIMEOUT_S 900
#define LIBRESEARCH_USER_AGENT        "ai-glib (libresearch client)"

struct _AiLibresearchSearch
{
	GObject      parent_instance;
	gchar       *endpoint;          /* owned, no trailing slash */
	gchar       *api_key;           /* owned, nullable */
	guint        timeout;           /* seconds, ordinary requests */
	guint        research_timeout;  /* seconds, research and research queries */
	SoupSession *session;           /* owned, ordinary requests */
	SoupSession *long_session;      /* owned, requests that take minutes */
};

static void ai_libresearch_search_iface_init(AiSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(
	AiLibresearchSearch,
	ai_libresearch_search,
	G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_SEARCH_PROVIDER, ai_libresearch_search_iface_init)
)

enum
{
	PROP_0,
	PROP_ENDPOINT,
	PROP_API_KEY,
	PROP_TIMEOUT,
	PROP_RESEARCH_TIMEOUT,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* ------------------------------------------------------------------
 * Request building
 * ------------------------------------------------------------------ */

/*
 * One query-string parameter, escaped.  The first parameter takes '?',
 * every later one '&' -- read from the URL itself rather than from a
 * counter, so a caller cannot get the two out of step.
 */
static void
append_param(
	GString     *url,
	const gchar *key,
	const gchar *value
){
	g_autofree gchar *enc = NULL;

	if (value == NULL)
		return;

	enc = g_uri_escape_string(value, NULL, FALSE);
	g_string_append_c(url, strchr(url->str, '?') != NULL ? '&' : '?');
	g_string_append_printf(url, "%s=%s", key, enc);
}

static void
append_uint_param(
	GString     *url,
	const gchar *key,
	guint        value
){
	g_autofree gchar *text = NULL;

	text = g_strdup_printf("%u", value);
	append_param(url, key, text);
}

static GString *
url_for(
	AiLibresearchSearch *self,
	const gchar         *path
){
	GString *url;

	url = g_string_new(self->endpoint);
	g_string_append(url, path);
	return url;
}

/*
 * libresearch's freshness vocabulary is time_range: any, day, week,
 * month, year.  NULL omits the parameter, which the server reads as any.
 */
static const gchar *
time_range_for(
	AiSearchFreshness freshness
){
	switch (freshness)
	{
	case AI_SEARCH_FRESHNESS_DAY:   return "day";
	case AI_SEARCH_FRESHNESS_WEEK:  return "week";
	case AI_SEARCH_FRESHNESS_MONTH: return "month";
	case AI_SEARCH_FRESHNESS_YEAR:  return "year";
	case AI_SEARCH_FRESHNESS_ANY:   return NULL;
	}
	return NULL;
}

static const gchar *
safesearch_for(
	AiSearchSafeSearch safe
){
	switch (safe)
	{
	case AI_SEARCH_SAFE_OFF:      return "off";
	case AI_SEARCH_SAFE_STRICT:   return "strict";
	case AI_SEARCH_SAFE_MODERATE: return "moderate";
	}
	return "moderate";
}

/*
 * The search URL, shared by the synchronous provider vfunc and the async
 * request so the two cannot map an option differently.
 */
static gchar *
build_search_url(
	AiLibresearchSearch *self,
	const gchar         *query,
	AiSearchOptions     *options,
	const gchar         *category
){
	GString          *url;
	g_autofree gchar *full_query = NULL;
	guint             count      = LIBRESEARCH_DEFAULT_COUNT;
	const gchar      *site       = NULL;
	const gchar      *language   = NULL;
	const gchar      *time_range = NULL;
	const gchar      *safe       = NULL;
	guint             offset     = 0;

	if (options != NULL)
	{
		count      = ai_search_options_get_count(options);
		site       = ai_search_options_get_site(options);
		language   = ai_search_options_get_language(options);
		time_range = time_range_for(ai_search_options_get_freshness(options));
		safe       = safesearch_for(ai_search_options_get_safesearch(options));
		offset     = ai_search_options_get_offset(options);
	}

	if (count == 0)
		count = LIBRESEARCH_DEFAULT_COUNT;
	if (count > LIBRESEARCH_MAX_COUNT)
		count = LIBRESEARCH_MAX_COUNT;

	/*
	 * libresearch reads site: inside the query and pushes it down to each
	 * engine (or checks it locally where an engine cannot), so a site
	 * restriction rides in the text like every other provider's.
	 */
	if (site != NULL && *site != '\0')
		full_query = g_strdup_printf("%s site:%s", query, site);
	else
		full_query = g_strdup(query);

	url = url_for(self, "/api/v1/search");
	append_param(url, "q", full_query);
	append_param(url, "format", "json");
	append_uint_param(url, "limit", count);

	/* The API pages by page number; an offset lands on the page holding it. */
	if (offset > 0)
		append_uint_param(url, "page", (offset / count) + 1);

	if (category != NULL && *category != '\0')
		append_param(url, "category", category);
	if (language != NULL && *language != '\0')
		append_param(url, "lang", language);
	if (time_range != NULL)
		append_param(url, "time_range", time_range);
	if (safe != NULL)
		append_param(url, "safesearch", safe);

	return g_string_free(url, FALSE);
}

/* ------------------------------------------------------------------
 * Errors
 * ------------------------------------------------------------------ */

/*
 * Every libresearch error is one envelope:
 *   {"error": {"code": "...", "message": "...", "detail": "...", "line": N}}
 * The HTTP status decides the AI_ERROR code; the envelope supplies the
 * words, including invalid_query's caret excerpt, which is the only thing
 * that tells a model where its LQL went wrong.
 */
static gint
error_code_for_status(
	guint status
){
	if (status == 401)
		return AI_ERROR_INVALID_API_KEY;
	if (status == 403)
		return AI_ERROR_PERMISSION_DENIED;
	if (status == 404)
		return AI_ERROR_NOT_SUPPORTED;
	if (status == 429)
		return AI_ERROR_RATE_LIMITED;
	if (status == 503)
		return AI_ERROR_SERVICE_UNAVAILABLE;
	if (status == 504)
		return AI_ERROR_TIMEOUT;
	if (status >= 500)
		return AI_ERROR_SERVER_ERROR;
	if (status >= 400)
		return AI_ERROR_INVALID_REQUEST;
	return AI_ERROR_INVALID_RESPONSE;
}

static GError *
error_from_response(
	AiLibresearchSearch *self,
	guint                status,
	GBytes              *body
){
	g_autoptr(JsonParser) parser = NULL;
	JsonObject           *root;
	JsonObject           *envelope;
	const gchar          *code    = NULL;
	const gchar          *message = NULL;
	const gchar          *detail  = NULL;
	gconstpointer         data;
	gsize                 len     = 0;
	GString              *text;

	data = body != NULL ? g_bytes_get_data(body, &len) : NULL;
	parser = json_parser_new();
	if (data != NULL && len > 0 &&
	    json_parser_load_from_data(parser, (const gchar *)data, (gssize)len, NULL))
	{
		root = ai_json_root_object(parser);
		envelope = ai_json_get_object(root, "error");
		code = ai_json_get_string(envelope, "code", NULL);
		message = ai_json_get_string(envelope, "message", NULL);
		detail = ai_json_get_string(envelope, "detail", NULL);
	}

	text = g_string_new("libresearch");
	if (code != NULL)
		g_string_append_printf(text, " %s", code);
	g_string_append_printf(text, " (HTTP %u from %s)", status, self->endpoint);
	g_string_append_printf(text, ": %s",
	                       message != NULL ? message : soup_status_get_phrase(status));
	if (detail != NULL && *detail != '\0')
		g_string_append_printf(text, "\n%s", detail);

	{
		GError *error = g_error_new_literal(AI_ERROR, error_code_for_status(status), text->str);

		g_string_free(text, TRUE);
		return error;
	}
}

/* ------------------------------------------------------------------
 * The asynchronous request
 * ------------------------------------------------------------------ */

typedef struct
{
	SoupMessage *msg;   /* owned */
} RequestData;

static void
request_data_free(
	gpointer data
){
	RequestData *rd = (RequestData *)data;

	g_clear_object(&rd->msg);
	g_free(rd);
}

static void
on_request_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask)      task   = (GTask *)user_data;
	AiLibresearchSearch  *self   = AI_LIBRESEARCH_SEARCH(g_task_get_source_object(task));
	RequestData          *rd     = (RequestData *)g_task_get_task_data(task);
	g_autoptr(GBytes)     body   = NULL;
	g_autoptr(GError)     error  = NULL;
	g_autoptr(JsonParser) parser = NULL;
	gconstpointer         data;
	gsize                 len    = 0;
	guint                 status;
	JsonNode             *root;

	body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	if (body == NULL)
	{
		/*
		 * A transport failure, not an answer.  Cancelled and timed out are
		 * kept apart from unreachable, because the remedy differs: an
		 * unreachable server is a configuration question, a timeout is a
		 * question about the request.
		 */
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_task_return_new_error(task, AI_ERROR, AI_ERROR_CANCELLED,
			                        "libresearch request cancelled");
		else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
			g_task_return_new_error(task, AI_ERROR, AI_ERROR_TIMEOUT,
			                        "libresearch at %s did not answer in time: %s",
			                        self->endpoint, error->message);
		else
			g_task_return_new_error(task, AI_ERROR, AI_ERROR_NETWORK_ERROR,
			                        "libresearch at %s is unreachable: %s",
			                        self->endpoint, error->message);
		return;
	}

	status = soup_message_get_status(rd->msg);
	if (status < 200 || status >= 300)
	{
		g_task_return_error(task, error_from_response(self, status, body));
		return;
	}

	data = g_bytes_get_data(body, &len);
	parser = json_parser_new();
	if (data == NULL || len == 0 ||
	    !json_parser_load_from_data(parser, (const gchar *)data, (gssize)len, &error))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
		                        "libresearch at %s answered something that is not JSON%s%s",
		                        self->endpoint,
		                        error != NULL ? ": " : "",
		                        error != NULL ? error->message : "");
		return;
	}

	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) || json_node_get_object(root) == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
		                        "libresearch at %s answered JSON that is not a document",
		                        self->endpoint);
		return;
	}

	g_task_return_pointer(task, json_node_copy(root), (GDestroyNotify)json_node_unref);
}

/*
 * Sends one GET and hands the parsed document to the task.  @long_running
 * picks the session whose idle timeout fits a request the server answers
 * only after minutes of work.
 */
static void
send_get(
	AiLibresearchSearch *self,
	gchar               *url,          /* (transfer full) */
	gboolean             long_running,
	gpointer             source_tag,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask              *task;
	RequestData        *rd;
	SoupMessageHeaders *headers;
	g_autofree gchar   *owned_url = url;
	g_autofree gchar   *bearer    = NULL;

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, source_tag);

	rd = g_new0(RequestData, 1);
	rd->msg = soup_message_new(SOUP_METHOD_GET, owned_url);
	g_task_set_task_data(task, rd, request_data_free);

	if (rd->msg == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		                        "libresearch endpoint %s does not make a valid URL",
		                        self->endpoint);
		g_object_unref(task);
		return;
	}

	headers = soup_message_get_request_headers(rd->msg);
	soup_message_headers_replace(headers, "Accept", "application/json");
	soup_message_headers_replace(headers, "User-Agent", LIBRESEARCH_USER_AGENT);
	if (self->api_key != NULL && *self->api_key != '\0')
	{
		bearer = g_strdup_printf("Bearer %s", self->api_key);
		soup_message_headers_replace(headers, "Authorization", bearer);
	}

	/* The task reference travels with the callback, which drops it. */
	soup_session_send_and_read_async(long_running ? self->long_session : self->session,
	                                 rd->msg, G_PRIORITY_DEFAULT, cancellable,
	                                 on_request_done, task);
}

/* ------------------------------------------------------------------
 * Public requests
 * ------------------------------------------------------------------ */

void
ai_libresearch_search_web_async(
	AiLibresearchSearch *self,
	const gchar         *query,
	AiSearchOptions     *options,
	const gchar         *category,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_return_if_fail(AI_IS_LIBRESEARCH_SEARCH(self));
	g_return_if_fail(query != NULL);

	send_get(self, build_search_url(self, query, options, category), FALSE,
	         ai_libresearch_search_web_async, cancellable, callback, user_data);
}

void
ai_libresearch_search_fetch_async(
	AiLibresearchSearch *self,
	const gchar         *url,
	guint                max_chars,
	const gchar         *archive,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GString *req;

	g_return_if_fail(AI_IS_LIBRESEARCH_SEARCH(self));
	g_return_if_fail(url != NULL);

	req = url_for(self, "/api/v1/fetch");
	append_param(req, "url", url);
	if (max_chars > 0)
		append_uint_param(req, "max_chars", max_chars);
	if (archive != NULL && *archive != '\0')
		append_param(req, "archive", archive);

	send_get(self, g_string_free(req, FALSE), FALSE,
	         ai_libresearch_search_fetch_async, cancellable, callback, user_data);
}

/*
 * Whether an LQL query may take minutes.  A `research` source does: the
 * server lengthens the query's own time limit for it, so the client must
 * not give up first.  Matching the word anywhere errs on the side of
 * waiting, which costs nothing when the answer comes back quickly.
 */
static gboolean
query_is_long_running(
	const gchar *lql
){
	return strstr(lql, "research") != NULL;
}

void
ai_libresearch_search_query_async(
	AiLibresearchSearch *self,
	const gchar         *lql,
	gboolean             explain,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GString *req;

	g_return_if_fail(AI_IS_LIBRESEARCH_SEARCH(self));
	g_return_if_fail(lql != NULL);

	req = url_for(self, "/api/v1/query");
	append_param(req, "q", lql);
	append_param(req, "format", "json");
	if (explain)
		append_param(req, "explain", "1");

	send_get(self, g_string_free(req, FALSE), !explain && query_is_long_running(lql),
	         ai_libresearch_search_query_async, cancellable, callback, user_data);
}

void
ai_libresearch_search_research_async(
	AiLibresearchSearch *self,
	const gchar         *question,
	gboolean             deep,
	guint                pages,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GString *req;

	g_return_if_fail(AI_IS_LIBRESEARCH_SEARCH(self));
	g_return_if_fail(question != NULL);

	req = url_for(self, "/api/v1/research");
	append_param(req, "q", question);
	append_param(req, "format", "json");
	if (deep)
		append_param(req, "deep", "1");
	if (pages > 0)
		append_uint_param(req, "pages", pages);

	send_get(self, g_string_free(req, FALSE), TRUE,
	         ai_libresearch_search_research_async, cancellable, callback, user_data);
}

void
ai_libresearch_search_environment_async(
	AiLibresearchSearch *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GString *req;

	g_return_if_fail(AI_IS_LIBRESEARCH_SEARCH(self));

	req = url_for(self, "/api/v1/query/environment");
	send_get(self, g_string_free(req, FALSE), FALSE,
	         ai_libresearch_search_environment_async, cancellable, callback, user_data);
}

JsonNode *
ai_libresearch_search_request_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
){
	g_return_val_if_fail(AI_IS_LIBRESEARCH_SEARCH(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return (JsonNode *)g_task_propagate_pointer(G_TASK(result), error);
}

JsonNode *
ai_libresearch_search_web_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
){
	return ai_libresearch_search_request_finish(self, result, error);
}

JsonNode *
ai_libresearch_search_fetch_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
){
	return ai_libresearch_search_request_finish(self, result, error);
}

JsonNode *
ai_libresearch_search_query_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
){
	return ai_libresearch_search_request_finish(self, result, error);
}

JsonNode *
ai_libresearch_search_research_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
){
	return ai_libresearch_search_request_finish(self, result, error);
}

JsonNode *
ai_libresearch_search_environment_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
){
	return ai_libresearch_search_request_finish(self, result, error);
}

/* ------------------------------------------------------------------
 * Converting and formatting documents
 * ------------------------------------------------------------------ */

static JsonObject *
document_object(
	JsonNode *document
){
	if (document == NULL || !JSON_NODE_HOLDS_OBJECT(document))
		return NULL;
	return json_node_get_object(document);
}

/* The engines that found a result, joined: "ddg, bing". */
static gchar *
joined_strings(
	JsonArray *array
){
	GString *out;
	guint    i;
	guint    n;

	if (array == NULL)
		return NULL;

	out = g_string_new(NULL);
	n = json_array_get_length(array);
	for (i = 0; i < n; i++)
	{
		const gchar *s = ai_json_array_get_string(array, i, NULL);

		if (s == NULL)
			continue;
		if (out->len > 0)
			g_string_append(out, ", ");
		g_string_append(out, s);
	}

	if (out->len == 0)
	{
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

GList *
ai_libresearch_results_from_document(
	JsonNode *document
){
	JsonArray *results;
	GList     *list = NULL;
	guint      n;
	guint      i;

	results = ai_json_get_array(document_object(document), "results");
	if (results == NULL)
		return NULL;

	n = json_array_get_length(results);
	for (i = 0; i < n; i++)
	{
		JsonObject       *item = ai_json_array_get_object(results, i);
		AiSearchResult   *res;
		g_autofree gchar *engines = NULL;
		const gchar      *published;
		gint64            position;

		if (item == NULL)
			continue;

		res = ai_search_result_new(ai_json_get_string(item, "title", NULL),
		                           ai_json_get_string(item, "url", NULL),
		                           ai_json_get_string(item, "snippet", NULL));

		position = ai_json_get_int(item, "position", (gint64)(i + 1));
		ai_search_result_set_rank(res, position > 0 ? (guint)position : i + 1);

		engines = joined_strings(ai_json_get_array(item, "engines"));
		if (engines != NULL)
			ai_search_result_set_source(res, engines);

		published = ai_json_get_string(item, "published", NULL);
		if (published != NULL)
			ai_search_result_set_age(res, published);

		list = g_list_prepend(list, res);
	}

	return g_list_reverse(list);
}

/*
 * The engines that did not answer.  A partial failure is still HTTP 200,
 * so without this line a model reads three results from one engine as the
 * whole of the web's opinion.
 */
static void
append_failed_engines(
	GString    *out,
	JsonObject *doc
){
	JsonArray *engines;
	GString   *failed;
	guint      n;
	guint      i;

	engines = ai_json_get_array(doc, "engines");
	if (engines == NULL)
		return;

	failed = g_string_new(NULL);
	n = json_array_get_length(engines);
	for (i = 0; i < n; i++)
	{
		JsonObject  *e = ai_json_array_get_object(engines, i);
		const gchar *state;

		if (e == NULL)
			continue;
		state = ai_json_get_string(e, "state", NULL);
		if (state == NULL || g_strcmp0(state, "ok") == 0)
			continue;
		if (failed->len > 0)
			g_string_append(failed, ", ");
		g_string_append_printf(failed, "%s (%s)",
		                       ai_json_get_string(e, "id", "?"), state);
	}

	if (failed->len > 0)
		g_string_append_printf(out, "\nEngines that did not answer: %s\n", failed->str);
	g_string_free(failed, TRUE);
}

gchar *
ai_libresearch_format_search(
	JsonNode *document,
	guint     max_results
){
	JsonObject *doc;
	JsonArray  *infoboxes;
	JsonArray  *results;
	GString    *out;
	guint       n;
	guint       shown = 0;
	guint       i;

	doc = document_object(document);
	out = g_string_new(NULL);

	/* Instant answers first: when there is one, it is usually the answer. */
	infoboxes = ai_json_get_array(doc, "infoboxes");
	n = infoboxes != NULL ? json_array_get_length(infoboxes) : 0;
	for (i = 0; i < n; i++)
	{
		JsonObject  *box = ai_json_array_get_object(infoboxes, i);
		const gchar *body;

		if (box == NULL)
			continue;
		body = ai_json_get_string(box, "body", NULL);
		if (body == NULL || *body == '\0')
			continue;
		g_string_append_printf(out, "Answer (%s): %s\n",
		                       ai_json_get_string(box, "provider", "instant answer"), body);
		if (ai_json_get_string(box, "url", NULL) != NULL)
			g_string_append_printf(out, "   URL: %s\n", ai_json_get_string(box, "url", NULL));
	}
	if (out->len > 0)
		g_string_append_c(out, '\n');

	results = ai_json_get_array(doc, "results");
	n = results != NULL ? json_array_get_length(results) : 0;
	for (i = 0; i < n; i++)
	{
		JsonObject       *item = ai_json_array_get_object(results, i);
		g_autofree gchar *engines = NULL;
		const gchar      *published;
		const gchar      *snippet;

		if (item == NULL)
			continue;
		if (max_results > 0 && shown >= max_results)
			break;
		shown++;

		g_string_append_printf(out, "%u. %s\n", shown,
		                       ai_json_get_string(item, "title", "(untitled)"));
		g_string_append_printf(out, "   URL: %s\n", ai_json_get_string(item, "url", ""));

		engines = joined_strings(ai_json_get_array(item, "engines"));
		published = ai_json_get_string(item, "published", NULL);
		if (engines != NULL || published != NULL)
		{
			g_string_append(out, "   ");
			if (engines != NULL)
				g_string_append_printf(out, "Engines: %s", engines);
			if (published != NULL)
				g_string_append_printf(out, "%sPublished: %s",
				                       engines != NULL ? " \xc2\xb7 " : "", published);
			g_string_append_c(out, '\n');
		}

		snippet = ai_json_get_string(item, "snippet", NULL);
		if (snippet != NULL && *snippet != '\0')
			g_string_append_printf(out, "   %s\n", snippet);
	}

	if (shown == 0 && out->len == 0)
		g_string_append(out, "No results.\n");
	else if (max_results > 0 && n > shown)
		g_string_append_printf(out, "(%u more results not shown)\n", n - shown);

	append_failed_engines(out, doc);
	return g_string_free(out, FALSE);
}

gchar *
ai_libresearch_format_fetch(
	JsonNode *document
){
	JsonObject  *doc;
	GString     *out;
	const gchar *title;
	const gchar *archive_url;
	const gchar *text;

	doc = document_object(document);
	out = g_string_new(NULL);

	title = ai_json_get_string(doc, "title", NULL);
	if (title != NULL && *title != '\0')
		g_string_append_printf(out, "Title: %s\n", title);
	g_string_append_printf(out, "URL: %s\n", ai_json_get_string(doc, "url", ""));

	/* A snapshot is a different document from the live page; say which. */
	archive_url = ai_json_get_string(doc, "archive_url", NULL);
	if (archive_url != NULL)
		g_string_append_printf(out, "Read from the Wayback Machine: %s (archived %s)\n",
		                       archive_url, ai_json_get_string(doc, "archived_at", "?"));

	text = ai_json_get_string(doc, "text", NULL);
	g_string_append_c(out, '\n');
	g_string_append(out, text != NULL ? text : "(the page has no readable text)");
	g_string_append_c(out, '\n');

	if (ai_json_get_boolean(doc, "truncated", FALSE))
		g_string_append(out, "\n[truncated by the server; ask for more with max_chars]\n");

	return g_string_free(out, FALSE);
}

/* One LQL value as text: NULL, strings as they are, anything else as JSON. */
static gchar *
value_text(
	JsonNode *value
){
	if (value == NULL || JSON_NODE_HOLDS_NULL(value))
		return g_strdup("NULL");
	if (JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING)
		return g_strdup(json_node_get_string(value));
	return json_to_string(value, FALSE);
}

gchar *
ai_libresearch_format_query(
	JsonNode *document,
	guint     max_rows
){
	JsonObject  *doc;
	JsonArray   *columns;
	JsonArray   *rows;
	GString     *out;
	const gchar *explain;
	guint        ncols;
	guint        nrows;
	guint        shown = 0;
	guint        i;
	guint        c;

	doc = document_object(document);
	out = g_string_new(NULL);

	explain = ai_json_get_string(doc, "explain", NULL);
	if (explain != NULL)
	{
		g_string_append_printf(out, "Plan for: %s\n\n%s\n",
		                       ai_json_get_string(doc, "query", ""), explain);
		return g_string_free(out, FALSE);
	}

	columns = ai_json_get_array(doc, "columns");
	rows = ai_json_get_array(doc, "rows");
	ncols = columns != NULL ? json_array_get_length(columns) : 0;
	nrows = rows != NULL ? json_array_get_length(rows) : 0;

	g_string_append_printf(out, "Query: %s\n", ai_json_get_string(doc, "query", ""));
	g_string_append_printf(out, "%u row%s", nrows, nrows == 1 ? "" : "s");
	if (ncols > 0)
	{
		g_string_append(out, " (columns:");
		for (c = 0; c < ncols; c++)
		{
			JsonObject *col = ai_json_array_get_object(columns, c);

			g_string_append_printf(out, "%s %s %s", c == 0 ? "" : ",",
			                       ai_json_get_string(col, "name", "?"),
			                       ai_json_get_string(col, "type", "?"));
		}
		g_string_append_c(out, ')');
	}
	g_string_append(out, "\n");

	if (ai_json_get_boolean(doc, "confidential", FALSE))
		g_string_append(out, "(a confidential engine answered: model functions were refused)\n");

	for (i = 0; i < nrows; i++)
	{
		JsonObject  *row = ai_json_array_get_object(rows, i);
		const gchar *kind;

		if (row == NULL)
			continue;
		if (max_rows > 0 && shown >= max_rows)
			break;
		shown++;

		/*
		 * A research query's first row is the answer itself; shown as one
		 * more key/value block it reads as data rather than as the answer.
		 */
		kind = ai_json_get_string(row, "kind", NULL);
		if (g_strcmp0(kind, "answer") == 0 && ai_json_get_string(row, "text", NULL) != NULL)
		{
			g_string_append_printf(out, "\nAnswer:\n%s\n", ai_json_get_string(row, "text", ""));
			continue;
		}

		g_string_append_printf(out, "\n%u.", shown);
		for (c = 0; c < ncols; c++)
		{
			JsonObject       *col  = ai_json_array_get_object(columns, c);
			const gchar      *name = ai_json_get_string(col, "name", NULL);
			g_autofree gchar *text = NULL;

			if (name == NULL)
				continue;
			text = value_text(json_object_get_member(row, name));
			g_string_append_printf(out, "%s%s: %s", c == 0 ? " " : "\n   ", name, text);
		}
		g_string_append_c(out, '\n');
	}

	if (max_rows > 0 && nrows > shown)
		g_string_append_printf(out, "\n(%u more rows not shown; add | limit N or narrow the filter)\n",
		                       nrows - shown);

	append_failed_engines(out, doc);
	return g_string_free(out, FALSE);
}

gchar *
ai_libresearch_format_research(
	JsonNode *document
){
	JsonObject *doc;
	JsonArray  *sources;
	GString    *out;
	guint       n;
	guint       i;

	doc = document_object(document);
	out = g_string_new(NULL);

	g_string_append_printf(out, "%s\n", ai_json_get_string(doc, "answer", "(no answer)"));

	sources = ai_json_get_array(doc, "sources");
	n = sources != NULL ? json_array_get_length(sources) : 0;
	if (n > 0)
		g_string_append(out, "\nSources:\n");
	for (i = 0; i < n; i++)
	{
		JsonObject *src = ai_json_array_get_object(sources, i);
		GString    *notes;

		if (src == NULL)
			continue;

		/* An uncited or unread source supports nothing in the answer. */
		notes = g_string_new(NULL);
		if (!ai_json_get_boolean(src, "read", TRUE))
			g_string_append(notes, "not read");
		if (!ai_json_get_boolean(src, "cited", TRUE))
			g_string_append_printf(notes, "%snot cited", notes->len > 0 ? ", " : "");
		if (ai_json_get_string(src, "archive_url", NULL) != NULL)
			g_string_append_printf(notes, "%sarchived copy %s", notes->len > 0 ? ", " : "",
			                       ai_json_get_string(src, "archive_url", NULL));

		g_string_append_printf(out, "[%" G_GINT64_FORMAT "] %s -- %s%s%s%s\n",
		                       ai_json_get_int(src, "n", (gint64)(i + 1)),
		                       ai_json_get_string(src, "title", "(untitled)"),
		                       ai_json_get_string(src, "url", ""),
		                       notes->len > 0 ? " (" : "", notes->str,
		                       notes->len > 0 ? ")" : "");
		g_string_free(notes, TRUE);
	}

	if (ai_json_get_int(doc, "dropped_citations", 0) > 0)
		g_string_append_printf(out, "\n(%" G_GINT64_FORMAT " citations named no source and were dropped)\n",
		                       ai_json_get_int(doc, "dropped_citations", 0));

	return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------
 * AiSearchProvider
 * ------------------------------------------------------------------ */

static GList *
ai_libresearch_search_do_search(
	AiSearchProvider  *provider,
	const gchar       *query,
	AiSearchOptions   *options,
	GCancellable      *cancellable,
	GError           **error
){
	AiLibresearchSearch *self = AI_LIBRESEARCH_SEARCH(provider);
	g_autofree gchar    *url  = NULL;
	g_autofree gchar    *bearer = NULL;
	g_autoptr(JsonNode)  root = NULL;
	const gchar         *headers[3] = { NULL, NULL, NULL };

	url = build_search_url(self, query, options, NULL);

	if (self->api_key != NULL && *self->api_key != '\0')
	{
		bearer = g_strdup_printf("Bearer %s", self->api_key);
		headers[0] = "Authorization";
		headers[1] = bearer;
	}

	root = ai_search_http_get_json(self->session, url, headers, cancellable, error);
	if (root == NULL)
		return NULL;

	return ai_libresearch_results_from_document(root);
}

static void
ai_libresearch_search_iface_init(
	AiSearchProviderInterface *iface
){
	iface->search = ai_libresearch_search_do_search;
}

/* ------------------------------------------------------------------
 * GObject
 * ------------------------------------------------------------------ */

static void
set_endpoint(
	AiLibresearchSearch *self,
	const gchar         *endpoint
){
	gsize len;

	g_clear_pointer(&self->endpoint, g_free);
	if (endpoint == NULL || *endpoint == '\0')
		endpoint = AI_LIBRESEARCH_DEFAULT_ENDPOINT;

	/* Paths are appended with a leading slash; a trailing one would double it. */
	self->endpoint = g_strdup(endpoint);
	len = strlen(self->endpoint);
	while (len > 0 && self->endpoint[len - 1] == '/')
		self->endpoint[--len] = '\0';
}

static void
ai_libresearch_search_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiLibresearchSearch *self = AI_LIBRESEARCH_SEARCH(object);

	switch (prop_id)
	{
	case PROP_ENDPOINT:
		g_value_set_string(value, self->endpoint);
		break;
	case PROP_API_KEY:
		g_value_set_string(value, self->api_key);
		break;
	case PROP_TIMEOUT:
		g_value_set_uint(value, self->timeout);
		break;
	case PROP_RESEARCH_TIMEOUT:
		g_value_set_uint(value, self->research_timeout);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
ai_libresearch_search_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiLibresearchSearch *self = AI_LIBRESEARCH_SEARCH(object);

	switch (prop_id)
	{
	case PROP_ENDPOINT:
		set_endpoint(self, g_value_get_string(value));
		break;
	case PROP_API_KEY:
		g_clear_pointer(&self->api_key, g_free);
		self->api_key = g_value_dup_string(value);
		break;
	case PROP_TIMEOUT:
		self->timeout = g_value_get_uint(value);
		g_object_set(self->session, "timeout", self->timeout, NULL);
		break;
	case PROP_RESEARCH_TIMEOUT:
		self->research_timeout = g_value_get_uint(value);
		g_object_set(self->long_session, "timeout", self->research_timeout, NULL);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
ai_libresearch_search_finalize(
	GObject *object
){
	AiLibresearchSearch *self = AI_LIBRESEARCH_SEARCH(object);

	g_clear_pointer(&self->endpoint, g_free);
	g_clear_pointer(&self->api_key, g_free);
	g_clear_object(&self->session);
	g_clear_object(&self->long_session);

	G_OBJECT_CLASS(ai_libresearch_search_parent_class)->finalize(object);
}

static void
ai_libresearch_search_class_init(
	AiLibresearchSearchClass *klass
){
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize     = ai_libresearch_search_finalize;
	object_class->get_property = ai_libresearch_search_get_property;
	object_class->set_property = ai_libresearch_search_set_property;

	/**
	 * AiLibresearchSearch:endpoint:
	 *
	 * The server's base URL, without `/api/v1`.  Empty means
	 * %AI_LIBRESEARCH_DEFAULT_ENDPOINT.
	 */
	properties[PROP_ENDPOINT] =
		g_param_spec_string("endpoint", "Endpoint", "libresearch server base URL",
		                    AI_LIBRESEARCH_DEFAULT_ENDPOINT,
		                    G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);

	/**
	 * AiLibresearchSearch:api-key:
	 *
	 * A bearer token from the server's `api.tokens`, sent as
	 * `Authorization: Bearer`.  %NULL when the server does not require one.
	 */
	properties[PROP_API_KEY] =
		g_param_spec_string("api-key", "API key", "Bearer token for the server", NULL,
		                    G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);

	/**
	 * AiLibresearchSearch:timeout:
	 *
	 * Seconds of silence after which an ordinary request (search, fetch,
	 * query, environment) is abandoned.
	 */
	properties[PROP_TIMEOUT] =
		g_param_spec_uint("timeout", "Timeout", "Idle timeout in seconds for ordinary requests",
		                  1, G_MAXUINT, LIBRESEARCH_TIMEOUT_S,
		                  G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);

	/**
	 * AiLibresearchSearch:research-timeout:
	 *
	 * Seconds of silence after which a research request, or an LQL query
	 * with a `research` source, is abandoned.  The server answers only
	 * when the whole run is done, which takes minutes.
	 */
	properties[PROP_RESEARCH_TIMEOUT] =
		g_param_spec_uint("research-timeout", "Research timeout",
		                  "Idle timeout in seconds for research requests",
		                  1, G_MAXUINT, LIBRESEARCH_RESEARCH_TIMEOUT_S,
		                  G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_libresearch_search_init(
	AiLibresearchSearch *self
){
	/* Both sessions exist before the CONSTRUCT properties set their timeouts. */
	self->session = soup_session_new();
	self->long_session = soup_session_new();
	self->timeout = LIBRESEARCH_TIMEOUT_S;
	self->research_timeout = LIBRESEARCH_RESEARCH_TIMEOUT_S;
}

AiLibresearchSearch *
ai_libresearch_search_new(
	const gchar *endpoint,
	const gchar *api_key
){
	return (AiLibresearchSearch *)g_object_new(AI_TYPE_LIBRESEARCH_SEARCH,
	                                           "endpoint", endpoint,
	                                           "api-key", api_key,
	                                           NULL);
}

const gchar *
ai_libresearch_search_get_endpoint(
	AiLibresearchSearch *self
){
	g_return_val_if_fail(AI_IS_LIBRESEARCH_SEARCH(self), NULL);

	return self->endpoint;
}
