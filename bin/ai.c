/*
 * ai.c - command-line front-end for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A small, dependency-free CLI to drive every ai-glib provider by hand:
 * pick a provider and model, send a prompt (from argv or stdin), and print
 * the response to stdout. Built primarily to exercise the providers --
 * including the Ollama-as-transport rewrite for the claude-code /
 * claude-tmux CLI providers (model "ollama/<name>") -- outside the cmacs
 * and libreclaw integrations.
 *
 * Usage:
 *   ai [OPTIONS] [PROMPT]
 *   echo "PROMPT" | ai [OPTIONS]
 *
 * Notable flags:
 *   --dry-run         print the argv that WOULD be spawned (CLI providers)
 *   --list-providers  list known provider names
 *   --stream          stream tokens live (providers that support it)
 *   --interactive     multi-turn REPL keeping history
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai-glib.h"

/* Private seams: assert/print the exact argv, incl. the Ollama rewrite. */
#include "providers/ai-claude-code-client-internal.h"
#include "providers/ai-claude-tmux-client-internal.h"
#include "providers/ai-grok-build-client-internal.h"
#include "providers/ai-claude-launch.h"

/* ---------------------------------------------------------------- */
/* Options                                                           */
/* ---------------------------------------------------------------- */

static gchar    *opt_provider        = NULL;
static gchar    *opt_model           = NULL;
static gchar    *opt_system          = NULL;
static gchar    *opt_effort          = NULL;
static gint      opt_max_tokens      = 4096;
static gdouble   opt_temperature     = -1.0;  /* <0 => leave provider default */
static gboolean  opt_stream          = FALSE;
static gboolean  opt_skip_perms      = FALSE;
static gboolean  opt_dry_run         = FALSE;
static gboolean  opt_list_providers  = FALSE;
static gboolean  opt_interactive     = FALSE;
static gboolean  opt_version         = FALSE;
static gboolean  opt_license         = FALSE;
static gchar   **opt_set             = NULL;

/* Image mode.  Negative / NULL means "not given", so an untouched flag
 * leaves the corresponding request parameter unset. */
static gboolean  opt_image_gen          = FALSE;
static gchar    *opt_image_out          = NULL;
static gchar    *opt_image_op           = NULL;
static gchar   **opt_image_refs         = NULL;
static gchar   **opt_image_ref_roles    = NULL;
static gchar    *opt_image_mask         = NULL;
static gchar    *opt_image_aspect       = NULL;
static gchar    *opt_image_size         = NULL;
static gchar    *opt_image_resolution   = NULL;
static gint      opt_image_count        = 0;
static gchar    *opt_image_quality      = NULL;
static gchar    *opt_image_style        = NULL;
static gchar    *opt_image_background   = NULL;
static gchar    *opt_image_format       = NULL;
static gint      opt_image_compression  = -1;
static gchar    *opt_image_negative     = NULL;
static gint64    opt_image_seed         = -1;
static gdouble   opt_image_guidance     = -1.0;
static gint      opt_image_steps        = 0;
static gdouble   opt_image_strength     = -1.0;
static gchar    *opt_image_moderation   = NULL;
static gchar    *opt_image_person       = NULL;
static gchar    *opt_image_language     = NULL;
static gchar    *opt_image_fidelity     = NULL;
static gboolean  opt_image_no_watermark = FALSE;
static gboolean  opt_image_url          = FALSE;
static gchar   **opt_image_extra        = NULL;
static gboolean  opt_image_list_models  = FALSE;
static gboolean  opt_image_strict       = FALSE;

