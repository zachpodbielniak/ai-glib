/*
 * image-gen-gemini.c - Image generation example using Google Gemini (Imagen)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This example demonstrates image generation using ai-glib with Google's
 * Imagen model via the Gemini API. It generates an image from a text prompt
 * and saves it to a file.
 *
 * Note: Gemini's Imagen API uses aspect ratios instead of pixel dimensions.
 * The size parameter is mapped to appropriate aspect ratios.
 *
 * Usage:
 *   export GEMINI_API_KEY="your-api-key"
 *   ./image-gen-gemini "a cat in space"
 *   ./image-gen-gemini "a futuristic city" output.png
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

	printf("MIME type: %s\n\n", ai_generated_image_get_mime_type(image));

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
	g_autoptr(AiGeminiClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	const gchar *prompt;
	const gchar *output_file;
	gpointer data[2];

	/* Get prompt from command line or use default */
	prompt = (argc > 1) ? argv[1] : "a cat wearing a space helmet floating in space";
	output_file = (argc > 2) ? argv[2] : "gemini-generated.png";

	printf("Prompt: %s\n", prompt);
	printf("Output: %s\n\n", output_file);

	/* Create client (uses GEMINI_API_KEY from environment) */
	client = ai_gemini_client_new();

	/* Create image request */
	request = ai_image_request_new(prompt);

	/* Configure the request */
	/* Use Nano Banana (native Gemini image generation) by default */
	ai_image_request_set_model(request, AI_GEMINI_IMAGE_MODEL_NANO_BANANA);
	ai_image_request_set_size(request, AI_IMAGE_SIZE_1024);  /* Maps to 1:1 aspect ratio */
	ai_image_request_set_count(request, 1);

	printf("Model: %s (Nano Banana)\n", ai_image_request_get_model(request));
	printf("Note: Nano Banana uses aspect ratios (1024 = 1:1)\n");
	printf("Alternative models: %s (Nano Banana Pro), %s (Imagen)\n\n",
	       AI_GEMINI_IMAGE_MODEL_NANO_BANANA_PRO, AI_GEMINI_IMAGE_MODEL_IMAGEN_4);
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
