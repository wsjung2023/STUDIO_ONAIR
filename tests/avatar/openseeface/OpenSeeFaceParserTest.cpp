// Tests for the OpenSeeFace UDP packet parser (R3 Stage B.1): pure
// bytes -> TrackingResult decoding, no socket, no thread. Every test
// synthesizes a 1785-byte record from known field values with
// serializeSyntheticFaceRecord() below (explicit little-endian byte
// assembly, mirroring the parser's own byte assembly) and asserts the parser
// extracts exactly what the mapping table in
// docs/openseeface-udp-format.md says it should - never a vacuous
// language-level fact.

#include "avatar/openseeface/OpenSeeFaceParser.h"

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarMotionSerializer.h"
#include "avatar/AvatarProviderId.h"
#include "avatar/CalibrationProfile.h"
#include "avatar/ExpressionNormalizer.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/TrackingResult.h"
#include "core/AppError.h"
#include "core/Result.h"
#include "core/Timebase.h"

#include <gtest/gtest.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarMotionSerializer;
using creator::avatar::AvatarProviderId;
using creator::avatar::CalibrationProfile;
using creator::avatar::ExpressionNormalizer;
using creator::avatar::ExpressionParameters;
using creator::avatar::TrackingResult;
using creator::avatar::openseeface::kDefaultUdpPort;
using creator::avatar::openseeface::kFaceRecordSizeBytes;
using creator::avatar::openseeface::parseDatagram;
using creator::avatar::openseeface::parseFace;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

constexpr float kTolerance = 1e-4F;

// ---------------------------------------------------------------------
// Synthetic-record builder: assembles a 1785-byte OpenSeeFace face record
// from named field values, little-endian, per the offsets verified in
// docs/openseeface-udp-format.md. This is the test-side mirror of the
// parser's own byte assembly - a separate implementation (not a call into
// the parser) so a bug in one is unlikely to be masked by an identical bug
// in the other.
// ---------------------------------------------------------------------

void writeUint8(std::vector<std::byte>& buf, std::size_t offset, std::uint8_t value) {
    buf[offset] = static_cast<std::byte>(value);
}

void writeFloatLe(std::vector<std::byte>& buf, std::size_t offset, float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (std::size_t i = 0; i < sizeof(float); ++i) {
        buf[offset + i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFU);
    }
}

void writeDoubleLe(std::vector<std::byte>& buf, std::size_t offset, double value) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < sizeof(double); ++i) {
        buf[offset + i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFU);
    }
}

void writeInt32Le(std::vector<std::byte>& buf, std::size_t offset, std::int32_t value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (std::size_t i = 0; i < sizeof(std::int32_t); ++i) {
        buf[offset + i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFU);
    }
}

// Every field named in the wire-format table (docs/openseeface-udp-format.md),
// defaulted to a "face found, camera-facing, neutral" record so a test only
// has to override the fields it cares about.
struct SyntheticFaceRecord {
    double time = 0.0;             // offset 0 - wire wall clock, diagnostic only
    std::int32_t id = 0;           // offset 8
    float width = 640.0F;          // offset 12
    float height = 480.0F;         // offset 16
    float eyeBlinkRight = 1.0F;    // offset 20 - eye_blink[0]
    float eyeBlinkLeft = 1.0F;     // offset 24 - eye_blink[1]
    std::uint8_t success = 1;      // offset 28
    float pnpError = 0.0F;         // offset 29
    float quatX = 0.0F;            // offset 33
    float quatY = 0.0F;            // offset 37
    float quatZ = 0.0F;            // offset 41
    float quatW = 1.0F;            // offset 45
    float eulerX = 0.0F;           // offset 49 - pitch axis
    float eulerY = 0.0F;           // offset 53 - yaw axis
    float eulerZ = 90.0F;          // offset 57 - roll axis, wire-centered at 90
    float translationX = 0.0F;     // offset 61
    float translationY = 0.0F;     // offset 65
    float translationZ = 0.0F;     // offset 69
    // offsets 73..1728: confidence[68], points2D[68], points3D[70] - always
    // zero-filled; no field in this parser's mapping reads them.
    float eyeL = 0.0F;                // features[0], offset 1729
    float eyeR = 0.0F;                // features[1], offset 1733
    float eyebrowSteepnessL = 0.0F;   // features[2], offset 1737
    float eyebrowUpdownL = 0.0F;      // features[3], offset 1741
    float eyebrowQuirkL = 0.0F;       // features[4], offset 1745
    float eyebrowSteepnessR = 0.0F;   // features[5], offset 1749
    float eyebrowUpdownR = 0.0F;      // features[6], offset 1753
    float eyebrowQuirkR = 0.0F;       // features[7], offset 1757
    float mouthCornerUpdownL = 0.0F;  // features[8], offset 1761
    float mouthCornerInoutL = 0.0F;   // features[9], offset 1765
    float mouthCornerUpdownR = 0.0F;  // features[10], offset 1769
    float mouthCornerInoutR = 0.0F;   // features[11], offset 1773
    float mouthOpen = 0.0F;           // features[12], offset 1777
    float mouthWide = 0.0F;           // features[13], offset 1781
};

