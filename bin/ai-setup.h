/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Private, header-only setup UI: each C source in bin becomes an executable.
 */
#pragma once

typedef struct {
	gint refs;
	GMainLoop *loop;
	GList *models;
	GError *error;
	gboolean done;
	GCancellable *cancel;
} AiSetupModels;

/* A late discovery callback owns its state even after the deadline exits. */
static void
ai_setup_models_unref(AiSetupModels *state)
{
	if (--state->refs != 0)
		return;
	g_list_free_full(state->models, g_free);
	g_clear_error(&state->error);
	g_main_loop_unref(state->loop);
	g_object_unref(state->cancel);
	g_free(state);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiSetupModels, ai_setup_models_unref)

/* Finish through the public interface, including CLI static model lists. */
static void
ai_setup_models_ready(GObject *source, GAsyncResult *result, gpointer data)
{
	AiSetupModels *state = (AiSetupModels *)data;

	state->models = ai_provider_list_models_finish(AI_PROVIDER(source), result,
	                                              &state->error);
	state->done = TRUE;
	g_main_loop_quit(state->loop);
	ai_setup_models_unref(state);
}

/* Stop waiting even if a provider does not promptly honor cancellation. */
static gboolean
ai_setup_models_timeout(gpointer data)
{
	AiSetupModels *state = (AiSetupModels *)data;

	g_cancellable_cancel(state->cancel);
	g_main_loop_quit(state->loop);
	return G_SOURCE_REMOVE;
}

/* Read complete lines without a TTY requirement or fixed model-id limit. */
static gchar *
ai_setup_read(const gchar *prompt)
{
	g_autoptr(GString) line = g_string_new(NULL);
	gint ch;

	g_print("%s", prompt);
	fflush(stdout);
	while ((ch = fgetc(stdin)) != EOF && ch != '\n')
		g_string_append_c(line, (gchar)ch);
	if (ch == EOF)
		return NULL;
	g_strstrip(line->str);
	if (g_ascii_strcasecmp(line->str, "q") == 0 ||
	    g_ascii_strcasecmp(line->str, "cancel") == 0)
		return NULL;
	return g_strdup(line->str);
}

/* Zero is cancellation; menu choices are strictly positive decimal numbers. */
static guint
ai_setup_pick(const gchar *prompt, guint count)
{
	for (;;)
	{
		g_autofree gchar *line = ai_setup_read(prompt);
		guint64 value;

		if (line == NULL)
			return 0;
		if (g_ascii_string_to_unsigned(line, 10, 1, count, &value, NULL))
			return (guint)value;
		g_print("Choose a number from 1 to %u, or q to cancel.\n", count);
	}
}

