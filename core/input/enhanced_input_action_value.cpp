
/**************************************************************************/
/*  enhanced_input_action_value.cpp                                       */
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

#include "enhanced_input_action_value.h"

void InputActionValue::_bind_methods() {
	// Factory methods.
	ClassDB::bind_static_method("InputActionValue", D_METHOD("create_bool", "value"), &InputActionValue::create_bool);
	ClassDB::bind_static_method("InputActionValue", D_METHOD("create_float", "value"), &InputActionValue::create_float);
	ClassDB::bind_static_method("InputActionValue", D_METHOD("create_vector2", "value"), &InputActionValue::create_vector2);
	ClassDB::bind_static_method("InputActionValue", D_METHOD("create_vector3", "value"), &InputActionValue::create_vector3);

	// Getters.
	ClassDB::bind_method(D_METHOD("get_value_type"), &InputActionValue::get_value_type);
	ClassDB::bind_method(D_METHOD("get_bool"), &InputActionValue::get_bool);
	ClassDB::bind_method(D_METHOD("get_float"), &InputActionValue::get_float);
	ClassDB::bind_method(D_METHOD("get_vector2"), &InputActionValue::get_vector2);
	ClassDB::bind_method(D_METHOD("get_vector3"), &InputActionValue::get_vector3);

	// Internal helpers.
	ClassDB::bind_method(D_METHOD("get_raw"), &InputActionValue::get_raw);
	ClassDB::bind_method(D_METHOD("set_raw", "value"), &InputActionValue::set_raw);
	ClassDB::bind_method(D_METHOD("set_value_type", "type"), &InputActionValue::set_value_type);
	ClassDB::bind_method(D_METHOD("is_non_zero"), &InputActionValue::is_non_zero);

	// Enum.
	BIND_ENUM_CONSTANT(BOOL);
	BIND_ENUM_CONSTANT(FLOAT);
	BIND_ENUM_CONSTANT(VECTOR2);
	BIND_ENUM_CONSTANT(VECTOR3);
}

// Factory methods.

Ref<InputActionValue> InputActionValue::create_bool(bool p_value) {
	Ref<InputActionValue> val;
	val.instantiate();
	val->value_type = BOOL;
	val->value = Vector3(p_value ? 1.0f : 0.0f, 0.0f, 0.0f);
	return val;
}

Ref<InputActionValue> InputActionValue::create_float(float p_value) {
	Ref<InputActionValue> val;
	val.instantiate();
	val->value_type = FLOAT;
	val->value = Vector3(p_value, 0.0f, 0.0f);
	return val;
}

Ref<InputActionValue> InputActionValue::create_vector2(const Vector2 &p_value) {
	Ref<InputActionValue> val;
	val.instantiate();
	val->value_type = VECTOR2;
	val->value = Vector3(p_value.x, p_value.y, 0.0f);
	return val;
}

Ref<InputActionValue> InputActionValue::create_vector3(const Vector3 &p_value) {
	Ref<InputActionValue> val;
	val.instantiate();
	val->value_type = VECTOR3;
	val->value = p_value;
	return val;
}

// Getters.

InputActionValue::ValueType InputActionValue::get_value_type() const {
	return value_type;
}

bool InputActionValue::get_bool() const {
	return value.x != 0.0f;
}

float InputActionValue::get_float() const {
	return value.x;
}

Vector2 InputActionValue::get_vector2() const {
	return Vector2(value.x, value.y);
}

Vector3 InputActionValue::get_vector3() const {
	return value;
}

// Internal helpers.

Vector3 InputActionValue::get_raw() const {
	return value;
}

void InputActionValue::set_raw(const Vector3 &p_value) {
	value = p_value;
}

void InputActionValue::set_value_type(ValueType p_type) {
	value_type = p_type;
}

bool InputActionValue::is_non_zero() const {
	return value.length_squared() > 0.0f;
}
