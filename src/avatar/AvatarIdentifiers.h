#pragma once

#include "domain/Identifiers.h"

namespace creator::avatar {

struct AvatarIdTag;
struct AvatarAssetIdTag;
struct AvatarPackageIdTag;

using AvatarId = domain::Identifier<AvatarIdTag>;
using AvatarAssetId = domain::Identifier<AvatarAssetIdTag>;
using AvatarPackageId = domain::Identifier<AvatarPackageIdTag>;

}  // namespace creator::avatar
