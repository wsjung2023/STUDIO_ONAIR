#include "domain/StudioScene.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using creator::core::ErrorCode;
using creator::domain::SceneId;
using creator::domain::SceneSource;
using creator::domain::SourceId;
using creator::domain::StudioScene;
using creator::domain::StudioSourceRole;
using creator::domain::VisualTransform;
using creator::domain::defaultStudioScenes;
using creator::domain::studioSourceRoleFromName;
using creator::domain::studioSourceRoleName;

SceneId sceneId(std::string value) {
    return SceneId::create(std::move(value)).value();
}

SourceId sourceId(std::string value) {
    return SourceId::create(std::move(value)).value();
}

VisualTransform fullFrame(std::int32_t zOrder = 0) {
    return VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, zOrder).value();
}

SceneSource source(std::string id, StudioSourceRole role,
                   std::int32_t position,
                   std::optional<VisualTransform> transform = std::nullopt,
                   bool enabled = true) {
    return SceneSource::create(sourceId(std::move(id)), role, "Source",
                               position, enabled, transform).value();
}

TEST(StudioSceneTest, RoleNamesRoundTripAndRejectUnknownValues) {
    for (const auto role : {StudioSourceRole::Screen, StudioSourceRole::Camera,
                            StudioSourceRole::Microphone,
                            StudioSourceRole::SystemAudio,
                            StudioSourceRole::Avatar}) {
        const auto restored = studioSourceRoleFromName(studioSourceRoleName(role));
        ASSERT_TRUE(restored.hasValue());
        EXPECT_EQ(restored.value(), role);
    }

    const auto unknown = studioSourceRoleFromName("unsupported");
    ASSERT_FALSE(unknown.hasValue());
    EXPECT_EQ(unknown.error().code(), ErrorCode::InvalidArgument);
}

TEST(StudioSceneTest, RejectsDuplicateRoleAndAudioTransform) {
    const auto screen = source("screen", StudioSourceRole::Screen, 0,
                               fullFrame());
    const auto duplicate = source("screen-2", StudioSourceRole::Screen, 1,
                                  fullFrame());
    EXPECT_FALSE(StudioScene::create(sceneId("scene"), "강의", 0,
                                     {screen, duplicate}).hasValue());

    const auto microphone = SceneSource::create(
        sourceId("mic"), StudioSourceRole::Microphone, "마이크", 0, true,
        fullFrame());
    EXPECT_FALSE(microphone.hasValue());
}

TEST(StudioSceneTest, RequiresEnabledVideoTransformAndBoundsPositions) {
    EXPECT_FALSE(SceneSource::create(sourceId("camera"),
                                     StudioSourceRole::Camera, "Camera", 0,
                                     true, std::nullopt).hasValue());
    EXPECT_TRUE(SceneSource::create(sourceId("camera"),
                                    StudioSourceRole::Camera, "Camera", 0,
                                    false, std::nullopt).hasValue());
    EXPECT_FALSE(SceneSource::create(sourceId("camera"),
                                     StudioSourceRole::Camera, "Camera", -1,
                                     false, std::nullopt).hasValue());
    EXPECT_FALSE(StudioScene::create(sceneId("scene"), "Scene", 1024, {})
                     .hasValue());
    EXPECT_TRUE(SceneSource::create(sourceId("avatar"),
                                    StudioSourceRole::Avatar, "Avatar", 0,
                                    true, fullFrame())
                    .hasValue());
}

TEST(StudioSceneTest, RejectsEmptyNamesAndDuplicateSourcePositions) {
    EXPECT_FALSE(SceneSource::create(sourceId("screen"),
                                     StudioSourceRole::Screen, "", 0, true,
                                     fullFrame()).hasValue());
    EXPECT_FALSE(StudioScene::create(sceneId("scene"), "", 0, {}).hasValue());

    const auto screen = source("screen", StudioSourceRole::Screen, 0,
                               fullFrame());
    const auto camera = source("camera", StudioSourceRole::Camera, 0,
                               fullFrame(10));
    EXPECT_FALSE(StudioScene::create(sceneId("scene"), "Scene", 0,
                                     {screen, camera}).hasValue());
}

