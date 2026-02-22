/*
 * ai-search-provider.c - Web search provider interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include "convenience/ai-search-provider.h"
#include "core/ai-error.h"

G_DEFINE_INTERFACE(AiSearchProvider, ai_search_provider, G_TYPE_OBJECT)

static void
ai_search_provider_default_init (AiSearchProviderInterface *iface)
{
    (void)iface;
}

gchar *
ai_search_provider_search (
    AiSearchProvider  *self,
    const gchar       *query,
    GCancellable      *cancellable,
    GError           **error
){
    AiSearchProviderInterface *iface;

    g_return_val_if_fail (AI_IS_SEARCH_PROVIDER (self), NULL);
    g_return_val_if_fail (query != NULL, NULL);

    iface = AI_SEARCH_PROVIDER_GET_IFACE (self);

    if (iface->search == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "search provider has no search() implementation");
        return NULL;
    }

    return iface->search (self, query, cancellable, error);
}
