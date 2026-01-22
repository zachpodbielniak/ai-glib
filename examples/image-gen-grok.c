/*
 * image-gen-grok.c - Image generation example using xAI Grok
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates image generation using ai-glib with xAI Grok.
 * It generates an image from a text prompt and saves it to a file.
 *
 * Note: Grok's image API does not support size, quality, or style parameters.
 *
 * Usage:
 *   export XAI_API_KEY="your-api-key"
 *   ./image-gen-grok "a cat in space"
 *   ./image-gen-grok "a futuristic city" output.png
 */

#include <stdio.h>
#include <stdlib.h>

#include "ai-glib.h"

static void
on_image_complete(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	gchar **data = user_data;
	const gchar *output_file = data[0];
	GMainLoop *loop = (GMainLoop *)data[1];
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	GList *images;
	AiGeneratedImage *image;
	const gchar *url;

	response = ai_image_generator_generate_image_finish(
		AI_IMAGE_GENERATOR(source), result, &error);

	if (error != NULL)
	{
		g_printerr("Error: %s\n", error->message);
		g_main_loop_quit(loop);
		return;
	}

	printf("Response ID: %s\n", ai_image_response_get_id(response));
	printf("Model: %s\n", ai_image_response_get_model(response));

	images = ai_image_response_get_images(response);
	if (images == NULL)
	{
		g_printerr("No images generated\n");
		g_main_loop_quit(loop);
		return;
	}

	printf("Generated %u image(s)\n\n", g_list_length(images));

	/* Get first image */
	image = (AiGeneratedImage *)images->data;

	url = ai_generated_image_get_url(image);
	if (url != NULL)
	{
		printf("Image URL: %s\n\n", url);
	}

	/* Save to file */
	if (!ai_generated_image_save_to_file(image, output_file, &error))
	{
		g_printerr("Failed to save image: %s\n", error->message);
		g_main_loop_quit(loop);
		return;
	}

	printf("Image saved to: %s\n", output_file);

	g_main_loop_quit(loop);
}

int
main(
	int   argc,
	char *argv[]
){
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	const gchar *prompt;
	const gchar *output_file;
	gpointer data[2];

	/* Get prompt from command line or use default */
	prompt = (argc > 1) ? argv[1] : "a cat wearing a space helmet floating in space";
	output_file = (argc > 2) ? argv[2] : "grok-generated.png";

	printf("Prompt: %s\n", prompt);
	printf("Output: %s\n\n", output_file);

	/* Create client (uses XAI_API_KEY from environment) */
	client = ai_grok_client_new();

	/* Create image request */
	request = ai_image_request_new(prompt);

	/* Configure the request */
	ai_image_request_set_model(request, AI_GROK_IMAGE_DEFAULT_MODEL);
	ai_image_request_set_count(request, 1);

	printf("Model: %s\n", ai_image_request_get_model(request));
	printf("Note: Grok does not support size/quality/style parameters\n\n");
	printf("Generating image...\n\n");

	/* Set up main loop for async operation */
	loop = g_main_loop_new(NULL, FALSE);

	/* Pass data to callback */
	data[0] = (gpointer)output_file;
	data[1] = loop;

	/* Generate image */
	ai_image_generator_generate_image_async(
		AI_IMAGE_GENERATOR(client),
		request,
		NULL,  /* cancellable */
		on_image_complete,
		data
	);

	/* Run until complete */
	g_main_loop_run(loop);

	return 0;
}
