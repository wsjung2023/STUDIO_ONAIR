#pragma once

#include "project_store/IAvatarSpecStore.h"

#include <filesystem>
#include <memory>

namespace creator::project_store {

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
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

}  // namespace creator::project_store
