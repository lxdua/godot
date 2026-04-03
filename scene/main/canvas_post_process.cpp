/**************************************************************************/
/*  canvas_post_process.cpp                                               */
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

#include "canvas_post_process.h"

#include "core/object/class_db.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

void CanvasPostProcess::_rebuild_shader() {
	if (post_process_shader.is_null()) {
		post_process_shader.instantiate();
		post_process_material.instantiate();
		post_process_material->set_shader(post_process_shader);
	}

	String code = R"(shader_type canvas_item;

uniform sampler2D screen_texture : hint_screen_texture, filter_linear_mipmap;
uniform sampler2D screen_texture_nearest : hint_screen_texture, filter_nearest;

// Vignette
uniform float vignette_intensity : hint_range(0.0, 1.0) = 0.4;
uniform float vignette_softness : hint_range(0.0, 10.0) = 2.0;
uniform vec4 vignette_color : source_color = vec4(0.0, 0.0, 0.0, 1.0);
uniform bool vignette_enabled = false;

// Chromatic Aberration
uniform float ca_strength : hint_range(0.0, 20.0) = 1.0;
uniform bool ca_enabled = false;

// Color Adjustment
uniform float adj_brightness : hint_range(0.0, 3.0) = 1.0;
uniform float adj_contrast : hint_range(0.0, 3.0) = 1.0;
uniform float adj_saturation : hint_range(0.0, 3.0) = 1.0;
uniform bool adj_enabled = false;

// Pixelation
uniform int pixelation_size : hint_range(1, 64) = 4;
uniform bool pixelation_enabled = false;

// Gaussian Blur
uniform float blur_strength : hint_range(0.0, 10.0) = 2.0;
uniform int blur_iterations : hint_range(1, 5) = 1;
uniform bool blur_enabled = false;

// Scanlines
uniform float scanlines_density : hint_range(0.1, 10.0) = 1.0;
uniform float scanlines_opacity : hint_range(0.0, 1.0) = 0.3;
uniform float scanlines_speed : hint_range(0.0, 20.0) = 0.0;
uniform bool scanlines_enabled = false;

// Film Grain
uniform float grain_intensity : hint_range(0.0, 1.0) = 0.1;
uniform float grain_speed : hint_range(0.0, 60.0) = 15.0;
uniform bool grain_enabled = false;

// Radial Blur
uniform float radial_blur_strength : hint_range(0.0, 0.2) = 0.02;
uniform int radial_blur_samples : hint_range(4, 64) = 16;
uniform vec2 radial_blur_center = vec2(0.5, 0.5);
uniform bool radial_blur_enabled = false;

// Hash function for noise-based effects
float hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

void fragment() {
	vec2 uv = SCREEN_UV;
	vec2 pixel_size = SCREEN_PIXEL_SIZE;

	// Pixelation
	if (pixelation_enabled) {
		vec2 screen_res = 1.0 / pixel_size;
		vec2 pixelated_res = floor(screen_res / float(pixelation_size));
		uv = (floor(uv * pixelated_res) + 0.5) / pixelated_res;
	}

	// Chromatic Aberration + initial color sampling
	vec4 color;
	if (ca_enabled) {
		vec2 ca_offset = (uv - 0.5) * ca_strength * pixel_size;
		float r, g, b, a;
		if (pixelation_enabled) {
			r = texture(screen_texture_nearest, uv + ca_offset).r;
			g = texture(screen_texture_nearest, uv).g;
			b = texture(screen_texture_nearest, uv - ca_offset).b;
			a = texture(screen_texture_nearest, uv).a;
		} else {
			r = texture(screen_texture, uv + ca_offset).r;
			g = texture(screen_texture, uv).g;
			b = texture(screen_texture, uv - ca_offset).b;
			a = texture(screen_texture, uv).a;
		}
		color = vec4(r, g, b, a);
	} else if (pixelation_enabled) {
		color = texture(screen_texture_nearest, uv);
	} else {
		color = texture(screen_texture, uv);
	}

	// Gaussian Blur (2D kernel, proper Gaussian weights)
	if (blur_enabled) {
		for (int iter = 0; iter < blur_iterations; iter++) {
			float spread = blur_strength * float(iter + 1);
			vec4 blur_sum = vec4(0.0);
			float weight_sum = 0.0;
			const int RADIUS = 5;
			float sigma = spread * 0.5 + 0.001;
			for (int x = -RADIUS; x <= RADIUS; x++) {
				for (int y = -RADIUS; y <= RADIUS; y++) {
					float d = float(x * x + y * y);
					float w = exp(-d / (2.0 * sigma * sigma));
					vec2 off = vec2(float(x), float(y)) * pixel_size * spread;
					blur_sum += texture(screen_texture, uv + off) * w;
					weight_sum += w;
				}
			}
			color = blur_sum / weight_sum;
		}
	}

	// Radial Blur
	if (radial_blur_enabled) {
		vec2 dir = uv - radial_blur_center;
		vec4 radial_sum = vec4(0.0);
		for (int i = 0; i < radial_blur_samples; i++) {
			float t = float(i) / float(radial_blur_samples);
			vec2 sample_uv = uv - dir * t * radial_blur_strength;
			radial_sum += texture(screen_texture, sample_uv);
		}
		color = radial_sum / float(radial_blur_samples);
	}

	// Color Adjustment
	if (adj_enabled) {
		color.rgb *= adj_brightness;
		color.rgb = (color.rgb - 0.5) * adj_contrast + 0.5;
		float lum = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
		color.rgb = mix(vec3(lum), color.rgb, adj_saturation);
	}

	// Scanlines
	if (scanlines_enabled) {
		float y_pos = SCREEN_UV.y / pixel_size.y;
		float scroll = TIME * scanlines_speed * 100.0;
		float line = sin((y_pos + scroll) * 3.14159 * scanlines_density) * 0.5 + 0.5;
		color.rgb *= 1.0 - scanlines_opacity * (1.0 - line);
	}

	// Film Grain
	if (grain_enabled) {
		float t = floor(TIME * grain_speed);
		float noise = hash(uv * 1000.0 + vec2(t)) * 2.0 - 1.0;
		color.rgb += vec3(noise * grain_intensity);
	}

	// Vignette
	if (vignette_enabled) {
		vec2 vig_uv = SCREEN_UV;
		float dist = distance(vig_uv, vec2(0.5));
		float vig = smoothstep(0.5 - vignette_intensity * 0.5, 0.5 + vignette_softness * 0.1, dist);
		color.rgb = mix(color.rgb, vignette_color.rgb, vig * vignette_color.a);
	}

	COLOR = color;
}
)";

	post_process_shader->set_code(code);
}

