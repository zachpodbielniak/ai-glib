/* Tool previews from recorded requests/results, never the current filesystem.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include <string.h>
#include "view/ai-tool-preview.h"
#include "core/ai-json-util.h"

#define PREVIEW_BYTES (65536)
#define PREVIEW_LINES (256)
#define LINE_BYTES (320)
#define COMMENT_C (1)
#define COMMENT_HASH (2)
#define COMMENT_SEMI (4)
#define COMMENT_DASH (8)
#define PYTHON_QUOTES (16)
#define CASE_INSENSITIVE (32)

typedef struct {
	const gchar *extensions;
	const gchar *keywords;
	guint flags;
} Language;

/* Token roles, not colors. Unknown extensions remain literal code. */
static const Language LANGUAGES[] = {
	{ ".c .h", "auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static struct switch typedef union unsigned signed volatile while", COMMENT_C },
	{ ".cc .cpp .cxx .hpp .hh", "alignas auto bool break case catch class const constexpr continue default delete do else enum explicit false for if namespace new noexcept nullptr private protected public return sizeof static struct switch template this throw true try typedef typename using virtual void while", COMMENT_C },
	{ ".js .jsx .ts .tsx", "async await break case catch class const continue default delete do else export extends false finally for from function if import in instanceof interface let new null of return static super switch this throw true try typeof undefined var void while yield", COMMENT_C },
	{ ".rs", "as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while", COMMENT_C },
	{ ".go", "break case chan const continue default defer else fallthrough false for func go goto if import interface map nil package range return select struct switch true type var", COMMENT_C },
	{ ".py", "and as assert async await break class continue def del elif else except False finally for from global if import in is lambda None nonlocal not or pass raise return True try while with yield", COMMENT_HASH | PYTHON_QUOTES },
	{ ".sh .bash .zsh Makefile .mk", "case do done elif else esac export fi for function if in local readonly return select then until while", COMMENT_HASH },
	{ ".json", "true false null", 0 },
	{ ".yaml .yml", "true false null yes no on off", COMMENT_HASH },
	{ ".el .lisp .scm", "defun defmacro defvar defcustom let lambda if when unless cond progn quote setq nil t", COMMENT_SEMI },
	{ ".hs", "case class data default deriving do else foreign if import in infix infixl infixr instance let module newtype of then type where", COMMENT_DASH },
	{ ".sql", "select from where insert into values update set delete create table index join on as and or not null true false order by group having limit", COMMENT_DASH | COMMENT_C | CASE_INSENSITIVE }
};

typedef struct {
	gboolean comment;
	gchar quote;
	gboolean triple;
} CodeState;

typedef struct {
	AiRenderedText *out;
	guint shown;
	guint limit;
	gboolean truncated;
} Preview;

typedef struct {
	gchar kind;
	guint old_index;
	guint new_index;
} DiffLine;

/* Match complete vocabulary entries, never substrings such as 'in' in 'print'. */
static gboolean
word_in(const gchar *words, const gchar *word)
{
	const gchar *p = words;
	gsize len = strlen(word);
	while (len > 0 && (p = strstr(p, word)) != NULL)
	{
		if ((p == words || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' ')) return TRUE;
		p++;
	}
	return FALSE;
}

static const Language *
language_for(const gchar *path)
{
	g_autofree gchar *base = path != NULL ? g_path_get_basename(path) : NULL;
	const gchar *extension = base != NULL ? strrchr(base, '.') : NULL;
	guint i;
	for (i = 0; base != NULL && i < G_N_ELEMENTS(LANGUAGES); i++)
		if (word_in(LANGUAGES[i].extensions, extension != NULL ? extension : base)) return &LANGUAGES[i];
	return NULL;
}

/* Strip terminal control sequences and bound allocation before splitting.
 * A malformed UTF-8 or ANSI result must not control the screen or abort GTest. */