static const GOptionEntry option_entries[] = {
	{ "provider", 'p', 0, G_OPTION_ARG_STRING, &opt_provider,
	  "Provider: claude, openai, gemini, grok, ollama, claude-code, "
	  "claude-tmux, opencode, grok-build "
	  "(default: $AI_PROVIDER or claude)", "NAME" },
	{ "model", 'm', 0, G_OPTION_ARG_STRING, &opt_model,
	  "Model id (default: provider default). For claude-code/claude-tmux, "
	  "an \"ollama/<model>\" id routes via `ollama launch claude`.", "ID" },
	{ "system", 's', 0, G_OPTION_ARG_STRING, &opt_system,
	  "System prompt", "TEXT" },
	{ "effort", 0, 0, G_OPTION_ARG_STRING, &opt_effort,
	  "Effort/thinking level for CLI providers", "LEVEL" },
	{ "max-tokens", 0, 0, G_OPTION_ARG_INT, &opt_max_tokens,
	  "Max output tokens (default: 4096)", "N" },
	{ "temperature", 0, 0, G_OPTION_ARG_DOUBLE, &opt_temperature,
	  "Sampling temperature (HTTP providers)", "F" },
	{ "stream", 0, 0, G_OPTION_ARG_NONE, &opt_stream,
	  "Stream the response as it arrives (when supported)", NULL },
	{ "skip-permissions", 0, 0, G_OPTION_ARG_NONE, &opt_skip_perms,
	  "Bypass tool-use approval (claude-code/claude-tmux/grok-build)", NULL },
	{ "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_set,
	  "Set any provider property, e.g. --set sandbox=workspace. "
	  "Repeatable. A bare --set NAME sets a boolean property to true. "
	  "Pass an unknown name to list what the provider accepts.",
	  "PROP=VALUE" },
	{ "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run,
	  "Print the command that would be spawned, do not run it", NULL },
	{ "list-providers", 0, 0, G_OPTION_ARG_NONE, &opt_list_providers,
	  "List known provider names and exit", NULL },
	{ "interactive", 'i', 0, G_OPTION_ARG_NONE, &opt_interactive,
	  "Interactive multi-turn REPL (keeps history)", NULL },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &opt_version,
	  "Print version and exit", NULL },
	{ "license", 0, 0, G_OPTION_ARG_NONE, &opt_license,
	  "Print licensing information and exit", NULL },
	{ "image-gen", 'I', 0, G_OPTION_ARG_NONE, &opt_image_gen,
	  "Generate an image instead of text; see the image options below",
	  NULL },

	/*
	 * Image options are listed here rather than in a GOptionGroup of
	 * their own.  A group would keep `--help' shorter, but it also hides
	 * every image flag behind `--help-image', where nobody looking at
	 * `--help' will find them.  A long complete help beats a short one
	 * that omits most of the mode it is documenting.
	 */
	{ "image-out", 'o', 0, G_OPTION_ARG_FILENAME, &opt_image_out,
	  "Write the image here (default: auto-numbered output-0000.png)", "FILE" },
	{ "image-op", 0, 0, G_OPTION_ARG_STRING, &opt_image_op,
	  "generate, edit, variation or upscale (default: generate, or edit "
	  "when --ref is given)", "OP" },
	{ "ref", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_image_refs,
	  "Reference image to condition on; repeatable for multi-image "
	  "conditioning", "FILE" },
	{ "ref-role", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_image_ref_roles,
	  "Label for the preceding --ref, saying what that reference is for: "
	  "style, subject, background, ...", "ROLE" },
	{ "ref-style", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_image_ref_roles,
	  "Alias for --ref-role", "ROLE" },
	{ "mask", 0, 0, G_OPTION_ARG_FILENAME, &opt_image_mask,
	  "Edit mask; transparent areas are the ones regenerated", "FILE" },
	{ "aspect", 0, 0, G_OPTION_ARG_STRING, &opt_image_aspect,
	  "Aspect ratio, e.g. 16:9 (Gemini and Imagen)", "RATIO" },
	{ "size", 0, 0, G_OPTION_ARG_STRING, &opt_image_size,
	  "Pixel size, e.g. 1024x1024 or auto (OpenAI)", "WxH" },
	{ "resolution", 0, 0, G_OPTION_ARG_STRING, &opt_image_resolution,
	  "Resolution tier: 1k, 2k or 4k (Nano Banana Pro, Imagen)", "TIER" },
	{ "count", 'n', 0, G_OPTION_ARG_INT, &opt_image_count,
	  "How many images to generate", "N" },
	{ "quality", 0, 0, G_OPTION_ARG_STRING, &opt_image_quality,
	  "auto, low, medium, high, standard or hd; translated to whichever "
	  "the model accepts", "LEVEL" },
	{ "style", 0, 0, G_OPTION_ARG_STRING, &opt_image_style,
	  "vivid or natural, or a provider-specific preset name", "STYLE" },
	{ "background", 0, 0, G_OPTION_ARG_STRING, &opt_image_background,
	  "auto, transparent or opaque (needs a format with alpha)", "MODE" },
	{ "format", 0, 0, G_OPTION_ARG_STRING, &opt_image_format,
	  "Output encoding: png, jpeg or webp", "FMT" },
	{ "compression", 0, 0, G_OPTION_ARG_INT, &opt_image_compression,
	  "Compression 0-100 for lossy formats", "N" },
	{ "negative", 0, 0, G_OPTION_ARG_STRING, &opt_image_negative,
	  "What to keep out of the image (Imagen)", "TEXT" },
	{ "seed", 0, 0, G_OPTION_ARG_INT64, &opt_image_seed,
	  "Sampling seed, for reproducible results", "N" },
	{ "guidance", 0, 0, G_OPTION_ARG_DOUBLE, &opt_image_guidance,
	  "How strictly to follow the prompt", "F" },
	{ "steps", 0, 0, G_OPTION_ARG_INT, &opt_image_steps,
	  "Sampling steps", "N" },
	{ "strength", 0, 0, G_OPTION_ARG_DOUBLE, &opt_image_strength,
	  "0.0-1.0; how far an edit may depart from its reference", "F" },
	{ "moderation", 0, 0, G_OPTION_ARG_STRING, &opt_image_moderation,
	  "Content filtering: auto, low or none", "LEVEL" },
	{ "person-generation", 0, 0, G_OPTION_ARG_STRING, &opt_image_person,
	  "dont_allow, allow_adult or allow_all (Imagen)", "MODE" },
	{ "language", 0, 0, G_OPTION_ARG_STRING, &opt_image_language,
	  "Prompt language hint, e.g. en (Imagen)", "LANG" },
	{ "input-fidelity", 0, 0, G_OPTION_ARG_STRING, &opt_image_fidelity,
	  "auto, low or high; how closely an edit preserves its input", "LEVEL" },
	{ "no-watermark", 0, 0, G_OPTION_ARG_NONE, &opt_image_no_watermark,
	  "Ask the provider not to watermark (honoured where supported)", NULL },
	{ "image-url", 0, 0, G_OPTION_ARG_NONE, &opt_image_url,
	  "Ask for a URL rather than inline bytes (still saved locally)", NULL },
	{ "image-extra", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_image_extra,
	  "Pass KEY=VALUE straight through to the provider; repeatable",
	  "KEY=VALUE" },
	{ "list-image-models", 0, 0, G_OPTION_ARG_NONE, &opt_image_list_models,
	  "List image models with their capabilities and exit", NULL },
	{ "strict", 0, 0, G_OPTION_ARG_NONE, &opt_image_strict,
	  "Fail if the model does not support a requested option, rather than "
	  "dropping it", NULL },
	{ NULL, 0, 0, 0, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */

/*
 * Read all of stdin into a newly-allocated NUL-terminated string.
 * Returns NULL (and *len 0) on a read error. (transfer full)
 */
static gchar *
read_all_stdin(void)
{
	GString *buf = g_string_new(NULL);
	gchar    chunk[4096];
	gsize    n;

	while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
	{
		g_string_append_len(buf, chunk, n);
	}

	return g_string_free(buf, FALSE);
}

/* Print an argv (NULL-terminated) as a single, lightly-quoted line. */
static void
print_argv(gchar **argv)
{
	gint i;

	for (i = 0; argv != NULL && argv[i] != NULL; i++)
	{
		const gchar *a = argv[i];
		gboolean needs_quote = (a[0] == '\0') || strpbrk(a, " \t\"'\\") != NULL;

		if (i > 0)
		{
			fputc(' ', stdout);
		}
		if (needs_quote)
		{
			g_autofree gchar *esc = g_strescape(a, NULL);
			printf("\"%s\"", esc);
		}
		else
		{
			fputs(a, stdout);
		}
	}
	fputc('\n', stdout);
}

/* ---------------------------------------------------------------- */
/* --set PROP=VALUE                                                  */
/* ---------------------------------------------------------------- */

/*
 * Print every property the provider will accept from --set.
 *
 * An unknown name is almost always a guess at a knob the user knows the
 * provider has, so answering with the real list is more useful than
 * repeating the name back.
 */
static void
print_settable_properties(GObject *provider)
{
	g_autofree GParamSpec **pspecs = NULL;
	guint n = 0, i;

	pspecs = g_object_class_list_properties(G_OBJECT_GET_CLASS(provider), &n);

	g_printerr("ai: %s accepts:\n", G_OBJECT_TYPE_NAME(provider));
	for (i = 0; i < n; i++)
	{
		if ((pspecs[i]->flags & G_PARAM_WRITABLE) == 0)
			continue;

		g_printerr("  %-24s %s\n", g_param_spec_get_name(pspecs[i]),
		           g_type_name(pspecs[i]->value_type));
	}
}

/*
 * Parse @text into @value according to @pspec's type.
 *
 * Deliberately strict: a mistyped value silently becoming 0 or FALSE is a
 * worse outcome than an error, because the run still happens and the knob
 * the user asked for just is not there.
 */
static gboolean
value_from_string(GValue *value, GParamSpec *pspec, const gchar *text)
{
	GType type = G_PARAM_SPEC_VALUE_TYPE(pspec);
	gchar *end = NULL;

	g_value_init(value, type);

	if (type == G_TYPE_STRING)
	{
		g_value_set_string(value, text);
		return TRUE;
	}

	if (type == G_TYPE_BOOLEAN)
	{
		if (g_ascii_strcasecmp(text, "true") == 0 ||
		    g_ascii_strcasecmp(text, "yes") == 0 ||
		    g_ascii_strcasecmp(text, "on") == 0 ||
		    g_strcmp0(text, "1") == 0)
		{
			g_value_set_boolean(value, TRUE);
			return TRUE;
		}
		if (g_ascii_strcasecmp(text, "false") == 0 ||
		    g_ascii_strcasecmp(text, "no") == 0 ||
		    g_ascii_strcasecmp(text, "off") == 0 ||
		    g_strcmp0(text, "0") == 0)
		{
			g_value_set_boolean(value, FALSE);
			return TRUE;
		}
		return FALSE;
	}

	if (type == G_TYPE_INT || type == G_TYPE_UINT ||
	    type == G_TYPE_INT64 || type == G_TYPE_UINT64)
	{
		gint64 parsed = g_ascii_strtoll(text, &end, 10);

		if (end == text || *end != '\0')
			return FALSE;

		if (type == G_TYPE_INT)
			g_value_set_int(value, (gint) parsed);
		else if (type == G_TYPE_UINT)
			g_value_set_uint(value, (guint) parsed);
		else if (type == G_TYPE_INT64)
			g_value_set_int64(value, parsed);
		else
			g_value_set_uint64(value, (guint64) parsed);
		return TRUE;
	}

	if (type == G_TYPE_DOUBLE || type == G_TYPE_FLOAT)
	{
		gdouble parsed = g_ascii_strtod(text, &end);

		if (end == text || *end != '\0')
			return FALSE;

		if (type == G_TYPE_DOUBLE)
			g_value_set_double(value, parsed);
		else
			g_value_set_float(value, (gfloat) parsed);
		return TRUE;
	}

	if (G_TYPE_IS_ENUM(type))
	{
		g_autoptr(GEnumClass) klass = g_type_class_ref(type);
		GEnumValue *ev = g_enum_get_value_by_nick(klass, text);

		if (ev == NULL)
			ev = g_enum_get_value_by_name(klass, text);
		if (ev == NULL)
			return FALSE;

		g_value_set_enum(value, ev->value);
		return TRUE;
	}

	return FALSE;
}

/*
 * Apply every --set to @provider. Returns FALSE (having explained why) if
 * any of them names a property the provider does not have, one it will not
 * let us write, or a value that does not parse.
 */
static gboolean
apply_property_overrides(GObject *provider)
{
	gsize i;

	if (opt_set == NULL)
		return TRUE;

	for (i = 0; opt_set[i] != NULL; i++)
	{
		g_auto(GValue) value = G_VALUE_INIT;
		g_autofree gchar *name = NULL;
		const gchar *text;
		const gchar *eq;
		GParamSpec *pspec;

		eq = strchr(opt_set[i], '=');
		if (eq != NULL)
		{
			name = g_strndup(opt_set[i], (gsize)(eq - opt_set[i]));
			text = eq + 1;
		}
		else
		{
			/* A bare `--set NAME` is a boolean switch. */
			name = g_strdup(opt_set[i]);
			text = "true";
		}

		g_strstrip(name);

		pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(provider),
		                                     name);
		if (pspec == NULL)
		{
			g_printerr("ai: no property '%s' on this provider\n", name);
			print_settable_properties(provider);
			return FALSE;
		}

		if ((pspec->flags & G_PARAM_WRITABLE) == 0)
		{
			g_printerr("ai: property '%s' is read-only\n", name);
			return FALSE;
		}

		if (!value_from_string(&value, pspec, text))
		{
			g_printerr("ai: cannot read '%s' as a value for '%s' (%s)\n",
			           text, name,
			           g_type_name(G_PARAM_SPEC_VALUE_TYPE(pspec)));
			return FALSE;
		}

		g_object_set_property(provider, name, &value);
	}

	return TRUE;
}