/* Select and confirm one independent scope; no mutation precedes confirmation. */
static gint
ai_setup_run(AiConfig *config)
{
	const gchar *scopes[] = { "ai", "ai-tui", NULL };
	g_autoptr(GEnumClass) providers = g_type_class_ref(AI_TYPE_PROVIDER_TYPE);
	g_autoptr(GObject) provider = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *model = NULL;
	g_autoptr(AiSetupModels) state = NULL;
	g_autoptr(GSource) deadline = NULL;
	const gchar *app;
	const gchar *saved_model;
	AiProviderType selected;
	guint scope, choice, i, count;
	guint timeout;
	GList *item;

	g_print("ai-glib defaults setup\n"
	        "Each scope is independent. q/cancel or EOF leaves files unchanged.\n\n");
	for (i = 0; i < G_N_ELEMENTS(scopes); i++)
	{
		AiProviderType saved = scopes[i] != NULL ?
			ai_config_get_app_provider(config, scopes[i]) :
			ai_config_get_default_provider(config);

		saved_model = scopes[i] != NULL ? ai_config_get_app_model(config, scopes[i]) :
			ai_config_get_default_model(config);
		g_print("  %u. %s (current: %s / %s)\n", i + 1,
		        scopes[i] != NULL ? scopes[i] : "library",
		        ai_provider_type_to_string(saved),
		        saved_model != NULL && saved_model[0] != '\0' ? saved_model : "native");
	}
	scope = ai_setup_pick("Scope: ", 3);
	if (scope == 0)
		goto cancelled;
	app = scopes[scope - 1];
	if (g_getenv("AI_PROVIDER") != NULL && g_getenv("AI_PROVIDER")[0] != '\0')
		g_print("Warning: AI_PROVIDER overrides omitted provider selections; "
		        "use --provider default to bypass it.\n");
	if (app == NULL && (g_getenv("AI_GLIB_DEFAULT_PROVIDER") != NULL ||
	                    g_getenv("AI_GLIB_DEFAULT_MODEL") != NULL))
		g_print("Warning: AI_GLIB_DEFAULT_PROVIDER/AI_GLIB_DEFAULT_MODEL override "
		        "saved library defaults, not app defaults.\n");
	g_print("\nProviders\n");
	for (i = 0; i < providers->n_values; i++)
		g_print("  %u. %s\n", i + 1,
		        ai_provider_type_to_string((AiProviderType)providers->values[i].value));
	choice = ai_setup_pick("Provider: ", providers->n_values);
	if (choice == 0)
		goto cancelled;
	selected = (AiProviderType)providers->values[choice - 1].value;

	/* Bound network discovery, with no retries, without persisting those knobs. */
	timeout = CLAMP(ai_config_get_timeout(config), 1, 10);
	ai_config_set_timeout(config, timeout);
	ai_config_set_max_retries(config, 0);
	provider = ai_provider_factory_new(selected, config, &error);
	state = g_new0(AiSetupModels, 1);
	state->refs = 1;
	state->loop = g_main_loop_new(NULL, FALSE);
	state->cancel = g_cancellable_new();
	if (provider != NULL)
	{
		g_print("Loading models (at most %u seconds)...\n", timeout);
		deadline = g_timeout_source_new(timeout * 1000);
		g_source_set_callback(deadline, ai_setup_models_timeout, state, NULL);
		g_source_attach(deadline, NULL);
		state->refs++;
		ai_provider_list_models_async(AI_PROVIDER(provider), state->cancel,
		                              ai_setup_models_ready, state);
		if (!state->done)
			g_main_loop_run(state->loop);
		g_source_destroy(deadline);
		if (!state->done)
			g_print("Discovery timed out; native and manual choices remain available.\n");
		else if (state->error != NULL)
			g_print("Discovery unavailable: %s\n", state->error->message);
	}
	else
		g_print("Discovery unavailable: %s\n", error->message);
	g_print("\nModels\n  1. Provider native default\n  2. Enter model ID manually\n");
	count = 2;
	for (item = state->models; item != NULL; item = item->next)
		g_print("  %u. %s\n", ++count, (const gchar *)item->data);
	choice = ai_setup_pick("Model: ", count);
	if (choice == 0)
		goto cancelled;
	if (choice == 2)
	{
		for (;;)
		{
			model = ai_setup_read("Model ID: ");
			if (model == NULL)
				goto cancelled;
			if (model[0] != '\0' && g_utf8_validate(model, -1, NULL) &&
			    !g_str_equal(model, "default"))
				break;
			g_print("Enter a nonempty UTF-8 model ID (default is reserved).\n");
			g_clear_pointer(&model, g_free);
		}
	}
	else if (choice > 2)
		model = g_strdup((const gchar *)g_list_nth_data(state->models, choice - 3));
	g_print("\nSave %s defaults: provider=%s, model=%s\n",
	        app != NULL ? app : "library", ai_provider_type_to_string(selected),
	        model != NULL ? model : "native");
	for (;;)
	{
		g_autofree gchar *answer = ai_setup_read("Save? [y/N]: ");

		if (answer == NULL || answer[0] == '\0' ||
		    g_ascii_strcasecmp(answer, "n") == 0 || g_ascii_strcasecmp(answer, "no") == 0)
			goto cancelled;
		if (g_ascii_strcasecmp(answer, "y") == 0 || g_ascii_strcasecmp(answer, "yes") == 0)
			break;
		g_print("Enter y to save, or n to cancel.\n");
	}
	g_clear_error(&error);
	if (!ai_config_save_defaults(config, app, selected, model, &error))
	{
		g_printerr("ai: could not save defaults: %s\n", error->message);
		return 1;
	}
	g_print("Defaults saved. Run ai --setup again to change another scope.\n");
	return 0;

cancelled:
	g_print("Setup cancelled; nothing saved.\n");
	return 0;
}
