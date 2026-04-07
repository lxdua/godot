
/**************************************************************************/
/*  enhanced_input_modifier.cpp                                           */
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

#include "enhanced_input_modifier.h"

#include "scene/resources/curve.h"

// ============================================================================
// InputModifier — Base class
// ============================================================================

void InputModifier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("modify", "value", "delta"), &InputModifier::modify);

	GDVIRTUAL_BIND(_modify, "value", "delta");
}

Vector3 InputModifier::modify(const Vector3 &p_value, double p_delta) {
	Vector3 result = p_value;
	GDVIRTUAL_CALL(_modify, p_value, p_delta, result);
	return result;
}

// ============================================================================
// InputModifierDeadZone
// ============================================================================

static float _apply_dead_zone_1d(float p_value, float p_lower, float p_upper) {
	float abs_val = Math::abs(p_value);
	if (abs_val < p_lower) {
		return 0.0f;
	}
	if (abs_val > p_upper) {
		return SIGN(p_value);
	}
	// Remap [lower, upper] → [0, 1], preserve sign.
	float remapped = (abs_val - p_lower) / (p_upper - p_lower);
	return SIGN(p_value) * remapped;
}

void InputModifierDeadZone::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_lower_threshold", "threshold"), &InputModifierDeadZone::set_lower_threshold);
	ClassDB::bind_method(D_METHOD("get_lower_threshold"), &InputModifierDeadZone::get_lower_threshold);

	ClassDB::bind_method(D_METHOD("set_upper_threshold", "threshold"), &InputModifierDeadZone::set_upper_threshold);
	ClassDB::bind_method(D_METHOD("get_upper_threshold"), &InputModifierDeadZone::get_upper_threshold);

	ClassDB::bind_method(D_METHOD("set_dead_zone_type", "type"), &InputModifierDeadZone::set_dead_zone_type);
	ClassDB::bind_method(D_METHOD("get_dead_zone_type"), &InputModifierDeadZone::get_dead_zone_type);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lower_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_lower_threshold", "get_lower_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upper_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_upper_threshold", "get_upper_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "dead_zone_type", PROPERTY_HINT_ENUM, "Axial,Radial"), "set_dead_zone_type", "get_dead_zone_type");

	BIND_ENUM_CONSTANT(AXIAL);
	BIND_ENUM_CONSTANT(RADIAL);
}

Vector3 InputModifierDeadZone::modify(const Vector3 &p_value, double p_delta) {
	if (type == RADIAL) {
		float len = p_value.length();
		if (len < lower_threshold) {
			return Vector3();
		}
		if (len > upper_threshold) {
			return p_value.normalized();
		}
		float remapped = (len - lower_threshold) / (upper_threshold - lower_threshold);
		return p_value.normalized() * remapped;
	} else {
		// AXIAL: each axis independently.
		return Vector3(
				_apply_dead_zone_1d(p_value.x, lower_threshold, upper_threshold),
				_apply_dead_zone_1d(p_value.y, lower_threshold, upper_threshold),
				_apply_dead_zone_1d(p_value.z, lower_threshold, upper_threshold));
	}
}

void InputModifierDeadZone::set_lower_threshold(float p_threshold) {
	lower_threshold = CLAMP(p_threshold, 0.0f, 1.0f);
}

float InputModifierDeadZone::get_lower_threshold() const {
	return lower_threshold;
}

void InputModifierDeadZone::set_upper_threshold(float p_threshold) {
	upper_threshold = CLAMP(p_threshold, 0.0f, 1.0f);
}

float InputModifierDeadZone::get_upper_threshold() const {
	return upper_threshold;
}

void InputModifierDeadZone::set_dead_zone_type(DeadZoneType p_type) {
	type = p_type;
}

InputModifierDeadZone::DeadZoneType InputModifierDeadZone::get_dead_zone_type() const {
	return type;
}

// ============================================================================
// InputModifierScalar
// ============================================================================

void InputModifierScalar::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_scale", "scale"), &InputModifierScalar::set_scale);
	ClassDB::bind_method(D_METHOD("get_scale"), &InputModifierScalar::get_scale);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "scale"), "set_scale", "get_scale");
}

Vector3 InputModifierScalar::modify(const Vector3 &p_value, double p_delta) {
	return Vector3(p_value.x * scale.x, p_value.y * scale.y, p_value.z * scale.z);
}