/*
 * Create the concrete provider object for @ptype using @config and apply
 * the common knobs (model, system prompt, max tokens, temperature, effort,
 * skip-permissions). Returns a floating-free owned GObject, or NULL on a
 * genuinely unknown provider type. (transfer full)
 */
static GObject *
make_provider(AiConfig *config, AiProviderType ptype)
{
	GObject *provider = NULL;

	switch (ptype)
	{
	case AI_PROVIDER_CLAUDE:
		provider = G_OBJECT(ai_claude_client_new_with_config(config));
		break;
	case AI_PROVIDER_OPENAI:
		provider = G_OBJECT(ai_openai_client_new_with_config(config));
		break;
	case AI_PROVIDER_GEMINI:
		provider = G_OBJECT(ai_gemini_client_new_with_config(config));
		break;
	case AI_PROVIDER_GROK:
		provider = G_OBJECT(ai_grok_client_new_with_config(config));
		break;
	case AI_PROVIDER_OLLAMA:
		provider = G_OBJECT(ai_ollama_client_new_with_config(config));
		break;
	case AI_PROVIDER_CLAUDE_CODE:
		provider = G_OBJECT(ai_claude_code_client_new_with_config(config));
		break;
	case AI_PROVIDER_CLAUDE_TMUX:
		provider = G_OBJECT(ai_claude_tmux_client_new_with_config(config));
		break;
	case AI_PROVIDER_OPENCODE:
		provider = G_OBJECT(ai_opencode_client_new_with_config(config));
		break;
	case AI_PROVIDER_GROK_BUILD:
		provider = G_OBJECT(ai_grok_build_client_new_with_config(config));
		break;
	default:
		return NULL;
	}

	/* Apply the knobs to whichever base class this provider is. */
	if (AI_IS_CLIENT(provider))
	{
		AiClient *c = AI_CLIENT(provider);
		if (opt_model != NULL)
			ai_client_set_model(c, opt_model);
		if (opt_system != NULL)
			ai_client_set_system_prompt(c, opt_system);
		ai_client_set_max_tokens(c, opt_max_tokens);
		if (opt_temperature >= 0.0)
			ai_client_set_temperature(c, opt_temperature);
	}
	else if (AI_IS_CLI_CLIENT(provider))
	{
		AiCliClient *c = AI_CLI_CLIENT(provider);
		if (opt_model != NULL)
			ai_cli_client_set_model(c, opt_model);
		if (opt_system != NULL)
			ai_cli_client_set_system_prompt(c, opt_system);
		if (opt_effort != NULL)
			ai_cli_client_set_effort_level(c, opt_effort);
	}

	/* Provider-specific knobs. */
	if (AI_IS_CLAUDE_CODE_CLIENT(provider))
		ai_claude_code_client_set_skip_permissions(
			AI_CLAUDE_CODE_CLIENT(provider), opt_skip_perms);
	else if (AI_IS_CLAUDE_TMUX_CLIENT(provider))
		ai_claude_tmux_client_set_skip_permissions(
			AI_CLAUDE_TMUX_CLIENT(provider), opt_skip_perms);
	else if (AI_IS_GROK_BUILD_CLIENT(provider))
		ai_grok_build_client_set_skip_permissions(
			AI_GROK_BUILD_CLIENT(provider), opt_skip_perms);

	return provider;
}