std::vector<std::byte> serializeSyntheticFaceRecord(const SyntheticFaceRecord& r) {
    // 1729 (features block start) + 14 features * 4 bytes == 1785: the
    // arithmetic the spec doc claims, verified here rather than trusted.
    static_assert(1729 + 14 * 4 == 1785);

    std::vector<std::byte> buf(kFaceRecordSizeBytes, std::byte{0});
    writeDoubleLe(buf, 0, r.time);
    writeInt32Le(buf, 8, r.id);
    writeFloatLe(buf, 12, r.width);
    writeFloatLe(buf, 16, r.height);
    writeFloatLe(buf, 20, r.eyeBlinkRight);
    writeFloatLe(buf, 24, r.eyeBlinkLeft);
    writeUint8(buf, 28, r.success);
    writeFloatLe(buf, 29, r.pnpError);
    writeFloatLe(buf, 33, r.quatX);
    writeFloatLe(buf, 37, r.quatY);
    writeFloatLe(buf, 41, r.quatZ);
    writeFloatLe(buf, 45, r.quatW);
    writeFloatLe(buf, 49, r.eulerX);
    writeFloatLe(buf, 53, r.eulerY);
    writeFloatLe(buf, 57, r.eulerZ);
    writeFloatLe(buf, 61, r.translationX);
    writeFloatLe(buf, 65, r.translationY);
    writeFloatLe(buf, 69, r.translationZ);
    writeFloatLe(buf, 1729, r.eyeL);
    writeFloatLe(buf, 1733, r.eyeR);
    writeFloatLe(buf, 1737, r.eyebrowSteepnessL);
    writeFloatLe(buf, 1741, r.eyebrowUpdownL);
    writeFloatLe(buf, 1745, r.eyebrowQuirkL);
    writeFloatLe(buf, 1749, r.eyebrowSteepnessR);
    writeFloatLe(buf, 1753, r.eyebrowUpdownR);
    writeFloatLe(buf, 1757, r.eyebrowQuirkR);
    writeFloatLe(buf, 1761, r.mouthCornerUpdownL);
    writeFloatLe(buf, 1765, r.mouthCornerInoutL);
    writeFloatLe(buf, 1769, r.mouthCornerUpdownR);
    writeFloatLe(buf, 1773, r.mouthCornerInoutR);
    writeFloatLe(buf, 1777, r.mouthOpen);
    writeFloatLe(buf, 1781, r.mouthWide);
    return buf;
}

// ---------------------------------------------------------------------
// Named constants
// ---------------------------------------------------------------------

TEST(OpenSeeFaceParserTest, RecordSizeAndPortAreTheDocumentedValues) {
    EXPECT_EQ(kFaceRecordSizeBytes, 1785U);
    EXPECT_EQ(kDefaultUdpPort, 11573U);
}

// ---------------------------------------------------------------------
// Exact decode
// ---------------------------------------------------------------------

