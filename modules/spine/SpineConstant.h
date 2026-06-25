/******************************************************************************
 * Spine Runtimes License Agreement
 * Last updated July 28, 2023. Replaces all prior versions.
 *
 * Copyright (c) 2013-2023, Esoteric Software LLC
 *
 * Integration of the Spine Runtimes into software or otherwise creating
 * derivative works of the Spine Runtimes is permitted under the terms and
 * conditions of Section 2 of the Spine Editor License Agreement:
 * http://esotericsoftware.com/spine-editor-license
 *
 * Otherwise, it is permitted to integrate the Spine Runtimes into software or
 * otherwise create derivative works of the Spine Runtimes (collectively,
 * "Products"), provided that each user of the Products must obtain their own
 * Spine Editor license and redistribution of the Products in any form must
 * include this license and copyright notice.
 *
 * THE SPINE RUNTIMES ARE PROVIDED BY ESOTERIC SOFTWARE LLC "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL ESOTERIC SOFTWARE LLC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES,
 * BUSINESS INTERRUPTION, OR LOSS OF USE, DATA, OR PROFITS) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THE
 * SPINE RUNTIMES, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#pragma once

#include "SpineCommon.h"

class SpineConstant : public Object {
	GDCLASS(SpineConstant, Object);

protected:
	static void _bind_methods();

public:
	enum MixBlend {
		MixBlend_Setup = 0,
		MixBlend_First,
		MixBlend_Replace,
		MixBlend_Add
	};

	enum MixDirection {
		MixDirection_In = 0,
		MixDirection_Out
	};

	enum TimelineTypeId {
		Property_Rotate = 0,
		Property_X = 1,
		Property_Y = 1, // Translate combined
		Property_ScaleX = 2,
		Property_ScaleY = 2, // Scale combined
		Property_ShearX = 3,
		Property_ShearY = 3, // Shear combined
		Property_Rgb = 4,
		Property_Alpha = 4, // Color combined
		Property_Rgb2 = 14, // TwoColor
		Property_Attachment = 5,
		Property_Deform = 6,
		Property_Event = 7,
		Property_DrawOrder = 8,
		Property_IkConstraint = 9,
		Property_TransformConstraint = 10,
		Property_PathConstraintPosition = 11,
		Property_PathConstraintSpacing = 12,
		Property_PathConstraintMix = 13,
		Property_Sequence = -1 // Not available in 3.8
	};

	enum TransformMode {
		TransformMode_Normal = 0,
		TransformMode_OnlyTranslation,
		TransformMode_NoRotationOrReflection,
		TransformMode_NoScale,
		TransformMode_NoScaleOrReflection
	};

	enum PositionMode {
		PositionMode_Fixed = 0,
		PositionMode_Percent
	};

	enum SpacingMode {
		SpacingMode_Length = 0,
		SpacingMode_Fixed,
		SpacingMode_Percent
	};

	enum RotateMode {
		RotateMode_Tangent = 0,
		RotateMode_Chain,
		RotateMode_ChainScale
	};

	enum BlendMode {
		BlendMode_Normal = 0,
		BlendMode_Additive,
		BlendMode_Multiply,
		BlendMode_Screen
	};

	enum UpdateMode {
		UpdateMode_Process,
		UpdateMode_Physics,
		UpdateMode_Manual
	};

	enum BoneMode {
		BoneMode_Follow,
		BoneMode_Drive
	};
};

VARIANT_ENUM_CAST(SpineConstant::MixBlend)
VARIANT_ENUM_CAST(SpineConstant::MixDirection)
VARIANT_ENUM_CAST(SpineConstant::TimelineTypeId)
VARIANT_ENUM_CAST(SpineConstant::TransformMode)
VARIANT_ENUM_CAST(SpineConstant::PositionMode)
VARIANT_ENUM_CAST(SpineConstant::SpacingMode)
VARIANT_ENUM_CAST(SpineConstant::RotateMode)
VARIANT_ENUM_CAST(SpineConstant::BlendMode)
VARIANT_ENUM_CAST(SpineConstant::UpdateMode)
VARIANT_ENUM_CAST(SpineConstant::BoneMode)
