#include "platform_release/EntitlementStore.h"

#include "core/AppError.h"
#include "core/Uuid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace creator::platform_release {
namespace {

constexpr std::size_t kMaximumReceiptBytes = 1024U * 1024U;
constexpr std::uintmax_t kMaximumStateBytes = 64U * 1024U;

bool isToken(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' ||
                      character == '_';
           });
}

bool hasExactKeys(const nlohmann::json& object,
                  std::initializer_list<std::string_view> expected) {
    if (!object.is_object() || object.size() != expected.size()) return false;
    return std::all_of(expected.begin(), expected.end(), [&](std::string_view key) {
        return object.contains(key);
    });
}

core::Result<void> validateAssertion(const EntitlementAssertion& assertion) {
    if (!isToken(assertion.productId) || assertion.validUntilUtcSeconds <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "verified entitlement assertion is invalid"};
    }
    return core::ok();
}

core::Result<void> validateRecord(const EntitlementStateRecord& record) {
    const auto assertion = validateAssertion(record.assertion);
    if (!assertion.hasValue()) return assertion.error();
    if (!isToken(record.providerId) || record.lastOnlineCheckUtcSeconds <= 0 ||
        record.lastObservedUtcSeconds < record.lastOnlineCheckUtcSeconds ||
        (!record.assertion.revoked &&
         record.lastOnlineCheckUtcSeconds > record.assertion.validUntilUtcSeconds)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "entitlement clock state is invalid"};
    }
    return core::ok();
}

nlohmann::json toJson(const EntitlementStateRecord& record) {
    return {{"assertion",
             {{"productId", record.assertion.productId},
              {"revoked", record.assertion.revoked},
              {"validUntilUtcSeconds", record.assertion.validUntilUtcSeconds}}},
            {"lastObservedUtcSeconds", record.lastObservedUtcSeconds},
            {"lastOnlineCheckUtcSeconds", record.lastOnlineCheckUtcSeconds},
            {"providerId", record.providerId},
            {"schemaVersion", 1}};
}

core::Result<EntitlementStateRecord> fromJson(const nlohmann::json& document) {
    try {
        if (!hasExactKeys(document,
                          {"assertion", "lastObservedUtcSeconds",
                           "lastOnlineCheckUtcSeconds", "providerId",
                           "schemaVersion"}) ||
            document.at("schemaVersion") != 1 ||
            !hasExactKeys(document.at("assertion"),
                          {"productId", "revoked", "validUntilUtcSeconds"})) {
            return core::AppError{core::ErrorCode::ParseFailure,
                                  "entitlement state has unknown or missing fields"};
        }
        EntitlementStateRecord record{
            .providerId = document.at("providerId").get<std::string>(),
            .assertion =
                {.productId = document.at("assertion").at("productId").get<std::string>(),
                 .validUntilUtcSeconds =
                     document.at("assertion").at("validUntilUtcSeconds").get<std::int64_t>(),
                 .revoked = document.at("assertion").at("revoked").get<bool>()},
            .lastOnlineCheckUtcSeconds =
                document.at("lastOnlineCheckUtcSeconds").get<std::int64_t>(),
            .lastObservedUtcSeconds =
                document.at("lastObservedUtcSeconds").get<std::int64_t>()};
        const auto valid = validateRecord(record);
        if (!valid.hasValue()) return valid.error();
        return record;
    } catch (const std::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "entitlement state is not valid structured metadata"};
    }
}

core::Result<void> replaceAtomically(const std::filesystem::path& temporary,
                                     const std::filesystem::path& destination) {
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "entitlement state could not be atomically replaced"};
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "entitlement state could not be atomically replaced"};
    }
#endif
    return core::ok();
}

}  // namespace

core::Result<EntitlementAssertion> EntitlementStore::verifyReceipt(
    std::string_view providerId, std::span<const std::byte> opaqueReceipt,
    const IReceiptVerifier& verifier) const {
    if (!isToken(providerId) || opaqueReceipt.empty() ||
        opaqueReceipt.size() > kMaximumReceiptBytes) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "receipt provider or size is invalid"};
    }
    const auto verified = verifier.verify(providerId, opaqueReceipt);
    if (!verified.hasValue()) return verified.error();
    const auto valid = validateAssertion(verified.value());
    if (!valid.hasValue()) return valid.error();
    return verified.value();
}

core::Result<void> EntitlementStore::write(
    const EntitlementStateRecord& record) const {
    const auto valid = validateRecord(record);
    if (!valid.hasValue()) return valid.error();
    if (statePath_.empty() || statePath_.filename().empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "entitlement state path is invalid"};
    }
    std::error_code error;
    if (std::filesystem::exists(statePath_, error)) {
        const auto previous = read();
        if (!previous.hasValue()) return previous.error();
        if (record.lastOnlineCheckUtcSeconds <
                previous.value().lastOnlineCheckUtcSeconds ||
            record.lastObservedUtcSeconds <
                previous.value().lastObservedUtcSeconds) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "entitlement clock state cannot move backward"};
        }
    } else if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "entitlement state path is unavailable"};
    }

    const auto parent = statePath_.has_parent_path()
                            ? statePath_.parent_path()
                            : std::filesystem::current_path(error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "entitlement state directory is unavailable"};
    }
    std::filesystem::create_directories(parent, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "entitlement state directory could not be created"};
    }
    const auto temporary =
        parent / ("." + statePath_.filename().string() + ".part-" +
                  core::generateUuidV4());
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "entitlement temporary state could not be opened"};
        }
        output << toJson(record).dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            output.close();
            std::filesystem::remove(temporary, error);
            return core::AppError{core::ErrorCode::IoFailure,
                                  "entitlement temporary state could not be written"};
        }
    }
    const auto replaced = replaceAtomically(temporary, statePath_);
    if (!replaced.hasValue()) {
        std::filesystem::remove(temporary, error);
        return replaced.error();
    }
    return core::ok();
}

core::Result<EntitlementStateRecord> EntitlementStore::read() const {
    std::error_code error;
    const auto size = std::filesystem::file_size(statePath_, error);
    if (error) {
        return core::AppError{core::ErrorCode::NotFound,
                              "entitlement state does not exist"};
    }
    if (size == 0 || size > kMaximumStateBytes) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "entitlement state size is invalid"};
    }
    std::ifstream input{statePath_, std::ios::binary};
    if (!input) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "entitlement state could not be read"};
    }
    std::stringstream contents;
    contents << input.rdbuf();
    try {
        return fromJson(nlohmann::json::parse(contents.str()));
    } catch (const std::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "entitlement state is not valid JSON"};
    }
}

}  // namespace creator::platform_release
