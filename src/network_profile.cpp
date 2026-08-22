#include "netlaglab/network_profile.hpp"

#include <cmath>

namespace netlaglab {
namespace {

void validate_direction(
    const DirectionSettings& settings,
    const TrafficDirection direction,
    std::vector<ValidationError>& errors)
{
    if (settings.delay.count() < 0) {
        errors.push_back({direction, NetworkSetting::delay, "Delay cannot be negative"});
    }

    if (settings.jitter.count() < 0) {
        errors.push_back({direction, NetworkSetting::jitter, "Jitter cannot be negative"});
    }

    if (!std::isfinite(settings.packet_loss_percent)) {
        errors.push_back(
            {direction, NetworkSetting::packet_loss, "Packet loss must be a finite number"});
    } else if (settings.packet_loss_percent < 0.0
               || settings.packet_loss_percent > 100.0) {
        errors.push_back(
            {direction, NetworkSetting::packet_loss, "Packet loss must be between 0 and 100"});
    }

    if (settings.bandwidth_kbps.has_value() && settings.bandwidth_kbps.value() == 0) {
        errors.push_back(
            {direction, NetworkSetting::bandwidth, "Bandwidth limit must be greater than zero"});
    }
}

} // namespace

std::vector<ValidationError> validate(const NetworkProfile& profile)
{
    std::vector<ValidationError> errors;
    validate_direction(profile.outbound, TrafficDirection::outbound, errors);
    validate_direction(profile.inbound, TrafficDirection::inbound, errors);
    return errors;
}

} // namespace netlaglab
