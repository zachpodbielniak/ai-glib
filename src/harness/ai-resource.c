/*
 * ai-resource.c - A command, skill or agent, as a file on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include <yaml-glib.h>

#include "core/ai-error.h"
#include "harness/ai-resource.h"

struct _AiResource
{
    GObject          parent_instance;

    AiResourceKind   kind;
    AiResourceScope  scope;

    gchar           *name;
    gchar           *description;
    gchar           *body;
    gchar           *path;
    gchar           *origin;

    /* Frontmatter scalars, verbatim. A sequence is stored here joined
     * with ", " so ai_resource_get_meta() has something to return for
     * every key, whatever its YAML shape. */
    GHashTable      *meta;        /* gchar* -> gchar* */

    /* Only sequences appear here. ai_resource_get_meta_list() prefers
     * this and falls back to splitting the scalar, which is what makes
     * `tools: [a, b]` and `tools: a, b` indistinguishable to a caller. */
    GHashTable      *meta_lists;  /* gchar* -> GStrv */
};

enum
{
    PROP_0,
    PROP_KIND,
    PROP_SCOPE,
    PROP_NAME,
    PROP_DESCRIPTION,
    PROP_BODY,
    PROP_PATH,
    PROP_ORIGIN,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_TYPE(AiResource, ai_resource, G_TYPE_OBJECT)

/* ================================================================
 * GObject plumbing
 * ================================================================ */

static void
ai_resource_finalize(GObject *object)
{
    AiResource *self = AI_RESOURCE(object);

    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->description, g_free);
    g_clear_pointer(&self->body, g_free);
    g_clear_pointer(&self->path, g_free);
    g_clear_pointer(&self->origin, g_free);
    g_clear_pointer(&self->meta, g_hash_table_unref);
    g_clear_pointer(&self->meta_lists, g_hash_table_unref);

    G_OBJECT_CLASS(ai_resource_parent_class)->finalize(object);
}