/* ---------------------------------------------------------------- */
/* --list-providers                                                  */
/* ---------------------------------------------------------------- */

static void
list_providers(void)
{
	static const AiProviderType all[] = {
		AI_PROVIDER_CLAUDE, AI_PROVIDER_OPENAI, AI_PROVIDER_GEMINI,
		AI_PROVIDER_GROK, AI_PROVIDER_OLLAMA, AI_PROVIDER_CLAUDE_CODE,
		AI_PROVIDER_CLAUDE_TMUX, AI_PROVIDER_OPENCODE,
		AI_PROVIDER_GROK_BUILD
	};
	gsize i;

	printf("Providers:\n");
	for (i = 0; i < G_N_ELEMENTS(all); i++)
	{
		const gchar *name = ai_provider_type_to_string(all[i]);
		const gchar *kind;
		const gchar *note = "";

		switch (all[i])
		{
		case AI_PROVIDER_CLAUDE_CODE:
		case AI_PROVIDER_CLAUDE_TMUX:
			kind = "CLI";
			note = "  (model \"ollama/<name>\" => ollama launch claude)";
			break;
		case AI_PROVIDER_OPENCODE:
		case AI_PROVIDER_GROK_BUILD:
			kind = "CLI";
			break;
		default:
			kind = "HTTP";
			break;
		}
		printf("  %-14s %-4s%s\n", name, kind, note);
	}
}

/* ---------------------------------------------------------------- */
/* --dry-run                                                         */
/* ---------------------------------------------------------------- */

/*
 * Print the command that would be spawned. For the claude-code /
 * claude-tmux / grok-build CLI providers this is the real argv (with the
 * executable resolved and, for the claude providers, the Ollama transport
 * applied); for other providers it is a short summary, since they do not
 * spawn a CLI we can introspect.
 */
static int
dry_run(GObject *provider, AiProviderType ptype, GList *messages)
{
	if (ptype == AI_PROVIDER_CLAUDE_CODE)
	{
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) argv = NULL;
		g_autofree gchar *exe = NULL;

		argv = ai_claude_code_client_build_argv(
			AI_CLI_CLIENT(provider), messages, opt_system,
			opt_max_tokens, opt_stream);
		exe = ai_cli_client_resolve_executable(AI_CLI_CLIENT(provider),
		                                       &error);
		if (exe != NULL)
		{
			g_free(argv[0]);
			argv[0] = g_steal_pointer(&exe);
		}
		else
		{
			g_printerr("note: %s (printing unresolved placeholder)\n",
			           error->message);
		}
		print_argv(argv);
		return 0;
	}

	if (ptype == AI_PROVIDER_GROK_BUILD)
	{
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) argv = NULL;
		g_autofree gchar *exe = NULL;

		argv = ai_grok_build_client_build_argv(
			AI_CLI_CLIENT(provider), messages, opt_system,
			opt_max_tokens, opt_stream);
		exe = ai_cli_client_resolve_executable(AI_CLI_CLIENT(provider),
		                                       &error);
		if (exe != NULL)
		{
			g_free(argv[0]);
			argv[0] = g_steal_pointer(&exe);
		}
		else
		{
			g_printerr("note: %s (printing unresolved placeholder)\n",
			           error->message);
		}
		print_argv(argv);
		return 0;
	}

	if (ptype == AI_PROVIDER_CLAUDE_TMUX)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GPtrArray) argv = NULL;
		g_autofree gchar *claude_exe = NULL;
		g_autofree gchar *cwd = g_get_current_dir();
		const gchar *tmux_bin = g_getenv("TMUX_PATH");

		claude_exe = ai_cli_client_resolve_executable(
			AI_CLI_CLIENT(provider), &error);
		if (claude_exe == NULL)
		{
			g_printerr("note: %s (printing literal \"claude\")\n",
			           error->message);
			claude_exe = g_strdup("claude");
		}
		if (tmux_bin == NULL || tmux_bin[0] == '\0')
			tmux_bin = "tmux";

		/* Representative session: a fresh session-id, sample paths. */
		argv = ai_claude_tmux_client_build_session_argv(
			tmux_bin,
			ai_claude_tmux_client_get_socket_name(
				AI_CLAUDE_TMUX_CLIENT(provider)),
			"ai-glib-dry-run", cwd, claude_exe,
			/* resuming */ FALSE, "<session-id>", "<settings.json>",
			opt_model, opt_effort, opt_skip_perms,
			ai_claude_tmux_client_get_mcp_config_path(
				AI_CLAUDE_TMUX_CLIENT(provider)));
		print_argv((gchar **) argv->pdata);
		return 0;
	}

	printf("[dry-run] provider=%s model=%s (no CLI subprocess for this "
	       "provider)\n",
	       ai_provider_type_to_string(ptype),
	       opt_model ? opt_model : "(default)");
	return 0;
}

