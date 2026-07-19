#pragma once

// Test-only helper: validate a telemetry event object against the committed
// schemas/event.schema.json using the vendored pboettch json-schema-validator,
// mirroring project_store/ManifestSchemaValidator.cpp. The schema file path is
// injected by the build via CS_EVENT_SCHEMA_PATH so the test reads the same file
// the product ships, not a copy that could drift.

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace creator::audio_dsp::test {

/// Collects the first schema violation, matching ManifestSchemaValidator's
/// handler so validation reports a pointer + message on failure.
class RecordingErrorHandler final
    : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& instance,
               const std::string& message) override {
        nlohmann::json_schema::basic_error_handler::error(pointer, instance,
                                                          message);
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

/// Lazily-compiled validator for the event schema, shared across tests.
inline const nlohmann::json_schema::json_validator& eventSchemaValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        std::ifstream file(CS_EVENT_SCHEMA_PATH);
        if (!file.is_open()) {
            throw std::runtime_error("cannot open event schema at " +
                                     std::string(CS_EVENT_SCHEMA_PATH));
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(nlohmann::json::parse(buffer.str()));
        return compiled;
    }();
    return validator;
}

/// Returns true iff `event` validates against event.schema.json. On failure,
/// `whyNot` receives a human-readable pointer + message for the test log.
inline bool validatesAgainstEventSchema(const nlohmann::json& event,
                                        std::string* whyNot = nullptr) {
    RecordingErrorHandler errors;
    eventSchemaValidator().validate(event, errors);
    if (errors.failed() && whyNot != nullptr) {
        *whyNot = "at '" + errors.pointer() + "': " + errors.message();
    }
    return !errors.failed();
}

}  // namespace creator::audio_dsp::test
