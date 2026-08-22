#include "netlaglab/network_profile.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

namespace netlaglab {
namespace {

using namespace std::chrono_literals;

bool has_error(
    const std::vector<ValidationError>& errors,
    const TrafficDirection direction,
    const NetworkSetting setting)
{
    return std::any_of(errors.begin(), errors.end(), [direction, setting](const auto& error) {
        return error.direction == direction && error.setting == setting;
    });
}

TEST(NetworkProfileTest, DefaultProfileRepresentsUnrestrictedNetwork)
{
    const NetworkProfile profile{};

    EXPECT_EQ(profile.outbound.delay, 0ms);
    EXPECT_EQ(profile.outbound.jitter, 0ms);
    EXPECT_DOUBLE_EQ(profile.outbound.packet_loss_percent, 0.0);
    EXPECT_FALSE(profile.outbound.bandwidth_kbps.has_value());
    EXPECT_EQ(profile.inbound.delay, 0ms);
    EXPECT_EQ(profile.inbound.jitter, 0ms);
    EXPECT_DOUBLE_EQ(profile.inbound.packet_loss_percent, 0.0);
    EXPECT_FALSE(profile.inbound.bandwidth_kbps.has_value());
    EXPECT_TRUE(validate(profile).empty());
}

TEST(NetworkProfileTest, AcceptsValidSettingsForBothDirections)
{
    const NetworkProfile profile{
        .outbound = {.delay = 40ms,
                     .jitter = 5ms,
                     .packet_loss_percent = 1.5,
                     .bandwidth_kbps = 2'000},
        .inbound = {.delay = 70ms,
                    .jitter = 8ms,
                    .packet_loss_percent = 2.0,
                    .bandwidth_kbps = 5'000},
    };

    EXPECT_TRUE(validate(profile).empty());
}

TEST(NetworkProfileTest, RejectsNegativeDelayAndJitter)
{
    NetworkProfile profile{};
    profile.outbound.delay = -1ms;
    profile.inbound.jitter = -2ms;

    const auto errors = validate(profile);

    EXPECT_EQ(errors.size(), 2U);
    EXPECT_TRUE(has_error(errors, TrafficDirection::outbound, NetworkSetting::delay));
    EXPECT_TRUE(has_error(errors, TrafficDirection::inbound, NetworkSetting::jitter));
}

TEST(NetworkProfileTest, RejectsPacketLossOutsideAllowedRange)
{
    NetworkProfile profile{};
    profile.outbound.packet_loss_percent = -0.1;
    profile.inbound.packet_loss_percent = 100.1;

    const auto errors = validate(profile);

    EXPECT_EQ(errors.size(), 2U);
    EXPECT_TRUE(has_error(errors, TrafficDirection::outbound, NetworkSetting::packet_loss));
    EXPECT_TRUE(has_error(errors, TrafficDirection::inbound, NetworkSetting::packet_loss));
}

TEST(NetworkProfileTest, AcceptsPacketLossBoundaryValues)
{
    NetworkProfile profile{};
    profile.outbound.packet_loss_percent = 0.0;
    profile.inbound.packet_loss_percent = 100.0;

    EXPECT_TRUE(validate(profile).empty());
}

TEST(NetworkProfileTest, RejectsNonFinitePacketLoss)
{
    NetworkProfile profile{};
    profile.outbound.packet_loss_percent = std::numeric_limits<double>::quiet_NaN();
    profile.inbound.packet_loss_percent = std::numeric_limits<double>::infinity();

    const auto errors = validate(profile);

    EXPECT_EQ(errors.size(), 2U);
    EXPECT_TRUE(has_error(errors, TrafficDirection::outbound, NetworkSetting::packet_loss));
    EXPECT_TRUE(has_error(errors, TrafficDirection::inbound, NetworkSetting::packet_loss));
}

TEST(NetworkProfileTest, RejectsZeroBandwidthLimit)
{
    NetworkProfile profile{};
    profile.inbound.bandwidth_kbps = 0;

    const auto errors = validate(profile);

    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors.front().direction, TrafficDirection::inbound);
    EXPECT_EQ(errors.front().setting, NetworkSetting::bandwidth);
}

TEST(NetworkProfileTest, CollectsAllErrorsWithTheirDirectionAndSetting)
{
    NetworkProfile profile{};
    profile.outbound.delay = -10ms;
    profile.outbound.packet_loss_percent = 101.0;
    profile.inbound.jitter = -4ms;
    profile.inbound.bandwidth_kbps = 0;

    const auto errors = validate(profile);

    ASSERT_EQ(errors.size(), 4U);
    EXPECT_TRUE(has_error(errors, TrafficDirection::outbound, NetworkSetting::delay));
    EXPECT_TRUE(has_error(errors, TrafficDirection::outbound, NetworkSetting::packet_loss));
    EXPECT_TRUE(has_error(errors, TrafficDirection::inbound, NetworkSetting::jitter));
    EXPECT_TRUE(has_error(errors, TrafficDirection::inbound, NetworkSetting::bandwidth));
    EXPECT_TRUE(std::all_of(errors.begin(), errors.end(), [](const auto& error) {
        return !error.message.empty();
    }));
}

} // namespace
} // namespace netlaglab
