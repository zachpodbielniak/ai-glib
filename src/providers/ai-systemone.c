/*
 * ai-systemone.c - Shared POST /v1/systemone transport (private)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ai-systemone.h"
#include "ai-image-shared.h"
#include "core/ai-json-util.h"
#include "core/ai-error.h"
#include "providers/ai-laya-client.h"
#include <string.h>

typedef struct
{
	SoupSession *session;
	AiDecisionRequest *request;
	GCancellable *cancel;
	GCancellable *parent_cancel;
	gulong cancel_id;
	GSource *deadline;
	gint64 started;
	gboolean timed_out;
	gchar *label;
} Operation;

static void
operation_free(Operation *op)
{
	if (op->deadline != NULL)
	{
		g_source_destroy(op->deadline);
		g_source_unref(op->deadline);
	}
	if (op->cancel_id != 0)
		g_cancellable_disconnect(op->parent_cancel, op->cancel_id);
	g_clear_object(&op->parent_cancel);
	g_clear_object(&op->cancel);
	g_clear_object(&op->session);
	g_clear_object(&op->request);
	g_free(op->label);
	g_free(op);
}

static void
forward_cancel(GCancellable *parent, gpointer data)
{
	(void)parent;
	g_cancellable_cancel(data);
}

static gboolean
deadline_expired(gpointer data)
{
	Operation *op = data;

	op->timed_out = TRUE;
	g_clear_pointer(&op->deadline, g_source_unref);
	g_cancellable_cancel(op->cancel);
	return G_SOURCE_REMOVE;
}

static const gchar *
kind_label(AiSystemOneKind kind)
{
	switch (kind)
	{
		case AI_SYSTEMONE_TYPESAFE: return "TypeSafe";
		case AI_SYSTEMONE_OLLAMA: return "Ollama";
		case AI_SYSTEMONE_LAYA: return "Laya";
		default: return "decision";
	}
}

static gboolean
question_is(JsonObject *q, const gchar *kind)
{
	return g_strcmp0(ai_json_get_string(q, "type", ""), kind) == 0;
}

/*
 * The request object is the snapshot about to be encoded. Model injection
 * has already happened. Each refusal is a property of one server, not of
 * the question types the library knows how to spell.
 */
static gboolean
policy_accepts(AiSystemOneKind kind, JsonObject *root, const gchar *api_key, GError **error)
{
	JsonObject *questions = ai_json_get_object(root, "questions");
	g_autoptr(GList) names = NULL;
	GList *iter;
	const gchar *model = ai_json_get_string(root, "model", NULL);
	gboolean box = FALSE;

	names = json_object_get_members(questions);
	for (iter = names; iter != NULL; iter = iter->next)
	{
		JsonObject *q = ai_json_get_object(questions, iter->data);
		JsonObject *choices = ai_json_get_object(q, "criteria");
		JsonArray *levels = ai_json_get_array(q, "criteria");
		guint count;

		if (question_is(q, "box"))
			box = TRUE;
		if (kind == AI_SYSTEMONE_LAYA && question_is(q, "noul") &&
			ai_json_get_node(q, "criteria") != NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				"Laya boolean questions do not take criteria");
			return FALSE;
		}
		if (choices != NULL && question_is(q, "choice"))
		{
			count = json_object_get_size(choices);
			if (kind == AI_SYSTEMONE_LAYA && count > 100)
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
					"Laya accepts at most 100 choices");
				return FALSE;
			}
			if (kind == AI_SYSTEMONE_OLLAMA && (count < 2 || count > 26))
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
					"Ollama choice questions need 2 to 26 options");
				return FALSE;
			}
			if (kind == AI_SYSTEMONE_TYPESAFE && count > 255)
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
					"TypeSafe accepts at most 255 choices");
				return FALSE;
			}
		}
		if (levels != NULL && question_is(q, "score"))
		{
			count = json_array_get_length(levels);
			if ((kind == AI_SYSTEMONE_OLLAMA || kind == AI_SYSTEMONE_TYPESAFE) && count > 10)
			{
				g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
					"%s score questions need 2 to 10 levels", kind_label(kind));
				return FALSE;
			}
		}
	}

	if (kind == AI_SYSTEMONE_LAYA)
	{
		if (ai_json_get_string(root, "state", NULL) == NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				"Laya state must be text");
			return FALSE;
		}
		if (box || ai_json_get_node(root, "images") != NULL || ai_json_get_node(root, "assets") != NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				"Laya does not accept images, assets or box questions");
			return FALSE;
		}
		if (model != NULL &&
			!g_str_equal(model, AI_LAYA_MODEL_ENGLISH) &&
			!g_str_equal(model, AI_LAYA_MODEL_MULTILINGUAL) &&
			!g_str_equal(model, AI_LAYA_MODEL_TYPED_DECISIONS))
		{
			g_set_error(error, AI_ERROR, AI_ERROR_MODEL_NOT_FOUND,
				"Unknown Laya checkpoint: %s", model);
			return FALSE;
		}
	}
	else if (model == NULL || *model == '\0')
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			"%s requires a model", kind_label(kind));
		return FALSE;
	}

	if (kind == AI_SYSTEMONE_TYPESAFE)
	{
		if (api_key == NULL || *api_key == '\0')
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
				"TypeSafe requires TYPESAFE_API_KEY");
			return FALSE;
		}
		if (ai_json_get_node(root, "images") != NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				"TypeSafe images travel in assets, not the Ollama images array");
			return FALSE;
		}
	}
	if (kind == AI_SYSTEMONE_OLLAMA && (box || ai_json_get_node(root, "assets") != NULL))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			"Ollama decision models take an images array, not TypeSafe assets or box questions");
		return FALSE;
	}
	return TRUE;
}

