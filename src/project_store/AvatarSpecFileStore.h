#pragma once

#include "project_store/IAvatarSpecStore.h"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace creator::project_store {

struct AvatarSpecDirectoryIdentity final {
    std::uint64_t first{};
    std::uint64_t second{};
};

class AvatarSpecFileStore final : public IAvatarSpecStore {
public:
    explicit AvatarSpecFileStore(std::filesystem::path avatarsRoot);
    ~AvatarSpecFileStore() override;

    AvatarSpecFileStore(const AvatarSpecFileStore&) = delete;
    AvatarSpecFileStore& operator=(const AvatarSpecFileStore&) = delete;
    AvatarSpecFileStore(AvatarSpecFileStore&&) = delete;
    AvatarSpecFileStore& operator=(AvatarSpecFileStore&&) = delete;

    [[nodiscard]] core::Result<void> save(
        const avatar::AvatarSpec& spec) override;
    [[nodiscard]] core::Result<avatar::AvatarSpec> load(
        const avatar::AvatarId& id) const override;
    [[nodiscard]] core::Result<std::vector<avatar::AvatarId>> list()
        const override;

private:
    std::filesystem::path avatarsRoot_;
    std::optional<AvatarSpecDirectoryIdentity> rootIdentity_;
    int rootDescriptor_{-1};
    void* rootHandle_{nullptr};
};

}  // namespace creator::project_store
