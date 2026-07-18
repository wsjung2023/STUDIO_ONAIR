#include "app/ProjectExportEngine.h"

#include "core/AppError.h"
#include "core/Utc.h"
#include "mlt_adapter/MltEditEngine.h"
#include "project_store/PersistentRenderJobLifecycle.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/RenderJobRecovery.h"
#include "project_store/SqliteRenderJobStore.h"

#include <memory>
#include <utility>

namespace creator::app {
namespace {

core::AppError unsupported() {
    return core::AppError{core::ErrorCode::InvalidState,
                          "project export engine only supports render"};
}

}  // namespace

ProjectExportEngine::ProjectExportEngine(std::filesystem::path mltRuntimeRoot)
    : mltRuntimeRoot_(std::move(mltRuntimeRoot)) {}

core::Result<void> ProjectExportEngine::load(
    const edit_engine::TimelineSnapshot&) {
    return unsupported();
}
core::Result<void> ProjectExportEngine::update(
    const edit_engine::TimelineChangeSet&) {
    return unsupported();
}
core::Result<void> ProjectExportEngine::play() { return unsupported(); }
core::Result<void> ProjectExportEngine::pause() { return unsupported(); }
core::Result<void> ProjectExportEngine::seek(core::TimestampNs) {
    return unsupported();
}
core::Result<edit_engine::PreviewFrame> ProjectExportEngine::requestFrame(
    core::TimestampNs) {
    return unsupported();
}

core::Result<std::unique_ptr<edit_engine::IRenderJob>>
ProjectExportEngine::render(const edit_engine::RenderRequest& request) {
    project_store::ProjectPackageStore packages;
    auto opened = packages.open(request.snapshot().mediaRoot);
    if (!opened.hasValue()) return opened.error();
    if (opened.value().package.manifest.projectId != request.projectId()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "export request project identity changed"};
    }
    const auto lease = opened.value().databaseIdentityLease;
    if (!lease) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "validated export database identity is missing"};
    }
    auto sqlite = project_store::SqliteRenderJobStore::open(
        opened.value().databasePath, request.projectId(),
        [lease] { return lease->verifyCurrentIdentity(); });
    if (!sqlite.hasValue()) return sqlite.error();
    auto store = std::make_shared<project_store::SqliteRenderJobStore>(
        std::move(sqlite).value());
    auto recovered = project_store::RenderJobRecovery::recoverAll(
        *store, core::Utc::now());
    if (!recovered.hasValue()) return recovered.error();
    auto lifecycle =
        std::make_shared<project_store::PersistentRenderJobLifecycle>(store);
    mlt_adapter::MltEditEngine engine{{.runtimeRoot = mltRuntimeRoot_,
                                      .previewWidth = request.preset().width(),
                                      .previewHeight = request.preset().height(),
                                      .renderLifecycle = lifecycle}};
    return engine.render(request);
}

}  // namespace creator::app
