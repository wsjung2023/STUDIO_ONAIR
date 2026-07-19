#include "cut_suggest/CutSuggestionSerializer.h"

#include "cut_suggest/CutReason.h"
#include "cut_suggest/CutSuggestion.h"

#include "CutSuggestTestSupport.h"
#include "CutSuggestionSchemaValidator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

namespace {

using creator::cut_suggest::CutReason;
using creator::cut_suggest::CutSuggestion;
using creator::cut_suggest::CutSuggestionSerializer;
using cut_suggest_test::range;

CutSuggestion silenceSample() {
    return CutSuggestion::create(range(1'000'000, 2'000'000), CutReason::Silence, 0.8)
        .value();
}

CutSuggestion fillerSample() {
    return CutSuggestion::create(range(3'000'000, 500'000), CutReason::Filler, 0.9, "um")
        .value();
}

TEST(CutSuggestionSerializerTest, EmitsSchemaVersionedSilenceDocument) {
    const auto json = CutSuggestionSerializer::toJson(silenceSample());
    ASSERT_TRUE(json.hasValue());
    const auto& doc = json.value();
    EXPECT_EQ(doc.at("schemaVersion").get<int>(), 1);
    EXPECT_EQ(doc.at("span").at("startNs").get<std::int64_t>(), 1'000'000);
    EXPECT_EQ(doc.at("span").at("durationNs").get<std::int64_t>(), 2'000'000);
    EXPECT_EQ(doc.at("reason").get<std::string>(), "silence");
    EXPECT_DOUBLE_EQ(doc.at("score").get<double>(), 0.8);
    EXPECT_FALSE(doc.contains("label"));  // silence carries no label
}

TEST(CutSuggestionSerializerTest, EmitsFillerLabel) {
    const auto json = CutSuggestionSerializer::toJson(fillerSample());
    ASSERT_TRUE(json.hasValue());
    EXPECT_EQ(json.value().at("reason").get<std::string>(), "filler");
    EXPECT_EQ(json.value().at("label").get<std::string>(), "um");
}

TEST(CutSuggestionSerializerTest, DocumentsValidateAgainstSchema) {
    EXPECT_EQ(cut_suggest_test::validateSuggestion(
                  CutSuggestionSerializer::toJson(silenceSample()).value()),
              "");
    EXPECT_EQ(cut_suggest_test::validateSuggestion(
                  CutSuggestionSerializer::toJson(fillerSample()).value()),
              "");
}

TEST(CutSuggestionSerializerTest, SerializesArrayInOrder) {
    const std::vector<CutSuggestion> cuts{silenceSample(), fillerSample()};
    const auto json =
        CutSuggestionSerializer::toJsonArray(std::span<const CutSuggestion>{cuts});
    ASSERT_TRUE(json.hasValue());
    ASSERT_TRUE(json.value().is_array());
    ASSERT_EQ(json.value().size(), 2u);
    EXPECT_EQ(json.value()[0].at("reason").get<std::string>(), "silence");
    EXPECT_EQ(json.value()[1].at("reason").get<std::string>(), "filler");
}

TEST(CutSuggestionSerializerTest, IsDeterministic) {
    const auto a = CutSuggestionSerializer::toJson(fillerSample());
    const auto b = CutSuggestionSerializer::toJson(fillerSample());
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(b.hasValue());
    EXPECT_EQ(a.value(), b.value());
}

}  // namespace
