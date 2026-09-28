#include "netlaglab/network_profile.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <variant>
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

TEST(NetworkProfileTest, AppliesEveryProfileChangeToTheSelectedDirection)
{
    NetworkProfile profile{};

    auto result = apply_profile_change(
        profile, ProfileChange{SetDelay{TrafficDirection::outbound, 40ms}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(result));
    profile = std::get<NetworkProfile>(result);
    EXPECT_EQ(profile.outbound.delay, 40ms);
    EXPECT_EQ(profile.inbound.delay, 0ms);

    result = apply_profile_change(
        profile, ProfileChange{SetJitter{TrafficDirection::inbound, 7ms}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(result));
    profile = std::get<NetworkProfile>(result);
    EXPECT_EQ(profile.inbound.jitter, 7ms);

    result = apply_profile_change(
        profile, ProfileChange{SetPacketLoss{TrafficDirection::outbound, 1.5}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(result));
    profile = std::get<NetworkProfile>(result);
    EXPECT_DOUBLE_EQ(profile.outbound.packet_loss_percent, 1.5);

    result = apply_profile_change(
        profile, ProfileChange{SetBandwidth{TrafficDirection::inbound, 5'000}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(result));
    profile = std::get<NetworkProfile>(result);
    EXPECT_EQ(profile.inbound.bandwidth_kbps, 5'000U);
}

TEST(NetworkProfileTest, AppliesBoundaryValuesForEverySettingAndDirection)
{
    using Rep = std::chrono::milliseconds::rep;
    const std::vector<ProfileChange> boundary_changes{
        SetDelay{TrafficDirection::outbound, 0ms},
        SetDelay{
            TrafficDirection::inbound,
            std::chrono::milliseconds{std::numeric_limits<Rep>::max()}},
        SetJitter{TrafficDirection::outbound, 0ms},
        SetJitter{
            TrafficDirection::inbound,
            std::chrono::milliseconds{std::numeric_limits<Rep>::max()}},
        SetPacketLoss{TrafficDirection::outbound, 0.0},
        SetPacketLoss{TrafficDirection::inbound, 100.0},
        SetBandwidth{TrafficDirection::outbound, 1},
        SetBandwidth{
            TrafficDirection::inbound,
            std::numeric_limits<std::uint64_t>::max()},
    };

    for (const ProfileChange& change : boundary_changes) {
        EXPECT_TRUE(std::holds_alternative<NetworkProfile>(
            apply_profile_change(NetworkProfile{}, change)));
    }
}

TEST(NetworkProfileTest, ResetsEachSettingWithoutChangingTheOthers)
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

    const auto delay = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::outbound, NetworkSetting::delay}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(delay));
    EXPECT_EQ(std::get<NetworkProfile>(delay).outbound.delay, 0ms);
    EXPECT_EQ(std::get<NetworkProfile>(delay).outbound.jitter, 5ms);

    const auto jitter = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::inbound, NetworkSetting::jitter}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(jitter));
    EXPECT_EQ(std::get<NetworkProfile>(jitter).inbound.jitter, 0ms);

    const auto loss = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::outbound, NetworkSetting::packet_loss}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(loss));
    EXPECT_DOUBLE_EQ(
        std::get<NetworkProfile>(loss).outbound.packet_loss_percent, 0.0);

    const auto bandwidth = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::inbound, NetworkSetting::bandwidth}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(bandwidth));
    EXPECT_FALSE(
        std::get<NetworkProfile>(bandwidth).inbound.bandwidth_kbps.has_value());

    const auto inbound_delay = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::inbound, NetworkSetting::delay}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(inbound_delay));
    EXPECT_EQ(std::get<NetworkProfile>(inbound_delay).inbound.delay, 0ms);
    EXPECT_EQ(std::get<NetworkProfile>(inbound_delay).outbound.delay, 40ms);

    const auto outbound_jitter = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::outbound, NetworkSetting::jitter}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(outbound_jitter));
    EXPECT_EQ(std::get<NetworkProfile>(outbound_jitter).outbound.jitter, 0ms);
    EXPECT_EQ(std::get<NetworkProfile>(outbound_jitter).inbound.jitter, 8ms);

    const auto inbound_loss = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::inbound, NetworkSetting::packet_loss}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(inbound_loss));
    EXPECT_DOUBLE_EQ(
        std::get<NetworkProfile>(inbound_loss).inbound.packet_loss_percent, 0.0);
    EXPECT_DOUBLE_EQ(
        std::get<NetworkProfile>(inbound_loss).outbound.packet_loss_percent, 1.5);

    const auto outbound_bandwidth = apply_profile_change(
        profile,
        ProfileChange{ResetSetting{
            TrafficDirection::outbound, NetworkSetting::bandwidth}});
    ASSERT_TRUE(std::holds_alternative<NetworkProfile>(outbound_bandwidth));
    EXPECT_FALSE(std::get<NetworkProfile>(outbound_bandwidth)
                     .outbound.bandwidth_kbps.has_value());
    EXPECT_EQ(
        std::get<NetworkProfile>(outbound_bandwidth).inbound.bandwidth_kbps,
        5'000U);
}

TEST(NetworkProfileTest, RejectsInvalidChangesWithoutMutatingTheInputProfile)
{
    const NetworkProfile profile{
        .outbound = {.delay = 12ms,
                     .jitter = 3ms,
                     .packet_loss_percent = 2.5,
                     .bandwidth_kbps = 1'000},
    };
    const std::vector<ProfileChange> invalid_changes{
        SetDelay{TrafficDirection::outbound, -1ms},
        SetJitter{TrafficDirection::inbound, -1ms},
        SetPacketLoss{
            TrafficDirection::outbound,
            std::numeric_limits<double>::quiet_NaN()},
        SetPacketLoss{TrafficDirection::inbound, 100.1},
        SetBandwidth{TrafficDirection::outbound, 0},
        SetDelay{static_cast<TrafficDirection>(99), 1ms},
        ResetSetting{
            TrafficDirection::inbound, static_cast<NetworkSetting>(99)},
    };

    for (const ProfileChange& change : invalid_changes) {
        const auto result = apply_profile_change(profile, change);
        ASSERT_TRUE(std::holds_alternative<std::vector<ValidationError>>(result));
        EXPECT_FALSE(std::get<std::vector<ValidationError>>(result).empty());
    }

    EXPECT_EQ(profile.outbound.delay, 12ms);
    EXPECT_EQ(profile.outbound.jitter, 3ms);
    EXPECT_DOUBLE_EQ(profile.outbound.packet_loss_percent, 2.5);
    EXPECT_EQ(profile.outbound.bandwidth_kbps, 1'000U);
}

} // namespace
} // namespace netlaglab
