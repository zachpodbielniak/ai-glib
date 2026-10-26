/*
 * ai-benchmark-suite.c - Load a benchmark from a YAML (or JSON) suite file
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The schema is in docs/benchmark.org, "Suite file reference". In short:
 *
 *   name: Jev vs local          iterations: 5      warmup: 1
 *   timeout-ms: 30000           kind: decision     (default for both lists)
 *   targets:
 *     - { label: jev, provider: typesafe, model: jev-latest }
 *     - { label: tev1, provider: ollama, model: tev1, properties: [...] }
 *   cases:
 *     - id: refund
 *       request: { state: "...", questions: { refund: { type: noul, ... } } }
 *       expect: { refund: true }
 *
 * Rules that are not style:
 *
 *  - Every key is checked. An unknown key is an error naming it and where
 *    it was, because `iteration: 10` silently running the default 3 is a
 *    benchmark that measured something other than what was asked.
 *
 *  - Quoting is honoured. yaml-glib types a scalar by its text whatever
 *    its quoting, so `"yes"` would arrive as a boolean and a choice
 *    option called `no` would turn into `false`. This loader walks
 *    YamlNode itself: a quoted scalar is always a string, and only a
 *    plain one is read as a number or a boolean.
 *
 *  - The file is not ours. Size (4 MiB), depth (32) and node count
 *    (100000) are bounded, invalid UTF-8 and NUL bytes are refused, and
 *    nothing here can abort a fatal-warnings run: a bad file is a GError.
 *    Files (the suite and any request-file) are opened, then checked on
 *    that descriptor to be regular and small, then read: /dev/zero would
 *    otherwise exhaust memory and a FIFO would block forever.
 *
 *  - Booleans are YAML 1.2's: only true and false. An unquoted yes, no, on
 *    or off is text, so a choice option called "no" needs no quotes.
 *
 *  - Cases are matched to targets by kind, and a case no target can run is
 *    an error at load time, not a silently empty table.
 */

#include "bench/ai-benchmark.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml-glib.h>

#include "core/ai-error.h"

#define SUITE_MAX_BYTES (4 * 1024 * 1024)
#define SUITE_MAX_DEPTH (32)
#define SUITE_MAX_NODES (100000)

static gboolean
suite_fail(
	GError      **error,
	const gchar  *format,
	...
) G_GNUC_PRINTF(2, 3);

static gboolean
suite_fail(
	GError      **error,
	const gchar  *format,
	...
){
	va_list args;
	g_autofree gchar *message = NULL;

	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "benchmark suite: %s", message);
	return FALSE;
}

/* ---- bounded file read ------------------------------------------ */

/*
 * Reads @path only if, on the descriptor actually opened, it is a regular
 * file of at most SUITE_MAX_BYTES. A stat and a later read would be two
 * looks at a path that can change in between.
 */
static gboolean
read_bounded(
	const gchar  *path,
	gchar       **contents,
	gsize        *length,
	GError      **error
){
	g_autofree gchar *buffer = NULL;
	struct stat info;
	gsize total;
	gint fd;

	fd = g_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
	if (fd < 0)
		return suite_fail(error, "cannot read %s: %s", path, g_strerror(errno));
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
	{
		close(fd);
		return suite_fail(error, "cannot read %s: not a regular file", path);
	}
	if (info.st_size > SUITE_MAX_BYTES)
	{
		close(fd);
		return suite_fail(error, "cannot read %s: larger than %d bytes", path, SUITE_MAX_BYTES);
	}

	/* One byte of headroom detects a file that grew since fstat(). */
	buffer = g_malloc((gsize)info.st_size + 2);
	total = 0;
	while (total < (gsize)info.st_size + 1)
	{
		gssize got = read(fd, buffer + total, (gsize)info.st_size + 1 - total);

		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
		{
			gint saved = errno;

			close(fd);
			return suite_fail(error, "cannot read %s: %s", path, g_strerror(saved));
		}
		if (got == 0)
			break;
		total += (gsize)got;
	}
	close(fd);
	if (total > SUITE_MAX_BYTES)
		return suite_fail(error, "cannot read %s: larger than %d bytes", path, SUITE_MAX_BYTES);
	buffer[total] = '\0';
	*length = total;
	*contents = (gchar *)g_steal_pointer(&buffer);
	return TRUE;
}

/* ---- bounded walk ----------------------------------------------- */

/*
 * Depth and size are checked once over the whole tree before anything
 * else reads it, so every later walk can recurse without a guard.
 */