static void
ai_resource_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiResource *self = AI_RESOURCE(object);

    switch (prop_id)
    {
        case PROP_KIND:
            g_value_set_int(value, (gint)self->kind);
            break;

        case PROP_SCOPE:
            g_value_set_int(value, (gint)self->scope);
            break;

        case PROP_NAME:
            g_value_set_string(value, self->name);
            break;

        case PROP_DESCRIPTION:
            g_value_set_string(value, self->description);
            break;

        case PROP_BODY:
            g_value_set_string(value, self->body);
            break;

        case PROP_PATH:
            g_value_set_string(value, self->path);
            break;

        case PROP_ORIGIN:
            g_value_set_string(value, self->origin);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_resource_class_init(AiResourceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_resource_finalize;
    object_class->get_property = ai_resource_get_property;

    /**
     * AiResource:kind:
     *
     * Whether this is a command, a skill or an agent.
     */
    properties[PROP_KIND] =
        g_param_spec_int("kind", NULL, NULL,
                         0, 2, 0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiResource:scope:
     *
     * Whether this was found in the project, the user's home, or
     * compiled in.
     */
    properties[PROP_SCOPE] =
        g_param_spec_int("scope", NULL, NULL,
                         0, 2, 0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiResource:name:
     *
     * What the user types after the slash.
     */
    properties[PROP_NAME] =
        g_param_spec_string("name", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiResource:description:
     *
     * One line, shown in listings and completion.
     */
    properties[PROP_DESCRIPTION] =
        g_param_spec_string("description", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiResource:body:
     *
     * Everything after the frontmatter.
     */
    properties[PROP_BODY] =
        g_param_spec_string("body", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiResource:path:
     *
     * Where it was read from, or %NULL if it was built from data.
     */
    properties[PROP_PATH] =
        g_param_spec_string("path", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiResource:origin:
     *
     * Which harness's directory it came from --- "claude", "opencode",
     * "grok" or "ai-glib".
     */
    properties[PROP_ORIGIN] =
        g_param_spec_string("origin", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_resource_init(AiResource *self)
{
    self->meta = g_hash_table_new_full(g_str_hash, g_str_equal,
                                       g_free, g_free);
    self->meta_lists = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify)g_strfreev);
}

/* ================================================================
 * Frontmatter
 * ================================================================ */

/*
 * Is this line a frontmatter delimiter?
 *
 * Accepts "---" and "..." with trailing whitespace, and tolerates a CR
 * before the newline so a file saved on Windows still parses. @len is
 * the line's length excluding its newline.
 */
static gboolean
line_is_delimiter(const gchar *line, gsize len)
{
    /* Tolerate a lone CR at the end of the line (CRLF input). */
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                       line[len - 1] == '\t'))
    {
        len--;
    }

    if (len != 3)
    {
        return FALSE;
    }

    return strncmp(line, "---", 3) == 0 || strncmp(line, "...", 3) == 0;
}

/* The offset just past the newline ending the line at @offset. */
static gsize
next_line(const gchar *text, gsize len, gsize offset)
{
    while (offset < len && text[offset] != '\n')
    {
        offset++;
    }

    if (offset < len)
    {
        offset++;
    }

    return offset;
}

/* The length of the line starting at @offset, excluding its newline. */
static gsize
line_length(const gchar *text, gsize len, gsize offset)
{
    gsize end = offset;

    while (end < len && text[end] != '\n')
    {
        end++;
    }

    return end - offset;
}

/*
 * Split @text into its frontmatter and its body.
 *
 * Frontmatter is recognised only when the very first line is a
 * delimiter --- a `---` further down is ordinary markdown (a horizontal
 * rule), and treating it as frontmatter would silently swallow content.
 * A file whose opening delimiter is never closed is treated as having no
 * frontmatter at all rather than as an error: a half-written file should
 * still show up in a listing, minus its description.
 *
 * Returns: %TRUE when frontmatter was found, with @out_fm_* describing
 *   it. @out_body_start is set either way.
 */
static gboolean
split_frontmatter(
    const gchar *text,
    gsize        len,
    gsize       *out_fm_start,
    gsize       *out_fm_len,
    gsize       *out_body_start
){
    gsize first_len;
    gsize fm_start;
    gsize offset;

    *out_fm_start = 0;
    *out_fm_len = 0;
    *out_body_start = 0;

    first_len = line_length(text, len, 0);

    if (!line_is_delimiter(text, first_len))
    {
        return FALSE;
    }

    fm_start = next_line(text, len, 0);
    offset = fm_start;

    while (offset < len)
    {
        gsize this_len = line_length(text, len, offset);

        if (line_is_delimiter(text + offset, this_len))
        {
            *out_fm_start = fm_start;
            *out_fm_len = offset - fm_start;
            *out_body_start = next_line(text, len, offset);
            return TRUE;
        }

        offset = next_line(text, len, offset);
    }

    /* Opened but never closed. */
    return FALSE;
}

/* Render one scalar node as text. Non-scalars yield NULL. */
static gchar *
node_to_scalar(YamlNode *node)
{
    const gchar *str;

    if (node == NULL)
    {
        return NULL;
    }

    if (yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
    {
        return NULL;
    }

    str = yaml_node_get_string(node);

    if (str == NULL)
    {
        return NULL;
    }

    return g_strdup(str);
}

/*
 * Fold one frontmatter member into the two metadata tables.
 *
 * Scalars land in @meta. Sequences land in @meta_lists *and* in @meta
 * joined with ", ", so a caller that only knows about
 * ai_resource_get_meta() still sees something reasonable for a key an
 * unfamiliar harness wrote as a list.
 */
static void
store_member(
    AiResource  *self,
    const gchar *key,
    YamlNode    *node
){
    YamlNodeType type;

    if (key == NULL || node == NULL)
    {
        return;
    }

    type = yaml_node_get_node_type(node);

    if (type == YAML_NODE_SCALAR)
    {
        gchar *scalar = node_to_scalar(node);

        if (scalar != NULL)
        {
            g_hash_table_replace(self->meta, g_strdup(key), scalar);
        }

        return;
    }

    if (type == YAML_NODE_SEQUENCE)
    {
        YamlSequence *seq = yaml_node_get_sequence(node);
        g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func(g_free);
        guint n;
        guint i;

        if (seq == NULL)
        {
            return;
        }

        n = (guint)yaml_sequence_get_length(seq);

        for (i = 0; i < n; i++)
        {
            gchar *scalar = node_to_scalar(yaml_sequence_get_element(seq, i));

            if (scalar != NULL)
            {
                g_ptr_array_add(parts, scalar);
            }
        }

        g_ptr_array_add(parts, NULL);

        {
            gchar **strv = (gchar **)parts->pdata;
            gchar  *joined = g_strjoinv(", ", strv);

            g_hash_table_replace(self->meta, g_strdup(key), joined);
            g_hash_table_replace(self->meta_lists, g_strdup(key),
                                 g_strdupv(strv));
        }

        return;
    }

    /*
     * A mapping or a null. Neither has a sensible flat rendering, and
     * inventing one would be worse than admitting we do not model it ---
     * the key simply does not appear. g_debug rather than g_message
     * because the file is not wrong, it just uses a shape ai-glib has no
     * use for.
     */
    g_debug("ai_resource: frontmatter key '%s' is not a scalar or "
            "sequence; ignoring", key);
}

/*
 * Parse the frontmatter region into the metadata tables.
 *
 * Failure is deliberately not fatal. A file whose YAML does not parse,
 * or whose frontmatter is a sequence rather than a mapping, still yields
 * a usable resource --- one with no description. Losing sixteen good
 * command files because a seventeenth has a stray tab would be a much
 * worse outcome than a missing one-line summary, and this is exactly the
 * "the input is not ours" case the log-level rule covers.
 */
static void
parse_frontmatter(
    AiResource  *self,
    const gchar *data,
    gsize        len
){
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(GError)     local_error = NULL;
    YamlNode             *root;
    YamlMapping          *map;
    guint                 n;
    guint                 i;

    if (len == 0)
    {
        return;
    }

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_data(parser, data, (gssize)len, &local_error))
    {
        g_debug("ai_resource: frontmatter did not parse (%s); "
                "continuing without metadata",
                local_error != NULL ? local_error->message : "unknown");
        return;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
    {
        g_debug("ai_resource: frontmatter is not a mapping; "
                "continuing without metadata");
        return;
    }

    map = yaml_node_get_mapping(root);

    if (map == NULL)
    {
        return;
    }

    n = (guint)yaml_mapping_get_size(map);

    for (i = 0; i < n; i++)
    {
        const gchar *key = yaml_mapping_get_key(map, i);

        store_member(self, key, yaml_mapping_get_value(map, i));
    }
}

/*
 * A description for a file that did not declare one.
 *
 * The first line that is neither blank, nor a markdown heading, nor a
 * code fence. Every real example on this machine that omits
 * `description:` opens with a heading followed by a sentence, so this
 * lands on the sentence.
 */
static gchar *
derive_description(const gchar *body)
{
    gsize len;
    gsize offset = 0;

    if (body == NULL)
    {
        return NULL;
    }

    len = strlen(body);

    while (offset < len)
    {
        gsize             line_len = line_length(body, len, offset);
        g_autofree gchar *line = g_strndup(body + offset, line_len);
        gchar            *trimmed = g_strstrip(line);

        if (trimmed[0] != '\0' &&
            trimmed[0] != '#' &&
            !g_str_has_prefix(trimmed, "```") &&
            !g_str_has_prefix(trimmed, "---"))
        {
            return g_strdup(trimmed);
        }

        offset = next_line(body, len, offset);
    }

    return NULL;
}

/*
 * The name a path implies.
 *
 * A skill lives at <name>/SKILL.md, so for that shape the directory
 * names it and the basename does not. Everything else is the stem.
 */
static gchar *
name_from_path(const gchar *path)
{
    g_autofree gchar *base = g_path_get_basename(path);
    gchar            *dot;

    if (g_ascii_strcasecmp(base, "SKILL.md") == 0 ||
        g_ascii_strcasecmp(base, "AGENT.md") == 0 ||
        g_ascii_strcasecmp(base, "COMMAND.md") == 0)
    {
        g_autofree gchar *dir = g_path_get_dirname(path);

        return g_path_get_basename(dir);
    }

    dot = g_strrstr(base, ".");

    if (dot != NULL && dot != base)
    {
        *dot = '\0';
    }

    return g_steal_pointer(&base);
}

/* ================================================================
 * Construction
 * ================================================================ */

/**
 * ai_resource_new_from_data:
 * @data: the file's contents
 * @length: its length, or -1 if @data is NUL-terminated
 * @name: (nullable): the name to use when the frontmatter omits one
 * @kind: what this resource is
 * @origin: (nullable): which harness's directory it came from
 * @scope: project, user, or builtin
 * @error: (nullable): return location for a #GError
 *
 * Parses markdown with optional YAML frontmatter.
 *
 * Frontmatter is only recognised when the very first line is `---`; a
 * `---` further down is a horizontal rule and stays in the body. An
 * opening delimiter that is never closed is treated as no frontmatter,
 * so a half-saved file degrades rather than disappearing.
 *
 * Metadata that does not parse is dropped, not fatal --- see
 * ai_resource_get_meta(). The one thing that *is* fatal is content that
 * is not valid UTF-8, because everything downstream (offsets, wrapping,
 * the model itself) assumes it.
 *
 * Returns: (transfer full) (nullable): a new #AiResource, or %NULL
 */
AiResource *
ai_resource_new_from_data(
    const gchar     *data,
    gssize           length,
    const gchar     *name,
    AiResourceKind   kind,
    const gchar     *origin,
    AiResourceScope  scope,
    GError         **error
){
    g_autoptr(AiResource) self = NULL;
    const gchar          *text;
    gsize                 len;
    gsize                 fm_start = 0;
    gsize                 fm_len = 0;
    gsize                 body_start = 0;
    const gchar          *declared;

    g_return_val_if_fail(data != NULL, NULL);

    len = (length < 0) ? strlen(data) : (gsize)length;
    text = data;

    /* A UTF-8 BOM is invisible to a text editor and would otherwise make
     * the first line "\xef\xbb\xbf---", which is not a delimiter. */
    if (len >= 3 && (guchar)text[0] == 0xEF &&
        (guchar)text[1] == 0xBB && (guchar)text[2] == 0xBF)
    {
        text += 3;
        len -= 3;
    }

    if (!g_utf8_validate(text, (gssize)len, NULL))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "resource content is not valid UTF-8");
        return NULL;
    }

    self = g_object_new(AI_TYPE_RESOURCE, NULL);
    self->kind = kind;
    self->scope = scope;
    self->origin = g_strdup(origin != NULL ? origin : "ai-glib");

    if (split_frontmatter(text, len, &fm_start, &fm_len, &body_start))
    {
        g_autofree gchar *fm = g_strndup(text + fm_start, fm_len);

        parse_frontmatter(self, fm, fm_len);
    }

    self->body = g_strndup(text + body_start, len - body_start);

    declared = g_hash_table_lookup(self->meta, "name");

    if (declared != NULL && declared[0] != '\0')
    {
        self->name = g_strdup(declared);
    }
    else
    {
        self->name = g_strdup(name);
    }

    declared = g_hash_table_lookup(self->meta, "description");

    if (declared != NULL && declared[0] != '\0')
    {
        self->description = g_strdup(declared);
    }
    else
    {
        self->description = derive_description(self->body);
    }

    return (AiResource *)g_steal_pointer(&self);
}

/**
 * ai_resource_new_from_file:
 * @path: the file to read
 * @name: (nullable): the name to use, or %NULL to derive one from @path
 * @kind: what this resource is
 * @origin: (nullable): which harness's directory it came from
 * @scope: project, user, or builtin
 * @error: (nullable): return location for a #GError
 *
 * Loads a resource from disk.
 *
 * With @name %NULL the name falls back to the file's stem, except for
 * the nested `<name>/SKILL.md` layout where the directory names it ---
 * otherwise every skill on the system would be called "SKILL". Pass a
 * @name when the path alone does not say it, which is how a namespaced
 * command in `git/status.md` ends up called `git:status`. A frontmatter
 * `name:` still wins over both: a file that declares its own name means
 * it.
 *
 * Returns: (transfer full) (nullable): a new #AiResource, or %NULL
 */
AiResource *
ai_resource_new_from_file(
    const gchar     *path,
    const gchar     *name,
    AiResourceKind   kind,
    const gchar     *origin,
    AiResourceScope  scope,
    GError         **error
){
    g_autofree gchar     *contents = NULL;
    g_autofree gchar     *implied = NULL;
    g_autoptr(AiResource) self = NULL;
    gsize                 len = 0;

    g_return_val_if_fail(path != NULL, NULL);

    if (!g_file_get_contents(path, &contents, &len, error))
    {
        return NULL;
    }

    if (name == NULL)
    {
        implied = name_from_path(path);
        name = implied;
    }

    self = ai_resource_new_from_data(contents, (gssize)len, name,
                                     kind, origin, scope, error);

    if (self == NULL)
    {
        return NULL;
    }

    self->path = g_strdup(path);

    return (AiResource *)g_steal_pointer(&self);
}

/* ================================================================
 * Accessors
 * ================================================================ */

/**
 * ai_resource_get_kind:
 * @self: an #AiResource
 *
 * Returns: whether this is a command, a skill or an agent
 */
AiResourceKind
ai_resource_get_kind(AiResource *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE(self), AI_RESOURCE_COMMAND);

    return self->kind;
}

/**
 * ai_resource_get_scope:
 * @self: an #AiResource
 *
 * Returns: where it was found
 */
AiResourceScope
ai_resource_get_scope(AiResource *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE(self), AI_RESOURCE_SCOPE_BUILTIN);

    return self->scope;
}

/**
 * ai_resource_get_name:
 * @self: an #AiResource
 *
 * Returns: (transfer none) (nullable): what the user types after the slash
 */
const gchar *
ai_resource_get_name(AiResource *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);

    return self->name;
}

/**
 * ai_resource_get_description:
 * @self: an #AiResource
 *
 * The declared `description:`, or the first line of prose in the body
 * when there is none.
 *
 * Returns: (transfer none) (nullable): one line, or %NULL
 */
const gchar *
ai_resource_get_description(AiResource *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);

    return self->description;
}

