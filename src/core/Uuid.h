#pragma once

#include <string>
#include <string_view>

namespace creator::core {

/// Generates an RFC 4122 version 4 UUID in canonical lowercase 8-4-4-4-12 form.
///
/// Hand-rolled rather than pulled from a library: schemas/project.schema.json
/// requires projectId to be a uuid, and that single field does not justify a
/// dependency (LICENSE_POLICY 도입 절차 applies to every one we add).
///
/// Backed by std::random_device, seeded per thread. Not intended for security
/// tokens - it identifies projects, nothing more.
[[nodiscard]] std::string generateUuidV4();

/// True if text is a canonical lowercase version 4 UUID with the RFC 4122
/// variant bits set. Rejects uppercase, since we always emit lowercase and
/// accepting both would let two spellings of one id into the manifest.
[[nodiscard]] bool isUuidV4(std::string_view text) noexcept;

}  // namespace creator::core