/* ---------------------------------------------------------------- */
/* Streaming                                                         */
/* ---------------------------------------------------------------- */

static void
on_delta(AiStreamable *s, const gchar *text, gpointer user_data)
{
	(void) s;
	(void) user_data;
	fputs(text, stdout);
	fflush(stdout);
}

static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GMainLoop          *loop  = user_data;
	g_autoptr(AiResponse) resp = NULL;
	g_autoptr(GError)     error = NULL;

	resp = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
	                                        result, &error);
	fputc('\n', stdout);
	if (error != NULL)
		g_printerr("Error: %s\n", error->message);

	g_main_loop_quit(loop);
}

/*
 * Stream the response to stdout. Returns TRUE on success. Caller has
 * verified AI_IS_STREAMABLE(provider).
 */
static gboolean
stream_chat(GObject *provider, GList *messages)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	gulong delta_id;

	delta_id = g_signal_connect(provider, "delta",
	                            G_CALLBACK(on_delta), NULL);

	ai_streamable_chat_stream_async(
		AI_STREAMABLE(provider), messages, opt_system, opt_max_tokens,
		NULL, NULL, on_stream_done, loop);

	g_main_loop_run(loop);
	g_signal_handler_disconnect(provider, delta_id);
	return TRUE;
}

/* ---------------------------------------------------------------- */
/* Synchronous chat                                                  */
/* ---------------------------------------------------------------- */

/*
 * Run one synchronous turn and return the response (transfer full), or
 * NULL on error (with *error set). Dispatches to the right base class.
 */
static AiResponse *
sync_chat(GObject *provider, GList *messages, GError **error)
{
	if (AI_IS_CLIENT(provider))
		return ai_client_chat_sync(AI_CLIENT(provider), messages,
		                           NULL, error);
	if (AI_IS_CLI_CLIENT(provider))
		return ai_cli_client_chat_sync(AI_CLI_CLIENT(provider), messages,
		                               NULL, error);

	g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
	                    "Unknown provider base type");
	return NULL;
}

/* ---------------------------------------------------------------- */
/* Interactive REPL                                                  */
/* ---------------------------------------------------------------- */

static int
run_interactive(GObject *provider)
{
	GList    *history = NULL;  /* element-type AiMessage (owned) */
	gchar     line[8192];
	int       status = 0;

	fprintf(stderr, "ai interactive (provider=%s). Ctrl-D to exit.\n",
	        opt_provider ? opt_provider : "claude");

	for (;;)
	{
		AiMessage           *user_msg;
		g_autoptr(AiResponse) resp = NULL;
		g_autoptr(GError)     error = NULL;
		g_autofree gchar     *text = NULL;

		fputs("> ", stderr);
		fflush(stderr);
		if (fgets(line, sizeof line, stdin) == NULL)
		{
			fputc('\n', stderr);
			break;  /* EOF */
		}
		g_strchomp(line);
		if (line[0] == '\0')
			continue;

		user_msg = ai_message_new_user(line);
		history = g_list_append(history, user_msg);

		resp = sync_chat(provider, history, &error);
		if (resp == NULL)
		{
			g_printerr("Error: %s\n",
			           error ? error->message : "unknown");
			status = 1;
			continue;
		}

		text = ai_response_get_text(resp);
		printf("%s\n", text ? text : "");
		fflush(stdout);

		/* Keep the assistant turn in history for context. */
		history = g_list_append(history,
		                        ai_message_new_assistant(text ? text : ""));
	}

	g_list_free_full(history, g_object_unref);
	return status;
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Image generation                                                  */
/* ---------------------------------------------------------------- */

/*
 * Turn --ref/--ref-role pairs into reference images.
 *
 * --ref-role applies to the --ref that precedes it, so the two arrays are
 * walked together: role i belongs to ref i, and a trailing ref with no
 * role is simply unlabelled.
 */
static gboolean
image_add_references(AiImageRequest *request, GError **error)
{
	gsize i;

	if (opt_image_refs == NULL)
		return TRUE;

	for (i = 0; opt_image_refs[i] != NULL; i++)
	{
		const gchar *role = NULL;

		if (opt_image_ref_roles != NULL)
		{
			gsize n;

			for (n = 0; opt_image_ref_roles[n] != NULL; n++)
				;
			if (i < n)
				role = opt_image_ref_roles[i];
		}

		if (!ai_image_request_add_reference_file(request, opt_image_refs[i],
		                                         role, error))
			return FALSE;
	}

	return TRUE;
}

/*
 * Apply every --image-extra KEY=VALUE to the request verbatim.
 */
static gboolean
image_add_extras(AiImageRequest *request)
{
	gsize i;

	if (opt_image_extra == NULL)
		return TRUE;

	for (i = 0; opt_image_extra[i] != NULL; i++)
	{
		const gchar *eq = strchr(opt_image_extra[i], '=');

		if (eq == NULL)
		{
			g_printerr("ai: --image-extra expects KEY=VALUE, got '%s'\n",
			           opt_image_extra[i]);
			return FALSE;
		}

		{
			g_autofree gchar *key = g_strndup(opt_image_extra[i],
			                                  eq - opt_image_extra[i]);

			ai_image_request_set_extra_string(request, key, eq + 1);
		}
	}

	return TRUE;
}

/*
 * Pick an output path when none was given.
 *
 * Mirrors the numbering the reference scripts use: output-0000.png,
 * output-0001.png, and so on, first free name wins.
 */
static gchar *
image_auto_filename(const gchar *extension, guint index)
{
	guint counter = 0;

	for (;;)
	{
		g_autofree gchar *base = NULL;
		gchar *candidate;

		if (index > 0)
			base = g_strdup_printf("output-%04u-%u", counter, index);
		else
			base = g_strdup_printf("output-%04u", counter);

		candidate = g_strconcat(base, extension, NULL);

		if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
			return candidate;

		g_free(candidate);

		if (++counter > 9999)
			return g_strconcat("output", extension, NULL);
	}
}

/* Map a MIME type onto the extension to save under. */
static const gchar *
image_extension_for_mime(const gchar *mime)
{
	if (g_strcmp0(mime, "image/jpeg") == 0)
		return ".jpg";
	if (g_strcmp0(mime, "image/webp") == 0)
		return ".webp";

	return ".png";
}

/*
 * Build the output path for image @index of @total.
 *
 * With --image-out and several images, the index is appended before the
 * extension so a batch does not overwrite itself.
 */
static gchar *
image_output_path(const gchar *mime, guint index, guint total)
{
	const gchar *extension = image_extension_for_mime(mime);

	if (opt_image_out == NULL)
		return image_auto_filename(extension, total > 1 ? index + 1 : 0);

	if (total <= 1)
		return g_strdup(opt_image_out);

	{
		g_autofree gchar *stem = NULL;
		const gchar *dot = strrchr(opt_image_out, '.');

		if (dot != NULL && dot != opt_image_out)
			stem = g_strndup(opt_image_out, dot - opt_image_out);
		else
			stem = g_strdup(opt_image_out);

		return g_strdup_printf("%s-%03u%s", stem, index,
		                       dot != NULL ? dot : extension);
	}
}

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
	GError    *error;
} ImageFetch;

