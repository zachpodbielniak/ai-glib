/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <glib-object.h>
G_BEGIN_DECLS
#define AI_TYPE_ADDRESS_DETECTOR (ai_address_detector_get_type())
G_DECLARE_INTERFACE(AiAddressDetector, ai_address_detector, AI, ADDRESS_DETECTOR, GObject)
/**
 * AiAddressDetectorInterface:
 * @parent_iface: the parent interface
 * @is_addressed: whether an utterance is said to the assistant
 *
 * In room mode, decides which utterances are turns. #AiNameAddressDetector
 * listens for the assistant's name; an application can supply its own
 * (a wake word engine's verdict, a push-to-talk button) to
 * #AiVoiceSession:address-detector.
 */
struct _AiAddressDetectorInterface {
	GTypeInterface parent_iface;
	gboolean (*is_addressed)(AiAddressDetector *self, const gchar *speaker, const gchar *text);
	gpointer _reserved[8];
};
gboolean
ai_address_detector_is_addressed(AiAddressDetector *self, const gchar *speaker,
								 const gchar *text);

#define AI_TYPE_NAME_ADDRESS_DETECTOR (ai_name_address_detector_get_type())
G_DECLARE_DERIVABLE_TYPE(AiNameAddressDetector, ai_name_address_detector, AI,
						 NAME_ADDRESS_DETECTOR, GObject)
/**
 * AiNameAddressDetectorClass:
 * @parent_class: the parent class
 */
struct _AiNameAddressDetectorClass {
	GObjectClass parent_class;
	gpointer _reserved[8];
};
AiNameAddressDetector *
ai_name_address_detector_new(const gchar *names, const gchar *soundalikes);
G_END_DECLS
