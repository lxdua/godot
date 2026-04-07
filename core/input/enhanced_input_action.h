
/**************************************************************************/
/*  enhanced_input_action.h                                               */
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
#include "core/input/enhanced_input_action_value.h"

class InputAction : public Resource {
	GDCLASS(InputAction, Resource);

public:
	using ValueType = InputActionValue::ValueType;

	enum AccumulationMode {
		CUMULATIVE,
		TAKE_HIGHEST,
	};

private:
	StringName action_name;
	ValueType value_type = ValueType::BOOL;
	AccumulationMode accumulation_mode = CUMULATIVE;

protected:
	static void _bind_methods();

public:
	void set_action_name(const StringName &p_name);
	StringName get_action_name() const;

	void set_value_type(ValueType p_type);
	ValueType get_value_type() const;

	void set_accumulation_mode(AccumulationMode p_mode);
	AccumulationMode get_accumulation_mode() const;
};

VARIANT_ENUM_CAST(InputAction::AccumulationMode);
