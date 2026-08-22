#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
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
};

struct NetworkProfile {
    DirectionSettings outbound{};
    DirectionSettings inbound{};
};

struct ValidationError {
    TrafficDirection direction;
    NetworkSetting setting;
    std::string message;
};

[[nodiscard]] std::vector<ValidationError> validate(const NetworkProfile& profile);

} // namespace netlaglab
