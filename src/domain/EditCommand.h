#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"

#include <memory>
#include <string>

namespace creator::domain {

class Timeline;

struct EditCommandRecord final {
    CommandId commandId;
    std::string type;
    std::string payload;
    std::string undoPayload;

    friend bool operator==(const EditCommandRecord&, const EditCommandRecord&) = default;
};

class IEditCommand {
public:
    [[nodiscard]] virtual core::Result<void> execute(Timeline& timeline) = 0;
    [[nodiscard]] virtual core::Result<void> undo(Timeline& timeline) = 0;
    [[nodiscard]] virtual EditCommandRecord record() const = 0;
    [[nodiscard]] virtual std::unique_ptr<IEditCommand> clone() const = 0;
    virtual ~IEditCommand() = default;
};

}  // namespace creator::domain