static gboolean
check_shape(
	YamlNode  *node,
	guint      depth,
	guint     *count,
	GError   **error
){
	if (++(*count) > SUITE_MAX_NODES)
		return suite_fail(error, "more than %d nodes", SUITE_MAX_NODES);
	if (depth > SUITE_MAX_DEPTH)
		return suite_fail(error, "nested deeper than %d levels", SUITE_MAX_DEPTH);

	switch (yaml_node_get_node_type(node))
	{
		case YAML_NODE_MAPPING:
		{
			YamlMapping *mapping = yaml_node_get_mapping(node);
			g_autoptr(GList) names = yaml_mapping_get_members(mapping);
			GList *iter;

			for (iter = names; iter != NULL; iter = iter->next)
				if (!check_shape(yaml_mapping_get_member(mapping, (const gchar *)iter->data),
				                 depth + 1, count, error))
					return FALSE;
			return TRUE;
		}
		case YAML_NODE_SEQUENCE:
		{
			YamlSequence *sequence = yaml_node_get_sequence(node);
			guint length = yaml_sequence_get_length(sequence);
			guint i;

			for (i = 0; i < length; i++)
				if (!check_shape(yaml_sequence_get_element(sequence, i), depth + 1, count, error))
					return FALSE;
			return TRUE;
		}
		case YAML_NODE_SCALAR:
		case YAML_NODE_NULL:
		default:
			return TRUE;
	}
}

/* ---- scalar readers that honour quoting --------------------------- */

static gboolean
is_plain(
	YamlNode *node
){
	YamlScalarStyle style;

	style = yaml_node_get_scalar_style(node);
	return style == YAML_SCALAR_STYLE_PLAIN || style == YAML_SCALAR_STYLE_ANY;
}

static gboolean
is_mapping(
	YamlNode *node
){
	return node != NULL && yaml_node_get_node_type(node) == YAML_NODE_MAPPING;
}

static gboolean
is_sequence(
	YamlNode *node
){
	return node != NULL && yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE;
}

