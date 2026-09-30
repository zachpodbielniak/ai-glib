/* Markdown and Org rendered as style spans. The source text is unchanged. */
#include "config.h"

#include <string.h>

#include "view/ai-markup.h"

typedef struct
{
	const gchar *names;
	const gchar *keywords;
	guint        flags;
} Language;

#define COMMENT_C (1)
#define COMMENT_HASH (2)
#define COMMENT_SEMI (4)
#define PYTHON_QUOTES (8)

static const Language LANGUAGES[] = {
	{ "c h cpp cc cxx", "auto break case const continue default do else enum extern for goto if inline return sizeof static struct switch typedef union unsigned signed volatile while int void char", COMMENT_C },
	{ "js javascript jsx ts typescript tsx", "async await break case catch class const continue default else export extends false finally for from function if import in let new null of return static super switch this throw true try typeof undefined var void while yield", COMMENT_C },
	{ "rs rust", "as async await break const continue else enum false fn for if impl in let loop match mod mut pub ref return self struct true type use where while", COMMENT_C },
	{ "go", "break case chan const continue default defer else fallthrough false for func go goto if import interface map nil package range return select struct switch true type var", COMMENT_C },
	{ "py python", "and as assert async await break class continue def del elif else except False finally for from global if import in is lambda None nonlocal not or pass raise return True try while with yield", COMMENT_HASH | PYTHON_QUOTES },
	{ "sh bash zsh shell", "case do done elif else esac export fi for function if in local return then until while", COMMENT_HASH },
	{ "el elisp lisp", "defun defmacro defvar let lambda if when unless cond progn quote setq nil t", COMMENT_SEMI }
};

static gboolean
word_in(const gchar *words, const gchar *word)
{
	const gchar *p = words;
	gsize        len = strlen(word);

	while (len > 0 && (p = strstr(p, word)) != NULL)
	{
		if ((p == words || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' '))
			return TRUE;
		p++;
	}
	return FALSE;
}

static const Language *
language_for(const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0')
		return NULL;
	for (i = 0; i < G_N_ELEMENTS(LANGUAGES); i++)
		if (word_in(LANGUAGES[i].names, name))
			return &LANGUAGES[i];
	return NULL;
}

static gboolean
document_is_org(const gchar *text)
{
	const gchar *p;

	if (text == NULL)
		return FALSE;
	if (strstr(text, "#+BEGIN") != NULL || strstr(text, "#+begin") != NULL ||
	    strstr(text, "#+END") != NULL || strstr(text, "#+end") != NULL)
		return TRUE;
	for (p = text; *p != '\0';)
	{
		const gchar *end = strchr(p, '\n');
		gsize        len = end != NULL ? (gsize)(end - p) : strlen(p);
		guint        stars = 0;

		while (stars < len && p[stars] == '*')
			stars++;
		if (stars >= 2 && stars < len && p[stars] == ' ')
			return TRUE;
		p = end != NULL ? end + 1 : p + len;
	}
	return FALSE;
}