void InputModifierScalar::set_scale(const Vector3 &p_scale) {
	scale = p_scale;
}

Vector3 InputModifierScalar::get_scale() const {
	return scale;
}

// ============================================================================
// InputModifierNegate
// ============================================================================

void InputModifierNegate::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_negate_x", "negate"), &InputModifierNegate::set_negate_x);
	ClassDB::bind_method(D_METHOD("get_negate_x"), &InputModifierNegate::get_negate_x);

	ClassDB::bind_method(D_METHOD("set_negate_y", "negate"), &InputModifierNegate::set_negate_y);
	ClassDB::bind_method(D_METHOD("get_negate_y"), &InputModifierNegate::get_negate_y);

	ClassDB::bind_method(D_METHOD("set_negate_z", "negate"), &InputModifierNegate::set_negate_z);
	ClassDB::bind_method(D_METHOD("get_negate_z"), &InputModifierNegate::get_negate_z);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "negate_x"), "set_negate_x", "get_negate_x");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "negate_y"), "set_negate_y", "get_negate_y");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "negate_z"), "set_negate_z", "get_negate_z");
}

Vector3 InputModifierNegate::modify(const Vector3 &p_value, double p_delta) {
	return Vector3(
			negate_x ? -p_value.x : p_value.x,
			negate_y ? -p_value.y : p_value.y,
			negate_z ? -p_value.z : p_value.z);
}

void InputModifierNegate::set_negate_x(bool p_negate) { negate_x = p_negate; }
bool InputModifierNegate::get_negate_x() const { return negate_x; }
void InputModifierNegate::set_negate_y(bool p_negate) { negate_y = p_negate; }
bool InputModifierNegate::get_negate_y() const { return negate_y; }
void InputModifierNegate::set_negate_z(bool p_negate) { negate_z = p_negate; }
bool InputModifierNegate::get_negate_z() const { return negate_z; }

// ============================================================================
// InputModifierSmooth
// ============================================================================

void InputModifierSmooth::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_speed", "speed"), &InputModifierSmooth::set_speed);
	ClassDB::bind_method(D_METHOD("get_speed"), &InputModifierSmooth::get_speed);
	ClassDB::bind_method(D_METHOD("reset"), &InputModifierSmooth::reset);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speed", PROPERTY_HINT_RANGE, "0.1,100.0,0.1"), "set_speed", "get_speed");
}

Vector3 InputModifierSmooth::modify(const Vector3 &p_value, double p_delta) {
	float t = CLAMP(float(speed * p_delta), 0.0f, 1.0f);
	current_value = current_value.lerp(p_value, t);
	return current_value;
}

void InputModifierSmooth::set_speed(float p_speed) {
	speed = MAX(p_speed, 0.01f);
}

float InputModifierSmooth::get_speed() const {
	return speed;
}

void InputModifierSmooth::reset() {
	current_value = Vector3();
}

// ============================================================================
// InputModifierResponseCurve
// ============================================================================

static float _sample_curve(const Ref<Curve> &p_curve, float p_value) {
	if (p_curve.is_null()) {
		return p_value;
	}
	float abs_val = Math::abs(p_value);
	float sampled = p_curve->sample(abs_val);
	return SIGN(p_value) * sampled;
}

void InputModifierResponseCurve::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_curve_x", "curve"), &InputModifierResponseCurve::set_curve_x);
	ClassDB::bind_method(D_METHOD("get_curve_x"), &InputModifierResponseCurve::get_curve_x);

	ClassDB::bind_method(D_METHOD("set_curve_y", "curve"), &InputModifierResponseCurve::set_curve_y);
	ClassDB::bind_method(D_METHOD("get_curve_y"), &InputModifierResponseCurve::get_curve_y);

	ClassDB::bind_method(D_METHOD("set_curve_z", "curve"), &InputModifierResponseCurve::set_curve_z);
	ClassDB::bind_method(D_METHOD("get_curve_z"), &InputModifierResponseCurve::get_curve_z);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "curve_x", PROPERTY_HINT_RESOURCE_TYPE, "Curve"), "set_curve_x", "get_curve_x");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "curve_y", PROPERTY_HINT_RESOURCE_TYPE, "Curve"), "set_curve_y", "get_curve_y");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "curve_z", PROPERTY_HINT_RESOURCE_TYPE, "Curve"), "set_curve_z", "get_curve_z");
}