/**
 * ai_resource_get_body:
 * @self: an #AiResource
 *
 * Returns: (transfer none): everything after the frontmatter, never %NULL
 */
const gchar *
ai_resource_get_body(AiResource *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);

    return self->body != NULL ? self->body : "";
}

/**
 * ai_resource_get_path:
 * @self: an #AiResource
 *
 * Returns: (transfer none) (nullable): where it was read from
 */
const gchar *
ai_resource_get_path(AiResource *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);

    return self->path;
}

/**
 * ai_resource_get_origin:
 * @self: an #AiResource
 *
 * Returns: (transfer none): "claude", "opencode", "grok" or "ai-glib"
 */
const gchar *
ai_resource_get_origin(AiResource *self)
{
    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);

    return self->origin;
}

/**
 * ai_resource_get_meta:
 * @self: an #AiResource
 * @key: a frontmatter key
 *
 * Any frontmatter key, verbatim.
 *
 * This is generic on purpose. Harnesses invent fields --- `color`,
 * `memory`, `argument-hint`, `allowed-tools` --- and a typed getter per
 * field would mean editing ai-glib every time one of them does. A
 * sequence is returned joined with ", "; use
 * ai_resource_get_meta_list() when the shape matters.
 *
 * Returns: (transfer none) (nullable): the value, or %NULL
 */