static void
emit_code_lexer(AiRenderedText *out, const gchar *text, gsize length, const gchar *language)
{
	const Language *lang = language_for(language);
	g_autofree gchar *slice = g_strndup(text, length);
	const gchar *p = slice;
	gboolean comment = FALSE;
	gchar quote = 0;
	gboolean triple = FALSE;
	guint flags = lang != NULL ? lang->flags : 0;

	while (*p != '\0')
	{
		const gchar *start = p;
		AiStyleTag tag = lang != NULL ? AI_STYLE_DEFAULT : AI_STYLE_CODE;

		if (lang == NULL)
			p += strlen(p);
		else if (quote == 0 && (comment || ((flags & COMMENT_C) && g_str_has_prefix(p, "/*"))))
		{
			const gchar *close;

			if (!comment)
				p += 2;
			close = strstr(p, "*/");
			comment = close == NULL;
			p = close != NULL ? close + 2 : p + strlen(p);
			tag = AI_STYLE_SYNTAX_COMMENT;
		}
		else if (quote == 0 && (((flags & COMMENT_C) && g_str_has_prefix(p, "//")) ||
		         ((flags & COMMENT_HASH) && *p == '#' && (p == slice || g_ascii_isspace(p[-1]))) ||
		         ((flags & COMMENT_SEMI) && *p == ';')))
		{
			const gchar *nl = strchr(p, '\n');

			p = nl != NULL ? nl : p + strlen(p);
			tag = AI_STYLE_SYNTAX_COMMENT;
		}
		else if (quote != 0 || *p == '"' || *p == '\'' || *p == '`')
		{
			if (quote == 0)
			{
				quote = *p++;
				triple = (flags & PYTHON_QUOTES) && p[0] == quote && p[1] == quote;
				if (triple)
					p += 2;
			}
			while (*p != '\0')
			{
				if (*p == '\\' && p[1] != '\0')
				{
					p = g_utf8_next_char(p + 1);
					continue;
				}
				if (*p == quote && (!triple || (p[1] == quote && p[2] == quote)))
				{
					p += triple ? 3 : 1;
					quote = 0;
					break;
				}
				p = g_utf8_next_char(p);
			}
			tag = AI_STYLE_SYNTAX_STRING;
		}
		else if (g_ascii_isdigit(*p))
		{
			while (g_ascii_isalnum(*p) || *p == '.' || *p == '_')
				p++;
			tag = AI_STYLE_SYNTAX_NUMBER;
		}
		else if (g_ascii_isalpha(*p) || *p == '_' || ((flags & COMMENT_C) && *p == '#'))
		{
			g_autofree gchar *word = NULL;
			const gchar *next;

			p++;
			while (g_ascii_isalnum(*p) || *p == '_')
				p++;
			word = g_strndup(start, (gsize)(p - start));
			next = p;
			while (g_ascii_isspace(*next))
				next++;
			if (*start == '#' || word_in(lang->keywords, word))
				tag = AI_STYLE_SYNTAX_KEYWORD;
			else if (g_ascii_isupper(*start))
				tag = AI_STYLE_SYNTAX_TYPE;
			else if (*next == '(')
				tag = AI_STYLE_SYNTAX_FUNCTION;
		}
		else
			p = g_utf8_next_char(p);
		if (p > start)
		{
			g_autofree gchar *piece = g_strndup(start, (gsize)(p - start));

			ai_rendered_text_append(out, piece, tag);
		}
	}
}

static void
emit_code(AiRenderedText *out, const gchar *text, guint abs_start, gsize length,
          const gchar *language, GArray *tokens)
{
	guint i;
	gboolean any = FALSE;

	if (tokens != NULL)
	{
		for (i = 0; i < tokens->len; i++)
		{
			AiMarkupToken *token = &g_array_index(tokens, AiMarkupToken, i);

			if (token->start < abs_start + length && token->start + token->length > abs_start)
				any = TRUE;
		}
	}
	if (!any)
	{
		emit_code_lexer(out, text + abs_start, length, language);
		return;
	}
	{
		guint pos = 0;

		while (pos < length)
		{
			guint best = length;
			AiStyleTag tag = AI_STYLE_CODE;
			guint t;

			for (t = 0; t < tokens->len; t++)
			{
				AiMarkupToken *token = &g_array_index(tokens, AiMarkupToken, t);
				guint start = token->start > abs_start ? token->start - abs_start : 0;
				guint end = token->start + token->length > abs_start
					? MIN(token->start + token->length - abs_start, (guint)length) : 0;

				if (end <= pos || start >= length)
					continue;
				if (start <= pos && end > pos)
				{
					best = end;
					tag = token->tag;
					break;
				}
				if (start > pos && start < best)
					best = start;
			}
			if (best == pos)
				best = pos + 1;
			{
				g_autofree gchar *piece = g_strndup(text + abs_start + pos, best - pos);

				ai_rendered_text_append(out, piece, tag);
			}
			pos = best;
		}
	}
}

static gboolean
closer_at(const gchar *line, guint len, guint at, gchar marker, guint width)
{
	guint i;

	if (at + width > len)
		return FALSE;
	for (i = 0; i < width; i++)
		if (line[at + i] != marker)
			return FALSE;
	if (at + width < len && line[at + width] == marker)
		return FALSE;
	return TRUE;
}