TEST(OpenSeeFaceParserTest, ExactDecodeMapsAllDocumentedFields) {
    SyntheticFaceRecord record{};
    record.success = 1;
    record.pnpError = 25.0F;
    record.eyeBlinkRight = 0.65F;   // -> eyeOpenRight
    record.eyeBlinkLeft = 0.42F;    // -> eyeOpenLeft
    record.eyebrowUpdownL = 0.33F;  // -> browUpLeft
    record.eyebrowUpdownR = -0.21F; // -> browUpRight
    record.mouthOpen = 0.77F;
    record.mouthWide = -0.15F;
    record.eulerX = 30.0F;   // pitch
    record.eulerY = -45.0F;  // yaw
    record.eulerZ = 100.0F;  // roll, wire-centered: (100 - 90) = 10 deg

    const std::vector<std::byte> bytes = serializeSyntheticFaceRecord(record);
    const TimestampNs projectTime{DurationNs{123'456'789}};

    const auto result = parseFace(bytes, projectTime);
    ASSERT_TRUE(result.hasValue()) << result.error().message();

    const TrackingResult& tracked = result.value();
    EXPECT_EQ(tracked.timestamp, projectTime);
    EXPECT_TRUE(tracked.faceFound);

    EXPECT_FLOAT_EQ(tracked.raw.eyeOpenRight, 0.65F);
    EXPECT_FLOAT_EQ(tracked.raw.eyeOpenLeft, 0.42F);
    EXPECT_FLOAT_EQ(tracked.raw.browUpLeft, 0.33F);
    EXPECT_FLOAT_EQ(tracked.raw.browUpRight, -0.21F);
    EXPECT_FLOAT_EQ(tracked.raw.mouthOpen, 0.77F);
    EXPECT_FLOAT_EQ(tracked.raw.mouthWide, -0.15F);

    // Hand-computed per the mapping table: clamp(deg / 90, -1, 1).
    EXPECT_NEAR(tracked.raw.headYaw, -45.0F / 90.0F, kTolerance);    // -0.5
    EXPECT_NEAR(tracked.raw.headPitch, 30.0F / 90.0F, kTolerance);   // 0.3333
    EXPECT_NEAR(tracked.raw.headRoll, 10.0F / 90.0F, kTolerance);    // 0.1111

    // Hand-computed: clamp(1 - pnpError / 100, 0, 1) == clamp(1 - 0.25, 0, 1)
    EXPECT_NEAR(tracked.confidence, 0.75F, kTolerance);
}

// ---------------------------------------------------------------------
// Little-endian correctness
// ---------------------------------------------------------------------

TEST(OpenSeeFaceParserTest, LittleEndianByteAssemblyRoundTripsExactFloatValues) {
    for (const float value : {0.5F, 0.123456F}) {
        SyntheticFaceRecord record{};
        record.eyeBlinkLeft = value;  // passed straight through, no arithmetic

        const std::vector<std::byte> bytes = serializeSyntheticFaceRecord(record);
        const auto result = parseFace(bytes, TimestampNs{DurationNs{0}});
        ASSERT_TRUE(result.hasValue());

        EXPECT_EQ(result.value().raw.eyeOpenLeft, value)
            << "float " << value << " did not round-trip exactly through little-endian assembly";
    }
}

// The round-trip test above proves self-consistency: this file's writeFloatLe
// and the parser's readFloatLe share the same byte-order convention, so a
// refactor that flipped both identically would still pass while breaking
// real decoding. This test anchors decoding to external ground truth
// instead - literal little-endian IEEE-754 byte quartets, independently
// verified via Python's struct.pack('<f', ...), are hand-placed directly
// into the record at the real field offsets, bypassing every test-side and
// parser-side float-writing helper. If readFloatLe ever read the bytes in
// the wrong order, these literal patterns would decode to garbage and the
// test would fail.
TEST(OpenSeeFaceParserTest, DecodesKnownIeee754LittleEndianBytesFromGroundTruth) {
    std::array<std::byte, kFaceRecordSizeBytes> bytes{};

    // success (offset 28) must be non-zero for a face to be considered
    // found; the fields under test here are read regardless, but set it for
    // record realism.
    bytes[28] = std::byte{1};

    // eye_blink[1] / eyeBlinkLeft, offset 24, passed straight through to
    // raw.eyeOpenLeft with no arithmetic (see parseFace). 0.5f as
    // struct.pack('<f', 0.5).hex() == "0000003f".
    bytes[24] = std::byte{0x00};
    bytes[25] = std::byte{0x00};
    bytes[26] = std::byte{0x00};
    bytes[27] = std::byte{0x3F};

    // features[12] / mouthOpen, offset 1777, also a raw passthrough.
    // 0.123456f as struct.pack('<f', 0.123456).hex() == "80d6fc3d".
    bytes[1777] = std::byte{0x80};
    bytes[1778] = std::byte{0xD6};
    bytes[1779] = std::byte{0xFC};
    bytes[1780] = std::byte{0x3D};

    const auto result = parseFace(bytes, TimestampNs{DurationNs{0}});
    ASSERT_TRUE(result.hasValue()) << result.error().message();

    EXPECT_EQ(result.value().raw.eyeOpenLeft, 0.5F);
    EXPECT_EQ(result.value().raw.mouthOpen, 0.123456F);
}

