#include "transcription/TranscriptStore.h"

#include "core/AppError.h"
#include "core/Uuid.h"
#include "transcription/TranscriptSerializer.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace creator::transcription {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

// Durable write: content goes to a temporary sibling, is flushed to the OS, then
// atomically renamed over the target (CLAUDE.md 4). A crash mid-write leaves the
// previous file intact and only a discardable .part-* temporary behind. This is
// a small local helper rather than a dependency on cs_project_store's internal
// DurableFile so the transcription module keeps the narrow dependency set
// (cs_core, cs_domain, nlohmann_json) and does not pull in the DB layer.
Result<void> writeDurably(const std::filesystem::path& target, const std::string& contents) {
    std::filesystem::path temporary;
    try {
        temporary = target.parent_path() /
                    ("." + target.filename().string() + ".part-" + core::generateUuidV4());
    } catch (const std::exception&) {
        return AppError{ErrorCode::IoFailure, "could not derive a temporary transcript path"};
    }

    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out.is_open()) {
            return AppError{ErrorCode::IoFailure, "could not open a temporary transcript file"};
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return AppError{ErrorCode::IoFailure, "could not write the temporary transcript file"};
        }
    }  // ofstream destructor closes the handle before the rename below

    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return AppError{ErrorCode::IoFailure, "could not atomically replace the transcript file"};
    }
    return core::ok();
}

}  // namespace

Result<std::filesystem::path> TranscriptStore::write(std::string_view name,
                                                     const Transcript& transcript) const {
    if (name.empty()) {
        return AppError{ErrorCode::InvalidArgument, "transcript artifact name must not be empty"};
    }
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) {
        return AppError{ErrorCode::InvalidArgument,
                        "transcript artifact name must not contain a path separator"};
    }

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (!std::filesystem::is_directory(directory_, ec)) {
        return AppError{ErrorCode::IoFailure, "transcript directory could not be created"};
    }

    const std::filesystem::path target = directory_ / (std::string{name} + ".json");
    const std::string contents = TranscriptSerializer::toJson(transcript).dump(2);

    auto written = writeDurably(target, contents);
    if (!written.hasValue()) return written.error();
    return target;
}

Result<Transcript> TranscriptStore::read(const std::filesystem::path& path) const {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return AppError{ErrorCode::NotFound, "transcript file does not exist"};
    }

    std::ifstream in{path, std::ios::binary};
    if (!in.is_open()) {
        return AppError{ErrorCode::IoFailure, "could not open the transcript file"};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return AppError{ErrorCode::IoFailure, "could not read the transcript file"};
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(buffer.str());
    } catch (const std::exception&) {
        return AppError{ErrorCode::ParseFailure, "transcript file is not valid JSON"};
    }
    return TranscriptSerializer::fromJson(document);
}

}  // namespace creator::transcription