TEST(StudioSceneTest, CreatesSourcesInStablePositionOrder) {
    const auto screen = source("screen", StudioSourceRole::Screen, 2,
                               fullFrame());
    const auto camera = source("camera", StudioSourceRole::Camera, 0,
                               fullFrame(10));
    const auto scene = StudioScene::create(sceneId("scene"), "Scene", 4,
                                           {screen, camera});

    ASSERT_TRUE(scene.hasValue());
    ASSERT_EQ(scene.value().sources().size(), 2U);
    EXPECT_EQ(scene.value().sources()[0].id(), camera.id());
    EXPECT_EQ(scene.value().sources()[1].id(), screen.id());
}

TEST(StudioSceneTest, ImmutableOperationsPreserveOriginalAndRevalidate) {
    const auto screen = source("screen", StudioSourceRole::Screen, 0,
                               fullFrame());
    const auto original = StudioScene::create(sceneId("scene"), "Original", 0,
                                              {screen}).value();
    const auto camera = source("camera", StudioSourceRole::Camera, 1,
                               fullFrame(10));

    const auto added = original.withAddedSource(camera);
    ASSERT_TRUE(added.hasValue());
    EXPECT_EQ(original.sources().size(), 1U);
    EXPECT_EQ(added.value().sources().size(), 2U);

    const auto renamed = added.value().withName("발표 장면");
    ASSERT_TRUE(renamed.hasValue());
    EXPECT_EQ(added.value().name(), "Original");
    EXPECT_EQ(renamed.value().name(), "발표 장면");

    const auto disabledCamera = camera.withEnabled(false);
    ASSERT_TRUE(disabledCamera.hasValue());
    const auto replaced = renamed.value().withSource(disabledCamera.value());
    ASSERT_TRUE(replaced.hasValue());
    EXPECT_FALSE(replaced.value().sources()[1].enabled());

    const auto reordered = replaced.value().withSourcePosition(camera.id(), 2);
    ASSERT_TRUE(reordered.hasValue());
    EXPECT_EQ(reordered.value().sources()[1].position(), 2);

    const auto removed = reordered.value().withoutSource(screen.id());
    ASSERT_TRUE(removed.hasValue());
    ASSERT_EQ(removed.value().sources().size(), 1U);
    EXPECT_EQ(removed.value().sources()[0].id(), camera.id());

    EXPECT_FALSE(original.withAddedSource(screen).hasValue());
    EXPECT_FALSE(original.withoutSource(sourceId("missing")).hasValue());
}

TEST(StudioSceneTest, PresetsUseEditorVisualTransformExactly) {
    const auto scenes = defaultStudioScenes();
    ASSERT_TRUE(scenes.hasValue());
    ASSERT_EQ(scenes.value().size(), 3U);
    EXPECT_EQ(scenes.value()[0].name(), "Presentation");
    EXPECT_EQ(scenes.value()[1].name(), "Screen");
    EXPECT_EQ(scenes.value()[2].name(), "Camera");
    ASSERT_EQ(scenes.value()[0].sources().size(), 4U);

    const auto expectedPip = VisualTransform::create(
        0.70, 0.05, 0.25, 0.25, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 10).value();
    EXPECT_EQ(scenes.value()[0].sources()[1].transform(), expectedPip);
    EXPECT_TRUE(scenes.value()[0].sources()[0].enabled());
    EXPECT_TRUE(scenes.value()[0].sources()[1].enabled());
    EXPECT_FALSE(scenes.value()[1].sources()[1].enabled());
    EXPECT_FALSE(scenes.value()[2].sources()[0].enabled());
    EXPECT_TRUE(scenes.value()[2].sources()[1].enabled());
}

}  // namespace
