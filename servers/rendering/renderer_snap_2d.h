/**************************************************************************/
/*  renderer_snap_2d.h                                                  */
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
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,    */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                   */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"

class RendererSnap2D {
public:
	enum TransformSnapMethod : uint8_t {
		TRANSFORM_SNAP_CANVAS = 0,
		TRANSFORM_SNAP_SCREEN = 1,
		TRANSFORM_SNAP_SCREEN_MOVING = 2,
	};

	enum Snap2DTransformsItemMode : uint8_t {
		SNAP_2D_TRANSFORMS_ITEM_INHERIT = 0,
		SNAP_2D_TRANSFORMS_ITEM_CANVAS = 1,
	};

	static constexpr float SCREEN_TRANSFORM_SNAP_MOVING_EPSILON = 0.01f;
	// CharacterBody floor resolution can oscillate ~0.017 px on the secondary axis while
	// moving on the primary axis. Suppress GPU snap on such an axis when another axis dominates.
	static constexpr float SCREEN_TRANSFORM_SNAP_MOVING_JITTER_EPSILON = 0.02f;
	static constexpr float SCREEN_TRANSFORM_SNAP_MOVING_DOMINANT_AXIS_RATIO = 2.0f;

	static bool use_canvas_transform_snap(bool p_snap_2d_transforms_to_pixel, TransformSnapMethod p_method) {
		return p_snap_2d_transforms_to_pixel && p_method == TRANSFORM_SNAP_CANVAS;
	}

	static bool use_screen_transform_snap(bool p_snap_2d_transforms_to_pixel, TransformSnapMethod p_method) {
		return p_snap_2d_transforms_to_pixel && (p_method == TRANSFORM_SNAP_SCREEN || p_method == TRANSFORM_SNAP_SCREEN_MOVING);
	}

	static bool use_screen_transform_snap_moving(bool p_snap_2d_transforms_to_pixel, TransformSnapMethod p_method) {
		return p_snap_2d_transforms_to_pixel && p_method == TRANSFORM_SNAP_SCREEN_MOVING;
	}
};
