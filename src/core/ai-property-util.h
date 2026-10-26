/*
 * ai-property-util.h - Set a GObject property from "NAME=VALUE" text
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API. Every
 * symbol is `static inline`, so nothing new is exported and nothing new
 * is introspected (the ai-json-util.h pattern).
 *
 * Two places accept a provider knob as text: `ai --set NAME=VALUE` and a
 * benchmark suite's `properties:` list. They are one grammar, so they are
 * one parser. A second copy is how `--set effort-level=low` and the same
 * line in a suite file start meaning different things.
 *
 * Deliberately strict: a mistyped value silently becoming 0 or FALSE is a
 * worse outcome than an error, because the run still happens and the knob
 * the user asked for just is not there.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-property-util.h is an internal header"
#endif

#include <errno.h>
#include <glib-object.h>
#include <math.h>
#include <string.h>

#include "core/ai-error.h"

/*
 * ai_property_value_from_string:
 * @value: an uninitialised GValue; initialised to @pspec's type
 * @pspec: the property being set
 * @text: the textual value
 *
 * Parses @text as a value of @pspec's type. Accepts strings, booleans
 * (true/yes/on/1, false/no/off/0), the four integer types, double/float,
 * and enums by nick or by name.
 *
 * A number that does not fit the C type, or that falls outside the
 * pspec's own range, is refused rather than wrapped or clamped: "-1" for
 * a guint would otherwise become 4294967295, and g_object_set_property()
 * would warn (fatal under GTest) and leave the property unset -- the
 * knob silently not there, which is what this parser exists to prevent.
 *
 * Returns: TRUE when @text parsed; @value is initialised either way
 */
static inline gboolean
ai_property_value_from_string(
	GValue      *value,
	GParamSpec  *pspec,
	const gchar *text
){
	GType type;
	gchar *end;

	type = G_PARAM_SPEC_VALUE_TYPE(pspec);
	end = NULL;
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
		gint64 parsed;
		guint64 unsigned_parsed;

		/* Unsigned types are parsed unsigned, and refuse a sign outright. */
		errno = 0;
		if (type == G_TYPE_UINT || type == G_TYPE_UINT64)
		{
			if (strchr(text, '-') != NULL)
				return FALSE;
			unsigned_parsed = g_ascii_strtoull(text, &end, 10);
			parsed = 0;
		}
		else
		{
			parsed = g_ascii_strtoll(text, &end, 10);
			unsigned_parsed = 0;
		}
		if (end == text || *end != '\0' || errno == ERANGE)
			return FALSE;

		/* Fits the C type, or it is refused rather than wrapped. */
		if (type == G_TYPE_INT)
		{
			if (parsed < G_MININT || parsed > G_MAXINT)
				return FALSE;
			g_value_set_int(value, (gint)parsed);
		}
		else if (type == G_TYPE_UINT)
		{
			if (unsigned_parsed > G_MAXUINT)
				return FALSE;
			g_value_set_uint(value, (guint)unsigned_parsed);
		}
		else if (type == G_TYPE_INT64)
			g_value_set_int64(value, parsed);
		else
			g_value_set_uint64(value, unsigned_parsed);

		/* And fits the property: the validator changes what it would clamp. */
		return !g_param_value_validate(pspec, value);
	}

	if (type == G_TYPE_DOUBLE || type == G_TYPE_FLOAT)
	{
		gdouble parsed;

		parsed = g_ascii_strtod(text, &end);
		if (end == text || *end != '\0' || !isfinite(parsed))
			return FALSE;

		if (type == G_TYPE_DOUBLE)
			g_value_set_double(value, parsed);
		else
			g_value_set_float(value, (gfloat)parsed);
		return !g_param_value_validate(pspec, value);
	}

	if (G_TYPE_IS_ENUM(type))
	{
		g_autoptr(GEnumClass) klass = (GEnumClass *)g_type_class_ref(type);
		GEnumValue *ev;

		ev = g_enum_get_value_by_nick(klass, text);
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
 * ai_property_dup_settable:
 * @object: any GObject
 *
 * Lists every writable property of @object, comma separated, so an error
 * about an unknown name can answer with the real list rather than repeat
 * the guess back.
 *
 * Returns: (transfer full): "a, b, c"
 */
static inline gchar *
ai_property_dup_settable(
	GObject *object
){
	g_autofree GParamSpec **pspecs = NULL;
	g_autoptr(GString) out = NULL;
	guint n;
	guint i;

	n = 0;
	out = g_string_new(NULL);
	pspecs = g_object_class_list_properties(G_OBJECT_GET_CLASS(object), &n);
	for (i = 0; i < n; i++)
	{
		if ((pspecs[i]->flags & G_PARAM_WRITABLE) == 0 ||
		    (pspecs[i]->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
			continue;
		if (out->len > 0)
			g_string_append(out, ", ");
		g_string_append(out, g_param_spec_get_name(pspecs[i]));
	}
	return g_string_free((GString *)g_steal_pointer(&out), FALSE);
}

/*
 * ai_property_set_from_assignment:
 * @object: the object to configure
 * @assignment: "NAME=VALUE", or a bare "NAME" meaning NAME=true
 * @error: return location for an error
 *
 * The whole `--set` grammar. Refuses an unknown name (listing the real
 * ones), a read-only or construct-only property, and a value that does
 * not parse as the property's type. Errors are AI_ERROR_INVALID_REQUEST.
 *
 * Returns: TRUE when the property was set
 */
static inline gboolean
ai_property_set_from_assignment(
	GObject      *object,
	const gchar  *assignment,
	GError      **error
){
	g_auto(GValue) value = G_VALUE_INIT;
	g_autofree gchar *name = NULL;
	const gchar *text;
	const gchar *eq;
	GParamSpec *pspec;

	/* Split NAME=VALUE; a bare NAME is a boolean switch. */
	eq = strchr(assignment, '=');
	if (eq != NULL)
	{
		name = g_strndup(assignment, (gsize)(eq - assignment));
		text = eq + 1;
	}
	else
	{
		name = g_strdup(assignment);
		text = "true";
	}
	g_strstrip(name);

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), name);
	if (pspec == NULL)
	{
		g_autofree gchar *names = ai_property_dup_settable(object);

		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "%s has no property '%s' (writable: %s)",
		            G_OBJECT_TYPE_NAME(object), name, names);
		return FALSE;
	}

	if ((pspec->flags & G_PARAM_WRITABLE) == 0 ||
	    (pspec->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "property '%s' of %s is read-only",
		            name, G_OBJECT_TYPE_NAME(object));
		return FALSE;
	}

	if (!ai_property_value_from_string(&value, pspec, text))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "cannot read '%s' as a value for '%s' (%s)",
		            text, name, g_type_name(G_PARAM_SPEC_VALUE_TYPE(pspec)));
		return FALSE;
	}

	g_object_set_property(object, name, &value);
	return TRUE;
}
