#include "project_store/ManifestSchemaValidator.h"

#include "core/AppError.h"
#include "project_store/ProjectSchema.h"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>

namespace creator::project_store {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

class ManifestErrorHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& instance,
               const std::string& message) override {
        basic_error_handler::error(pointer, instance, message);
        if (!failed_) {
            failed_ = true;
            pointer_ = pointer.to_string();
            message_ = message;
        }
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& pointer() const noexcept { return pointer_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    bool failed_{false};
    std::string pointer_;
    std::string message_;
};

const nlohmann::json_schema::json_validator& manifestValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(nlohmann::json::parse(embedded::kProjectSchema));
        return compiled;
    }();
    return validator;
}

}  // namespace

Result<void> validateManifestJson(const nlohmann::json& document) {
    try {
        ManifestErrorHandler errors;
        manifestValidator().validate(document, errors);
        if (errors.failed()) {
            return AppError{ErrorCode::ParseFailure,
                            "manifest schema violation at '" + errors.pointer() + "': " +
                                errors.message()};
        }
        return core::ok();
    } catch (const std::exception& error) {
        return AppError{ErrorCode::ParseFailure,
                        "manifest schema could not be compiled: " + std::string{error.what()}};
    }
}

}  // namespace creator::project_store