static gboolean
body_fits(AiSystemOneKind kind, JsonObject *root, gsize length, GError **error)
{
	gboolean images = ai_json_get_node(root, "images") != NULL;
	gsize limit;

	if (kind == AI_SYSTEMONE_OLLAMA)
		limit = images ? (gsize)32 * 1024 * 1024 : (gsize)64 * 1024;
	else if (kind == AI_SYSTEMONE_TYPESAFE)
		limit = (gsize)8 * 1024 * 1024;
	else
		limit = (gsize)2 * 1024 * 1024;
	if (length <= limit)
		return TRUE;
	g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		"%s request exceeds %s", kind_label(kind),
		kind == AI_SYSTEMONE_OLLAMA && !images ? "64 KiB" : "its body limit");
	return FALSE;
}

static void
on_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GTask) task = user_data;
	Operation *op = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = ai_image_shared_send_finish(result, &error);
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autofree gchar *json = NULL;
	gconstpointer data;
	gsize size;

	(void)source;
	if (op->deadline != NULL)
	{
		g_source_destroy(op->deadline);
		g_clear_pointer(&op->deadline, g_source_unref);
	}
	if (g_task_return_error_if_cancelled(task))
		return;
	if (op->timed_out)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_TIMEOUT,
			"%s decision deadline expired", op->label);
		return;
	}
	if (bytes == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	data = g_bytes_get_data(bytes, &size);
	if (size == 0 || size > 4 * 1024 * 1024 || memchr(data, '\0', size) != NULL ||
		!g_utf8_validate(data, (gssize)size, NULL))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
			"Invalid %s response encoding or size", op->label);
		return;
	}
	json = g_strndup(data, size);
	response = ai_decision_response_new_from_json(op->request, json,
		(g_get_monotonic_time() - op->started) / 1000.0, &error);
	if (response == NULL)
		g_task_return_error(task, g_steal_pointer(&error));
	else
		g_task_return_pointer(task, g_steal_pointer(&response), g_object_unref);
}

static gboolean
needs_image_header(JsonObject *root)
{
	JsonObject *questions = ai_json_get_object(root, "questions");
	g_autoptr(GList) names = NULL;
	GList *iter;

	if (ai_json_get_node(root, "assets") != NULL)
		return TRUE;
	names = json_object_get_members(questions);
	for (iter = names; iter != NULL; iter = iter->next)
		if (question_is(ai_json_get_object(questions, iter->data), "box"))
			return TRUE;
	return FALSE;
}