static void
on_image_bytes(GObject *source, GAsyncResult *result, gpointer user_data)
{
	ImageFetch *fetch = user_data;

	(void)source;

	fetch->bytes = ai_generated_image_load_bytes_finish(NULL, result,
	                                                    &fetch->error);
	g_main_loop_quit(fetch->loop);
}

/*
 * Resolve an image to bytes, whichever form the provider returned.
 *
 * A URL result needs a second HTTP request, so this is async under a
 * nested loop rather than a plain accessor.
 */
static GBytes *
image_resolve_bytes(AiGeneratedImage *image, GError **error)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	ImageFetch fetch = { NULL, NULL, NULL };

	g_main_context_push_thread_default(context);
	fetch.loop = g_main_loop_new(context, FALSE);

	ai_generated_image_load_bytes_async(image, NULL, on_image_bytes, &fetch);
	g_main_loop_run(fetch.loop);

	g_main_loop_unref(fetch.loop);
	g_main_context_pop_thread_default(context);

	if (fetch.error != NULL)
	{
		g_propagate_error(error, fetch.error);
		return NULL;
	}

	return fetch.bytes;
}

/*
 * Print the model table for one provider, or for every provider that can
 * generate images.
 *
 * Driven entirely off each provider's AiImageModelInfo table, so it stays
 * correct as models and providers are added.
 */
static int
list_image_models(AiConfig *config, const gchar *provider_name)
{
	static const AiProviderType all[] = {
		AI_PROVIDER_OPENAI, AI_PROVIDER_GEMINI, AI_PROVIDER_GROK
	};
	gsize i;
	gboolean any = FALSE;

	printf("%-34s %-9s %5s %5s  %s\n",
	       "MODEL", "PROVIDER", "REFS", "MAX-N", "CAPABILITIES");

	for (i = 0; i < G_N_ELEMENTS(all); i++)
	{
		g_autoptr(GObject) provider = NULL;
		GList *models;
		GList *l;

		if (provider_name != NULL &&
		    ai_provider_type_from_string(provider_name) != all[i])
			continue;

		provider = make_provider(config, all[i]);
		if (provider == NULL || !AI_IS_IMAGE_GENERATOR(provider))
			continue;

		models = ai_image_generator_list_image_models(
			AI_IMAGE_GENERATOR(provider));

		for (l = models; l != NULL; l = l->next)
		{
			AiImageModelInfo *info = l->data;
			g_autofree gchar *caps = ai_image_capabilities_to_string(
				ai_image_model_info_get_capabilities(info));
			const gchar *notes = ai_image_model_info_get_notes(info);

			any = TRUE;

			printf("%-34s %-9s %5u %5u  %s\n",
			       ai_image_model_info_get_id(info),
			       ai_provider_type_to_string(all[i]),
			       ai_image_model_info_get_max_reference_images(info),
			       ai_image_model_info_get_max_count(info),
			       caps);

			if (notes != NULL)
				printf("%-34s %s\n", "", notes);
		}

		g_list_free_full(models, (GDestroyNotify)ai_image_model_info_free);
	}

	if (!any)
	{
		g_printerr("ai: no image models for provider '%s'\n",
		           provider_name != NULL ? provider_name : "(any)");
		return 1;
	}

	return 0;
}

/*
 * Run one image generation and write the results out.
 */