static void
emit_inline(AiRenderedText *out, const gchar *line, guint len, gboolean org)
{
	guint i = 0;

	while (i < len)
	{
		gboolean marker = FALSE;
		gchar kind = line[i];
		guint width = 1;
		guint close = 0;
		guint j;
		AiStyleTag tag = AI_STYLE_DEFAULT;

		if (kind == '`')
		{
			for (j = i + 1; j < len; j++)
				if (line[j] == '`')
				{
					g_autofree gchar *piece = g_strndup(line + i + 1, j - i - 1);

					if (piece[0] != '\0')
						ai_rendered_text_append(out, piece, AI_STYLE_CODE);
					i = j + 1;
					marker = TRUE;
					break;
				}
			if (marker)
				continue;
		}
		if (!org && kind == '[')
		{
			const gchar *end = memchr(line + i, ']', len - i);
			const gchar *url;

			if (end != NULL && end[1] == '(' && (url = memchr(end + 2, ')', len - (guint)(end + 2 - line))) != NULL)
			{
				g_autofree gchar *piece = g_strndup(line + i + 1, (gsize)(end - (line + i + 1)));

				ai_rendered_text_append(out, piece, AI_STYLE_LINK);
				i = (guint)(url - line) + 1;
				continue;
			}
		}
		if (org && (kind == '=' || kind == '~') && (i == 0 || !g_ascii_isalnum(line[i - 1])))
		{
			for (j = i + 1; j < len; j++)
				if (line[j] == kind && j > i + 1 && (j + 1 >= len || !g_ascii_isalnum(line[j + 1])))
				{
					g_autofree gchar *piece = g_strndup(line + i + 1, j - i - 1);

					ai_rendered_text_append(out, piece, AI_STYLE_CODE);
					i = j + 1;
					marker = TRUE;
					break;
				}
			if (marker)
				continue;
		}
		if ((kind == '*' || (!org && kind == '_') || (org && kind == '/')) &&
		    (i == 0 || !g_ascii_isalnum(line[i - 1])))
		{
			if (!org && i + 1 < len && line[i + 1] == kind)
				width = 2;
			if (i + width < len && line[i + width] != ' ' && line[i + width] != kind)
			{
				for (j = i + width; j < len; j++)
					if (closer_at(line, len, j, kind, width) && line[j - 1] != ' ')
					{
						close = j;
						break;
					}
			}
			if (close > i + width)
			{
				g_autofree gchar *piece = g_strndup(line + i + width, close - (i + width));

				tag = (org && kind == '*') || width == 2 ? AI_STYLE_HEADING : AI_STYLE_DEFAULT;
				ai_rendered_text_append(out, piece, tag);
				i = close + width;
				continue;
			}
		}
		{
			const gchar *next = g_utf8_next_char(line + i);
			g_autofree gchar *piece = g_strndup(line + i, (gsize)(next - (line + i)));

			ai_rendered_text_append(out, piece, AI_STYLE_DEFAULT);
			i = (guint)(next - line);
		}
	}
}

static gchar *
fence_language(const gchar *start, const gchar *end)
{
	const gchar *p = start;
	const gchar *stop;
	gchar *lang;

	while (p < end && g_ascii_isspace(*p))
		p++;
	stop = p;
	while (stop < end && (g_ascii_isalnum(*stop) || *stop == '_' || *stop == '+' || *stop == '-'))
		stop++;
	lang = g_ascii_strdown(p, (gssize)(stop - p));
	return lang;
}

static void
skip_line(const gchar **p)
{
	const gchar *nl = strchr(*p, '\n');

	*p = nl != NULL ? nl + 1 : *p + strlen(*p);
}

static gboolean
fence_open(const gchar *p, gchar *marker, guint *width)
{
	guint n = 0;
	gchar ch;

	if (*p != '`' && *p != '~')
		return FALSE;
	ch = *p;
	while (p[n] == ch)
		n++;
	if (n < 3)
		return FALSE;
	*marker = ch;
	*width = n;
	return TRUE;
}

void
ai_markup_fence_free(gpointer data)
{
	AiMarkupFence *fence = data;

	g_free(fence->language);
}

