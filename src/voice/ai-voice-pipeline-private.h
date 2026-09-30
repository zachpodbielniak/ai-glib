/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#ifndef AI_GLIB_COMPILATION
#error "Private header"
#endif
#include <gst/gst.h>

/*
 * A transport starts its pipeline on a worker thread and stops it on another,
 * so nothing orders a leave's stop against the join's start. When the stop
 * wins, the start would set a pipeline nobody holds any more to PLAYING, and
 * its streaming threads would call into a transport that has since been freed.
 * The gate makes a stop final: a start after it does nothing.
 */
typedef struct {
	GMutex lock;
	gboolean stopped;
} AiPipelineGate;

static inline void
ai_pipeline_gate_free(gpointer data)
{
	AiPipelineGate *gate = data;
	g_mutex_clear(&gate->lock);
	g_free(gate);
}
/* Before the pipeline is handed to any thread. */
static inline void
ai_pipeline_gate_attach(GstElement *pipeline)
{
	AiPipelineGate *gate = g_new0(AiPipelineGate, 1);
	g_mutex_init(&gate->lock);
	g_object_set_data_full(G_OBJECT(pipeline), "ai-pipeline-gate", gate, ai_pipeline_gate_free);
}
/* FALSE, and nothing started, when the pipeline has already been stopped. */
static inline gboolean
ai_pipeline_gate_play(GstElement *pipeline, GstStateChangeReturn *result)
{
	AiPipelineGate *gate = g_object_get_data(G_OBJECT(pipeline), "ai-pipeline-gate");
	gboolean started = FALSE;
	if (gate == NULL) {
		*result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
		return TRUE;
	}
	g_mutex_lock(&gate->lock);
	if (!gate->stopped) {
		*result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
		started = TRUE;
	}
	g_mutex_unlock(&gate->lock);
	return started;
}
static inline void
ai_pipeline_gate_stop(GstElement *pipeline)
{
	AiPipelineGate *gate = g_object_get_data(G_OBJECT(pipeline), "ai-pipeline-gate");
	if (gate != NULL) {
		g_mutex_lock(&gate->lock);
		gate->stopped = TRUE;
	}
	gst_element_set_state(pipeline, GST_STATE_NULL);
	if (gate != NULL)
		g_mutex_unlock(&gate->lock);
}