static YamlNode *
member(
	YamlNode    *mapping,
	const gchar *name
){
	YamlNode *node;

	node = yaml_mapping_get_member(yaml_node_get_mapping(mapping), name);
	if (node != NULL && yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return NULL;
	return node;
}

/*
 * Any scalar as its text. Numbers and booleans are fine here: `prompt: 42`
 * means the text "42".
 */
static const gchar *
scalar_text(
	YamlNode *node
){
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

static gboolean
read_string(
	YamlNode     *mapping,
	const gchar  *name,
	const gchar  *where,
	gboolean      required,
	const gchar **out,
	GError      **error
){
	YamlNode *node;

	node = member(mapping, name);
	*out = NULL;
	if (node == NULL)
	{
		if (required)
			return suite_fail(error, "%s: '%s' is required", where, name);
		return TRUE;
	}
	*out = scalar_text(node);
	if (*out == NULL)
		return suite_fail(error, "%s: '%s' must be a string", where, name);
	return TRUE;
}

static gboolean
read_uint(
	YamlNode     *mapping,
	const gchar  *name,
	const gchar  *where,
	guint         minimum,
	guint         maximum,
	guint        *out,
	GError      **error
){
	YamlNode *node;
	const gchar *text;
	gchar *end;
	gint64 value;

	node = member(mapping, name);
	if (node == NULL)
		return TRUE;
	text = scalar_text(node);
	end = NULL;
	value = text != NULL && is_plain(node) ? g_ascii_strtoll(text, &end, 10) : -1;
	if (text == NULL || end == text || end == NULL || *end != '\0' ||
	    value < (gint64)minimum || value > (gint64)maximum)
		return suite_fail(error, "%s: '%s' must be a whole number from %u to %u",
		                  where, name, minimum, maximum);
	*out = (guint)value;
	return TRUE;
}

static gboolean
read_double(
	YamlNode     *mapping,
	const gchar  *name,
	const gchar  *where,
	gdouble       minimum,
	gdouble       maximum,
	gdouble      *out,
	gboolean     *present,
	GError      **error
){
	YamlNode *node;
	const gchar *text;
	gchar *end;
	gdouble value;

	node = member(mapping, name);
	*present = FALSE;
	if (node == NULL)
		return TRUE;
	text = scalar_text(node);
	end = NULL;
	value = text != NULL && is_plain(node) ? g_ascii_strtod(text, &end) : -1;
	if (text == NULL || end == text || end == NULL || *end != '\0' || !isfinite(value) ||
	    value < minimum || value > maximum)
		return suite_fail(error, "%s: '%s' must be a number from %g to %g", where, name, minimum, maximum);
	*out = value;
	*present = TRUE;
	return TRUE;
}

/*
 * A plain true/false, YAML 1.2 style: yes, no, on and off are text. A
 * quoted "true" is a string, deliberately.
 */
static gboolean
plain_boolean(
	YamlNode *node,
	gboolean *out
){
	const gchar *text;

	text = scalar_text(node);
	if (text == NULL || !is_plain(node))
		return FALSE;
	if (g_ascii_strcasecmp(text, "true") == 0)
	{
		*out = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "false") == 0)
	{
		*out = FALSE;
		return TRUE;
	}
	return FALSE;
}

static gboolean
check_keys(
	YamlNode           *mapping,
	const gchar *const *allowed,
	const gchar        *where,
	GError            **error
){
	g_autoptr(GList) names = NULL;
	GList *iter;

	names = yaml_mapping_get_members(yaml_node_get_mapping(mapping));
	for (iter = names; iter != NULL; iter = iter->next)
	{
		if (!g_strv_contains(allowed, (const gchar *)iter->data))
		{
			g_autofree gchar *list = g_strjoinv(", ", (gchar **)allowed);

			return suite_fail(error, "%s: unknown key '%s' (expected one of: %s)",
			                  where, (const gchar *)iter->data, list);
		}
	}
	return TRUE;
}

/*
 * YAML to JSON for a decision request, keeping quoted scalars as strings.
 * A plain scalar becomes a boolean, an integer or a double when its whole
 * text is one, as JSON would have it.
 */
static JsonNode *
to_json(
	YamlNode *node
){
	JsonNode *json;

	switch (yaml_node_get_node_type(node))
	{
		case YAML_NODE_MAPPING:
		{
			YamlMapping *mapping = yaml_node_get_mapping(node);
			g_autoptr(GList) names = yaml_mapping_get_members(mapping);
			JsonObject *object = json_object_new();
			GList *iter;

			for (iter = names; iter != NULL; iter = iter->next)
				json_object_set_member(object, (const gchar *)iter->data,
				                       to_json(yaml_mapping_get_member(mapping, (const gchar *)iter->data)));
			json = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(json, object);
			return json;
		}
		case YAML_NODE_SEQUENCE:
		{
			YamlSequence *sequence = yaml_node_get_sequence(node);
			guint length = yaml_sequence_get_length(sequence);
			JsonArray *array = json_array_sized_new(length);
			guint i;

			for (i = 0; i < length; i++)
				json_array_add_element(array, to_json(yaml_sequence_get_element(sequence, i)));
			json = json_node_new(JSON_NODE_ARRAY);
			json_node_take_array(json, array);
			return json;
		}
		case YAML_NODE_SCALAR:
		{
			const gchar *text = yaml_node_get_scalar(node);
			gboolean flag;
			gchar *end;
			gint64 whole;
			gdouble real;

			json = json_node_new(JSON_NODE_VALUE);
			if (text == NULL)
			{
				json_node_set_string(json, "");
				return json;
			}
			if (is_plain(node))
			{
				if (plain_boolean(node, &flag))
				{
					json_node_set_boolean(json, flag);
					return json;
				}
				end = NULL;
				whole = g_ascii_strtoll(text, &end, 10);
				if (end != text && *end == '\0')
				{
					json_node_set_int(json, whole);
					return json;
				}
				end = NULL;
				real = g_ascii_strtod(text, &end);
				if (end != text && *end == '\0' && isfinite(real))
				{
					json_node_set_double(json, real);
					return json;
				}
			}
			json_node_set_string(json, text);
			return json;
		}
		case YAML_NODE_NULL:
		default:
			return json_node_new(JSON_NODE_NULL);
	}
}

/* ---- targets ---------------------------------------------------- */

static const gchar *const TARGET_KEYS[] = {
	"label", "kind", "provider", "model", "stream", "properties", NULL
};

static gboolean
load_target(
	AiBenchmark      *self,
	YamlNode         *node,
	guint             index,
	AiBenchmarkKind   default_kind,
	gboolean          has_default_kind,
	GError          **error
){
	g_autoptr(AiBenchmarkTarget) target = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *label_owned = NULL;
	const gchar *label;
	const gchar *kind_name;
	const gchar *provider;
	const gchar *model;
	AiBenchmarkKind kind;
	YamlNode *child;

	where = g_strdup_printf("targets[%u]", index);
	if (!is_mapping(node))
		return suite_fail(error, "%s must be a mapping", where);
	if (!check_keys(node, TARGET_KEYS, where, error) ||
	    !read_string(node, "provider", where, TRUE, &provider, error) ||
	    !read_string(node, "model", where, FALSE, &model, error) ||
	    !read_string(node, "label", where, FALSE, &label, error) ||
	    !read_string(node, "kind", where, FALSE, &kind_name, error))
		return FALSE;

	/* kind: the target's own, else the suite default. */
	if (kind_name != NULL)
	{
		if (!ai_benchmark_kind_from_string(kind_name, &kind))
			return suite_fail(error, "%s: unknown kind '%s' (chat, decision, embedding or image)",
			                  where, kind_name);
	}
	else if (has_default_kind)
		kind = default_kind;
	else
		return suite_fail(error, "%s: 'kind' is required (or set a top-level 'kind')", where);

	/* label defaults to provider:model, which is what a person would type. */
	if (label == NULL)
	{
		label_owned = model != NULL ? g_strdup_printf("%s:%s", provider, model) : g_strdup(provider);
		label = label_owned;
	}

	target = ai_benchmark_target_new(label, kind, provider, model);

	child = member(node, "stream");
	if (child != NULL)
	{
		gboolean stream;

		if (!plain_boolean(child, &stream))
			return suite_fail(error, "%s: 'stream' must be true or false", where);
		if (stream && kind != AI_BENCHMARK_KIND_CHAT)
			return suite_fail(error, "%s: 'stream' applies to chat targets only", where);
		ai_benchmark_target_set_stream(target, stream);
	}

	/* properties: a list of NAME=VALUE, or a mapping of NAME: VALUE. */
	child = member(node, "properties");
	if (child != NULL)
	{
		if (is_sequence(child))
		{
			YamlSequence *sequence = yaml_node_get_sequence(child);
			guint length = yaml_sequence_get_length(sequence);
			guint i;

			for (i = 0; i < length; i++)
			{
				const gchar *assignment = scalar_text(yaml_sequence_get_element(sequence, i));

				if (assignment == NULL)
					return suite_fail(error, "%s: properties[%u] must be NAME=VALUE", where, i);
				if (!ai_benchmark_target_add_property(target, assignment, NULL))
					return suite_fail(error, "%s: properties[%u] has no name", where, i);
			}
		}
		else if (is_mapping(child))
		{
			YamlMapping *mapping = yaml_node_get_mapping(child);
			g_autoptr(GList) names = yaml_mapping_get_members(mapping);
			GList *iter;

			for (iter = names; iter != NULL; iter = iter->next)
			{
				const gchar *value = scalar_text(yaml_mapping_get_member(mapping, (const gchar *)iter->data));
				g_autofree gchar *assignment = NULL;

				if (value == NULL)
					return suite_fail(error, "%s: property '%s' must have a scalar value",
					                  where, (const gchar *)iter->data);
				assignment = g_strdup_printf("%s=%s", (const gchar *)iter->data, value);
				if (!ai_benchmark_target_add_property(target, assignment, NULL))
					return suite_fail(error, "%s: a property has an empty name", where);
			}
		}
		else
			return suite_fail(error, "%s: 'properties' must be a list of NAME=VALUE or a mapping", where);
	}

	if (!ai_benchmark_add_target(self, target, NULL))
		return suite_fail(error, "%s: duplicate target label '%s'", where, label);
	return TRUE;
}

/* ---- cases ------------------------------------------------------ */

static const gchar *const CHAT_KEYS[] = {
	"id", "kind", "prompt", "system", "max-tokens", "expect", NULL
};
static const gchar *const CHAT_EXPECT_KEYS[] = { "contains", "equals", "regex", NULL };
static const gchar *const DECISION_KEYS[] = {
	"id", "kind", "request", "request-file", "expect", "noul-threshold", "box-iou", NULL
};
static const gchar *const EMBEDDING_KEYS[] = { "id", "kind", "texts", "expect", NULL };
static const gchar *const EMBEDDING_EXPECT_KEYS[] = { "similar", "dissimilar", NULL };
static const gchar *const IMAGE_KEYS[] = { "id", "kind", "prompt", "size", "count", NULL };

static gboolean
load_chat_case(
	YamlNode         *node,
	const gchar      *id,
	const gchar      *where,
	AiBenchmarkCase **out,
	GError          **error
){
	g_autoptr(AiBenchmarkCase) bench_case = NULL;
	const gchar *prompt;
	const gchar *system;
	guint max_tokens;
	YamlNode *expect;

	max_tokens = 0;
	if (!check_keys(node, CHAT_KEYS, where, error) ||
	    !read_string(node, "prompt", where, TRUE, &prompt, error) ||
	    !read_string(node, "system", where, FALSE, &system, error) ||
	    !read_uint(node, "max-tokens", where, 1, G_MAXINT, &max_tokens, error))
		return FALSE;

	bench_case = ai_benchmark_case_new_chat(id, prompt);
	ai_benchmark_case_set_system(bench_case, system);
	if (max_tokens > 0)
		ai_benchmark_case_set_max_tokens(bench_case, (gint)max_tokens);

	/* expect: exactly one of contains, equals, regex. */
	expect = member(node, "expect");
	if (expect != NULL)
	{
		g_autofree gchar *expect_where = g_strdup_printf("%s.expect", where);
		const gchar *contains;
		const gchar *equals;
		const gchar *regex;

		if (!is_mapping(expect))
			return suite_fail(error, "%s must be a mapping with contains, equals or regex", expect_where);
		if (!check_keys(expect, CHAT_EXPECT_KEYS, expect_where, error) ||
		    !read_string(expect, "contains", expect_where, FALSE, &contains, error) ||
		    !read_string(expect, "equals", expect_where, FALSE, &equals, error) ||
		    !read_string(expect, "regex", expect_where, FALSE, &regex, error))
			return FALSE;
		if ((contains != NULL) + (equals != NULL) + (regex != NULL) != 1)
			return suite_fail(error, "%s needs exactly one of contains, equals or regex", expect_where);
		if (contains != NULL)
			ai_benchmark_case_expect_contains(bench_case, contains);
		else if (equals != NULL)
			ai_benchmark_case_expect_equals(bench_case, equals);
		else
		{
			g_autoptr(GError) local = NULL;

			if (!ai_benchmark_case_expect_regex(bench_case, regex, &local))
				return suite_fail(error, "%s", local->message);
		}
	}

	*out = (AiBenchmarkCase *)g_steal_pointer(&bench_case);
	return TRUE;
}

/*
 * An expected answer as the text ai_benchmark_case_expect_answer() takes:
 * a plain boolean, a scalar, or four numbers for a box.
 */
static gchar *
expectation_text(
	YamlNode *node
){
	gboolean flag;

	if (node == NULL)
		return NULL;
	if (plain_boolean(node, &flag))
		return g_strdup(flag ? "true" : "false");
	if (is_sequence(node))
	{
		YamlSequence *sequence = yaml_node_get_sequence(node);
		g_autoptr(GString) out = g_string_new(NULL);
		guint i;

		if (yaml_sequence_get_length(sequence) != 4)
			return NULL;
		for (i = 0; i < 4; i++)
		{
			const gchar *part = scalar_text(yaml_sequence_get_element(sequence, i));

			if (part == NULL)
				return NULL;
			if (i > 0)
				g_string_append_c(out, ',');
			g_string_append(out, part);
		}
		return g_string_free((GString *)g_steal_pointer(&out), FALSE);
	}
	return g_strdup(scalar_text(node));
}

static gboolean
load_decision_case(
	YamlNode         *node,
	const gchar      *id,
	const gchar      *where,
	const gchar      *base_dir,
	AiBenchmarkCase **out,
	GError          **error
){
	g_autoptr(AiBenchmarkCase) bench_case = NULL;
	g_autoptr(AiDecisionRequest) request = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *json = NULL;
	const gchar *request_file;
	YamlNode *inline_request;
	YamlNode *expect;
	gdouble number;
	gboolean present;

	if (!check_keys(node, DECISION_KEYS, where, error) ||
	    !read_string(node, "request-file", where, FALSE, &request_file, error))
		return FALSE;

	/* request: inline (the wire shape, written as YAML) or from a JSON file. */
	inline_request = member(node, "request");
	if ((inline_request != NULL) == (request_file != NULL))
		return suite_fail(error, "%s needs exactly one of 'request' or 'request-file'", where);

	if (inline_request != NULL)
	{
		g_autoptr(JsonNode) root = NULL;

		if (!is_mapping(inline_request))
			return suite_fail(error, "%s.request must be a mapping", where);
		root = to_json(inline_request);
		json = json_to_string(root, FALSE);
	}
	else
	{
		g_autofree gchar *path = base_dir != NULL && !g_path_is_absolute(request_file)
			? g_build_filename(base_dir, request_file, NULL) : g_strdup(request_file);
		gsize length;

		if (!read_bounded(path, &json, &length, &local))
			return suite_fail(error, "%s: request-file: %s", where,
			                  local->message + strlen("benchmark suite: "));
		if (memchr(json, '\0', length) != NULL)
			return suite_fail(error, "%s: request-file '%s' contains NUL bytes", where, request_file);
	}

	request = ai_decision_request_new_from_json(json, &local);
	if (request == NULL)
		return suite_fail(error, "%s.request: %s", where, local->message);
	bench_case = ai_benchmark_case_new_decision(id, request);

	if (!read_double(node, "noul-threshold", where, 0, 1, &number, &present, error))
		return FALSE;
	if (present)
		ai_benchmark_case_set_noul_threshold(bench_case, number);
	if (!read_double(node, "box-iou", where, 0, 1, &number, &present, error))
		return FALSE;
	if (present)
		ai_benchmark_case_set_box_iou(bench_case, number);

	/* expect: question id -> labelled answer. */
	expect = member(node, "expect");
	if (expect != NULL)
	{
		YamlMapping *mapping;
		g_autoptr(GList) names = NULL;
		GList *iter;

		if (!is_mapping(expect))
			return suite_fail(error, "%s.expect must map question ids to answers", where);
		mapping = yaml_node_get_mapping(expect);
		names = yaml_mapping_get_members(mapping);
		for (iter = names; iter != NULL; iter = iter->next)
		{
			const gchar *question = (const gchar *)iter->data;
			g_autofree gchar *answer = expectation_text(yaml_mapping_get_member(mapping, question));

			if (answer == NULL)
				return suite_fail(error, "%s.expect.%s must be true/false, an option, a level, "
				                  "or [top, bottom, left, right]", where, question);
			if (!ai_benchmark_case_expect_answer(bench_case, question, answer, &local))
				return suite_fail(error, "%s", local->message);
		}
	}

	*out = (AiBenchmarkCase *)g_steal_pointer(&bench_case);
	return TRUE;
}

static gboolean
read_index_pair(
	YamlNode     *mapping,
	const gchar  *name,
	const gchar  *where,
	guint         pair[2],
	GError      **error
){
	YamlNode *node;
	YamlSequence *sequence;
	guint i;

	node = member(mapping, name);
	if (!is_sequence(node) || yaml_sequence_get_length(yaml_node_get_sequence(node)) != 2)
		return suite_fail(error, "%s: '%s' must be a pair of text indices, e.g. [0, 1]", where, name);
	sequence = yaml_node_get_sequence(node);
	for (i = 0; i < 2; i++)
	{
		YamlNode *element = yaml_sequence_get_element(sequence, i);
		const gchar *text = scalar_text(element);
		gchar *end = NULL;
		gint64 value = text != NULL ? g_ascii_strtoll(text, &end, 10) : -1;

		if (text == NULL || end == text || *end != '\0' || value < 0 || value > G_MAXINT32)
			return suite_fail(error, "%s: '%s' must be a pair of text indices, e.g. [0, 1]", where, name);
		pair[i] = (guint)value;
	}
	return TRUE;
}

static gboolean
load_embedding_case(
	YamlNode         *node,
	const gchar      *id,
	const gchar      *where,
	AiBenchmarkCase **out,
	GError          **error
){
	g_autoptr(AiBenchmarkCase) bench_case = NULL;
	g_autoptr(GPtrArray) texts = NULL;
	YamlNode *list;
	YamlNode *expect;
	guint i;

	if (!check_keys(node, EMBEDDING_KEYS, where, error))
		return FALSE;

	list = member(node, "texts");
	if (!is_sequence(list) || yaml_sequence_get_length(yaml_node_get_sequence(list)) == 0)
		return suite_fail(error, "%s: 'texts' must be a non-empty list of strings", where);
	texts = g_ptr_array_new();
	for (i = 0; i < yaml_sequence_get_length(yaml_node_get_sequence(list)); i++)
	{
		const gchar *text = scalar_text(yaml_sequence_get_element(yaml_node_get_sequence(list), i));

		if (text == NULL)
			return suite_fail(error, "%s: texts[%u] must be a string", where, i);
		g_ptr_array_add(texts, (gpointer)text);
	}
	g_ptr_array_add(texts, NULL);
	bench_case = ai_benchmark_case_new_embedding(id, (const gchar *const *)texts->pdata);

	expect = member(node, "expect");
	if (expect != NULL)
	{
		g_autofree gchar *expect_where = g_strdup_printf("%s.expect", where);
		guint similar[2];
		guint dissimilar[2];

		g_autoptr(GError) local = NULL;

		if (!is_mapping(expect))
			return suite_fail(error, "%s must be a mapping with similar and dissimilar", expect_where);
		if (!check_keys(expect, EMBEDDING_EXPECT_KEYS, expect_where, error) ||
		    !read_index_pair(expect, "similar", expect_where, similar, error) ||
		    !read_index_pair(expect, "dissimilar", expect_where, dissimilar, error))
			return FALSE;
		if (!ai_benchmark_case_expect_similarity(bench_case, similar[0], similar[1],
		                                         dissimilar[0], dissimilar[1], &local))
			return suite_fail(error, "%s", local->message);
	}

	*out = (AiBenchmarkCase *)g_steal_pointer(&bench_case);
	return TRUE;
}

static gboolean
load_image_case(
	YamlNode         *node,
	const gchar      *id,
	const gchar      *where,
	AiBenchmarkCase **out,
	GError          **error
){
	g_autoptr(AiBenchmarkCase) bench_case = NULL;
	const gchar *prompt;
	const gchar *size;
	guint count;

	count = 0;
	if (!check_keys(node, IMAGE_KEYS, where, error) ||
	    !read_string(node, "prompt", where, TRUE, &prompt, error) ||
	    !read_string(node, "size", where, FALSE, &size, error) ||
	    !read_uint(node, "count", where, 1, 16, &count, error))
		return FALSE;

	bench_case = ai_benchmark_case_new_image(id, prompt);
	if (size != NULL)
	{
		AiImageSize parsed = ai_image_size_from_string(size);

		if (parsed == AI_IMAGE_SIZE_CUSTOM || (parsed == AI_IMAGE_SIZE_AUTO && g_strcmp0(size, "auto") != 0))
			return suite_fail(error, "%s: unknown size '%s' (auto, 256x256, 512x512, 1024x1024, "
			                  "1024x1792 or 1792x1024)", where, size);
		ai_benchmark_case_set_image_size(bench_case, parsed);
	}
	if (count > 0)
		ai_benchmark_case_set_image_count(bench_case, (gint)count);

	*out = (AiBenchmarkCase *)g_steal_pointer(&bench_case);
	return TRUE;
}

static gboolean
load_case(
	AiBenchmark      *self,
	YamlNode         *node,
	guint             index,
	AiBenchmarkKind   default_kind,
	gboolean          has_default_kind,
	const gchar      *base_dir,
	GError          **error
){
	g_autoptr(AiBenchmarkCase) bench_case = NULL;
	g_autofree gchar *where = NULL;
	const gchar *id;
	const gchar *kind_name;
	AiBenchmarkKind kind;
	gboolean ok;

	where = g_strdup_printf("cases[%u]", index);
	if (!is_mapping(node))
		return suite_fail(error, "%s must be a mapping", where);
	if (!read_string(node, "id", where, TRUE, &id, error) ||
	    !read_string(node, "kind", where, FALSE, &kind_name, error))
		return FALSE;

	/* From here on, name the case by its id rather than its position. */
	g_free(where);
	where = g_strdup_printf("case '%s'", id);

	if (kind_name != NULL)
	{
		if (!ai_benchmark_kind_from_string(kind_name, &kind))
			return suite_fail(error, "%s: unknown kind '%s' (chat, decision, embedding or image)",
			                  where, kind_name);
	}
	else if (has_default_kind)
		kind = default_kind;
	else
		return suite_fail(error, "%s: 'kind' is required (or set a top-level 'kind')", where);

	switch (kind)
	{
		case AI_BENCHMARK_KIND_CHAT:
			ok = load_chat_case(node, id, where, &bench_case, error);
			break;
		case AI_BENCHMARK_KIND_DECISION:
			ok = load_decision_case(node, id, where, base_dir, &bench_case, error);
			break;
		case AI_BENCHMARK_KIND_EMBEDDING:
			ok = load_embedding_case(node, id, where, &bench_case, error);
			break;
		case AI_BENCHMARK_KIND_IMAGE:
			ok = load_image_case(node, id, where, &bench_case, error);
			break;
		default:
			ok = FALSE;
			break;
	}
	if (!ok)
		return FALSE;
	if (!ai_benchmark_add_case(self, bench_case, NULL))
		return suite_fail(error, "duplicate case id '%s'", id);
	return TRUE;
}

/* ---- document --------------------------------------------------- */

static const gchar *const SUITE_KEYS[] = {
	"name", "kind", "iterations", "warmup", "timeout-ms", "targets", "cases", NULL
};

static gboolean
load_document(
	AiBenchmark  *self,
	YamlNode     *root,
	const gchar  *base_dir,
	GError      **error
){
	const gchar *name;
	const gchar *kind_name;
	AiBenchmarkKind default_kind;
	gboolean has_default_kind;
	guint value;
	YamlNode *list;
	GPtrArray *targets;
	GPtrArray *cases;
	guint i;
	guint k;

	if (!is_mapping(root))
		return suite_fail(error, "the document must be a mapping with 'targets' and 'cases'");
	if (!check_keys(root, SUITE_KEYS, "top level", error) ||
	    !read_string(root, "name", "top level", FALSE, &name, error) ||
	    !read_string(root, "kind", "top level", FALSE, &kind_name, error))
		return FALSE;

	/* Settings. */
	ai_benchmark_set_name(self, name);
	value = ai_benchmark_get_iterations(self);
	if (!read_uint(root, "iterations", "top level", 1, AI_BENCHMARK_MAX_ITERATIONS, &value, error))
		return FALSE;
	ai_benchmark_set_iterations(self, value);
	value = ai_benchmark_get_warmup(self);
	if (!read_uint(root, "warmup", "top level", 0, AI_BENCHMARK_MAX_WARMUP, &value, error))
		return FALSE;
	ai_benchmark_set_warmup(self, value);
	value = ai_benchmark_get_timeout_ms(self);
	if (!read_uint(root, "timeout-ms", "top level", 0, G_MAXUINT32, &value, error))
		return FALSE;
	ai_benchmark_set_timeout_ms(self, value);

	has_default_kind = kind_name != NULL;
	default_kind = AI_BENCHMARK_KIND_CHAT;
	if (has_default_kind && !ai_benchmark_kind_from_string(kind_name, &default_kind))
		return suite_fail(error, "top level: unknown kind '%s' (chat, decision, embedding or image)", kind_name);

	/* targets and cases, in file order. */
	list = member(root, "targets");
	if (!is_sequence(list) || yaml_sequence_get_length(yaml_node_get_sequence(list)) == 0)
		return suite_fail(error, "'targets' must be a non-empty list");
	for (i = 0; i < yaml_sequence_get_length(yaml_node_get_sequence(list)); i++)
		if (!load_target(self, yaml_sequence_get_element(yaml_node_get_sequence(list), i), i,
		                 default_kind, has_default_kind, error))
			return FALSE;

	list = member(root, "cases");
	if (!is_sequence(list) || yaml_sequence_get_length(yaml_node_get_sequence(list)) == 0)
		return suite_fail(error, "'cases' must be a non-empty list");
	for (i = 0; i < yaml_sequence_get_length(yaml_node_get_sequence(list)); i++)
		if (!load_case(self, yaml_sequence_get_element(yaml_node_get_sequence(list), i), i,
		               default_kind, has_default_kind, base_dir, error))
			return FALSE;

	/* Every case must have at least one target that can run it. */
	targets = ai_benchmark_get_targets(self);
	cases = ai_benchmark_get_cases(self);
	for (i = 0; i < cases->len; i++)
	{
		AiBenchmarkCase *bench_case = (AiBenchmarkCase *)g_ptr_array_index(cases, i);
		gboolean matched = FALSE;

		for (k = 0; k < targets->len && !matched; k++)
			matched = ai_benchmark_target_get_kind((AiBenchmarkTarget *)g_ptr_array_index(targets, k))
			          == ai_benchmark_case_get_kind(bench_case);
		if (!matched)
			return suite_fail(error, "case '%s' has kind %s and no target has that kind",
			                  ai_benchmark_case_get_id(bench_case),
			                  ai_benchmark_kind_to_string(ai_benchmark_case_get_kind(bench_case)));
	}
	return TRUE;
}

/**
 * ai_benchmark_new_from_data:
 * @data: (array length=length) (element-type guint8): a YAML or JSON suite
 * @length: length of @data in bytes, or -1 when NUL-terminated
 * @base_dir: (nullable): directory that relative `request-file` paths
 *   resolve against; %NULL for the process's working directory
 * @error: return location for an error
 *
 * Parses a suite. Every key is validated, every decision request and
 * expectation is checked against its question, and every case must have
 * a target of its kind. Nothing is built or called. The schema is in
 * docs/benchmark.org.
 *
 * Returns: (transfer full) (nullable): the benchmark, or %NULL with an
 *   error naming the key and where it was
 */
AiBenchmark *
ai_benchmark_new_from_data(
	const gchar  *data,
	gssize        length,
	const gchar  *base_dir,
	GError      **error
){
	g_autoptr(AiBenchmark) self = NULL;
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) local = NULL;
	YamlNode *root;
	gsize size;
	guint count;

	g_return_val_if_fail(data != NULL, NULL);

	/* 1. Bytes: bounded, UTF-8, no NUL inside. */
	size = length < 0 ? strlen(data) : (gsize)length;
	if (size > SUITE_MAX_BYTES)
	{
		suite_fail(error, "larger than %d bytes", SUITE_MAX_BYTES);
		return NULL;
	}
	if (memchr(data, '\0', size) != NULL || !g_utf8_validate(data, (gssize)size, NULL))
	{
		suite_fail(error, "not valid UTF-8 text");
		return NULL;
	}

	/* 2. Parse; exactly one document. */
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, data, (gssize)size, &local))
	{
		suite_fail(error, "not valid YAML: %s", local->message);
		return NULL;
	}
	if (yaml_parser_get_n_documents(parser) != 1)
	{
		suite_fail(error, "expected one YAML document, found %u", yaml_parser_get_n_documents(parser));
		return NULL;
	}
	root = yaml_parser_get_root(parser);
	if (root == NULL)
	{
		suite_fail(error, "the document is empty");
		return NULL;
	}

	/* 3. Bound the tree, then read it. */
	count = 0;
	if (!check_shape(root, 0, &count, error))
		return NULL;

	self = ai_benchmark_new();
	if (!load_document(self, root, base_dir, error))
		return NULL;
	return (AiBenchmark *)g_steal_pointer(&self);
}

/**
 * ai_benchmark_new_from_file:
 * @path: (type filename): a YAML or JSON suite file
 * @error: return location for an error
 *
 * As ai_benchmark_new_from_data(), with relative `request-file` paths
 * resolved against the suite's own directory.
 *
 * Returns: (transfer full) (nullable): the benchmark, or %NULL on error
 */
AiBenchmark *
ai_benchmark_new_from_file(
	const gchar  *path,
	GError      **error
){
	g_autofree gchar *data = NULL;
	g_autofree gchar *dir = NULL;
	gsize length;

	g_return_val_if_fail(path != NULL, NULL);

	if (!read_bounded(path, &data, &length, error))
		return NULL;
	dir = g_path_get_dirname(path);
	return ai_benchmark_new_from_data(data, (gssize)length, dir, error);
}