void
ai_systemone_decide_async(GObject *source, const AiSystemOneTarget *target,
	AiDecisionRequest *request, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data, gpointer source_tag)
{
	g_autoptr(GTask) task = g_task_new(source, cancellable, callback, user_data);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *encoded = NULL;
	g_autofree gchar *authorization = NULL;
	const gchar *scheme;
	JsonObject *root;
	Operation *op;
	gsize length;

	g_task_set_source_tag(task, source_tag);
	if (g_task_return_error_if_cancelled(task))
		return;
	if (target == NULL || target->base_url == NULL || target->timeout_ms == 0)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
			"A decision client needs a base URL and a positive timeout");
		return;
	}
	json = ai_decision_request_dup_json(request);
	if (!json_parser_load_from_data(parser, json, -1, NULL))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			"Could not read the decision request");
		return;
	}
	root = ai_json_root_object(parser);
	if (ai_json_get_string(root, "model", NULL) == NULL && target->model != NULL && *target->model != '\0')
		json_object_set_string_member(root, "model", target->model);
	if (!policy_accepts(target->kind, root, target->api_key, &error))
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	uri = g_uri_parse(target->base_url, G_URI_FLAGS_NONE, NULL);
	scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
	if (uri == NULL || (g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0) ||
		g_uri_get_host(uri) == NULL || g_uri_get_userinfo(uri) != NULL ||
		g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
		(target->api_key != NULL && strpbrk(target->api_key, "\r\n") != NULL))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
			"%s needs an HTTP(S) base URL without credentials, query or fragment and a valid API key",
			kind_label(target->kind));
		return;
	}
	length = strlen(target->base_url);
	while (length > 0 && target->base_url[length - 1] == '/')
		length--;
	url = g_strdup_printf("%.*s/v1/systemone", (gint)length, target->base_url);
	message = soup_message_new("POST", url);
	if (message == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
			"Invalid %s URL", kind_label(target->kind));
		return;
	}
	/* The state is the thing being classified. Do not follow it anywhere else. */
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (target->api_key != NULL && *target->api_key != '\0')
	{
		authorization = g_strconcat("Bearer ", target->api_key, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(message),
			"Authorization", authorization);
	}
	if (target->kind == AI_SYSTEMONE_TYPESAFE && target->long_context)
		soup_message_headers_replace(soup_message_get_request_headers(message),
			"TypeSafe-Danger-Zone-Long-Context", "true");
	if (target->kind == AI_SYSTEMONE_TYPESAFE && needs_image_header(root))
		soup_message_headers_replace(soup_message_get_request_headers(message),
			"TypeSafe-Danger-Zone-Images", "true");
	encoded = json_to_string(json_parser_get_root(parser), FALSE);
	if (!body_fits(target->kind, root, strlen(encoded), &error))
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	body = g_bytes_new(encoded, strlen(encoded));
	soup_message_set_request_body_from_bytes(message, "application/json", body);
	op = g_new0(Operation, 1);
	op->session = soup_session_new();
	op->request = g_object_ref(request);
	op->cancel = g_cancellable_new();
	op->started = g_get_monotonic_time();
	op->label = g_strdup(kind_label(target->kind));
	if (cancellable != NULL)
	{
		op->parent_cancel = g_object_ref(cancellable);
		op->cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(forward_cancel),
			g_object_ref(op->cancel), g_object_unref);
	}
	op->deadline = g_timeout_source_new(target->timeout_ms);
	g_source_set_callback(op->deadline, deadline_expired, op, NULL);
	g_source_attach(op->deadline, g_main_context_get_thread_default());
	g_task_set_task_data(task, op, (GDestroyNotify)operation_free);
	ai_image_shared_send_async(op->session, message, body, 0, op->cancel, on_reply, g_steal_pointer(&task));
}

AiDecisionResponse *
ai_systemone_decide_finish(GObject *source, GAsyncResult *result, gpointer source_tag, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, source), NULL);
	g_return_val_if_fail(g_async_result_is_tagged(result, source_tag), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
