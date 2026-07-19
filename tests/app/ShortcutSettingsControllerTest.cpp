#include "app/ShortcutSettingsController.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace {

using creator::app::ShortcutSettingsController;
namespace fs = std::filesystem;

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

class ShortcutSettingsControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                (fs::path{u8"creator-studio-단축키-설정-"} /
                 std::to_wstring(QCoreApplication::applicationPid()));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_);
        settingsPath_ = root_ / fs::path{u8"단축키.ini"};
    }

    void TearDown() override {
        controller_.reset();
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void open(const fs::path& path) {
        controller_ = std::make_unique<ShortcutSettingsController>(path);
        ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    }

    fs::path root_;
    fs::path settingsPath_;
    std::unique_ptr<ShortcutSettingsController> controller_;
};

TEST_F(ShortcutSettingsControllerTest, PublishesDocumentedDefaults) {
    open(settingsPath_);

    EXPECT_EQ(controller_->recordShortcut(), QStringLiteral("Ctrl+Shift+R"));
    EXPECT_EQ(controller_->markerShortcut(), QStringLiteral("M"));
    EXPECT_EQ(controller_->previousSceneShortcut(), QStringLiteral("Ctrl+PgUp"));
    EXPECT_EQ(controller_->nextSceneShortcut(), QStringLiteral("Ctrl+PgDown"));
    EXPECT_EQ(controller_->scene1Shortcut(), QStringLiteral("Ctrl+1"));
    EXPECT_EQ(controller_->scene5Shortcut(), QStringLiteral("Ctrl+5"));
    EXPECT_EQ(controller_->scene9Shortcut(), QStringLiteral("Ctrl+9"));
    EXPECT_TRUE(controller_->statusMessage().isEmpty());
}

TEST_F(ShortcutSettingsControllerTest, PersistsCanonicalSequenceAtUnicodePath) {
    open(settingsPath_);
    QSignalSpy changed{controller_.get(),
                       &ShortcutSettingsController::shortcutsChanged};

    controller_->setShortcut(QStringLiteral("marker"),
                             QStringLiteral("ctrl+alt+m"));

    ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    EXPECT_EQ(controller_->markerShortcut(), QStringLiteral("Ctrl+Alt+M"));
    EXPECT_EQ(changed.count(), 1);
    controller_.reset();

    open(settingsPath_);
    EXPECT_EQ(controller_->markerShortcut(), QStringLiteral("Ctrl+Alt+M"));
}

TEST_F(ShortcutSettingsControllerTest, ReopensValidMultiActionReallocation) {
    open(settingsPath_);

    controller_->setShortcut(QStringLiteral("scene1"),
                             QStringLiteral("Alt+1"));
    ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    controller_->setShortcut(QStringLiteral("record"),
                             QStringLiteral("Ctrl+1"));
    ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    controller_.reset();

    open(settingsPath_);
    EXPECT_EQ(controller_->recordShortcut(), QStringLiteral("Ctrl+1"));
    EXPECT_EQ(controller_->scene1Shortcut(), QStringLiteral("Alt+1"));
}

TEST_F(ShortcutSettingsControllerTest, RejectsEmptyInvalidUnknownAndDuplicate) {
    open(settingsPath_);
    const QString original = controller_->markerShortcut();

    for (const auto& [actionId, sequence] :
         std::initializer_list<std::pair<QString, QString>>{
             {QStringLiteral("marker"), QString{}},
             {QStringLiteral("marker"), QStringLiteral("Ctrl+NotAKey")},
             {QStringLiteral("not-an-action"), QStringLiteral("Ctrl+Alt+K")},
             {QStringLiteral("marker"), QStringLiteral("Ctrl+Shift+R")}}) {
        controller_->setShortcut(actionId, sequence);
        EXPECT_FALSE(controller_->busy());
        EXPECT_FALSE(controller_->statusMessage().isEmpty());
        EXPECT_EQ(controller_->markerShortcut(), original);
    }
}

TEST_F(ShortcutSettingsControllerTest, RejectsEveryApplicationReservedSequence) {
    open(settingsPath_);
    const QString original = controller_->markerShortcut();

    for (const QString& sequence :
         {QStringLiteral("Ctrl+Q"), QStringLiteral("Ctrl+S"),
          QStringLiteral("Ctrl+Z"), QStringLiteral("Ctrl+Shift+Z"),
          QStringLiteral("Ctrl+O"), QStringLiteral("Ctrl+N")}) {
        controller_->setShortcut(QStringLiteral("marker"), sequence);
        EXPECT_FALSE(controller_->busy()) << sequence.toStdString();
        EXPECT_TRUE(controller_->statusMessage().contains(
            QStringLiteral("reserved"), Qt::CaseInsensitive));
        EXPECT_EQ(controller_->markerShortcut(), original);
    }
}

TEST_F(ShortcutSettingsControllerTest, FailedAsyncWriteDoesNotPublishValue) {
    const fs::path blocker = root_ / "not-a-directory";
    std::ofstream{blocker}.put('x');
    open(blocker / "shortcuts.ini");
    const QString original = controller_->markerShortcut();
    QSignalSpy changed{controller_.get(),
                       &ShortcutSettingsController::shortcutsChanged};

    controller_->setShortcut(QStringLiteral("marker"),
                             QStringLiteral("Ctrl+Alt+M"));

    ASSERT_TRUE(waitUntil([this] { return !controller_->busy(); }));
    EXPECT_EQ(controller_->markerShortcut(), original);
    EXPECT_EQ(changed.count(), 0);
    EXPECT_TRUE(controller_->statusMessage().contains(
        QStringLiteral("save"), Qt::CaseInsensitive));
}

TEST_F(ShortcutSettingsControllerTest, DestructionWaitsForSettingsWorker) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto controller =
            std::make_unique<ShortcutSettingsController>(settingsPath_);
        ASSERT_TRUE(waitUntil([&controller] { return !controller->busy(); }));
        controller->setShortcut(QStringLiteral("marker"),
                                attempt % 2 == 0 ? QStringLiteral("Ctrl+Alt+M")
                                                 : QStringLiteral("Ctrl+Alt+K"));
        controller.reset();
    }
    SUCCEED();
}

}  // namespace