const gchar *
ai_resource_get_meta(
    AiResource  *self,
    const gchar *key
){
    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);
    g_return_val_if_fail(key != NULL, NULL);

    return g_hash_table_lookup(self->meta, key);
}

/**
 * ai_resource_get_meta_list:
 * @self: an #AiResource
 * @key: a frontmatter key
 *
 * A frontmatter key as a list.
 *
 * A YAML sequence is returned as written. A scalar is split on commas
 * and trimmed, because `tools:` appears in both forms in the wild ---
 * the agent files under `~/.claude/agents` write a comma-separated
 * string and opencode writes a sequence, and a caller should not have to
 * care which.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the
 *   values, or %NULL if @key is absent
 */
gchar **
ai_resource_get_meta_list(
    AiResource  *self,
    const gchar *key
){
    const gchar *scalar;
    gchar      **listed;
    gchar      **split;
    guint        i;

    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);
    g_return_val_if_fail(key != NULL, NULL);

    listed = g_hash_table_lookup(self->meta_lists, key);

    if (listed != NULL)
    {
        return g_strdupv(listed);
    }

    scalar = g_hash_table_lookup(self->meta, key);

    if (scalar == NULL)
    {
        return NULL;
    }

    split = g_strsplit(scalar, ",", -1);

    for (i = 0; split[i] != NULL; i++)
    {
        gchar *trimmed = g_strdup(g_strstrip(split[i]));

        g_free(split[i]);
        split[i] = trimmed;
    }

    return split;
}

