/* Semantic tokens from a language server, falling back to nothing. */
#include "config.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gunixinputstream.h>
#include <poll.h>

#include <json-glib/json-glib.h>

#include "core/ai-json-util.h"
#include "view/ai-lsp.h"

typedef struct
{
	const gchar *languages;
	const gchar *language_id;
	const gchar *program;
} Server;

static const Server SERVERS[] = {
	{ "c h cpp cc cxx", "c", "clangd" },
	{ "py python", "python", "pylsp" },
	{ "rs rust", "rust", "rust-analyzer" },
	{ "go", "go", "gopls" },
	{ "js javascript jsx ts typescript tsx", "javascript", "typescript-language-server" }
};

static gboolean
name_in(const gchar *words, const gchar *word)
{
	const gchar *p = words;
	gsize len;

	if (word == NULL || word[0] == '\0')
		return FALSE;
	len = strlen(word);
	while ((p = strstr(p, word)) != NULL)
	{
		if ((p == words || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' '))
			return TRUE;
		p++;
	}
	return FALSE;
}

static const Server *
server_for(const gchar *language)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(SERVERS); i++)
		if (name_in(SERVERS[i].languages, language))
			return &SERVERS[i];
	return NULL;
}

const gchar *
ai_lsp_command_for_language(const gchar *language)
{
	const gchar *env = g_getenv("AI_LSP");
	const Server *server;
	gchar **parts = NULL;
	guint i;
	const gchar *found = NULL;

	if (env == NULL || env[0] == '\0' || g_ascii_strcasecmp(env, "off") == 0 || g_strcmp0(env, "0") == 0)
		return NULL;
	server = server_for(language);
	if (g_ascii_strcasecmp(env, "auto") != 0)
	{
		parts = g_strsplit(env, ",", -1);
		for (i = 0; parts[i] != NULL; i++)
		{
			gchar *eq = strchr(parts[i], '=');

			if (eq == NULL)
				continue;
			*eq = '\0';
			if (name_in(server != NULL ? server->languages : language, parts[i]) ||
			    g_strcmp0(parts[i], language) == 0)
			{
				found = g_intern_string(eq + 1);
				break;
			}
		}
		g_strfreev(parts);
		return found;
	}
	if (server == NULL)
		return NULL;
	{
		g_autofree gchar *path = g_find_program_in_path(server->program);

		return path != NULL ? server->program : NULL;
	}
}

static void
setpgid_child(gpointer data)
{
	(void)data;
	setpgid(0, 0);
}

static gboolean
write_message(GOutputStream *out, const gchar *json, volatile gint *cancelled, GError **error)
{
	g_autofree gchar *frame = g_strdup_printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
	gsize len = strlen(frame);
	gsize sent = 0;

	/* A full pipe must not block the worker past a cancel. The read
	 * side already polls; this is the same bound on the way in. */
	while (sent < len)
	{
		gssize n;

		if (cancelled != NULL && g_atomic_int_get(cancelled))
		{
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "language server cancelled");
			return FALSE;
		}
		n = g_pollable_output_stream_write_nonblocking(G_POLLABLE_OUTPUT_STREAM(out),
		                                               frame + sent, len - sent, NULL, error);
		if (n > 0)
			sent += (gsize)n;
		else if (n < 0 && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
		{
			g_clear_error(error);
			g_usleep(5 * 1000);
		}
		else
			return FALSE;
	}
	return TRUE;
}

static gboolean
cancelled_or_late(volatile gint *cancelled, gint64 deadline)
{
	return (cancelled != NULL && g_atomic_int_get(cancelled)) || g_get_monotonic_time() >= deadline;
}

/* Block until the pipe is readable, the query is cancelled, or the
 * deadline passes. Sleeping here is what made a freed block wait out
 * the whole three seconds. */
/* Short waits, rechecking the flag. A blocking poll cannot see a flag
 * written by another thread, and GTask's cancellable is not visible here. */
