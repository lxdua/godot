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

#include "SpineTimeline.h"
#include "SpineSkeleton.h"
#include "SpineEvent.h"
#if VERSION_MAJOR == 3
#include "core/method_bind_ext.gen.inc"
#endif

void SpineTimeline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("apply", "skeleton", "last_time", "time", "events", "alpha", "blend", "direction"), &SpineTimeline::apply);
	ClassDB::bind_method(D_METHOD("get_property_id"), &SpineTimeline::get_property_id);
	ClassDB::bind_method(D_METHOD("get_type"), &SpineTimeline::get_type);
}

void SpineTimeline::apply(Ref<SpineSkeleton> skeleton, float last_time, float time, Array events, float alpha,
						  SpineConstant::MixBlend blend, SpineConstant::MixDirection direction) {
	SPINE_CHECK(get_spine_object(), )
	if (!skeleton->get_spine_object()) return;
	spine::Vector<spine::Event *> spine_events;
	spine_events.setSize((int) events.size(), nullptr);
	for (int i = 0; i < events.size(); ++i) {
		events[i] = ((Ref<SpineEvent>) spine_events[i])->get_spine_object();
	}
	get_spine_object()->apply(*(skeleton->get_spine_object()), last_time, time, &spine_events, alpha, (spine::MixBlend) blend, (spine::MixDirection) direction);
}

int SpineTimeline::get_property_id() {
	SPINE_CHECK(get_spine_object(), 0)
	return get_spine_object()->getPropertyId();
}

String SpineTimeline::get_type() {
	SPINE_CHECK(get_spine_object(), "")
	return get_spine_object()->getRTTI().getClassName();
}
