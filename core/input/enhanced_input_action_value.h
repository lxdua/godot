
/**************************************************************************/
/*  enhanced_input_action_value.h                                         */
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

#include "core/object/ref_counted.h"
#include "core/object/class_db.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

class InputActionValue : public RefCounted {
	GDCLASS(InputActionValue, RefCounted);

public:
	enum ValueType {
		BOOL,
		FLOAT,
		VECTOR2,
		VECTOR3,
	};

private:
	ValueType value_type = BOOL;
	Vector3 value = Vector3();

protected:
	static void _bind_methods();

public:
	// Factory methods.
	static Ref<InputActionValue> create_bool(bool p_value);
	static Ref<InputActionValue> create_float(float p_value);
	static Ref<InputActionValue> create_vector2(const Vector2 &p_value);
	static Ref<InputActionValue> create_vector3(const Vector3 &p_value);

	// Getters.
	ValueType get_value_type() const;
	bool get_bool() const;
	float get_float() const;
	Vector2 get_vector2() const;
	Vector3 get_vector3() const;

	// Internal helpers.
	Vector3 get_raw() const;
	void set_raw(const Vector3 &p_value);
	void set_value_type(ValueType p_type);
	bool is_non_zero() const;
};

VARIANT_ENUM_CAST(InputActionValue::ValueType);