GArray *
ai_markup_fences(const gchar *text)
{
	GArray *fences = g_array_new(FALSE, TRUE, sizeof(AiMarkupFence));

	g_array_set_clear_func(fences, ai_markup_fence_free);
	gboolean org = document_is_org(text);
	const gchar *p = text != NULL ? text : "";

	while (*p != '\0')
	{
		gchar marker = 0;
		guint width = 0;
		if (fence_open(p, &marker, &width))
		{
			const gchar *lang_at = p + width;
			const gchar *nl = strchr(p, '\n');
			g_autofree gchar *language = fence_language(lang_at, nl != NULL ? nl : lang_at + strlen(lang_at));
			const gchar *body = nl != NULL ? nl + 1 : NULL;
			const gchar *q;
			AiMarkupFence fence = { 0, 0, NULL };

			if (body == NULL)
				break;
			for (q = body; *q != '\0';)
			{
				gchar close_marker = 0;
				guint close_width = 0;

				if ((q == body || q[-1] == '\n') && fence_open(q, &close_marker, &close_width) &&
				    close_marker == marker && close_width >= width)
				{
					fence.body = (guint)(body - text);
					fence.length = (guint)(q - body);
					if (fence.length > 0 && text[fence.body + fence.length - 1] == '\n')
						fence.length--;
					fence.language = g_steal_pointer(&language);
					g_array_append_val(fences, fence);
					p = q;
					skip_line(&p);
					break;
				}
				skip_line(&q);
				if (*q == '\0' && fence.language == NULL)
				{
					fence.body = (guint)(body - text);
					fence.length = (guint)((text + strlen(text)) - body);
					fence.language = g_steal_pointer(&language);
					g_array_append_val(fences, fence);
					p = text + strlen(text);
				}
			}
			continue;
		}
		if (org && (g_ascii_strncasecmp(p, "#+BEGIN_SRC", 11) == 0 ||
		            g_ascii_strncasecmp(p, "#+BEGIN_EXAMPLE", 15) == 0))
		{
			const gchar *nl = strchr(p, '\n');
			const gchar *lang_at = p + (g_ascii_strncasecmp(p, "#+BEGIN_SRC", 11) == 0 ? 11 : 15);
			g_autofree gchar *language = fence_language(lang_at, nl != NULL ? nl : p + strlen(p));
			const gchar *body = nl != NULL ? nl + 1 : NULL;
			const gchar *q;

			if (body == NULL)
				break;
			for (q = body; *q != '\0';)
			{
				if ((q == body || q[-1] == '\n') &&
				    (g_ascii_strncasecmp(q, "#+END_SRC", 9) == 0 ||
				     g_ascii_strncasecmp(q, "#+END_EXAMPLE", 13) == 0))
				{
					AiMarkupFence fence;

					fence.body = (guint)(body - text);
					fence.length = (guint)(q - body);
					if (fence.length > 0 && text[fence.body + fence.length - 1] == '\n')
						fence.length--;
					fence.language = g_steal_pointer(&language);
					g_array_append_val(fences, fence);
					p = q;
					skip_line(&p);
					break;
				}
				skip_line(&q);
			}
			if (language != NULL)
			{
				AiMarkupFence fence;

				fence.body = (guint)(body - text);
				fence.length = (guint)((text + strlen(text)) - body);
				fence.language = g_steal_pointer(&language);
				g_array_append_val(fences, fence);
				p = text + strlen(text);
			}
			continue;
		}
		skip_line(&p);
	}
	return fences;
}

