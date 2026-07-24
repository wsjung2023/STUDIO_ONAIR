#pragma once

#include "avatar/AvatarSpec.h"
#include "core/Result.h"

#include <vector>

namespace creator::project_store {

class IAvatarSpecStore {
public:
    virtual ~IAvatarSpecStore() = default;

    [[nodiscard]] virtual core::Result<void> save(
        const avatar::AvatarSpec& spec) = 0;
    [[nodiscard]] virtual core::Result<avatar::AvatarSpec> load(
        const avatar::AvatarId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<avatar::AvatarId>> list()
        const = 0;

protected:
    IAvatarSpecStore() = default;
};

}  // namespace creator::project_store