static GStrv
preview_lines(const gchar *raw, gboolean *clipped)
{
	g_autoptr(GString) clean = g_string_new(NULL);
	g_autofree gchar *valid = NULL;
	const gchar *p = raw != NULL ? raw : "";
	gsize size = strnlen(p, PREVIEW_BYTES + 1);
	const gchar *end = p + MIN(size, PREVIEW_BYTES);
	GStrv lines;
	guint count;
	*clipped |= size > PREVIEW_BYTES;
	while (p < end)
	{
		if (*p == '\033')
		{
			p++;
			if (p < end && *p == '[')
			{
				p++;
				while (p < end && !(*p >= '@' && *p <= '~')) p++;
				if (p < end) p++;
			}
			else if (p < end && *p == ']')
			{
				p++;
				while (p < end && *p != '\a' && !(p + 1 < end && p[0] == '\033' && p[1] == '\\')) p++;
				if (p < end) p += *p == '\a' ? 1 : 2;
			}
			else if (p < end) p++;
			continue;
		}
		/* Preserve tabs for line comparison; only presentation expands them. */
		if (*p == '\t') g_string_append_c(clean, '\t');
		else if (*p == '\n' || (guchar)*p >= 32) g_string_append_c(clean, *p == 127 ? ' ' : *p);
		p++;
	}
	valid = g_utf8_make_valid(clean->str, clean->len);
	lines = g_strsplit(valid, "\n", PREVIEW_LINES + 1);
	count = g_strv_length(lines);
	if (count > PREVIEW_LINES)
	{
		g_clear_pointer(&lines[PREVIEW_LINES], g_free);
		count = PREVIEW_LINES;
		*clipped = TRUE;
	}
	if (count > 0 && lines[count - 1][0] == '\0') g_clear_pointer(&lines[count - 1], g_free);
	return lines;
}

/* Small language-aware lexer. Multiline comment/string state is independent
 * for the old and new sides; scanning hidden context keeps it accurate. */
static void
append_code(AiRenderedText *out, const gchar *text, const Language *language, CodeState *state)
{
	const gchar *p = text;
	guint flags = language != NULL ? language->flags : 0;
	gsize emitted = 0;
	gboolean clipped = FALSE;
	while (*p)
	{
		const gchar *start = p;
		AiStyleTag tag = language != NULL ? AI_STYLE_DEFAULT : AI_STYLE_CODE;
		if (language == NULL) p += strlen(p);
		else if (!state->quote && (state->comment || ((flags & COMMENT_C) && g_str_has_prefix(p, "/*"))))
		{
			const gchar *end;
			if (!state->comment) p += 2;
			end = strstr(p, "*/");
			state->comment = end == NULL;
			p = end != NULL ? end + 2 : p + strlen(p);
			tag = AI_STYLE_SYNTAX_COMMENT;
		}
		else if (!state->quote && (((flags & COMMENT_C) && g_str_has_prefix(p, "//")) ||
			((flags & COMMENT_HASH) && *p == '#' && (p == text || g_ascii_isspace(p[-1]))) ||
			((flags & COMMENT_SEMI) && *p == ';') ||
			((flags & COMMENT_DASH) && g_str_has_prefix(p, "--"))))
		{
			p += strlen(p);
			tag = AI_STYLE_SYNTAX_COMMENT;
		}
		else if (state->quote || *p == '"' || *p == '\'' || *p == '`')
		{
			if (!state->quote)
			{
				state->quote = *p++;
				state->triple = (flags & PYTHON_QUOTES) && p[0] == state->quote && p[1] == state->quote;
				if (state->triple) p += 2;
			}
			while (*p)
			{
				if (*p == '\\' && p[1]) { p = g_utf8_next_char(p + 1); continue; }
				if (*p == state->quote && (!state->triple || (p[1] == state->quote && p[2] == state->quote)))
				{
					p += state->triple ? 3 : 1;
					state->quote = 0;
					break;
				}
				p = g_utf8_next_char(p);
			}
			tag = AI_STYLE_SYNTAX_STRING;
		}
		else if (g_ascii_isdigit(*p))
		{
			while (g_ascii_isalnum(*p) || *p == '.' || *p == '_') p++;
			tag = AI_STYLE_SYNTAX_NUMBER;
		}
		else if (g_ascii_isalpha(*p) || *p == '_' || ((flags & COMMENT_C) && *p == '#'))
		{
			g_autofree gchar *word = NULL;
			const gchar *next;
			p++;
			while (g_ascii_isalnum(*p) || *p == '_') p++;
			word = g_strndup(start, (gsize)(p - start));
			if (flags & CASE_INSENSITIVE)
			{
				gchar *lower = g_ascii_strdown(word, -1);
				g_free(word);
				word = lower;
			}
			next = p;
			while (g_ascii_isspace(*next)) next++;
			if (*start == '#' || word_in(language->keywords, word)) tag = AI_STYLE_SYNTAX_KEYWORD;
			else if (word_in("void char short int long float double bool size_t ssize_t gint guint gchar gboolean gsize gint64 guint64 gpointer", word) || g_ascii_isupper(*start)) tag = AI_STYLE_SYNTAX_TYPE;
			else if (*next == '(') tag = AI_STYLE_SYNTAX_FUNCTION;
		}
		else p = g_utf8_next_char(p);
		if (out != NULL && !clipped)
		{
			const gchar *q;
			g_autoptr(GString) piece = g_string_new(NULL);
			for (q = start; q < p; q = g_utf8_next_char(q))
			{
				gsize len = *q == '\t' ? 4 : (gsize)(g_utf8_next_char(q) - q);
				if (emitted + len > LINE_BYTES) { clipped = TRUE; break; }
				if (*q == '\t') g_string_append(piece, "    ");
				else g_string_append_len(piece, q, len);
				emitted += len;
			}
			ai_rendered_text_append(out, piece->str, tag);
		}
	}
	if (out != NULL && clipped)
		ai_rendered_text_append(out, " ... [line truncated]", AI_STYLE_DIM);
}