/**
 * ai_resource_get_meta_boolean:
 * @self: an #AiResource
 * @key: a frontmatter key
 * @fallback: what to return when @key is absent or unparseable
 *
 * A frontmatter key read as a flag.
 *
 * "true", "yes", "on" and "1" are true; "false", "no", "off" and "0" are
 * false; anything else is @fallback. YAML itself is looser than this,
 * but a key that decides whether a file may run shell commands is worth
 * reading strictly.
 *
 * Returns: the value, or @fallback
 */
gboolean
ai_resource_get_meta_boolean(
    AiResource  *self,
    const gchar *key,
    gboolean     fallback
){
    const gchar *value;

    g_return_val_if_fail(AI_IS_RESOURCE(self), fallback);
    g_return_val_if_fail(key != NULL, fallback);

    value = g_hash_table_lookup(self->meta, key);

    if (value == NULL)
    {
        return fallback;
    }

    if (g_ascii_strcasecmp(value, "true") == 0 ||
        g_ascii_strcasecmp(value, "yes") == 0 ||
        g_ascii_strcasecmp(value, "on") == 0 ||
        g_strcmp0(value, "1") == 0)
    {
        return TRUE;
    }

    if (g_ascii_strcasecmp(value, "false") == 0 ||
        g_ascii_strcasecmp(value, "no") == 0 ||
        g_ascii_strcasecmp(value, "off") == 0 ||
        g_strcmp0(value, "0") == 0)
    {
        return FALSE;
    }

    return fallback;
}

