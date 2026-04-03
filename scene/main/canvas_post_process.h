/**************************************************************************/
/*  canvas_post_process.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "scene/gui/control.h"
#include "scene/resources/shader.h"
#include "scene/resources/material.h"

class CanvasPostProcess : public Control {
	GDCLASS(CanvasPostProcess, Control);

	// --- Internal shader/material ---
	Ref<Shader> post_process_shader;
	Ref<ShaderMaterial> post_process_material;

	void _rebuild_shader();
	void _update_shader_params();

	// --- 1. Pixelation (modifies UV, must be first) ---
	bool pixelation_enabled = false;
	int pixel_size = 4;

	// --- 2. Chromatic Aberration (samples with UV offset) ---
	bool chromatic_aberration_enabled = false;
	float chromatic_aberration_strength = 1.0;

	// --- 3. Gaussian Blur ---
	bool blur_enabled = false;
	float blur_strength = 2.0;
	int blur_iterations = 1;

	// --- 4. Radial Blur ---
	bool radial_blur_enabled = false;
	float radial_blur_strength = 0.02;
	int radial_blur_samples = 16;
	Vector2 radial_blur_center = Vector2(0.5, 0.5);

	// --- 5. Color Adjustment ---
	bool adjustment_enabled = false;
	float brightness = 1.0;
	float contrast = 1.0;
	float saturation = 1.0;

	// --- 6. Scanlines ---
	bool scanlines_enabled = false;
	float scanlines_density = 1.0;
	float scanlines_opacity = 0.3;
	float scanlines_speed = 0.0;

	// --- 7. Film Grain ---
	bool grain_enabled = false;
	float grain_intensity = 0.1;
	float grain_speed = 15.0;

	// --- 8. Vignette (final overlay) ---
	bool vignette_enabled = false;
	float vignette_intensity = 0.4;
	float vignette_softness = 2.0;
	Color vignette_color = Color(0, 0, 0, 1);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// --- 1. Pixelation ---
	void set_pixelation_enabled(bool p_enabled);
	bool get_pixelation_enabled() const;
	void set_pixel_size(int p_size);
	int get_pixel_size() const;

	// --- 2. Chromatic Aberration ---
	void set_chromatic_aberration_enabled(bool p_enabled);
	bool get_chromatic_aberration_enabled() const;
	void set_chromatic_aberration_strength(float p_strength);
	float get_chromatic_aberration_strength() const;

	// --- 3. Gaussian Blur ---
	void set_blur_enabled(bool p_enabled);
	bool get_blur_enabled() const;
	void set_blur_strength(float p_strength);
	float get_blur_strength() const;
	void set_blur_iterations(int p_iterations);
	int get_blur_iterations() const;

	// --- 4. Radial Blur ---
	void set_radial_blur_enabled(bool p_enabled);
	bool get_radial_blur_enabled() const;
	void set_radial_blur_strength(float p_strength);
	float get_radial_blur_strength() const;
	void set_radial_blur_samples(int p_samples);
	int get_radial_blur_samples() const;
	void set_radial_blur_center(const Vector2 &p_center);
	Vector2 get_radial_blur_center() const;

	// --- 5. Color Adjustment ---
	void set_adjustment_enabled(bool p_enabled);
	bool get_adjustment_enabled() const;
	void set_brightness(float p_brightness);
	float get_brightness() const;
	void set_contrast(float p_contrast);
	float get_contrast() const;
	void set_saturation(float p_saturation);
	float get_saturation() const;

	// --- 6. Scanlines ---
	void set_scanlines_enabled(bool p_enabled);
	bool get_scanlines_enabled() const;
	void set_scanlines_density(float p_density);
	float get_scanlines_density() const;
	void set_scanlines_opacity(float p_opacity);
	float get_scanlines_opacity() const;
	void set_scanlines_speed(float p_speed);
	float get_scanlines_speed() const;

	// --- 7. Film Grain ---
	void set_grain_enabled(bool p_enabled);
	bool get_grain_enabled() const;
	void set_grain_intensity(float p_intensity);
	float get_grain_intensity() const;
	void set_grain_speed(float p_speed);
	float get_grain_speed() const;

	// --- 8. Vignette ---
	void set_vignette_enabled(bool p_enabled);
	bool get_vignette_enabled() const;
	void set_vignette_intensity(float p_intensity);
	float get_vignette_intensity() const;
	void set_vignette_softness(float p_softness);
	float get_vignette_softness() const;
	void set_vignette_color(const Color &p_color);
	Color get_vignette_color() const;

	PackedStringArray get_configuration_warnings() const override;

	CanvasPostProcess();
	~CanvasPostProcess();
};