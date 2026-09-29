#include "controller_session_outcome.hpp"
#include "attach_session_presentation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <sstream>
#include <string>
#include <variant>

namespace netlaglab {
namespace {

void expect_protocol_error(const std::string_view block)
{
    ControllerSessionOutcomeParser parser;
    EXPECT_TRUE(std::holds_alternative<ControllerSessionOutcomeProtocolError>(
        parser.receive(block)))
        << block;
}

void expect_incomplete_at_eof(const std::string_view bytes)
{
    ControllerSessionOutcomeParser parser;
    EXPECT_TRUE(std::holds_alternative<IncompleteControllerSessionOutcome>(
        parser.receive(bytes)))
        << bytes;
    EXPECT_EQ(
        std::get<ControllerSessionOutcomeProtocolError>(parser.finish()),
        ControllerSessionOutcomeProtocolError::incomplete_input)
        << bytes;
}

TEST(ControllerSessionOutcomeTest, RoundTripsCleanWorkloadExit)
{
    const SessionOutcome outcome{
        .workload = WorkloadResult{WorkloadResultKind::exited, 23},
        .infrastructure_failures = {},
    };

    const ControllerSessionOutcomeSerialization serialized{
        serialize_controller_session_outcome(outcome)};
    const auto* block{std::get_if<std::string>(&serialized)};
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(
        *block,
        "SESSION_OUTCOME_BEGIN\n"
        "WORKLOAD EXIT 23\n"
        "INFRASTRUCTURE OK\n"
        "SESSION_OUTCOME_END\n");

    ControllerSessionOutcomeParser parser;
    const ControllerSessionOutcomeParseResult parsed{parser.receive(*block)};
    const auto* parsed_outcome{std::get_if<SessionOutcome>(&parsed)};
    ASSERT_NE(parsed_outcome, nullptr);
    ASSERT_TRUE(parsed_outcome->workload.has_value());
    EXPECT_EQ(parsed_outcome->workload->kind, WorkloadResultKind::exited);
    EXPECT_EQ(parsed_outcome->workload->value, 23);
    EXPECT_TRUE(parsed_outcome->infrastructure_succeeded());
}

TEST(ControllerSessionOutcomeTest, AcceptsWorkloadBoundaryValues)
{
    const std::array<WorkloadResult, 4> boundaries{
        WorkloadResult{WorkloadResultKind::exited, 0},
        WorkloadResult{WorkloadResultKind::exited, 255},
        WorkloadResult{WorkloadResultKind::signaled, 1},
        WorkloadResult{WorkloadResultKind::signaled, 127},
    };

    for (const WorkloadResult workload : boundaries) {
        const SessionOutcome outcome{
            .workload = workload,
            .infrastructure_failures = {},
        };
        const auto serialized{serialize_controller_session_outcome(outcome)};
        const auto* block{std::get_if<std::string>(&serialized)};
        ASSERT_NE(block, nullptr);
        ControllerSessionOutcomeParser parser;
        const auto parsed{parser.receive(*block)};
        const auto* parsed_outcome{std::get_if<SessionOutcome>(&parsed)};
        ASSERT_NE(parsed_outcome, nullptr);
        ASSERT_TRUE(parsed_outcome->workload.has_value());
        EXPECT_EQ(parsed_outcome->workload->kind, workload.kind);
        EXPECT_EQ(parsed_outcome->workload->value, workload.value);
    }
}

TEST(ControllerSessionOutcomeTest, RejectsPreActivationStartFailure)
{
    const SessionOutcome outcome{
        .workload = WorkloadResult{WorkloadResultKind::start_failure, 127},
        .infrastructure_failures = {},
    };

    EXPECT_TRUE(std::holds_alternative<ControllerSessionOutcomeProtocolError>(
        serialize_controller_session_outcome(outcome)));
}

TEST(ControllerSessionOutcomeTest, RoundTripsSignalAndEveryFailureCode)
{
    const SessionOutcome outcome{
        .workload = WorkloadResult{WorkloadResultKind::signaled, 9},
        .infrastructure_failures = {
            InfrastructureFailure::start,
            InfrastructureFailure::conversation,
            InfrastructureFailure::profile_state,
            InfrastructureFailure::stop_request,
            InfrastructureFailure::cleanup,
            InfrastructureFailure::launcher_reaping,
            InfrastructureFailure::supervisor_cleanup,
            InfrastructureFailure::invalid_event,
        },
    };

    const ControllerSessionOutcomeSerialization serialized{
        serialize_controller_session_outcome(outcome)};
    const auto* block{std::get_if<std::string>(&serialized)};
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(
        *block,
        "SESSION_OUTCOME_BEGIN\n"
        "WORKLOAD SIGNAL 9\n"
        "INFRASTRUCTURE FAILED\n"
        "FAILURE STARTUP\n"
        "FAILURE HELPER_CONVERSATION\n"
        "FAILURE PROFILE_STATE\n"
        "FAILURE STOP_REQUEST\n"
        "FAILURE PRIVILEGED_CLEANUP\n"
        "FAILURE LAUNCHER_REAPING\n"
        "FAILURE SUPERVISOR_CLEANUP\n"
        "FAILURE INVALID_LIFECYCLE_EVENT\n"
        "SESSION_OUTCOME_END\n");

    ControllerSessionOutcomeParser parser;
    ControllerSessionOutcomeParseResult parsed{
        IncompleteControllerSessionOutcome{}};
    for (const char byte : *block) {
        parsed = parser.receive(std::string_view{&byte, 1});
    }

    const auto* parsed_outcome{std::get_if<SessionOutcome>(&parsed)};
    ASSERT_NE(parsed_outcome, nullptr);
    ASSERT_TRUE(parsed_outcome->workload.has_value());
    EXPECT_EQ(parsed_outcome->workload->kind, WorkloadResultKind::signaled);
    EXPECT_EQ(parsed_outcome->workload->value, 9);
    EXPECT_EQ(
        parsed_outcome->infrastructure_failures,
        outcome.infrastructure_failures);
}

TEST(ControllerSessionOutcomeTest, RoundTripsUnknownWorkloadWithFailure)
{
    const SessionOutcome outcome{
        .workload = std::nullopt,
        .infrastructure_failures = {InfrastructureFailure::conversation},
    };

    const auto serialized{serialize_controller_session_outcome(outcome)};
    const auto* block{std::get_if<std::string>(&serialized)};
    ASSERT_NE(block, nullptr);

    ControllerSessionOutcomeParser parser;
    const auto parsed{parser.receive(*block)};
    const auto* parsed_outcome{std::get_if<SessionOutcome>(&parsed)};
    ASSERT_NE(parsed_outcome, nullptr);
    EXPECT_FALSE(parsed_outcome->workload.has_value());
    EXPECT_EQ(
        parsed_outcome->infrastructure_failures,
        std::vector{InfrastructureFailure::conversation});
}

TEST(ControllerSessionOutcomeTest, PreservesLifecycleFailureOrder)
{
    const SessionOutcome outcome{
        .workload = WorkloadResult{WorkloadResultKind::exited, 4},
        .infrastructure_failures = {
            InfrastructureFailure::invalid_event,
            InfrastructureFailure::launcher_reaping,
            InfrastructureFailure::supervisor_cleanup,
        },
    };

    const auto serialized{serialize_controller_session_outcome(outcome)};
    const auto* block{std::get_if<std::string>(&serialized)};
    ASSERT_NE(block, nullptr);
    ControllerSessionOutcomeParser parser;
    const auto parsed{parser.receive(*block)};
    const auto* parsed_outcome{std::get_if<SessionOutcome>(&parsed)};
    ASSERT_NE(parsed_outcome, nullptr);
    EXPECT_EQ(
        parsed_outcome->infrastructure_failures,
        outcome.infrastructure_failures);
}

TEST(ControllerSessionOutcomeTest, RejectsMalformedAndImpossibleBlocks)
{
    const std::array<std::string_view, 10> invalid_blocks{
        "session_OUTCOME_BEGIN\nWORKLOAD EXIT 0\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT -1\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 256\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD SIGNAL 0\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD SIGNAL 128\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD UNKNOWN\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\nINFRASTRUCTURE FAILED\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\nINFRASTRUCTURE OK\nFAILURE STARTUP\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\nINFRASTRUCTURE FAILED\nFAILURE STARTUP\nFAILURE STARTUP\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\nINFRASTRUCTURE FAILED\nFAILURE UNKNOWN\nSESSION_OUTCOME_END\n",
    };

    for (const std::string_view block : invalid_blocks) {
        expect_protocol_error(block);
    }
}

TEST(ControllerSessionOutcomeTest, RejectsMalformedTokensAndStructuralOrdering)
{
    const std::array<std::string_view, 9> invalid_blocks{
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT +1\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 1 extra\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nworkload EXIT 1\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 1\nWORKLOAD EXIT 2\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 1\nINFRASTRUCTURE OK\nINFRASTRUCTURE OK\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nFAILURE STARTUP\nWORKLOAD EXIT 1\nINFRASTRUCTURE FAILED\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 1\nFAILURE STARTUP\nINFRASTRUCTURE FAILED\nSESSION_OUTCOME_END\n",
        "SESSION_OUTCOME_BEGIN\nSESSION_OUTCOME_END\nWORKLOAD EXIT 1\nINFRASTRUCTURE OK\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 1\nSESSION_OUTCOME_END\nINFRASTRUCTURE OK\n",
    };

    for (const std::string_view block : invalid_blocks) {
        expect_protocol_error(block);
    }
}

TEST(ControllerSessionOutcomeTest, RejectsEofAtEveryRequiredBlockStage)
{
    const std::array<std::string_view, 6> incomplete_inputs{
        "",
        "SESSION_OUTCOME_",
        "SESSION_OUTCOME_BEGIN\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\nINFRASTRUCTURE OK\n",
        "SESSION_OUTCOME_BEGIN\nWORKLOAD UNKNOWN\nINFRASTRUCTURE FAILED\nFAILURE STARTUP\n",
    };

    for (const std::string_view bytes : incomplete_inputs) {
        expect_incomplete_at_eof(bytes);
    }
}

TEST(ControllerSessionOutcomeTest, RejectsIncompleteTrailingAndOversizedInput)
{
    ControllerSessionOutcomeParser incomplete;
    EXPECT_TRUE(std::holds_alternative<IncompleteControllerSessionOutcome>(
        incomplete.receive(
            "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\n")));
    EXPECT_EQ(
        std::get<ControllerSessionOutcomeProtocolError>(incomplete.finish()),
        ControllerSessionOutcomeProtocolError::incomplete_input);

    ControllerSessionOutcomeParser trailing;
    EXPECT_EQ(
        std::get<ControllerSessionOutcomeProtocolError>(trailing.receive(
            "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\nINFRASTRUCTURE OK\n"
            "SESSION_OUTCOME_END\nextra")),
        ControllerSessionOutcomeProtocolError::trailing_data);

    ControllerSessionOutcomeParser oversized;
    EXPECT_TRUE(std::holds_alternative<IncompleteControllerSessionOutcome>(
        oversized.receive(std::string(
            maximum_controller_session_outcome_size, 'x'))));
    EXPECT_EQ(
        std::get<ControllerSessionOutcomeProtocolError>(
            oversized.receive("x")),
        ControllerSessionOutcomeProtocolError::message_too_large);
}

TEST(ControllerSessionOutcomeTest, SerializerRejectsInvalidRangesAndDuplicates)
{
    const std::array<SessionOutcome, 4> invalid_outcomes{
        SessionOutcome{
            .workload = WorkloadResult{WorkloadResultKind::exited, 256},
            .infrastructure_failures = {},
        },
        SessionOutcome{
            .workload = WorkloadResult{WorkloadResultKind::signaled, 0},
            .infrastructure_failures = {},
        },
        SessionOutcome{
            .workload = std::nullopt,
            .infrastructure_failures = {},
        },
        SessionOutcome{
            .workload = WorkloadResult{WorkloadResultKind::exited, 0},
            .infrastructure_failures = {
                InfrastructureFailure::start,
                InfrastructureFailure::start,
            },
        },
    };

    for (const SessionOutcome& outcome : invalid_outcomes) {
        EXPECT_TRUE(std::holds_alternative<ControllerSessionOutcomeProtocolError>(
            serialize_controller_session_outcome(outcome)));
    }
}

TEST(AttachSessionPresentationTest, ReportsCleanExitAndSignalAsObservedSuccess)
{
    std::ostringstream output;
    std::ostringstream error;
    const SessionOutcome exit{
        .workload = WorkloadResult{WorkloadResultKind::exited, 23},
        .infrastructure_failures = {},
    };

    EXPECT_EQ(report_attached_session_outcome(exit, output, error), 0);
    EXPECT_EQ(output.str(), "Session ended; Workload exit code: 23.\n");
    EXPECT_TRUE(error.str().empty());

    output.str({});
    const SessionOutcome signal{
        .workload = WorkloadResult{WorkloadResultKind::signaled, 9},
        .infrastructure_failures = {},
    };
    EXPECT_EQ(report_attached_session_outcome(signal, output, error), 0);
    EXPECT_EQ(
        output.str(),
        "Session ended; Workload terminated by signal 9.\n");
}

TEST(AttachSessionPresentationTest, PreservesFailuresAndWorkloadKnowledge)
{
    std::ostringstream output;
    std::ostringstream error;
    const SessionOutcome known{
        .workload = WorkloadResult{WorkloadResultKind::exited, 7},
        .infrastructure_failures = {
            InfrastructureFailure::conversation,
            InfrastructureFailure::supervisor_cleanup,
        },
    };

    EXPECT_EQ(report_attached_session_outcome(known, output, error), 1);
    EXPECT_TRUE(output.str().empty());
    EXPECT_EQ(
        error.str(),
        "NetLagLab: Session infrastructure failed: helper conversation, "
        "Supervisor cleanup; Workload exit code: 7.\n");

    error.str({});
    const SessionOutcome unknown{
        .workload = std::nullopt,
        .infrastructure_failures = {InfrastructureFailure::start},
    };
    EXPECT_EQ(report_attached_session_outcome(unknown, output, error), 1);
    EXPECT_EQ(
        error.str(),
        "NetLagLab: Session infrastructure failed: startup; Workload result "
        "is unknown.\n");
}

TEST(AttachSessionPresentationTest, RejectsImpossibleCleanUnknownWorkload)
{
    std::ostringstream output;
    std::ostringstream error;
    const SessionOutcome impossible{
        .workload = std::nullopt,
        .infrastructure_failures = {},
    };

    EXPECT_EQ(report_attached_session_outcome(impossible, output, error), 1);
    EXPECT_TRUE(output.str().empty());
    EXPECT_EQ(error.str(), "NetLagLab: invalid Session Outcome.\n");
}

TEST(AttachSessionPresentationTest, PreservesLegacyTerminalResponses)
{
    std::ostringstream output;
    std::ostringstream error;

    const std::optional<int> ended{report_legacy_attached_session_outcome(
        "SESSION_ENDED", output, error)};
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(*ended, 0);
    EXPECT_EQ(output.str(), "Session ended.\n");
    EXPECT_TRUE(error.str().empty());

    const std::optional<int> failed{report_legacy_attached_session_outcome(
        "SESSION_FAILED", output, error)};
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(*failed, 1);
    EXPECT_EQ(error.str(), "NetLagLab: session failed\n");
    EXPECT_FALSE(report_legacy_attached_session_outcome(
        "SESSION_ENDED extra", output, error)
                     .has_value());
}

} // namespace
} // namespace netlaglab
