#pragma once

// Test-only helper: validates a serialized zoom candidate against the shipped
// schema (schemas/zoom_candidate.schema.json), the same way the cursor tests
// validate telemetry events. The schema path is injected by CMake through the
// CS_ZOOM_CANDIDATE_SCHEMA_PATH compile definition so the test reads the real,
// single source-of-truth schema file rather than a copy.

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace autozoom_test {

inline const nlohmann::json_schema::json_validator& candidateValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        std::ifstream file{CS_ZOOM_CANDIDATE_SCHEMA_PATH};
        nlohmann::json schema = nlohmann::json::parse(file);
        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(schema);
        return compiled;
    }();
    return validator;
}

// Returns an empty string on success, or the first schema violation message.
inline std::string validateCandidate(const nlohmann::json& document) {
    class FirstError final : public nlohmann::json_schema::basic_error_handler {
    public:
        void error(const nlohmann::json::json_pointer& pointer,
                   const nlohmann::json& instance,
                   const std::string& message) override {
            basic_error_handler::error(pointer, instance, message);
            if (message_.empty()) {
                message_ = pointer.to_string() + ": " + message;
            }
        }
        std::string message_;
    };

    FirstError handler;
    candidateValidator().validate(document, handler);
    return handler.message_;
}

}  // namespace autozoom_test
