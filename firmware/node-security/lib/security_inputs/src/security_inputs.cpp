#include "security_inputs.h"

namespace node_sec {

SecurityInputSampler::SecurityInputSampler()
    : SecurityInputSampler(SecurityInputSamplerConfig{}) {}

SecurityInputSampler::SecurityInputSampler(const SecurityInputSamplerConfig& config)
    : config_(config) {}

void SecurityInputSampler::reset() {
    has_polled_ = false;
    has_valid_sample_ = false;
    last_poll_ms_ = 0UL;
}

MappedSecurityInput SecurityInputSampler::mapRawSample(
    const RawSecurityInputSample& raw,
    const SecurityInputPolarityConfig& polarity) {
    MappedSecurityInput mapped;
    mapped.valid = raw.valid;
    if (!raw.valid) {
        return mapped;
    }

    mapped.pir_motion_detected = (raw.pir_level_high == polarity.pir_motion_active_high);
    mapped.door_raw_open = (raw.reed_level_high == polarity.reed_door_open_high);
    return mapped;
}

SecuritySamplerStep SecurityInputSampler::poll(ISecurityInputSource& source,
                                               SecurityController& controller,
                                               uint32_t now_ms) {
    SecuritySamplerStep step;
    step.door_outcome.trigger_source = TriggerSource::REED_SWITCH;
    step.door_outcome.state = controller.state();
    step.motion_outcome.trigger_source = TriggerSource::PIR;
    step.motion_outcome.state = controller.state();
    step.state = controller.state();

    if (has_polled_ &&
        (static_cast<uint32_t>(now_ms - last_poll_ms_) < config_.sample_interval_ms)) {
        return step;
    }

    has_polled_ = true;
    last_poll_ms_ = now_ms;
    step.polled = true;

    const RawSecurityInputSample raw = source.readInputs();
    step.mapped = mapRawSample(raw, config_.polarity);
    step.sample_valid = step.mapped.valid;

    if (!step.mapped.valid) {
        if (has_valid_sample_) {
            // Cancel any pending debounce transition so recovery requires a full valid debounce window.
            step.door_outcome = controller.updateDoorInput(controller.state().door_open, now_ms);
        }
        step.state = controller.state();
        return step;
    }

    has_valid_sample_ = true;
    step.door_outcome = controller.updateDoorInput(step.mapped.door_raw_open, now_ms);
    step.motion_outcome = controller.updateMotionInput(step.mapped.pir_motion_detected);
    step.any_alarm_triggered =
        step.door_outcome.alarm_triggered || step.motion_outcome.alarm_triggered;
    step.state = controller.state();
    return step;
}

}  // namespace node_sec