static gboolean
wait_readable(GInputStream *in, volatile gint *cancelled, gint64 deadline)
{
	GPollFD fd;
	gint timeout;
	gint slice;

	if (cancelled_or_late(cancelled, deadline))
		return FALSE;
	timeout = (gint)MIN(G_MAXINT, (deadline - g_get_monotonic_time()) / 1000);
	slice = MIN(timeout, 5);
	fd.fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(in));
	fd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
	fd.revents = 0;
	/* One quiet slice is not the end. A slow server still has the
	 * rest of the deadline; only the flag or the clock ends it. */
	while (!cancelled_or_late(cancelled, deadline))
	{
		timeout = (gint)MIN(G_MAXINT, (deadline - g_get_monotonic_time()) / 1000);
		slice = MIN(MAX(timeout, 0), 5);
		fd.revents = 0;
		if (g_poll(&fd, 1, slice) > 0 && fd.revents != 0)
			return !cancelled_or_late(cancelled, deadline);
	}
	return FALSE;
}

static gchar *
read_message(GPollableInputStream *in, volatile gint *cancelled, gint64 deadline, GError **error)
{
	GString *header = g_string_new(NULL);
	gint length = -1;

	while (!cancelled_or_late(cancelled, deadline))
	{
		gssize n;
		gchar ch;

		n = g_pollable_input_stream_read_nonblocking(in, &ch, 1, NULL, error);
		if (n == 0)
		{
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "language server closed the stream");
			g_string_free(header, TRUE);
			return NULL;
		}
		if (n < 0)
		{
			if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
			{
				g_clear_error(error);
				if (!wait_readable(G_INPUT_STREAM(in), cancelled, deadline))
					break;
				continue;
			}
			g_string_free(header, TRUE);
			return NULL;
		}
		g_string_append_c(header, ch);
		if (g_str_has_suffix(header->str, "\r\n\r\n") || g_str_has_suffix(header->str, "\n\n"))
		{
			const gchar *mark = g_strrstr(header->str, "Content-Length:");

			if (mark != NULL)
				length = atoi(mark + strlen("Content-Length:"));
			break;
		}
	}
	g_string_free(header, TRUE);
	if (cancelled != NULL && g_atomic_int_get(cancelled))
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "language server cancelled");
		return NULL;
	}
	if (length < 0 || g_get_monotonic_time() >= deadline)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "language server timed out");
		return NULL;
	}
	{
		g_autofree gchar *body = g_malloc((gsize)length + 1);
		gsize got = 0;

		while (got < (gsize)length && !cancelled_or_late(cancelled, deadline))
		{
			gssize n = g_pollable_input_stream_read_nonblocking(in, body + got, (gsize)length - got, NULL, error);

			if (n > 0)
				got += (gsize)n;
			else if (n < 0 && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
			{
				g_clear_error(error);
				if (!wait_readable(G_INPUT_STREAM(in), cancelled, deadline))
					break;
			}
			else
				return NULL;
		}
		if (got < (gsize)length)
		{
			if (cancelled != NULL && g_atomic_int_get(cancelled))
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "language server cancelled");
			else
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "language server timed out");
			return NULL;
		}
		body[length] = '\0';
		return g_steal_pointer(&body);
	}
}

static gint64
array_int(JsonArray *array, guint index_)
{
	JsonNode *node = array != NULL ? json_array_get_element(array, index_) : NULL;
	GType type;

	if (node == NULL || json_node_get_node_type(node) != JSON_NODE_VALUE)
		return 0;
	type = json_node_get_value_type(node);
	if (type == G_TYPE_INT64 || type == G_TYPE_INT)
		return json_node_get_int(node);
	if (type == G_TYPE_DOUBLE)
		return (gint64)json_node_get_double(node);
	return 0;
}

