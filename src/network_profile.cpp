#include "netlaglab/network_profile.hpp"

#include <cmath>
#include <type_traits>

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

ProfileChangeApplication apply_profile_change(
    const NetworkProfile& profile,
    const ProfileChange& change)
{
    NetworkProfile changed{profile};
    std::vector<ValidationError> operation_errors;

    std::visit(
        [&changed, &operation_errors](const auto& operation) {
            using Operation = std::decay_t<decltype(operation)>;
            NetworkSetting setting{};
            if constexpr (std::is_same_v<Operation, SetDelay>) {
                setting = NetworkSetting::delay;
            } else if constexpr (std::is_same_v<Operation, SetJitter>) {
                setting = NetworkSetting::jitter;
            } else if constexpr (std::is_same_v<Operation, SetPacketLoss>) {
                setting = NetworkSetting::packet_loss;
            } else if constexpr (std::is_same_v<Operation, SetBandwidth>) {
                setting = NetworkSetting::bandwidth;
            } else {
                setting = operation.setting;
            }

            DirectionSettings* settings{};
            switch (operation.direction) {
            case TrafficDirection::outbound:
                settings = &changed.outbound;
                break;
            case TrafficDirection::inbound:
                settings = &changed.inbound;
                break;
            default:
                operation_errors.push_back(
                    {operation.direction, setting, "Traffic direction is invalid"});
                return;
            }

            if constexpr (std::is_same_v<Operation, SetDelay>) {
                settings->delay = operation.value;
            } else if constexpr (std::is_same_v<Operation, SetJitter>) {
                settings->jitter = operation.value;
            } else if constexpr (std::is_same_v<Operation, SetPacketLoss>) {
                settings->packet_loss_percent = operation.percent;
            } else if constexpr (std::is_same_v<Operation, SetBandwidth>) {
                settings->bandwidth_kbps = operation.kbps;
            } else {
                switch (operation.setting) {
                case NetworkSetting::delay:
                    settings->delay = std::chrono::milliseconds{0};
                    break;
                case NetworkSetting::jitter:
                    settings->jitter = std::chrono::milliseconds{0};
                    break;
                case NetworkSetting::packet_loss:
                    settings->packet_loss_percent = 0.0;
                    break;
                case NetworkSetting::bandwidth:
                    settings->bandwidth_kbps.reset();
                    break;
                default:
                    operation_errors.push_back(
                        {operation.direction,
                         operation.setting,
                         "Network setting is invalid"});
                    break;
                }
            }
        },
        change);

    if (!operation_errors.empty()) {
        return operation_errors;
    }
    std::vector<ValidationError> errors{validate(changed)};
    if (!errors.empty()) {
        return errors;
    }
    return changed;
}

} // namespace netlaglab
