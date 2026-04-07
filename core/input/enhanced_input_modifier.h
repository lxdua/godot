/**************************************************************************/
/*  enhanced_input_modifier.h                                             */
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

#include "core/io/resource.h"
#include "core/object/class_db.h"
#include "core/object/gdvirtual.gen.h"
#include "core/math/vector3.h"

class Curve;

// ============================================================================
// InputModifier — Base class
// ============================================================================

class InputModifier : public Resource {
	GDCLASS(InputModifier, Resource);

protected:
	static void _bind_methods();
	GDVIRTUAL2R(Vector3, _modify, Vector3, double);

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta);
};

// ============================================================================
// InputModifierDeadZone — Dead zone filtering
// ============================================================================

class InputModifierDeadZone : public InputModifier {
	GDCLASS(InputModifierDeadZone, InputModifier);

public:
	enum DeadZoneType {
		AXIAL,
		RADIAL,
	};

private:
	float lower_threshold = 0.2f;
	float upper_threshold = 1.0f;
	DeadZoneType type = RADIAL;

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;

	void set_lower_threshold(float p_threshold);
	float get_lower_threshold() const;

	void set_upper_threshold(float p_threshold);
	float get_upper_threshold() const;

	void set_dead_zone_type(DeadZoneType p_type);
	DeadZoneType get_dead_zone_type() const;
};

VARIANT_ENUM_CAST(InputModifierDeadZone::DeadZoneType);

// ============================================================================
// InputModifierScalar — Per-axis scaling
// ============================================================================

class InputModifierScalar : public InputModifier {
	GDCLASS(InputModifierScalar, InputModifier);

private:
	Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;

	void set_scale(const Vector3 &p_scale);
	Vector3 get_scale() const;
};

// ============================================================================
// InputModifierNegate — Axis inversion
// ============================================================================

class InputModifierNegate : public InputModifier {
	GDCLASS(InputModifierNegate, InputModifier);

private:
	bool negate_x = false;
	bool negate_y = false;
	bool negate_z = false;

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;

	void set_negate_x(bool p_negate);
	bool get_negate_x() const;

	void set_negate_y(bool p_negate);
	bool get_negate_y() const;

	void set_negate_z(bool p_negate);
	bool get_negate_z() const;
};

// ============================================================================
// InputModifierSmooth — Smoothing via interpolation
// ============================================================================

class InputModifierSmooth : public InputModifier {
	GDCLASS(InputModifierSmooth, InputModifier);

private:
	float speed = 10.0f;
	Vector3 current_value = Vector3();

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;

	void set_speed(float p_speed);
	float get_speed() const;

	void reset();
};

// ============================================================================
// InputModifierResponseCurve — Custom response curve per axis
// ============================================================================

class InputModifierResponseCurve : public InputModifier {
	GDCLASS(InputModifierResponseCurve, InputModifier);

private:
	Ref<Curve> curve_x;
	Ref<Curve> curve_y;
	Ref<Curve> curve_z;

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;

	void set_curve_x(const Ref<Curve> &p_curve);
	Ref<Curve> get_curve_x() const;

	void set_curve_y(const Ref<Curve> &p_curve);
	Ref<Curve> get_curve_y() const;

	void set_curve_z(const Ref<Curve> &p_curve);
	Ref<Curve> get_curve_z() const;
};

// ============================================================================
// InputModifierSwizzle — Axis remapping
// ============================================================================

class InputModifierSwizzle : public InputModifier {
	GDCLASS(InputModifierSwizzle, InputModifier);

public:
	enum SwizzleOrder {
		SWIZZLE_XYZ, // Default, no change.
		SWIZZLE_YXZ,
		SWIZZLE_ZYX,
		SWIZZLE_XZY,
		SWIZZLE_YZX,
		SWIZZLE_ZXY,
	};

private:
	SwizzleOrder order = SWIZZLE_YXZ;

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;

	void set_order(SwizzleOrder p_order);
	SwizzleOrder get_order() const;
};

VARIANT_ENUM_CAST(InputModifierSwizzle::SwizzleOrder);

// ============================================================================
// InputModifierNormalize — Normalize vector to unit length
// ============================================================================

class InputModifierNormalize : public InputModifier {
	GDCLASS(InputModifierNormalize, InputModifier);

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;
};

// ============================================================================
// InputModifierClamp — Per-axis value clamping
// ============================================================================

class InputModifierClamp : public InputModifier {
	GDCLASS(InputModifierClamp, InputModifier);

private:
	Vector3 min_value = Vector3(-1.0f, -1.0f, -1.0f);
	Vector3 max_value = Vector3(1.0f, 1.0f, 1.0f);

protected:
	static void _bind_methods();

public:
	virtual Vector3 modify(const Vector3 &p_value, double p_delta) override;

	void set_min_value(const Vector3 &p_min);
	Vector3 get_min_value() const;

	void set_max_value(const Vector3 &p_max);
	Vector3 get_max_value() const;
};