void CanvasPostProcess::_update_shader_params() {
	if (post_process_material.is_null()) {
		return;
	}

	// 1. Pixelation
	post_process_material->set_shader_parameter("pixelation_enabled", pixelation_enabled);
	post_process_material->set_shader_parameter("pixelation_size", pixel_size);

	// 2. Chromatic Aberration
	post_process_material->set_shader_parameter("ca_enabled", chromatic_aberration_enabled);
	post_process_material->set_shader_parameter("ca_strength", chromatic_aberration_strength);

	// 3. Gaussian Blur
	post_process_material->set_shader_parameter("blur_enabled", blur_enabled);
	post_process_material->set_shader_parameter("blur_strength", blur_strength);
	post_process_material->set_shader_parameter("blur_iterations", blur_iterations);

	// 4. Radial Blur
	post_process_material->set_shader_parameter("radial_blur_enabled", radial_blur_enabled);
	post_process_material->set_shader_parameter("radial_blur_strength", radial_blur_strength);
	post_process_material->set_shader_parameter("radial_blur_samples", radial_blur_samples);
	post_process_material->set_shader_parameter("radial_blur_center", radial_blur_center);

	// 5. Color Adjustment
	post_process_material->set_shader_parameter("adj_enabled", adjustment_enabled);
	post_process_material->set_shader_parameter("adj_brightness", brightness);
	post_process_material->set_shader_parameter("adj_contrast", contrast);
	post_process_material->set_shader_parameter("adj_saturation", saturation);

	// 6. Scanlines
	post_process_material->set_shader_parameter("scanlines_enabled", scanlines_enabled);
	post_process_material->set_shader_parameter("scanlines_density", scanlines_density);
	post_process_material->set_shader_parameter("scanlines_opacity", scanlines_opacity);
	post_process_material->set_shader_parameter("scanlines_speed", scanlines_speed);

	// 7. Film Grain
	post_process_material->set_shader_parameter("grain_enabled", grain_enabled);
	post_process_material->set_shader_parameter("grain_intensity", grain_intensity);
	post_process_material->set_shader_parameter("grain_speed", grain_speed);

	// 8. Vignette
	post_process_material->set_shader_parameter("vignette_enabled", vignette_enabled);
	post_process_material->set_shader_parameter("vignette_intensity", vignette_intensity);
	post_process_material->set_shader_parameter("vignette_softness", vignette_softness);
	post_process_material->set_shader_parameter("vignette_color", vignette_color);
}

