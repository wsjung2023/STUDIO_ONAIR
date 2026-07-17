#pragma once

#include "edit_engine/IEditEngine.h"

#include <functional>
#include <memory>
#include <stop_token>

namespace creator::mlt_adapter {

class MltRenderJob final : public edit_engine::IRenderJob {
public:
    using ProgressReporter = std::function<bool(
        edit_engine::RenderJobState, double, core::TimestampNs)>;
    using Operation = std::function<core::Result<void>(
        const edit_engine::RenderRequest&, std::stop_token,
        const ProgressReporter&)>;

    [[nodiscard]] static core::Result<std::unique_ptr<edit_engine::IRenderJob>>
    start(edit_engine::RenderRequest request, Operation operation);

    ~MltRenderJob() override;

    [[nodiscard]] core::Result<edit_engine::RenderProgress> progress()
        const override;
    [[nodiscard]] core::Result<void> cancel() override;

    MltRenderJob(const MltRenderJob&) = delete;
    MltRenderJob& operator=(const MltRenderJob&) = delete;

private:
    class Impl;
    explicit MltRenderJob(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::mlt_adapter
