
/**************************************************************************/
/*  enhanced_input_action.cpp                                             */
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

#include "enhanced_input_action.h"

void InputAction::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_name", "name"), &InputAction::set_action_name);
	ClassDB::bind_method(D_METHOD("get_action_name"), &InputAction::get_action_name);

	ClassDB::bind_method(D_METHOD("set_value_type", "type"), &InputAction::set_value_type);
	ClassDB::bind_method(D_METHOD("get_value_type"), &InputAction::get_value_type);

	ClassDB::bind_method(D_METHOD("set_accumulation_mode", "mode"), &InputAction::set_accumulation_mode);
	ClassDB::bind_method(D_METHOD("get_accumulation_mode"), &InputAction::get_accumulation_mode);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "action_name"), "set_action_name", "get_action_name");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "value_type", PROPERTY_HINT_ENUM, "Bool,Float,Vector2,Vector3"), "set_value_type", "get_value_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "accumulation_mode", PROPERTY_HINT_ENUM, "Cumulative,Take Highest"), "set_accumulation_mode", "get_accumulation_mode");

	BIND_ENUM_CONSTANT(CUMULATIVE);
	BIND_ENUM_CONSTANT(TAKE_HIGHEST);
}

void InputAction::set_action_name(const StringName &p_name) {
	action_name = p_name;
}

StringName InputAction::get_action_name() const {
	return action_name;
}

void InputAction::set_value_type(ValueType p_type) {
	value_type = p_type;
}

InputAction::ValueType InputAction::get_value_type() const {
	return value_type;
}

void InputAction::set_accumulation_mode(AccumulationMode p_mode) {
	accumulation_mode = p_mode;
}

InputAction::AccumulationMode InputAction::get_accumulation_mode() const {
	return accumulation_mode;
}
