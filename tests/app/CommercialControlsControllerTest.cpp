#include "app/CommercialControlsController.h"

#include "platform_release/EntitlementPolicy.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace {
namespace fs = std::filesystem;
using creator::app::CommercialControlsController;
using creator::platform_release::EntitlementDecision;
using creator::platform_release::EntitlementState;

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

class CommercialControlsControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("cs_commercial_controls_" +
                 std::to_string(QCoreApplication::applicationPid()));
        accountRoot_ = root_ / "account-state";
        projectRoot_ = root_ / "projects";
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(accountRoot_);
        fs::create_directories(projectRoot_);
        std::ofstream{projectRoot_ / "lesson.cstudio"} << "project data";
    }

    void TearDown() override {
        controller_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void open() {
        controller_ = std::make_unique<CommercialControlsController>(
            root_, accountRoot_,
            EntitlementDecision{.state = EntitlementState::Active,
                                .commercialFeaturesAllowed = true,
                                .reason = "community-development-build"});
        ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    }

    fs::path root_;
    fs::path accountRoot_;
    fs::path projectRoot_;
    std::unique_ptr<CommercialControlsController> controller_;
};

TEST_F(CommercialControlsControllerTest, ConsentDefaultsFalseAndStateHasReason) {
    open();
    EXPECT_FALSE(controller_->diagnosticsConsent());
    EXPECT_EQ(controller_->entitlementState(), QStringLiteral("active"));
    EXPECT_EQ(controller_->entitlementReason(),
              QStringLiteral("community-development-build"));
}

TEST_F(CommercialControlsControllerTest, SignOutRemovesSessionButPreservesProjects) {
    std::ofstream{accountRoot_ / "session.json"} << "session";
    std::ofstream{accountRoot_ / "entitlement-state.json"} << "entitlement";
    open();

    controller_->signOut();

    ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    EXPECT_FALSE(fs::exists(accountRoot_ / "session.json"));
    EXPECT_FALSE(fs::exists(accountRoot_ / "entitlement-state.json"));
    EXPECT_TRUE(fs::exists(projectRoot_ / "lesson.cstudio"));
}

TEST_F(CommercialControlsControllerTest, DeletionWithoutConfirmationIsRejected) {
    std::ofstream{accountRoot_ / "session.json"} << "session";
    open();

    controller_->deleteLocalAccountData(false);

    EXPECT_FALSE(controller_->busy());
    EXPECT_TRUE(controller_->statusMessage().contains(
        QStringLiteral("confirm"), Qt::CaseInsensitive));
    EXPECT_TRUE(fs::exists(accountRoot_ / "session.json"));
}

TEST_F(CommercialControlsControllerTest, ConfirmedDeletionTargetsOnlyAccountRoot) {
    std::ofstream{accountRoot_ / "session.json"} << "session";
    std::ofstream{root_ / "outside.txt"} << "outside";
    open();
    controller_->setDiagnosticsConsent(true);
    ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    ASSERT_TRUE(controller_->diagnosticsConsent());

    controller_->deleteLocalAccountData(true);

    ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    EXPECT_FALSE(fs::exists(accountRoot_));
    EXPECT_FALSE(controller_->diagnosticsConsent());
    EXPECT_TRUE(fs::exists(projectRoot_ / "lesson.cstudio"));
    EXPECT_TRUE(fs::exists(root_ / "outside.txt"));
}

}  // namespace