/**
 * ai_resource_get_meta_keys:
 * @self: an #AiResource
 *
 * Every frontmatter key that was understood.
 *
 * Returns: (transfer full) (array zero-terminated=1): the keys, sorted
 */
gchar **
ai_resource_get_meta_keys(AiResource *self)
{
    g_autoptr(GPtrArray) keys = NULL;
    GHashTableIter       iter;
    gpointer             key;

    g_return_val_if_fail(AI_IS_RESOURCE(self), NULL);

    keys = g_ptr_array_new_with_free_func(g_free);

    g_hash_table_iter_init(&iter, self->meta);

    while (g_hash_table_iter_next(&iter, &key, NULL))
    {
        g_ptr_array_add(keys, g_strdup((const gchar *)key));
    }

    g_ptr_array_sort_values(keys, (GCompareFunc)g_strcmp0);
    g_ptr_array_add(keys, NULL);

    return (gchar **)g_ptr_array_free(g_steal_pointer(&keys), FALSE);
}

/**
 * ai_resource_kind_to_string:
 * @kind: an #AiResourceKind
 *
 * A stable lowercase name, for listings and for bindings.
 *
 * Returns: (transfer none): the name, never %NULL
 */
const gchar *
ai_resource_kind_to_string(AiResourceKind kind)
{
    switch (kind)
    {
        case AI_RESOURCE_COMMAND:
            return "command";

        case AI_RESOURCE_SKILL:
            return "skill";

        case AI_RESOURCE_AGENT:
            return "agent";

        default:
            return "command";
    }
}

/**
 * ai_resource_scope_to_string:
 * @scope: an #AiResourceScope
 *
 * Returns: (transfer none): the name, never %NULL
 */
const gchar *
ai_resource_scope_to_string(AiResourceScope scope)
{
    switch (scope)
    {
        case AI_RESOURCE_SCOPE_BUILTIN:
            return "builtin";

        case AI_RESOURCE_SCOPE_USER:
            return "user";

        case AI_RESOURCE_SCOPE_PROJECT:
            return "project";

        default:
            return "builtin";
    }
}