static const gchar *
array_string(JsonArray *array, guint index_)
{
	JsonNode *node = array != NULL ? json_array_get_element(array, index_) : NULL;

	if (node == NULL || json_node_get_node_type(node) != JSON_NODE_VALUE)
		return NULL;
	if (json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

static AiStyleTag
tag_for_type(const gchar *name)
{
	if (name == NULL)
		return AI_STYLE_DEFAULT;
	if (g_strcmp0(name, "keyword") == 0)
		return AI_STYLE_SYNTAX_KEYWORD;
	if (g_strcmp0(name, "string") == 0)
		return AI_STYLE_SYNTAX_STRING;
	if (g_strcmp0(name, "comment") == 0)
		return AI_STYLE_SYNTAX_COMMENT;
	if (g_strcmp0(name, "number") == 0)
		return AI_STYLE_SYNTAX_NUMBER;
	if (strstr(name, "function") != NULL || g_strcmp0(name, "method") == 0)
		return AI_STYLE_SYNTAX_FUNCTION;
	if (strstr(name, "type") != NULL || g_strcmp0(name, "class") == 0 || g_strcmp0(name, "struct") == 0)
		return AI_STYLE_SYNTAX_TYPE;
	return AI_STYLE_DEFAULT;
}

static guint
offset_at(const gchar *text, guint line, guint utf16_col)
{
	const gchar *p = text;
	guint current = 0;

	while (*p != '\0' && current < line)
	{
		if (*p == '\n')
			current++;
		p++;
	}
	{
		guint units = 0;

		while (*p != '\0' && *p != '\n' && units < utf16_col)
		{
			gunichar ch = g_utf8_get_char(p);

			units += ch > 0xFFFF ? 2 : 1;
			p = g_utf8_next_char(p);
		}
	}
	return (guint)(p - text);
}

static void
decode_tokens(GArray *out, const gchar *body, guint base, JsonArray *data, JsonArray *types)
{
	guint line = 0;
	guint col = 0;
	guint i;

	if (data == NULL)
		return;
	for (i = 0; i + 4 < json_array_get_length(data); i += 5)
	{
		guint delta_line = (guint)array_int(data, i);
		guint delta_col = (guint)array_int(data, i + 1);
		guint length = (guint)array_int(data, i + 2);
		guint type = (guint)array_int(data, i + 3);
		const gchar *name = array_string(types, type);
		AiStyleTag tag = tag_for_type(name);
		AiMarkupToken token;

		line += delta_line;
		col = delta_line > 0 ? delta_col : col + delta_col;
		if (tag == AI_STYLE_DEFAULT || length == 0)
			continue;
		{
			guint rel = offset_at(body, line, col);

			token.start = base + rel;
			token.length = offset_at(body + rel, 0, length);
		}
		token.tag = tag;
		g_array_append_val(out, token);
	}
}

static JsonArray *
legend_types(JsonObject *init)
{
	JsonObject *result = ai_json_get_object(init, "result");
	JsonObject *capabilities = ai_json_get_object(result, "capabilities");
	JsonObject *provider = ai_json_get_object(capabilities, "semanticTokensProvider");
	JsonObject *legend = ai_json_get_object(provider, "legend");

	return ai_json_get_array(legend, "tokenTypes");
}

/* Every path out of a spawn has to reap. A continue that skipped the
 * wait left the server running after the block that asked for it was gone. */
static void
reap_server(GSubprocess *proc, GOutputStream *out)
{
	if (proc == NULL)
		return;
	if (out != NULL)
	{
		write_message(out, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}", NULL, NULL);
		write_message(out, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}", NULL, NULL);
		g_output_stream_close(out, NULL, NULL);
	}
	/* The child is its own process group. SIGKILL that group: a server
	 * that has forked, or that is ignoring the default SIGTERM, would
	 * otherwise outlive the block that started it. */
	kill(-atoi(g_subprocess_get_identifier(proc)), SIGKILL);
	g_subprocess_wait(proc, NULL, NULL);
}

static gboolean
highlight_fence(GSubprocess *proc, GPollableInputStream *in, GOutputStream *out,
                const gchar *language_id, const gchar *body, guint base,
                JsonArray *types, GArray *tokens, volatile gint *cancelled,
                gint64 deadline, guint *next_id, GError **error)
{
	g_autofree gchar *open = NULL;
	g_autofree gchar *request = NULL;
	g_autofree gchar *reply = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonObject *object;
	JsonObject *result;
	const gchar *escaped;
	g_autofree gchar *uri = g_strdup_printf("file:///ai-glib/%u.%s", *next_id, language_id);

	escaped = body;
	{
		g_autoptr(JsonNode) text_node = json_node_init_string(json_node_alloc(), body);
		g_autofree gchar *encoded = json_to_string(text_node, FALSE);

		open = g_strdup_printf(
			"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"%s\",\"version\":1,\"text\":%s}}}",
			uri, language_id, encoded);
	}
	(void)escaped;
	if (!write_message(out, open, cancelled, error))
		return FALSE;
	request = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"textDocument/semanticTokens/full\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
		*next_id, uri);
	(*next_id)++;
	if (!write_message(out, request, cancelled, error))
		return FALSE;
	do
	{
		g_free(reply);
		reply = read_message(in, cancelled, deadline, error);
		if (reply == NULL)
			return FALSE;
	} while (strstr(reply, "semanticTokens") == NULL && strstr(reply, "\"result\"") == NULL);
	if (!json_parser_load_from_data(parser, reply, -1, error))
		return FALSE;
	root = json_parser_get_root(parser);
	object = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
	result = ai_json_get_object(object, "result");
	if (result == NULL)
		return TRUE;
	decode_tokens(tokens, body, base, ai_json_get_array(result, "data"), types);
	(void)proc;
	return TRUE;
}

GArray *
ai_lsp_semantic_tokens(const gchar *text, volatile gint *cancelled, GError **error)
{
	g_autoptr(GArray) fences = NULL;
	g_autoptr(GArray) tokens = NULL;
	guint i;

	fences = ai_markup_fences(text);
	tokens = g_array_new(FALSE, TRUE, sizeof(AiMarkupToken));
	for (i = 0; i < fences->len; i++)
	{
		AiMarkupFence *fence = &g_array_index(fences, AiMarkupFence, i);
		const gchar *command = ai_lsp_command_for_language(fence->language);
		const Server *server = server_for(fence->language);
		g_autoptr(GSubprocessLauncher) launcher = NULL;
		g_autoptr(GSubprocess) proc = NULL;
		g_autoptr(GError) local = NULL;
		g_autofree gchar *body = NULL;
		g_autofree gchar *init = NULL;
		g_autofree gchar *ready = NULL;
		g_autoptr(JsonParser) parser = NULL;
		JsonArray *types = NULL;
		gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
		guint next_id = 2;

		if (cancelled != NULL && g_atomic_int_get(cancelled))
			break;
		if (command == NULL || command[0] == '\0' || fence->length == 0)
			continue;
		body = g_strndup(text + fence->body, fence->length);
		launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
		/* Own process group, so teardown can signal the server and
		 * anything it has already forked. */
		g_subprocess_launcher_set_child_setup(launcher, (GSpawnChildSetupFunc)setpgid_child, NULL, NULL);
		proc = g_subprocess_launcher_spawn(launcher, &local, command, NULL);
		if (proc == NULL)
			continue;
		init = g_strdup("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{\"textDocument\":{\"semanticTokens\":{\"requests\":{\"full\":true},\"tokenTypes\":[\"keyword\",\"string\",\"comment\",\"number\",\"type\",\"function\"],\"tokenModifiers\":[]}}}}}");
		if (!write_message(g_subprocess_get_stdin_pipe(proc), init, cancelled, &local) ||
		    (ready = read_message(G_POLLABLE_INPUT_STREAM(g_subprocess_get_stdout_pipe(proc)),
		                          cancelled, deadline, &local)) == NULL)
		{
			reap_server(proc, g_subprocess_get_stdin_pipe(proc));
			continue;
		}
		parser = json_parser_new();
		if (json_parser_load_from_data(parser, ready, -1, NULL))
		{
			JsonNode *root = json_parser_get_root(parser);

			if (JSON_NODE_HOLDS_OBJECT(root))
				types = legend_types(json_node_get_object(root));
		}
		if (!write_message(g_subprocess_get_stdin_pipe(proc),
		                   "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}", cancelled, &local) ||
		    !highlight_fence(proc, G_POLLABLE_INPUT_STREAM(g_subprocess_get_stdout_pipe(proc)),
		                     g_subprocess_get_stdin_pipe(proc),
		                     server != NULL ? server->language_id : fence->language,
		                     body, fence->body, types, tokens, cancelled, deadline, &next_id, &local))
		{
			reap_server(proc, g_subprocess_get_stdin_pipe(proc));
			continue;
		}
		reap_server(proc, g_subprocess_get_stdin_pipe(proc));
	}
	g_array_set_clear_func(fences, ai_markup_fence_free);
	return g_steal_pointer(&tokens);
}