/* A gutter conveys addition/removal without painting every syntax token
 * green or red. The two numbers are relative to the supplied edit snippets. */
static void
diff_line(Preview *preview, gchar kind, guint old_no, guint new_no,
          const gchar *text, const Language *language, CodeState *state, gboolean visible)
{
	AiRenderedText *out = visible && preview->shown < preview->limit ? preview->out : NULL;
	if (out != NULL)
	{
		g_autofree gchar *old_label = old_no ? g_strdup_printf("%u", old_no) : g_strdup("");
		g_autofree gchar *new_label = new_no ? g_strdup_printf("%u", new_no) : g_strdup("");
		ai_rendered_text_append_printf(out, kind == '+' ? AI_STYLE_ADDED : kind == '-' ? AI_STYLE_REMOVED : AI_STYLE_DIM,
			"\n    %c %4s %4s | ", kind, old_label, new_label);
		preview->shown++;
	}
	else if (visible) preview->truncated = TRUE;
	append_code(out, text, language, state);
}

/* Bounded LCS produces actual line changes, preserving short context around
 * disjoint hunks. No external diff process, file I/O, or quadratic unbounded input. */
static void
append_diff(Preview *preview, const gchar *old_text, const gchar *new_text, const gchar *path)
{
	g_auto(GStrv) old_lines = preview_lines(old_text, &preview->truncated);
	g_auto(GStrv) new_lines = preview_lines(new_text, &preview->truncated);
	guint n = g_strv_length(old_lines), m = g_strv_length(new_lines), i, j;
	g_autofree guint *lcs = g_new0(guint, (n + 1) * (m + 1));
	g_autoptr(GArray) diff = g_array_new(FALSE, FALSE, sizeof(DiffLine));
	CodeState old_state = { 0 }, new_state = { 0 };
	const Language *language = language_for(path);
	gboolean changed = FALSE, skipped = FALSE;

	for (i = n; i > 0; i--)
		for (j = m; j > 0; j--)
			lcs[(i - 1) * (m + 1) + j - 1] = g_str_equal(old_lines[i - 1], new_lines[j - 1])
				? 1 + lcs[i * (m + 1) + j] : MAX(lcs[i * (m + 1) + j - 1], lcs[(i - 1) * (m + 1) + j]);
	i = j = 0;
	while (i < n || j < m)
	{
		DiffLine line = { ' ', i, j };
		if (i < n && j < m && g_str_equal(old_lines[i], new_lines[j])) { i++; j++; }
		else if (i < n && (j == m || lcs[(i + 1) * (m + 1) + j] >= lcs[i * (m + 1) + j + 1])) { line.kind = '-'; i++; changed = TRUE; }
		else { line.kind = '+'; j++; changed = TRUE; }
		g_array_append_val(diff, line);
	}
	ai_rendered_text_append(preview->out, old_text == NULL
		? "\n    @@ written content; previous content unavailable @@"
		: "\n    @@ edit region; relative old/new lines @@", AI_STYLE_DIM);
	if (!changed) ai_rendered_text_append(preview->out, preview->truncated
		? "\n    (no textual changes in bounded preview window)"
		: "\n    (no changed lines)", AI_STYLE_DIM);
	for (i = 0; i < diff->len && changed; i++)
	{
		DiffLine *line = &g_array_index(diff, DiffLine, i);
		gboolean visible = line->kind != ' ';
		for (j = i > 2 ? i - 2 : 0; !visible && j < MIN(diff->len, i + 3); j++)
			visible = g_array_index(diff, DiffLine, j).kind != ' ';
		if (visible && skipped) ai_rendered_text_append(preview->out, "\n    ... unchanged context ...", AI_STYLE_DIM);
		skipped = !visible;
		diff_line(preview, line->kind, line->kind == '+' ? 0 : line->old_index + 1,
			line->kind == '-' ? 0 : line->new_index + 1,
			line->kind == '+' ? new_lines[line->new_index] : old_lines[line->old_index], language,
			line->kind == '+' ? &new_state : &old_state, visible);
		if (line->kind == ' ') append_code(NULL, new_lines[line->new_index], language, &new_state);
		if (preview->truncated && preview->shown >= preview->limit) break;
	}
	if (old_text != NULL && new_text != NULL &&
		g_str_has_suffix(old_text, "\n") != g_str_has_suffix(new_text, "\n"))
		ai_rendered_text_append(preview->out, g_str_has_suffix(new_text, "\n")
			? "\n    \\ final newline added" : "\n    \\ final newline removed",
			g_str_has_suffix(new_text, "\n") ? AI_STYLE_ADDED : AI_STYLE_REMOVED);
}

