#include "core/Result.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::ok;
using creator::core::Result;

TEST(ResultTest, CarriesValueOnSuccess) {
    const Result<int> result{42};

    EXPECT_TRUE(result.hasValue());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, CarriesErrorOnFailure) {
    const Result<int> result{AppError{ErrorCode::NotFound, "no such project"}};

    EXPECT_FALSE(result.hasValue());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(result.error().message(), "no such project");
}

TEST(ResultTest, ValueOrReturnsFallbackOnError) {
    const Result<int> result{AppError{ErrorCode::IoFailure, "disk gone"}};

    EXPECT_EQ(result.valueOr(-1), -1);
}

TEST(ResultTest, ValueOrReturnsValueOnSuccess) {
    const Result<int> result{7};

    EXPECT_EQ(result.valueOr(-1), 7);
}

TEST(ResultTest, MovesNonCopyableValues) {
    Result<std::string> result{std::string{"segment_000001.mkv"}};

    ASSERT_TRUE(result.hasValue());
    const std::string taken = std::move(result).value();
    EXPECT_EQ(taken, "segment_000001.mkv");
}

TEST(ResultVoidTest, DefaultIsSuccess) {
    const Result<void> result = ok();

    EXPECT_TRUE(result.hasValue());
    EXPECT_TRUE(static_cast<bool>(result));
}

TEST(ResultVoidTest, CarriesError) {
    const Result<void> result{AppError{ErrorCode::InvalidState, "already recording"}};

    EXPECT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(result.error().message(), "already recording");
}

TEST(AppErrorTest, ComparesByCodeAndMessage) {
    const AppError a{ErrorCode::NotFound, "x"};
    const AppError b{ErrorCode::NotFound, "x"};
    const AppError c{ErrorCode::NotFound, "y"};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(AppErrorTest, StringifiesEveryCode) {
    EXPECT_EQ(creator::core::toString(ErrorCode::Unknown), "Unknown");
    EXPECT_EQ(creator::core::toString(ErrorCode::InvalidArgument), "InvalidArgument");
    EXPECT_EQ(creator::core::toString(ErrorCode::NotFound), "NotFound");
    EXPECT_EQ(creator::core::toString(ErrorCode::AlreadyExists), "AlreadyExists");
    EXPECT_EQ(creator::core::toString(ErrorCode::InvalidState), "InvalidState");
    EXPECT_EQ(creator::core::toString(ErrorCode::IoFailure), "IoFailure");
    EXPECT_EQ(creator::core::toString(ErrorCode::ParseFailure), "ParseFailure");
    EXPECT_EQ(creator::core::toString(ErrorCode::UnsupportedVersion), "UnsupportedVersion");
}

}  // namespace