void CanvasPostProcess::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_anchors_preset(PRESET_FULL_RECT);
			set_mouse_filter(MOUSE_FILTER_IGNORE);
			RS::get_singleton()->canvas_item_set_copy_to_backbuffer(get_canvas_item(), true, Rect2());
			_rebuild_shader();
			set_material(post_process_material);
			_update_shader_params();
		} break;

		case NOTIFICATION_DRAW: {
			draw_rect(Rect2(Point2(), get_size()), Color(1, 1, 1, 1));
		} break;

		case NOTIFICATION_RESIZED: {
			queue_redraw();
		} break;
	}
}

// =====================================================================
// Vignette
// =====================================================================

void CanvasPostProcess::set_vignette_enabled(bool p_enabled) {
	vignette_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_vignette_enabled() const {
	return vignette_enabled;
}

void CanvasPostProcess::set_vignette_intensity(float p_intensity) {
	vignette_intensity = p_intensity;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_vignette_intensity() const {
	return vignette_intensity;
}

void CanvasPostProcess::set_vignette_softness(float p_softness) {
	vignette_softness = p_softness;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_vignette_softness() const {
	return vignette_softness;
}

void CanvasPostProcess::set_vignette_color(const Color &p_color) {
	vignette_color = p_color;
	_update_shader_params();
	queue_redraw();
}

Color CanvasPostProcess::get_vignette_color() const {
	return vignette_color;
}

// =====================================================================
// Chromatic Aberration
// =====================================================================

void CanvasPostProcess::set_chromatic_aberration_enabled(bool p_enabled) {
	chromatic_aberration_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_chromatic_aberration_enabled() const {
	return chromatic_aberration_enabled;
}

void CanvasPostProcess::set_chromatic_aberration_strength(float p_strength) {
	chromatic_aberration_strength = p_strength;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_chromatic_aberration_strength() const {
	return chromatic_aberration_strength;
}

// =====================================================================
// Color Adjustment
// =====================================================================

void CanvasPostProcess::set_adjustment_enabled(bool p_enabled) {
	adjustment_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_adjustment_enabled() const {
	return adjustment_enabled;
}

void CanvasPostProcess::set_brightness(float p_brightness) {
	brightness = p_brightness;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_brightness() const {
	return brightness;
}

void CanvasPostProcess::set_contrast(float p_contrast) {
	contrast = p_contrast;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_contrast() const {
	return contrast;
}

void CanvasPostProcess::set_saturation(float p_saturation) {
	saturation = p_saturation;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_saturation() const {
	return saturation;
}

// =====================================================================
// Pixelation
// =====================================================================

void CanvasPostProcess::set_pixelation_enabled(bool p_enabled) {
	pixelation_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_pixelation_enabled() const {
	return pixelation_enabled;
}

void CanvasPostProcess::set_pixel_size(int p_size) {
	pixel_size = MAX(1, p_size);
	_update_shader_params();
	queue_redraw();
}

int CanvasPostProcess::get_pixel_size() const {
	return pixel_size;
}

// =====================================================================
// Gaussian Blur
// =====================================================================

void CanvasPostProcess::set_blur_enabled(bool p_enabled) {
	blur_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_blur_enabled() const {
	return blur_enabled;
}

void CanvasPostProcess::set_blur_strength(float p_strength) {
	blur_strength = p_strength;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_blur_strength() const {
	return blur_strength;
}

void CanvasPostProcess::set_blur_iterations(int p_iterations) {
	blur_iterations = CLAMP(p_iterations, 1, 5);
	_update_shader_params();
	queue_redraw();
}

int CanvasPostProcess::get_blur_iterations() const {
	return blur_iterations;
}

// =====================================================================
// Scanlines
// =====================================================================

void CanvasPostProcess::set_scanlines_enabled(bool p_enabled) {
	scanlines_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_scanlines_enabled() const {
	return scanlines_enabled;
}

void CanvasPostProcess::set_scanlines_density(float p_density) {
	scanlines_density = p_density;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_scanlines_density() const {
	return scanlines_density;
}

void CanvasPostProcess::set_scanlines_opacity(float p_opacity) {
	scanlines_opacity = p_opacity;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_scanlines_opacity() const {
	return scanlines_opacity;
}

void CanvasPostProcess::set_scanlines_speed(float p_speed) {
	scanlines_speed = p_speed;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_scanlines_speed() const {
	return scanlines_speed;
}

// =====================================================================
// Film Grain
// =====================================================================

void CanvasPostProcess::set_grain_enabled(bool p_enabled) {
	grain_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_grain_enabled() const {
	return grain_enabled;
}

void CanvasPostProcess::set_grain_intensity(float p_intensity) {
	grain_intensity = p_intensity;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_grain_intensity() const {
	return grain_intensity;
}

void CanvasPostProcess::set_grain_speed(float p_speed) {
	grain_speed = p_speed;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_grain_speed() const {
	return grain_speed;
}

// =====================================================================
// Radial Blur
// =====================================================================

void CanvasPostProcess::set_radial_blur_enabled(bool p_enabled) {
	radial_blur_enabled = p_enabled;
	_update_shader_params();
	queue_redraw();
}

bool CanvasPostProcess::get_radial_blur_enabled() const {
	return radial_blur_enabled;
}

void CanvasPostProcess::set_radial_blur_strength(float p_strength) {
	radial_blur_strength = p_strength;
	_update_shader_params();
	queue_redraw();
}

float CanvasPostProcess::get_radial_blur_strength() const {
	return radial_blur_strength;
}

void CanvasPostProcess::set_radial_blur_samples(int p_samples) {
	radial_blur_samples = CLAMP(p_samples, 4, 64);
	_update_shader_params();
	queue_redraw();
}

int CanvasPostProcess::get_radial_blur_samples() const {
	return radial_blur_samples;
}

void CanvasPostProcess::set_radial_blur_center(const Vector2 &p_center) {
	radial_blur_center = p_center;
	_update_shader_params();
	queue_redraw();
}

Vector2 CanvasPostProcess::get_radial_blur_center() const {
	return radial_blur_center;
}

// =====================================================================
// Configuration Warnings
// =====================================================================

PackedStringArray CanvasPostProcess::get_configuration_warnings() const {
	PackedStringArray warnings = Control::get_configuration_warnings();

	if (!vignette_enabled && !chromatic_aberration_enabled && !adjustment_enabled &&
			!pixelation_enabled && !blur_enabled && !scanlines_enabled &&
			!grain_enabled && !radial_blur_enabled) {
		warnings.push_back(RTR("No post-processing effects are enabled. Enable at least one effect for CanvasPostProcess to have a visible result."));
	}

	return warnings;
}

// =====================================================================
// Bind Methods
// =====================================================================

void CanvasPostProcess::_bind_methods() {
	// --- 1. Pixelation ---
	ClassDB::bind_method(D_METHOD("set_pixelation_enabled", "enabled"), &CanvasPostProcess::set_pixelation_enabled);
	ClassDB::bind_method(D_METHOD("get_pixelation_enabled"), &CanvasPostProcess::get_pixelation_enabled);
	ClassDB::bind_method(D_METHOD("set_pixel_size", "size"), &CanvasPostProcess::set_pixel_size);
	ClassDB::bind_method(D_METHOD("get_pixel_size"), &CanvasPostProcess::get_pixel_size);

	ADD_GROUP("1. Pixelation", "pixelation_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pixelation_enabled"), "set_pixelation_enabled", "get_pixelation_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pixelation_size", PROPERTY_HINT_RANGE, "1,64,1"), "set_pixel_size", "get_pixel_size");

	// --- 2. Chromatic Aberration ---
	ClassDB::bind_method(D_METHOD("set_chromatic_aberration_enabled", "enabled"), &CanvasPostProcess::set_chromatic_aberration_enabled);
	ClassDB::bind_method(D_METHOD("get_chromatic_aberration_enabled"), &CanvasPostProcess::get_chromatic_aberration_enabled);
	ClassDB::bind_method(D_METHOD("set_chromatic_aberration_strength", "strength"), &CanvasPostProcess::set_chromatic_aberration_strength);
	ClassDB::bind_method(D_METHOD("get_chromatic_aberration_strength"), &CanvasPostProcess::get_chromatic_aberration_strength);

	ADD_GROUP("2. Chromatic Aberration", "chromatic_aberration_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "chromatic_aberration_enabled"), "set_chromatic_aberration_enabled", "get_chromatic_aberration_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chromatic_aberration_strength", PROPERTY_HINT_RANGE, "0.0,20.0,0.1"), "set_chromatic_aberration_strength", "get_chromatic_aberration_strength");

	// --- 3. Gaussian Blur ---
	ClassDB::bind_method(D_METHOD("set_blur_enabled", "enabled"), &CanvasPostProcess::set_blur_enabled);
	ClassDB::bind_method(D_METHOD("get_blur_enabled"), &CanvasPostProcess::get_blur_enabled);
	ClassDB::bind_method(D_METHOD("set_blur_strength", "strength"), &CanvasPostProcess::set_blur_strength);
	ClassDB::bind_method(D_METHOD("get_blur_strength"), &CanvasPostProcess::get_blur_strength);
	ClassDB::bind_method(D_METHOD("set_blur_iterations", "iterations"), &CanvasPostProcess::set_blur_iterations);
	ClassDB::bind_method(D_METHOD("get_blur_iterations"), &CanvasPostProcess::get_blur_iterations);

	ADD_GROUP("3. Gaussian Blur", "blur_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blur_enabled"), "set_blur_enabled", "get_blur_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blur_strength", PROPERTY_HINT_RANGE, "0.0,10.0,0.1"), "set_blur_strength", "get_blur_strength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blur_iterations", PROPERTY_HINT_RANGE, "1,5,1"), "set_blur_iterations", "get_blur_iterations");

	// --- 4. Radial Blur ---
	ClassDB::bind_method(D_METHOD("set_radial_blur_enabled", "enabled"), &CanvasPostProcess::set_radial_blur_enabled);
	ClassDB::bind_method(D_METHOD("get_radial_blur_enabled"), &CanvasPostProcess::get_radial_blur_enabled);
	ClassDB::bind_method(D_METHOD("set_radial_blur_strength", "strength"), &CanvasPostProcess::set_radial_blur_strength);
	ClassDB::bind_method(D_METHOD("get_radial_blur_strength"), &CanvasPostProcess::get_radial_blur_strength);
	ClassDB::bind_method(D_METHOD("set_radial_blur_samples", "samples"), &CanvasPostProcess::set_radial_blur_samples);
	ClassDB::bind_method(D_METHOD("get_radial_blur_samples"), &CanvasPostProcess::get_radial_blur_samples);
	ClassDB::bind_method(D_METHOD("set_radial_blur_center", "center"), &CanvasPostProcess::set_radial_blur_center);
	ClassDB::bind_method(D_METHOD("get_radial_blur_center"), &CanvasPostProcess::get_radial_blur_center);

	ADD_GROUP("4. Radial Blur", "radial_blur_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "radial_blur_enabled"), "set_radial_blur_enabled", "get_radial_blur_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radial_blur_strength", PROPERTY_HINT_RANGE, "0.0,0.2,0.001"), "set_radial_blur_strength", "get_radial_blur_strength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "radial_blur_samples", PROPERTY_HINT_RANGE, "4,64,1"), "set_radial_blur_samples", "get_radial_blur_samples");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "radial_blur_center"), "set_radial_blur_center", "get_radial_blur_center");

	// --- 5. Color Adjustment ---
	ClassDB::bind_method(D_METHOD("set_adjustment_enabled", "enabled"), &CanvasPostProcess::set_adjustment_enabled);
	ClassDB::bind_method(D_METHOD("get_adjustment_enabled"), &CanvasPostProcess::get_adjustment_enabled);
	ClassDB::bind_method(D_METHOD("set_brightness", "brightness"), &CanvasPostProcess::set_brightness);
	ClassDB::bind_method(D_METHOD("get_brightness"), &CanvasPostProcess::get_brightness);
	ClassDB::bind_method(D_METHOD("set_contrast", "contrast"), &CanvasPostProcess::set_contrast);
	ClassDB::bind_method(D_METHOD("get_contrast"), &CanvasPostProcess::get_contrast);
	ClassDB::bind_method(D_METHOD("set_saturation", "saturation"), &CanvasPostProcess::set_saturation);
	ClassDB::bind_method(D_METHOD("get_saturation"), &CanvasPostProcess::get_saturation);

	ADD_GROUP("5. Color Adjustment", "adjustment_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "adjustment_enabled"), "set_adjustment_enabled", "get_adjustment_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "adjustment_brightness", PROPERTY_HINT_RANGE, "0.0,3.0,0.01"), "set_brightness", "get_brightness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "adjustment_contrast", PROPERTY_HINT_RANGE, "0.0,3.0,0.01"), "set_contrast", "get_contrast");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "adjustment_saturation", PROPERTY_HINT_RANGE, "0.0,3.0,0.01"), "set_saturation", "get_saturation");

	// --- 6. Scanlines ---
	ClassDB::bind_method(D_METHOD("set_scanlines_enabled", "enabled"), &CanvasPostProcess::set_scanlines_enabled);
	ClassDB::bind_method(D_METHOD("get_scanlines_enabled"), &CanvasPostProcess::get_scanlines_enabled);
	ClassDB::bind_method(D_METHOD("set_scanlines_density", "density"), &CanvasPostProcess::set_scanlines_density);
	ClassDB::bind_method(D_METHOD("get_scanlines_density"), &CanvasPostProcess::get_scanlines_density);
	ClassDB::bind_method(D_METHOD("set_scanlines_opacity", "opacity"), &CanvasPostProcess::set_scanlines_opacity);
	ClassDB::bind_method(D_METHOD("get_scanlines_opacity"), &CanvasPostProcess::get_scanlines_opacity);
	ClassDB::bind_method(D_METHOD("set_scanlines_speed", "speed"), &CanvasPostProcess::set_scanlines_speed);
	ClassDB::bind_method(D_METHOD("get_scanlines_speed"), &CanvasPostProcess::get_scanlines_speed);

	ADD_GROUP("6. Scanlines", "scanlines_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "scanlines_enabled"), "set_scanlines_enabled", "get_scanlines_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scanlines_density", PROPERTY_HINT_RANGE, "0.1,10.0,0.1"), "set_scanlines_density", "get_scanlines_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scanlines_opacity", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_scanlines_opacity", "get_scanlines_opacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scanlines_speed", PROPERTY_HINT_RANGE, "0.0,20.0,0.1"), "set_scanlines_speed", "get_scanlines_speed");

	// --- 7. Film Grain ---
	ClassDB::bind_method(D_METHOD("set_grain_enabled", "enabled"), &CanvasPostProcess::set_grain_enabled);
	ClassDB::bind_method(D_METHOD("get_grain_enabled"), &CanvasPostProcess::get_grain_enabled);
	ClassDB::bind_method(D_METHOD("set_grain_intensity", "intensity"), &CanvasPostProcess::set_grain_intensity);
	ClassDB::bind_method(D_METHOD("get_grain_intensity"), &CanvasPostProcess::get_grain_intensity);
	ClassDB::bind_method(D_METHOD("set_grain_speed", "speed"), &CanvasPostProcess::set_grain_speed);
	ClassDB::bind_method(D_METHOD("get_grain_speed"), &CanvasPostProcess::get_grain_speed);

	ADD_GROUP("7. Film Grain", "grain_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "grain_enabled"), "set_grain_enabled", "get_grain_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grain_intensity", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_grain_intensity", "get_grain_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grain_speed", PROPERTY_HINT_RANGE, "0.0,60.0,0.1"), "set_grain_speed", "get_grain_speed");

	// --- 8. Vignette ---
	ClassDB::bind_method(D_METHOD("set_vignette_enabled", "enabled"), &CanvasPostProcess::set_vignette_enabled);
	ClassDB::bind_method(D_METHOD("get_vignette_enabled"), &CanvasPostProcess::get_vignette_enabled);
	ClassDB::bind_method(D_METHOD("set_vignette_intensity", "intensity"), &CanvasPostProcess::set_vignette_intensity);
	ClassDB::bind_method(D_METHOD("get_vignette_intensity"), &CanvasPostProcess::get_vignette_intensity);
	ClassDB::bind_method(D_METHOD("set_vignette_softness", "softness"), &CanvasPostProcess::set_vignette_softness);
	ClassDB::bind_method(D_METHOD("get_vignette_softness"), &CanvasPostProcess::get_vignette_softness);
	ClassDB::bind_method(D_METHOD("set_vignette_color", "color"), &CanvasPostProcess::set_vignette_color);
	ClassDB::bind_method(D_METHOD("get_vignette_color"), &CanvasPostProcess::get_vignette_color);

	ADD_GROUP("8. Vignette", "vignette_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "vignette_enabled"), "set_vignette_enabled", "get_vignette_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vignette_intensity", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_vignette_intensity", "get_vignette_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vignette_softness", PROPERTY_HINT_RANGE, "0.0,10.0,0.01"), "set_vignette_softness", "get_vignette_softness");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "vignette_color"), "set_vignette_color", "get_vignette_color");
}

// =====================================================================
// Constructor / Destructor
// =====================================================================

CanvasPostProcess::CanvasPostProcess() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_z_index(4096);
}

CanvasPostProcess::~CanvasPostProcess() {
}