/* Unified diffs and apply_patch envelopes already contain their context.
 * Preserve their hunk headers rather than inventing absolute line numbers. */
static void
append_patch(Preview *preview, const gchar *patch, const gchar *path)
{
	g_auto(GStrv) lines = preview_lines(patch, &preview->truncated);
	const Language *language = language_for(path);
	CodeState old_state = { 0 }, new_state = { 0 };
	guint i;
	for (i = 0; lines[i] != NULL; i++)
	{
		const gchar *line = lines[i];
		if (preview->shown >= preview->limit) { preview->truncated = TRUE; break; }
		if (g_str_has_prefix(line, "*** ") || g_str_has_prefix(line, "@@") ||
			g_str_has_prefix(line, "Binary files ") ||
			g_str_has_prefix(line, "--- ") || g_str_has_prefix(line, "+++ "))
		{
			const gchar *file = strstr(line, " File: ");
			if (file != NULL) language = language_for(file + 7);
			else if (g_str_has_prefix(line, "+++ ")) language = language_for(line + 4);
			ai_rendered_text_append(preview->out, "\n    ", AI_STYLE_DEFAULT);
			{
				CodeState state = { 0 };
				append_code(preview->out, line, NULL, &state);
			}
			preview->shown++;
			memset(&old_state, 0, sizeof old_state);
			memset(&new_state, 0, sizeof new_state);
		}
		else if (*line == '+' || *line == '-' || *line == ' ')
		{
			diff_line(preview, *line, 0, 0, line + 1, language, *line == '+' ? &new_state : &old_state, TRUE);
			if (*line == ' ') append_code(NULL, line + 1, language, &new_state);
		}
	}
}

/* Keep aliases here for recorded CLI dialects, rather than changing requests. */
static const gchar *
input_string(JsonObject *input, const gchar *first, const gchar *second)
{
	return ai_json_get_string(input, first, second != NULL ? ai_json_get_string(input, second, NULL) : NULL);
}