// ---------------------------------------------------------------------
// faceFound / confidence
// ---------------------------------------------------------------------

TEST(OpenSeeFaceParserTest, SuccessByteZeroYieldsFaceNotFoundAndZeroConfidence) {
    SyntheticFaceRecord record{};
    record.success = 0;
    record.pnpError = 5.0F;  // a low, otherwise-good fit error

    const std::vector<std::byte> bytes = serializeSyntheticFaceRecord(record);
    const auto result = parseFace(bytes, TimestampNs{DurationNs{0}});
    ASSERT_TRUE(result.hasValue());

    EXPECT_FALSE(result.value().faceFound);
    EXPECT_FLOAT_EQ(result.value().confidence, 0.0F);
}

// ---------------------------------------------------------------------
// Head-angle normalization
// ---------------------------------------------------------------------

TEST(OpenSeeFaceParserTest, HeadAngleBeyondMaxClampsToPlusMinusOne) {
    SyntheticFaceRecord record{};
    record.eulerY = 180.0F;   // yaw, way beyond kMaxHeadAngleDeg == 90
    record.eulerX = -180.0F;  // pitch, way beyond -90

    const std::vector<std::byte> bytes = serializeSyntheticFaceRecord(record);
    const auto result = parseFace(bytes, TimestampNs{DurationNs{0}});
    ASSERT_TRUE(result.hasValue());

    EXPECT_NEAR(result.value().raw.headYaw, 1.0F, kTolerance);
    EXPECT_NEAR(result.value().raw.headPitch, -1.0F, kTolerance);
}

TEST(OpenSeeFaceParserTest, RollIsRecenteredSoWireNinetyDegreesIsNeutral) {
    SyntheticFaceRecord record{};
    record.eulerZ = 90.0F;  // wire-centered neutral roll

    const std::vector<std::byte> bytes = serializeSyntheticFaceRecord(record);
    const auto result = parseFace(bytes, TimestampNs{DurationNs{0}});
    ASSERT_TRUE(result.hasValue());

    EXPECT_NEAR(result.value().raw.headRoll, 0.0F, kTolerance);
}

// ---------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------

TEST(OpenSeeFaceParserTest, WrongSizeRecordIsRejected) {
    std::vector<std::byte> tooShort(100, std::byte{0});

    const auto result = parseFace(tooShort, TimestampNs{DurationNs{0}});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OpenSeeFaceParserTest, DatagramSizeNotAMultipleOfRecordSizeIsRejected) {
    std::vector<std::byte> malformed(kFaceRecordSizeBytes * 2 + 10, std::byte{0});

    const auto result = parseDatagram(malformed, TimestampNs{DurationNs{0}});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OpenSeeFaceParserTest, EmptyDatagramIsRejected) {
    const std::vector<std::byte> empty;

    const auto result = parseDatagram(empty, TimestampNs{DurationNs{0}});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OpenSeeFaceParserTest, TwoRecordDatagramYieldsTwoResultsInWireOrder) {
    SyntheticFaceRecord first{};
    first.mouthOpen = 0.1F;
    SyntheticFaceRecord second{};
    second.mouthOpen = 0.9F;

    std::vector<std::byte> datagram = serializeSyntheticFaceRecord(first);
    const std::vector<std::byte> secondBytes = serializeSyntheticFaceRecord(second);
    datagram.insert(datagram.end(), secondBytes.begin(), secondBytes.end());

    const TimestampNs projectTime{DurationNs{42}};
    const auto result = parseDatagram(datagram, projectTime);
    ASSERT_TRUE(result.hasValue()) << result.error().message();

    const std::vector<TrackingResult>& faces = result.value();
    ASSERT_EQ(faces.size(), 2U);
    EXPECT_FLOAT_EQ(faces[0].raw.mouthOpen, 0.1F);
    EXPECT_FLOAT_EQ(faces[1].raw.mouthOpen, 0.9F);
    // Both faces arrived in the same datagram, i.e. the same project-clock
    // instant: they share the caller-supplied timestamp.
    EXPECT_EQ(faces[0].timestamp, projectTime);
    EXPECT_EQ(faces[1].timestamp, projectTime);
}

