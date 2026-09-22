/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static const gchar *STUB =
	"#!/usr/bin/env python3\n"
	"import json, sys\n"
	"def read_msg():\n"
	"    headers = {}\n"
	"    while True:\n"
	"        line = sys.stdin.buffer.readline()\n"
	"        if line in (b'\\r\\n', b'\\n', b''):\n"
	"            break\n"
	"        k, v = line.decode().split(':', 1)\n"
	"        headers[k.strip().lower()] = v.strip()\n"
	"    n = int(headers.get('content-length', '0'))\n"
	"    return json.loads(sys.stdin.buffer.read(n) or b'null')\n"
	"def write_msg(obj):\n"
	"    data = json.dumps(obj).encode()\n"
	"    sys.stdout.buffer.write(('Content-Length: %d\\r\\n\\r\\n' % len(data)).encode() + data)\n"
	"    sys.stdout.buffer.flush()\n"
	"while True:\n"
	"    msg = read_msg()\n"
	"    if not msg: break\n"
	"    method = msg.get('method')\n"
	"    if method == 'initialize':\n"
	"        write_msg({'jsonrpc':'2.0','id':msg['id'],'result':{'capabilities':{'semanticTokensProvider':{'legend':{'tokenTypes':['keyword','string','comment','number','type','function'],'tokenModifiers':[]},'full':True}}}})\n"
	"    elif method == 'textDocument/semanticTokens/full':\n"
	"        write_msg({'jsonrpc':'2.0','id':msg['id'],'result':{'data':[0,0,4,0,0]}})\n"
	"    elif method == 'shutdown':\n"
	"        write_msg({'jsonrpc':'2.0','id':msg['id'],'result':None})\n"
	"    elif method == 'exit':\n"
	"        break\n";

static void
test_lsp_retags_a_fence(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-glib-lsp-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "stub", NULL);
	g_autofree gchar *spec = g_strdup_printf("c=%s", path);
	g_autoptr(AiViewBlock) block = ai_view_text_block_new();
	g_autoptr(AiRenderedText) first = NULL;
	const gchar *source = "```c\nzzzz x;\n```\n";
	gint64 deadline;
	gboolean highlighted = FALSE;

	g_assert_true(g_file_set_contents(path, STUB, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	g_setenv("AI_LSP", spec, TRUE);

	ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block), source);
	ai_view_block_set_complete(block, TRUE);
	first = ai_view_block_render(block, 0);
	g_assert_nonnull(strstr(ai_rendered_text_get_text(first), "zzzz"));
	g_assert_cmpint(ai_rendered_text_get_tag_at(first,
		(guint)(strstr(ai_rendered_text_get_text(first), "zzzz") - ai_rendered_text_get_text(first))),
		==, AI_STYLE_DEFAULT);

	deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < deadline)
	{
		g_autoptr(AiRenderedText) rendered = ai_view_block_render(block, 0);
		const gchar *text = ai_rendered_text_get_text(rendered);
		const gchar *word = strstr(text, "zzzz");

		g_main_context_iteration(NULL, FALSE);
		if (word != NULL && ai_rendered_text_get_tag_at(rendered, (guint)(word - text)) == AI_STYLE_SYNTAX_KEYWORD)
		{
			highlighted = TRUE;
			break;
		}
		g_usleep(20 * 1000);
	}
	g_unsetenv("AI_LSP");
	g_assert_true(highlighted);
	g_unlink(path);
	g_rmdir(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/lsp/fence", test_lsp_retags_a_fence);
	return g_test_run();
}
