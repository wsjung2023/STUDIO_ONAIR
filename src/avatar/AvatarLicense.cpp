#include "avatar/AvatarLicense.h"

namespace creator::avatar {

const AvatarRightsDecision& AvatarRightsMatrix::forUse(UseKind use) const {
    return byUse.at(use);
}

}  // namespace creator::avatar
