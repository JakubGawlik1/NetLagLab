#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace netlaglab {

enum class TrafficDirection {
    outbound,
    inbound,
};

enum class NetworkSetting {
    delay,
    jitter,
    packet_loss,
    bandwidth,
};

struct DirectionSettings {
    std::chrono::milliseconds delay{0};
    std::chrono::milliseconds jitter{0};
    double packet_loss_percent{0.0};
    std::optional<std::uint64_t> bandwidth_kbps{};

    bool operator==(const DirectionSettings&) const = default;
};

struct NetworkProfile {
    DirectionSettings outbound{};
    DirectionSettings inbound{};

    bool operator==(const NetworkProfile&) const = default;
};

struct ValidationError {
    TrafficDirection direction;
    NetworkSetting setting;
    std::string message;

    bool operator==(const ValidationError&) const = default;
};

struct SetDelay {
    TrafficDirection direction;
    std::chrono::milliseconds value;

    bool operator==(const SetDelay&) const = default;
};

struct SetJitter {
    TrafficDirection direction;
    std::chrono::milliseconds value;

    bool operator==(const SetJitter&) const = default;
};

struct SetPacketLoss {
    TrafficDirection direction;
    double percent;

    bool operator==(const SetPacketLoss&) const = default;
};

struct SetBandwidth {
    TrafficDirection direction;
    std::uint64_t kbps;

    bool operator==(const SetBandwidth&) const = default;
};

struct ResetSetting {
    TrafficDirection direction;
    NetworkSetting setting;

    bool operator==(const ResetSetting&) const = default;
};

using ProfileChange = std::variant<
    SetDelay,
    SetJitter,
    SetPacketLoss,
    SetBandwidth,
    ResetSetting>;

using ProfileChangeApplication =
    std::variant<NetworkProfile, std::vector<ValidationError>>;

[[nodiscard]] std::vector<ValidationError> validate(const NetworkProfile& profile);
[[nodiscard]] ProfileChangeApplication apply_profile_change(
    const NetworkProfile& profile,
    const ProfileChange& change);

} // namespace netlaglab