Vector3 InputModifierResponseCurve::modify(const Vector3 &p_value, double p_delta) {
	return Vector3(
			_sample_curve(curve_x, p_value.x),
			_sample_curve(curve_y.is_valid() ? curve_y : curve_x, p_value.y),
			_sample_curve(curve_z.is_valid() ? curve_z : curve_x, p_value.z));
}

void InputModifierResponseCurve::set_curve_x(const Ref<Curve> &p_curve) { curve_x = p_curve; }
Ref<Curve> InputModifierResponseCurve::get_curve_x() const { return curve_x; }
void InputModifierResponseCurve::set_curve_y(const Ref<Curve> &p_curve) { curve_y = p_curve; }
Ref<Curve> InputModifierResponseCurve::get_curve_y() const { return curve_y; }
void InputModifierResponseCurve::set_curve_z(const Ref<Curve> &p_curve) { curve_z = p_curve; }
Ref<Curve> InputModifierResponseCurve::get_curve_z() const { return curve_z; }

// ============================================================================
// InputModifierSwizzle
// ============================================================================

void InputModifierSwizzle::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_order", "order"), &InputModifierSwizzle::set_order);
	ClassDB::bind_method(D_METHOD("get_order"), &InputModifierSwizzle::get_order);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "order", PROPERTY_HINT_ENUM, "XYZ,YXZ,ZYX,XZY,YZX,ZXY"), "set_order", "get_order");

	BIND_ENUM_CONSTANT(SWIZZLE_XYZ);
	BIND_ENUM_CONSTANT(SWIZZLE_YXZ);
	BIND_ENUM_CONSTANT(SWIZZLE_ZYX);
	BIND_ENUM_CONSTANT(SWIZZLE_XZY);
	BIND_ENUM_CONSTANT(SWIZZLE_YZX);
	BIND_ENUM_CONSTANT(SWIZZLE_ZXY);
}

Vector3 InputModifierSwizzle::modify(const Vector3 &p_value, double p_delta) {
	switch (order) {
		case SWIZZLE_XYZ:
			return p_value;
		case SWIZZLE_YXZ:
			return Vector3(p_value.y, p_value.x, p_value.z);
		case SWIZZLE_ZYX:
			return Vector3(p_value.z, p_value.y, p_value.x);
		case SWIZZLE_XZY:
			return Vector3(p_value.x, p_value.z, p_value.y);
		case SWIZZLE_YZX:
			return Vector3(p_value.y, p_value.z, p_value.x);
		case SWIZZLE_ZXY:
			return Vector3(p_value.z, p_value.x, p_value.y);
		default:
			return p_value;
	}
}

void InputModifierSwizzle::set_order(SwizzleOrder p_order) {
	order = p_order;
}

InputModifierSwizzle::SwizzleOrder InputModifierSwizzle::get_order() const {
	return order;
}

// ============================================================================
// InputModifierNormalize
// ============================================================================

void InputModifierNormalize::_bind_methods() {
	// No properties.
}

Vector3 InputModifierNormalize::modify(const Vector3 &p_value, double p_delta) {
	float len = p_value.length();
	if (len > 0.0f) {
		return p_value / len;
	}
	return Vector3();
}

// ============================================================================
// InputModifierClamp
// ============================================================================

void InputModifierClamp::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_min_value", "min"), &InputModifierClamp::set_min_value);
	ClassDB::bind_method(D_METHOD("get_min_value"), &InputModifierClamp::get_min_value);

	ClassDB::bind_method(D_METHOD("set_max_value", "max"), &InputModifierClamp::set_max_value);
	ClassDB::bind_method(D_METHOD("get_max_value"), &InputModifierClamp::get_max_value);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "min_value"), "set_min_value", "get_min_value");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "max_value"), "set_max_value", "get_max_value");
}

Vector3 InputModifierClamp::modify(const Vector3 &p_value, double p_delta) {
	return p_value.clamp(min_value, max_value);
}

void InputModifierClamp::set_min_value(const Vector3 &p_min) {
	min_value = p_min;
}

Vector3 InputModifierClamp::get_min_value() const {
	return min_value;
}

void InputModifierClamp::set_max_value(const Vector3 &p_max) {
	max_value = p_max;
}

Vector3 InputModifierClamp::get_max_value() const {
	return max_value;
}