static void
emit_line(AiRenderedText *out, const gchar *line, guint len, gboolean org)
{
	const gchar *p = line;
	guint spaces = 0;
	gchar bullet[8];

	while (spaces < 3 && spaces < len && p[spaces] == ' ')
		spaces++;
	p += spaces;
	len -= spaces;
	if (!org && len >= 2 && p[0] == '#')
	{
		guint hashes = 0;

		while (hashes < len && hashes < 6 && p[hashes] == '#')
			hashes++;
		if (hashes > 0 && hashes < len && p[hashes] == ' ')
		{
			g_autofree gchar *title = g_strndup(p + hashes + 1, len - hashes - 1);

			ai_rendered_text_append(out, title, AI_STYLE_HEADING);
			return;
		}
	}
	if (org && len > 2 && p[0] == '*')
	{
		guint stars = 0;

		while (stars < len && p[stars] == '*')
			stars++;
		if (stars < len && p[stars] == ' ')
		{
			g_autofree gchar *title = NULL;

			while (stars < len && p[stars] == ' ')
				stars++;
			title = g_strndup(p + stars, len - stars);
			ai_rendered_text_append(out, title, AI_STYLE_HEADING);
			return;
		}
	}
	if (len > 2 && (p[0] == '-' || p[0] == '+' || (!org && p[0] == '*') || g_ascii_isdigit(p[0])))
	{
		guint mark = 0;
		gboolean list = FALSE;

		if ((p[0] == '-' || p[0] == '+' || (!org && p[0] == '*')) && len > 1 && p[1] == ' ')
		{
			mark = 2;
			list = TRUE;
		}
		else if (g_ascii_isdigit(p[0]))
		{
			while (mark < len && g_ascii_isdigit(p[mark]))
				mark++;
			if (mark < len && (p[mark] == '.' || p[mark] == ')') && mark + 1 < len && p[mark + 1] == ' ')
			{
				mark += 2;
				list = TRUE;
			}
		}
		if (list)
		{
			guint s;

			for (s = 0; s < spaces; s++)
				ai_rendered_text_append(out, " ", AI_STYLE_DEFAULT);
			g_snprintf(bullet, sizeof(bullet), "• ");
			ai_rendered_text_append(out, bullet, AI_STYLE_DIM);
			emit_inline(out, p + mark, len - mark, org);
			return;
		}
	}
	if (!org && len > 0 && p[0] == '>' )
	{
		guint at = p[1] == ' ' ? 2 : 1;

		ai_rendered_text_append(out, "│ ", AI_STYLE_DIM);
		emit_inline(out, p + at, len > at ? len - at : 0, FALSE);
		return;
	}
	{
		guint s;

		for (s = 0; s < spaces; s++)
			ai_rendered_text_append(out, " ", AI_STYLE_DEFAULT);
		emit_inline(out, p, len, org);
	}
}

AiRenderedText *
ai_markup_render(const gchar *text, GArray *tokens)
{
	AiRenderedText *out = ai_rendered_text_new();
	gboolean org = document_is_org(text);
	const gchar *p = text != NULL ? text : "";
	GArray *fences = ai_markup_fences(text);
	guint fence_index = 0;

	while (*p != '\0')
	{
		guint abs = (guint)(p - (text != NULL ? text : p));
		AiMarkupFence *fence = fence_index < fences->len
			? &g_array_index(fences, AiMarkupFence, fence_index) : NULL;

		if (fence != NULL && abs == fence->body)
		{
			emit_code(out, text, fence->body, fence->length, fence->language, tokens);
			p = text + fence->body + fence->length;
			if (*p == '\n')
			{
				ai_rendered_text_append(out, "\n", AI_STYLE_CODE);
				p++;
			}
			/* Skip the closing fence line when it is still ahead. */
			if (*p == '`' || *p == '~' || g_ascii_strncasecmp(p, "#+END", 5) == 0)
				skip_line(&p);
			fence_index++;
			continue;
		}
		if (fence != NULL && abs > fence->body && abs < fence->body + fence->length)
		{
			p = text + fence->body;
			continue;
		}
		{
			const gchar *nl = strchr(p, '\n');
			guint len = nl != NULL ? (guint)(nl - p) : (guint)strlen(p);
			gboolean opening = FALSE;
			gchar marker = 0;
			guint width = 0;

			if (fence_open(p, &marker, &width) ||
			    (org && (g_ascii_strncasecmp(p, "#+BEGIN_SRC", 11) == 0 ||
			             g_ascii_strncasecmp(p, "#+BEGIN_EXAMPLE", 15) == 0)))
				opening = TRUE;
			if (opening)
			{
				skip_line(&p);
				continue;
			}
			emit_line(out, p, len, org);
			if (nl != NULL)
			{
				ai_rendered_text_append(out, "\n", AI_STYLE_DEFAULT);
				p = nl + 1;
			}
			else
				p += len;
		}
	}
	g_array_free(fences, TRUE);
	return out;
}
