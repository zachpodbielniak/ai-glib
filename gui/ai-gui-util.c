/*
 * ai-gui-util.c - Small shared helpers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-util.h"

gboolean
ai_gui_value_from_string(
	GValue      *value,
	GParamSpec  *pspec,
	const gchar *text
){
	GType type;
	gchar *end = NULL;

	g_return_val_if_fail(value != NULL, FALSE);
	g_return_val_if_fail(pspec != NULL, FALSE);
	g_return_val_if_fail(text != NULL, FALSE);

	type = G_PARAM_SPEC_VALUE_TYPE(pspec);
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
			g_value_set_int(value, (gint)parsed);
		else if (type == G_TYPE_UINT)
			g_value_set_uint(value, (guint)parsed);
		else if (type == G_TYPE_INT64)
			g_value_set_int64(value, parsed);
		else
			g_value_set_uint64(value, (guint64)parsed);

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
			g_value_set_float(value, (gfloat)parsed);

		return TRUE;
	}

	if (G_TYPE_IS_ENUM(type))
	{
		GEnumClass *klass = g_type_class_ref(type);
		GEnumValue *entry = g_enum_get_value_by_nick(klass, text);

		if (entry == NULL)
			entry = g_enum_get_value_by_name(klass, text);

		if (entry != NULL)
			g_value_set_enum(value, entry->value);

		g_type_class_unref(klass);
		return entry != NULL;
	}

	return FALSE;
}

gchar *
ai_gui_value_to_string(
	GObject    *object,
	GParamSpec *pspec
){
	GValue value = G_VALUE_INIT;
	GType type;
	gchar *text = NULL;

	g_return_val_if_fail(G_IS_OBJECT(object), NULL);
	g_return_val_if_fail(pspec != NULL, NULL);

	type = G_PARAM_SPEC_VALUE_TYPE(pspec);
	g_value_init(&value, type);
	g_object_get_property(object, g_param_spec_get_name(pspec), &value);

	if (type == G_TYPE_STRING)
		text = g_strdup(g_value_get_string(&value) != NULL
		                ? g_value_get_string(&value) : "");
	else if (type == G_TYPE_BOOLEAN)
		text = g_strdup(g_value_get_boolean(&value) ? "true" : "false");
	else if (type == G_TYPE_INT)
		text = g_strdup_printf("%d", g_value_get_int(&value));
	else if (type == G_TYPE_UINT)
		text = g_strdup_printf("%u", g_value_get_uint(&value));
	else if (type == G_TYPE_INT64)
		text = g_strdup_printf("%" G_GINT64_FORMAT, g_value_get_int64(&value));
	else if (type == G_TYPE_UINT64)
		text = g_strdup_printf("%" G_GUINT64_FORMAT, g_value_get_uint64(&value));
	else if (type == G_TYPE_DOUBLE)
		text = g_strdup_printf("%g", g_value_get_double(&value));
	else if (type == G_TYPE_FLOAT)
		text = g_strdup_printf("%g", (gdouble)g_value_get_float(&value));
	else if (G_TYPE_IS_ENUM(type))
	{
		GEnumClass *klass = g_type_class_ref(type);
		GEnumValue *entry = g_enum_get_value(klass, g_value_get_enum(&value));

		text = g_strdup(entry != NULL ? entry->value_nick : "");
		g_type_class_unref(klass);
	}

	g_value_unset(&value);

	return text != NULL ? text : g_strdup("");
}

gchar *
ai_gui_format_relative_time(gint64 unix_time)
{
	gint64 now;
	gint64 delta;

	if (unix_time <= 0)
		return g_strdup("never");

	now = g_get_real_time() / G_USEC_PER_SEC;
	delta = now - unix_time;

	if (delta < 60)
		return g_strdup("just now");

	if (delta < 3600)
		return g_strdup_printf("%dm ago", (gint)(delta / 60));

	if (delta < 86400)
		return g_strdup_printf("%dh ago", (gint)(delta / 3600));

	if (delta < 86400 * 7)
		return g_strdup_printf("%dd ago", (gint)(delta / 86400));

	{
		g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(unix_time);

		return when != NULL ? g_date_time_format(when, "%Y-%m-%d")
		                    : g_strdup("a while ago");
	}
}

gchar *
ai_gui_summarise_prompt(const gchar *text)
{
	const gchar *start;
	const gchar *newline;
	g_autofree gchar *line = NULL;
	g_autofree gchar *stripped = NULL;

	if (text == NULL || *text == '\0')
		return g_strdup("New session");

	/* The first non-blank line is what a person would call it. */
	start = text;
	while (*start == '\n' || *start == '\r' || *start == ' ' || *start == '\t')
		start++;

	newline = strchr(start, '\n');
	line = newline != NULL ? g_strndup(start, (gsize)(newline - start))
	                       : g_strdup(start);
	stripped = g_strdup(g_strstrip(line));

	if (*stripped == '\0')
		return g_strdup("New session");

	/*
	 * Elided on characters rather than bytes: a title cut mid-sequence is
	 * invalid UTF-8, and GTK renders that as a run of replacement glyphs
	 * rather than the words somebody typed.
	 */
	if (g_utf8_strlen(stripped, -1) > 60)
	{
		const gchar *end = g_utf8_offset_to_pointer(stripped, 60);

		return g_strdup_printf("%.*s…", (gint)(end - stripped), stripped);
	}

	return g_steal_pointer(&stripped);
}

AiGuiOptions *
ai_gui_options_copy(const AiGuiOptions *self)
{
	AiGuiOptions *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = g_new0(AiGuiOptions, 1);
	copy->provider = g_strdup(self->provider);
	copy->model = g_strdup(self->model);
	copy->system = g_strdup(self->system);
	copy->effort = g_strdup(self->effort);
	copy->working_directory = g_strdup(self->working_directory);
	copy->sets = self->sets != NULL ? g_strdupv(self->sets) : NULL;
	copy->max_tokens = self->max_tokens;
	copy->stream = self->stream;
	copy->continue_session = self->continue_session;
	copy->skip_permissions = self->skip_permissions;
	copy->local_tools = self->local_tools;
	copy->approve_all = self->approve_all;
	copy->expand = self->expand;
	copy->agents = self->agents;
	copy->dashboard = self->dashboard;

	return copy;
}

void
ai_gui_options_free(AiGuiOptions *self)
{
	if (self == NULL)
		return;

	g_free(self->provider);
	g_free(self->model);
	g_free(self->system);
	g_free(self->effort);
	g_free(self->working_directory);
	g_strfreev(self->sets);
	g_free(self);
}