void
_ai_tool_preview_append(AiToolCall *call, AiRenderedText *out, gboolean expanded)
{
	AiToolUse *use = ai_tool_call_get_tool_use(call);
	JsonNode *node = use != NULL ? ai_tool_use_get_input(use) : NULL;
	JsonObject *input = node != NULL && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
	AiToolCategory category = ai_tool_call_get_category(call);
	AiToolCallState state = ai_tool_call_get_state(call);
	Preview preview = { out, 0, expanded ? 64 : 12, FALSE };
	const gchar *path = input_string(input, "file_path", "filePath");
	const gchar *result = ai_tool_call_get_result(call);
	const gchar *patch;

	if (path == NULL) path = ai_json_get_string(input, "path", NULL);
	if (category == AI_TOOL_CATEGORY_COMMAND)
	{
		g_auto(GStrv) commands = preview_lines(ai_json_get_string(input, "command", NULL), &preview.truncated);
		g_auto(GStrv) output = NULL;
		CodeState code = { 0 };
		guint i;
		for (i = 0; commands[i] != NULL && i < 6; i++)
		{
			ai_rendered_text_append(out, i == 0 ? "\n    $ " : "\n      ", AI_STYLE_TOOL_TARGET);
			append_code(out, commands[i], language_for("command.sh"), &code);
		}
		if (g_strv_length(commands) > 6) preview.truncated = TRUE;
		output = preview_lines(result, &preview.truncated);
		for (i = 0; output[i] != NULL && i < (expanded ? 64 : 6); i++)
		{
			ai_rendered_text_append(out, "\n      | ", AI_STYLE_DIM);
			append_code(out, output[i], NULL, &code);
		}
		if (output[i] != NULL) preview.truncated = TRUE;
		if (i == 0) ai_rendered_text_append(out, state == AI_TOOL_CALL_DENIED ? "\n      (denied)" :
			state == AI_TOOL_CALL_PENDING || state == AI_TOOL_CALL_RUNNING ? "\n      (waiting for output)" : "\n      (no output)", AI_STYLE_DIM);
	}
	else if (category == AI_TOOL_CATEGORY_FILE_WRITE)
	{
		const gchar *old_text = input_string(input, "old_string", "oldString");
		const gchar *new_text = input_string(input, "new_string", "newString");
		JsonArray *edits = ai_json_get_array(input, "edits");
		JsonArray *changes = ai_json_get_array(input, "changes");
		guint i;
		if (state != AI_TOOL_CALL_OK)
			ai_rendered_text_append(out, "\n    Requested change (not confirmed successful)", AI_STYLE_STATUS);
		patch = input_string(input, "patchText", "patch");
		if (patch == NULL) patch = ai_json_get_string(input, "diff", NULL);
		if (patch != NULL) append_patch(&preview, patch, path);
		else if (old_text != NULL && new_text != NULL) append_diff(&preview, old_text, new_text, path);
		else if (edits != NULL)
		{
			for (i = 0; i < json_array_get_length(edits) && i < 8; i++)
			{
				JsonObject *edit = ai_json_array_get_object(edits, i);
				old_text = input_string(edit, "old_string", "oldString");
				new_text = input_string(edit, "new_string", "newString");
				if (old_text != NULL && new_text != NULL) append_diff(&preview, old_text, new_text, path);
				if (preview.shown >= preview.limit) { i++; break; }
			}
			if (i < json_array_get_length(edits)) preview.truncated = TRUE;
		}
		else if (ai_json_get_string(input, "content", NULL) != NULL)
			append_diff(&preview, NULL, ai_json_get_string(input, "content", NULL), path);
		else if (changes != NULL)
		{
			for (i = 0; i < json_array_get_length(changes) && i < 8; i++)
			{
				JsonObject *change = ai_json_array_get_object(changes, i);
				const gchar *file = ai_json_get_string(change, "path", "(unknown file)");
				CodeState code = { 0 };
				g_auto(GStrv) names = preview_lines(file, &preview.truncated);
				ai_rendered_text_append(out, "\n    File: ", AI_STYLE_TOOL_TARGET);
				if (names[0] != NULL) append_code(out, names[0], NULL, &code);
				patch = ai_json_get_string(change, "diff", NULL);
				if (patch != NULL)
				{
					const gchar *source = ai_json_get_string(change, "diff_source", NULL);
					if (g_strcmp0(source, "working_tree") == 0)
						ai_rendered_text_append(out,
							"\n    Working-tree diff against HEAD (may include earlier changes)", AI_STYLE_DIM);
					else if (g_strcmp0(source, "current_file") == 0)
						ai_rendered_text_append(out,
							"\n    Current file content (no tracked baseline)", AI_STYLE_DIM);
					append_patch(&preview, patch, file);
					if (ai_json_get_boolean(change, "diff_truncated", FALSE)) preview.truncated = TRUE;
				}
				else ai_rendered_text_append(out, " (edit text unavailable)", AI_STYLE_DIM);
				if (preview.shown >= preview.limit) { i++; break; }
			}
			if (i < json_array_get_length(changes)) preview.truncated = TRUE;
		}
		else
			ai_rendered_text_append(out, "\n    Preview unavailable: provider supplied no edit text", AI_STYLE_DIM);
		if ((state == AI_TOOL_CALL_FAILED || state == AI_TOOL_CALL_DENIED) && result != NULL)
		{
			g_auto(GStrv) lines = preview_lines(result, &preview.truncated);
			CodeState code = { 0 };
			if (lines[0] != NULL)
			{
				ai_rendered_text_append(out, "\n    Error: ", AI_STYLE_TOOL_FAILED);
				append_code(out, lines[0], NULL, &code);
			}
		}
	}
	if (preview.truncated)
		ai_rendered_text_append(out, expanded ? "\n    ... preview limit reached" : "\n    ... more; expand for details", AI_STYLE_DIM);
}