static int
generate_images(GObject *provider, const gchar *prompt)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	guint count;
	guint i;

	if (!AI_IS_IMAGE_GENERATOR(provider))
	{
		g_printerr("ai: provider does not support image generation "
		           "(try -p openai, -p gemini or -p grok)\n");
		return 2;
	}

	request = ai_image_request_new(prompt);

	if (opt_model != NULL)
		ai_image_request_set_model(request, opt_model);
	if (opt_image_op != NULL)
		ai_image_request_set_operation(
			request, ai_image_operation_from_string(opt_image_op));
	if (opt_image_aspect != NULL)
		ai_image_request_set_aspect_ratio(request, opt_image_aspect);
	if (opt_image_size != NULL)
		ai_image_request_set_custom_size(request, opt_image_size);
	if (opt_image_resolution != NULL)
		ai_image_request_set_resolution(
			request, ai_image_resolution_from_string(opt_image_resolution));
	if (opt_image_count > 0)
		ai_image_request_set_count(request, opt_image_count);
	if (opt_image_quality != NULL)
		ai_image_request_set_quality(
			request, ai_image_quality_from_string(opt_image_quality));
	if (opt_image_style != NULL)
	{
		AiImageStyle style = ai_image_style_from_string(opt_image_style);

		/* An unrecognised name is a provider-specific preset rather than
		 * an error, so pass it through instead of dropping it. */
		if (style != AI_IMAGE_STYLE_AUTO)
			ai_image_request_set_style(request, style);
		else
			ai_image_request_set_style_preset(request, opt_image_style);
	}
	if (opt_image_background != NULL)
		ai_image_request_set_background(
			request, ai_image_background_from_string(opt_image_background));
	if (opt_image_format != NULL)
		ai_image_request_set_output_format(
			request, ai_image_format_from_string(opt_image_format));
	if (opt_image_compression >= 0)
		ai_image_request_set_output_compression(request,
		                                        opt_image_compression);
	if (opt_image_negative != NULL)
		ai_image_request_set_negative_prompt(request, opt_image_negative);
	if (opt_image_seed >= 0)
		ai_image_request_set_seed(request, opt_image_seed);
	if (opt_image_guidance >= 0.0)
		ai_image_request_set_guidance_scale(request, opt_image_guidance);
	if (opt_image_steps > 0)
		ai_image_request_set_steps(request, opt_image_steps);
	if (opt_image_strength >= 0.0)
		ai_image_request_set_strength(request, opt_image_strength);
	if (opt_image_moderation != NULL)
		ai_image_request_set_moderation(
			request, ai_image_moderation_from_string(opt_image_moderation));
	if (opt_image_person != NULL)
		ai_image_request_set_person_generation(
			request,
			ai_image_person_generation_from_string(opt_image_person));
	if (opt_image_no_watermark)
		ai_image_request_set_watermark(request, AI_TRI_FALSE);
	if (opt_image_language != NULL)
		ai_image_request_set_language(request, opt_image_language);
	if (opt_image_fidelity != NULL)
		ai_image_request_set_input_fidelity(
			request, ai_image_fidelity_from_string(opt_image_fidelity));

	/* Default to inline bytes: the CLI writes files, and a URL result is
	 * an extra round trip against a link that expires. */
	ai_image_request_set_response_format(
		request,
		opt_image_url ? AI_IMAGE_RESPONSE_URL : AI_IMAGE_RESPONSE_BASE64);

	if (!image_add_references(request, &error))
	{
		g_printerr("ai: %s\n", error->message);
		return 1;
	}

	if (opt_image_mask != NULL)
	{
		g_autoptr(AiImage) mask = ai_image_new_from_file(opt_image_mask,
		                                                 &error);

		if (mask == NULL)
		{
			g_printerr("ai: %s\n", error->message);
			return 1;
		}
		ai_image_request_set_mask(request, mask);
	}

	if (!image_add_extras(request))
		return 2;

	/* Under --strict an unsupported parameter is an error rather than
	 * being quietly dropped, so a script can tell its request through. */
	if (opt_image_strict)
	{
		const AiImageModelInfo *info = ai_image_generator_get_model_info(
			AI_IMAGE_GENERATOR(provider),
			ai_image_request_get_model(request));

		if (!ai_image_request_validate(request, info,
		                               AI_IMAGE_VALIDATE_STRICT, &error))
		{
			g_printerr("ai: %s\n", error->message);
			return 1;
		}
	}

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(provider),
	                                             request, NULL, &error);
	if (response == NULL)
	{
		g_printerr("ai: %s\n", error->message);
		return 1;
	}

	count = ai_image_response_get_image_count(response);
	if (count == 0)
	{
		g_printerr("ai: the provider returned no images\n");
		return 1;
	}

	for (i = 0; i < count; i++)
	{
		AiGeneratedImage *image = ai_image_response_get_image(response, i);
		g_autoptr(GBytes) bytes = NULL;
		g_autofree gchar *path = NULL;
		g_autoptr(GError) local_error = NULL;
		const gchar *revised;
		gconstpointer data;
		gsize length = 0;

		bytes = image_resolve_bytes(image, &local_error);
		if (bytes == NULL)
		{
			g_printerr("ai: could not retrieve image %u: %s\n", i + 1,
			           local_error->message);
			return 1;
		}

		path = image_output_path(ai_generated_image_get_mime_type(image), i,
		                         count);
		data = g_bytes_get_data(bytes, &length);

		if (!g_file_set_contents(path, data, length, &local_error))
		{
			g_printerr("ai: could not write %s: %s\n", path,
			           local_error->message);
			return 1;
		}

		printf("%s\n", path);

		/* Some models rewrite the prompt before generating; report it so
		 * the result can be reproduced. */
		revised = ai_generated_image_get_revised_prompt(image);
		if (revised != NULL)
			g_printerr("ai: revised prompt: %s\n", revised);
	}

	return 0;
}

