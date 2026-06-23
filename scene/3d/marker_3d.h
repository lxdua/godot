/**************************************************************************/
/*  marker_3d.h                                                           */
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

#include "scene/3d/node_3d.h"

class Marker3D : public Node3D {
	GDCLASS(Marker3D, Node3D);

public:
	enum GizmoShape {
		GIZMO_SHAPE_CROSS,
		GIZMO_SHAPE_BOX,
	};

private:
	real_t gizmo_extents = 0.25;
	Color gizmo_color = Color(1, 1, 1, 1);
	float gizmo_opacity = 1.0f;
	GizmoShape gizmo_shape = GIZMO_SHAPE_CROSS;

protected:
	static void _bind_methods();

public:
	void set_gizmo_extents(real_t p_extents);
	real_t get_gizmo_extents() const;

	void set_gizmo_color(const Color &p_color);
	Color get_gizmo_color() const;

	void set_gizmo_opacity(float p_opacity);
	float get_gizmo_opacity() const;

	void set_gizmo_shape(GizmoShape p_shape);
	GizmoShape get_gizmo_shape() const;

	Marker3D();
};

VARIANT_ENUM_CAST(Marker3D::GizmoShape);
