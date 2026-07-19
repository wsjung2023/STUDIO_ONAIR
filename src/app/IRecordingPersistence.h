#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/RecordingSession.h"

#include <functional>
#include <string>

namespace creator::app {

class IRecordingPersistence {
public:
    using Completion = std::function<void(core::Result<void>)>;

    virtual ~IRecordingPersistence() = default;
    virtual void begin(const domain::SessionId& sessionId, core::TimestampNs startedAt,
                       Completion completion) = 0;
    virtual void complete(const domain::RecordingSession& session,
                          Completion completion) = 0;
    virtual void abort(const domain::SessionId& sessionId, std::string reason,
                       Completion completion) = 0;

protected:
    IRecordingPersistence() = default;
};

}  // namespace creator::app