int
main(int argc, char *argv[])
{
	g_autoptr(GOptionContext) ctx = NULL;
	g_autoptr(GError)         error = NULL;
	g_autoptr(AiConfig)       config = NULL;
	GObject                  *provider = NULL;
	AiProviderType            ptype;
	const gchar              *provider_name;
	g_autofree gchar         *prompt = NULL;
	AiMessage                *msg;
	GList                    *messages = NULL;
	int                       status = 0;

	ctx = g_option_context_new("[PROMPT] - chat with an AI provider");
	g_option_context_add_main_entries(ctx, option_entries, NULL);
	g_option_context_set_summary(
		ctx,
		"Send PROMPT (from the argument or stdin) to an AI provider and "
		"print the reply to stdout.\n"
		"With --image-gen, generate an image from PROMPT instead and write "
		"it to a file; the image options below apply in that mode.");

	g_option_context_set_description(
		ctx,
		"Image examples:\n"
		"  ai --image-gen -o sunset.png \"a sunset over mountains\"\n"
		"  ai --image-gen -p gemini --aspect 16:9 --resolution 2k \\\n"
		"                 \"a brass telescope on a wooden desk\"\n"
		"  ai --image-gen -p gemini -m gemini-3-pro-image-preview \\\n"
		"                 --ref logo.png --ref-role style \\\n"
		"                 --ref subject.jpg --ref-role subject \\\n"
		"                 -o poster.png \"combine these into a poster\"\n"
		"  ai --image-gen -p openai -m gpt-image-2 --background transparent \\\n"
		"                 --format webp -o icon.webp \"a minimal gear icon\"\n"
		"  ai --image-gen -n 4 --seed 42 --negative \"text, watermark\" \\\n"
		"                 \"abstract shapes in blue\"\n"
		"  ai --image-gen --image-op edit --ref photo.png --mask sky.png \\\n"
		"                 \"replace the sky with a storm\"\n"
		"  ai --list-image-models -p gemini\n"
		"\n"
		"Chat examples:\n"
		"  ai \"why is the sky blue?\"\n"
		"  git diff | ai -s \"Review this diff for bugs\"\n"
		"  ai -p ollama -m llama3.2 --stream \"one-liner: rsync a directory\"\n"
		"\n"
		"Unsupported options are dropped for models that cannot honour them;\n"
		"pass --strict to fail instead.  API keys come from the environment\n"
		"(ANTHROPIC_API_KEY, OPENAI_API_KEY, GEMINI_API_KEY, XAI_API_KEY);\n"
		"base URLs from OPENAI_BASE_URL, GEMINI_BASE_URL, XAI_BASE_URL and\n"
		"ANTHROPIC_BASE_URL.");
	if (!g_option_context_parse(ctx, &argc, &argv, &error))
	{
		g_printerr("ai: %s\n", error->message);
		return 2;
	}

	if (opt_version)
	{
		printf("ai (ai-glib) %d.%d.%d\n", AI_VERSION_MAJOR,
		       AI_VERSION_MINOR, AI_VERSION_MICRO);
		return 0;
	}

	if (opt_license)
	{
		printf("ai (ai-glib) %d.%d.%d\n"
		       "Copyright (C) 2025\n"
		       "SPDX-License-Identifier: AGPL-3.0-or-later\n\n"
		       "This program is free software: you may redistribute and/or\n"
		       "modify it under the terms of the GNU Affero General Public\n"
		       "License as published by the Free Software Foundation, either\n"
		       "version 3 of the License, or (at your option) any later\n"
		       "version.  There is NO WARRANTY, to the extent permitted by\n"
		       "law.  See <https://www.gnu.org/licenses/agpl-3.0.html>.\n",
		       AI_VERSION_MAJOR, AI_VERSION_MINOR, AI_VERSION_MICRO);
		return 0;
	}

	if (opt_list_providers)
	{
		list_providers();
		return 0;
	}

	/* Resolve provider type. */
	provider_name = opt_provider;
	if (provider_name == NULL)
		provider_name = g_getenv("AI_PROVIDER");
	if (provider_name == NULL)
		provider_name = "claude";
	ptype = ai_provider_type_from_string(provider_name);

	config = ai_config_new();

	/* Listing models needs a config but no prompt, so handle it before
	 * the provider and prompt are resolved. */
	if (opt_image_list_models)
	{
		return list_image_models(config, opt_provider);
	}

	provider = make_provider(config, ptype);
	if (provider == NULL)
	{
		g_printerr("ai: unknown provider '%s'\n", provider_name);
		return 2;
	}

	/*
	 * --set is applied after the dedicated flags so it can override them,
	 * and before anything runs so --dry-run shows what --set produced.
	 */
	if (!apply_property_overrides(provider))
	{
		g_object_unref(provider);
		return 2;
	}

	if (opt_interactive)
	{
		status = run_interactive(provider);
		g_object_unref(provider);
		return status;
	}

	/* Resolve the prompt: positional arg(s) win, else stdin. */
	if (argc > 1)
	{
		prompt = g_strjoinv(" ", &argv[1]);
	}
	else if (!isatty(STDIN_FILENO))
	{
		prompt = read_all_stdin();
		if (prompt != NULL)
			g_strchomp(prompt);
	}

	if (prompt == NULL || prompt[0] == '\0')
	{
		/* For dry-run we still want a representative message. */
		if (opt_dry_run)
		{
			g_free(prompt);
			prompt = g_strdup("(prompt)");
		}
		else
		{
			g_printerr("ai: no prompt given (pass it as an argument, "
			           "pipe it on stdin, or use --interactive)\n");
			g_object_unref(provider);
			return 2;
		}
	}

	/* Image mode diverges here: it needs the prompt but none of the
	 * message plumbing below. */
	if (opt_image_gen)
	{
		/* `prompt` is g_autofree; do not free it here as well. */
		status = generate_images(provider, prompt);
		g_object_unref(provider);
		return status;
	}

	msg = ai_message_new_user(prompt);
	messages = g_list_append(NULL, msg);

	if (opt_dry_run)
	{
		status = dry_run(provider, ptype, messages);
	}
	else if (opt_stream && AI_IS_STREAMABLE(provider))
	{
		stream_chat(provider, messages);
	}
	else
	{
		g_autoptr(AiResponse) resp = NULL;
		g_autofree gchar     *text = NULL;

		if (opt_stream)
			g_printerr("note: provider '%s' does not support streaming; "
			           "using a single response\n", provider_name);

		resp = sync_chat(provider, messages, &error);
		if (resp == NULL)
		{
			g_printerr("ai: %s\n", error ? error->message : "request failed");
			status = 1;
		}
		else
		{
			text = ai_response_get_text(resp);
			printf("%s\n", text ? text : "");
		}
	}

	g_list_free_full(messages, g_object_unref);
	g_object_unref(provider);
	return status;
}