// ---------------------------------------------------------------------
// End-to-end: OpenSeeFace bytes -> parseFace -> ExpressionNormalizer
// -> AvatarMotionSample -> AvatarMotionSerializer -> schema-valid JSON.
// Proves the OpenSeeFace decoder actually flows into the existing Stage A
// pipeline, not just that it decodes bytes in isolation.
// ---------------------------------------------------------------------

// Same collecting-error-handler shape AvatarMotionSerializerTest and
// PipelineTest use against the same schema.
class EventSchemaCollectingErrorHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& instance,
               const std::string& message) override {
        basic_error_handler::error(pointer, instance, message);
        failed_ = true;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    bool failed_{false};
};

const nlohmann::json_schema::json_validator& eventSchemaValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        std::ifstream schemaFile(CS_EVENT_SCHEMA_PATH);
        nlohmann::json schemaJson;
        schemaFile >> schemaJson;

        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(schemaJson);
        return compiled;
    }();
    return validator;
}

bool validatesAgainstEventSchema(const nlohmann::json& document) {
    EventSchemaCollectingErrorHandler errors;
    eventSchemaValidator().validate(document, errors);
    return !errors.failed();
}

TEST(OpenSeeFaceParserTest, SynthesizedPacketFlowsToSchemaValidAvatarMotionTelemetry) {
    SyntheticFaceRecord record{};
    record.success = 1;
    record.pnpError = 10.0F;
    record.eyeBlinkRight = 0.55F;
    record.eyeBlinkLeft = 0.6F;
    record.eyebrowUpdownL = 0.3F;
    record.eyebrowUpdownR = 0.25F;
    record.mouthOpen = 0.6F;
    record.mouthWide = 0.4F;
    record.eulerX = -30.0F;  // pitch -> headPitch
    record.eulerY = 45.0F;   // yaw -> headYaw
    record.eulerZ = 108.0F;  // roll: (108 - 90) = 18 deg

    const std::vector<std::byte> bytes = serializeSyntheticFaceRecord(record);
    const TimestampNs projectTime{DurationNs{7'000'000}};

    const auto parsed = parseFace(bytes, projectTime);
    ASSERT_TRUE(parsed.hasValue()) << parsed.error().message();

    const ExpressionNormalizer normalizer{CalibrationProfile::identity()};
    const ExpressionParameters normalized = normalizer.normalize(parsed.value());

    const AvatarMotionSample sample{
        .timestamp = parsed.value().timestamp,
        .parameters = normalized,
        .provider = AvatarProviderId::create("openseeface").value(),
    };
    const nlohmann::json json = AvatarMotionSerializer{}.toJson(sample);

    EXPECT_TRUE(validatesAgainstEventSchema(json));
    EXPECT_EQ(json["tNs"].get<std::int64_t>(), 7'000'000);
    EXPECT_EQ(json["provider"].get<std::string>(), "openseeface");

    // Every raw value here is already within its documented range, so
    // identity calibration (subtract-zero, clamp) passes it through
    // unchanged - the decoded value really did reach the telemetry.
    EXPECT_NEAR(json["parameters"]["eyeOpenRight"].get<float>(), 0.55F, kTolerance);
    EXPECT_NEAR(json["parameters"]["eyeOpenLeft"].get<float>(), 0.6F, kTolerance);
    EXPECT_NEAR(json["parameters"]["browUpLeft"].get<float>(), 0.3F, kTolerance);
    EXPECT_NEAR(json["parameters"]["browUpRight"].get<float>(), 0.25F, kTolerance);
    EXPECT_NEAR(json["parameters"]["mouthOpen"].get<float>(), 0.6F, kTolerance);
    EXPECT_NEAR(json["parameters"]["mouthWide"].get<float>(), 0.4F, kTolerance);
    EXPECT_NEAR(json["parameters"]["headYaw"].get<float>(), 45.0F / 90.0F, kTolerance);
    EXPECT_NEAR(json["parameters"]["headPitch"].get<float>(), -30.0F / 90.0F, kTolerance);
    EXPECT_NEAR(json["parameters"]["headRoll"].get<float>(), 18.0F / 90.0F, kTolerance);
}

}  // namespace
