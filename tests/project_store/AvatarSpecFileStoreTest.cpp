#include "project_store/AvatarSpecFileStore.h"

#include "avatar/AvatarSpecCodec.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

using creator::avatar::AssetRef;
using creator::avatar::AvatarAssetId;
using creator::avatar::AvatarId;
using creator::avatar::AvatarRepresentation;
using creator::avatar::AvatarSlot;
using creator::avatar::AvatarSpec;
using creator::avatar::AvatarSpecCodec;
using creator::avatar::AvatarSpecDraft;
using creator::avatar::RigFamily;
using creator::core::ErrorCode;
using creator::project_store::AvatarSpecFileStore;

AvatarSpec validSpec(std::string id = "hero", std::string name = "Hero") {
    AvatarSpecDraft draft{
        .avatarId = AvatarId::create(std::move(id)).value(),
        .displayName = std::move(name),
        .rigFamily = RigFamily::Humanoid,
        .speciesFamily = "human",
        .styleTheme = "studio",
        .preferredRepresentation = AvatarRepresentation::Inochi2d,
        .bodyMorphs = {},
        .faceMorphs = {},
        .animalMorphs = {},
        .slots = {
            {AvatarSlot::Body,
             AssetRef{AvatarAssetId::create("body").value(), "1.0.0", "default"}},
            {AvatarSlot::Head,
             AssetRef{AvatarAssetId::create("head").value(), "1.0.0", "default"}},
            {AvatarSlot::Eyes,
             AssetRef{AvatarAssetId::create("eyes").value(), "1.0.0", "default"}},
            {AvatarSlot::Mouth,
             AssetRef{AvatarAssetId::create("mouth").value(), "1.0.0", "default"}},
        },
        .palette = {},
        .materials = {},
        .expressions = {},
        .physics = {},
        .trackingProfileId = "default",
    };
    return AvatarSpec::create(std::move(draft)).value();
}

class AvatarSpecFileStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() /
                ("cs_avatar_spec_store_" + std::string{info->name()});
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        ASSERT_TRUE(fs::create_directories(root_ / "avatars"));
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    fs::path primary(std::string_view id = "hero") const {
        return root_ / "avatars" / id / "avatar.json";
    }

    fs::path backup(std::string_view id = "hero") const {
        return root_ / "avatars" / id / "avatar.last-good.json";
    }

    void write(const fs::path& path, std::string_view contents) {
        fs::create_directories(path.parent_path());
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output << contents;
        ASSERT_TRUE(output.good());
    }

    fs::path root_;
};

TEST_F(AvatarSpecFileStoreTest, SavesLoadsAndKeepsLastGoodCopy) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto spec = validSpec();

    const auto saved = store.save(spec);
    ASSERT_TRUE(saved.hasValue()) << saved.error().message();
    EXPECT_EQ(AvatarSpecCodec{}.toJson(store.load(spec.avatarId()).value()),
              AvatarSpecCodec{}.toJson(spec));
    write(primary(), "{broken");

    const auto recovered = store.load(spec.avatarId());
    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(AvatarSpecCodec{}.toJson(recovered.value()),
              AvatarSpecCodec{}.toJson(spec));
}

TEST_F(AvatarSpecFileStoreTest, RecoveryKeepsPreviousSuccessfulRevision) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto first = validSpec("hero", "First");
    const auto second = validSpec("hero", "Second");
    ASSERT_TRUE(store.save(first).hasValue());
    ASSERT_TRUE(store.save(second).hasValue());
    write(primary(), "{}");

    const auto recovered = store.load(first.avatarId());

    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(recovered.value().displayName(), "First");
}

TEST_F(AvatarSpecFileStoreTest, ListsOnlyValidStoredAvatarDirectoriesInStableOrder) {
    AvatarSpecFileStore store{root_ / "avatars"};
    ASSERT_TRUE(store.save(validSpec("zebra", "Zebra")).hasValue());
    ASSERT_TRUE(store.save(validSpec("alpha", "Alpha")).hasValue());
    write(root_ / "avatars" / "junk" / "note.txt", "not an avatar");

    const auto listed = store.list();

    ASSERT_TRUE(listed.hasValue()) << listed.error().message();
    ASSERT_EQ(listed.value().size(), 2U);
    EXPECT_EQ(listed.value()[0].value(), "alpha");
    EXPECT_EQ(listed.value()[1].value(), "zebra");
}

TEST_F(AvatarSpecFileStoreTest, FreshStoreListIsEmpty) {
    AvatarSpecFileStore store{root_ / "avatars"};

    const auto listed = store.list();

    ASSERT_TRUE(listed.hasValue()) << listed.error().message();
    EXPECT_TRUE(listed.value().empty());
}

TEST_F(AvatarSpecFileStoreTest, RejectsIdentifiersThatCanEscapeOrAliasAPath) {
    AvatarSpecFileStore store{root_ / "avatars"};
    for (const std::string id : {"..", "../outside", R"(..\outside)", "/absolute",
                                 R"(C:\absolute)", ".", "a/b", R"(a\b)", "a:b",
                                 "CON", "hero.", "Hero"}) {
        const auto result = store.save(validSpec(id, "Unsafe"));
        ASSERT_FALSE(result.hasValue()) << id;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << id;
    }
    EXPECT_FALSE(fs::exists(root_ / "outside"));
}

TEST_F(AvatarSpecFileStoreTest, RejectsMissingOrReplacedAvatarRoot) {
    const fs::path avatarRoot = root_ / "avatars";
    AvatarSpecFileStore store{avatarRoot};
    std::error_code removalError;
    if (!fs::remove(avatarRoot, removalError)) {
        EXPECT_TRUE(store.save(validSpec()).hasValue());
        return;
    }
    ASSERT_TRUE(fs::create_directory(root_ / "replacement"));
    std::error_code error;
    fs::create_directory_symlink(root_ / "replacement", avatarRoot, error);
    if (error) GTEST_SKIP() << "directory symlink unavailable: " << error.message();

    const auto result = store.save(validSpec());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(root_ / "replacement" / "hero"));
}

TEST_F(AvatarSpecFileStoreTest, ConstructorOnMissingRootStaysFailClosed) {
    const fs::path missing = root_ / "missing";
    AvatarSpecFileStore store{missing};
    ASSERT_TRUE(fs::create_directory(missing));

    EXPECT_FALSE(store.save(validSpec()).hasValue());
    EXPECT_FALSE(store.list().hasValue());
    EXPECT_FALSE(fs::exists(missing / "hero"));
}

TEST_F(AvatarSpecFileStoreTest, RecreatedRootWithSamePathStaysFailClosed) {
    const fs::path avatarRoot = root_ / "avatars";
    AvatarSpecFileStore store{avatarRoot};
    std::error_code removalError;
    if (!fs::remove(avatarRoot, removalError)) {
        EXPECT_TRUE(store.save(validSpec()).hasValue());
        return;
    }
    ASSERT_TRUE(fs::create_directory(avatarRoot));

    EXPECT_FALSE(store.save(validSpec()).hasValue());
    EXPECT_FALSE(fs::exists(avatarRoot / "hero"));
}

TEST_F(AvatarSpecFileStoreTest, RejectsAvatarDirectoryReplacedBySymlinkBeforeEveryOperation) {
    const fs::path outside = root_ / "outside";
    ASSERT_TRUE(fs::create_directory(outside));
    AvatarSpecFileStore store{root_ / "avatars"};
    ASSERT_TRUE(store.save(validSpec()).hasValue());
    ASSERT_TRUE(fs::remove_all(root_ / "avatars" / "hero") > 0);
    std::error_code error;
    fs::create_directory_symlink(outside, root_ / "avatars" / "hero", error);
    if (error) GTEST_SKIP() << "directory symlink unavailable: " << error.message();

    EXPECT_FALSE(store.save(validSpec("hero", "Changed")).hasValue());
    EXPECT_FALSE(store.load(AvatarId::create("hero").value()).hasValue());
    EXPECT_FALSE(store.list().hasValue());
    EXPECT_FALSE(fs::exists(outside / "avatar.json"));
}

TEST_F(AvatarSpecFileStoreTest, RejectsMalformedAndOversizedPrimaryWhenNoRecoveryExists) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto id = AvatarId::create("hero").value();
    write(primary(), "{broken");
    auto malformed = store.load(id);
    ASSERT_FALSE(malformed.hasValue());
    EXPECT_EQ(malformed.error().code(), ErrorCode::ParseFailure);

    write(primary(), std::string((8U * 1024U * 1024U) + 1U, 'x'));
    auto oversized = store.load(id);
    ASSERT_FALSE(oversized.hasValue());
    EXPECT_EQ(oversized.error().code(), ErrorCode::ParseFailure);
}

TEST_F(AvatarSpecFileStoreTest, FailsWhenPrimaryAndLastGoodAreBothCorrupt) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto id = AvatarId::create("hero").value();
    write(primary(), "{broken");
    write(backup(), "[]");

    const auto loaded = store.load(id);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(AvatarSpecFileStoreTest, DoesNotRecoverAFileForTheWrongAvatarId) {
    AvatarSpecFileStore store{root_ / "avatars"};
    ASSERT_TRUE(store.save(validSpec()).hasValue());
    write(primary(), AvatarSpecCodec{}.toJson(validSpec("other", "Other")).dump());

    const auto loaded = store.load(AvatarId::create("hero").value());

    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value().avatarId().value(), "hero");
}

TEST_F(AvatarSpecFileStoreTest, FailsWhenBothCopiesClaimAnotherAvatarId) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto id = AvatarId::create("hero").value();
    const std::string other =
        AvatarSpecCodec{}.toJson(validSpec("other", "Other")).dump();
    write(primary(), other);
    write(backup(), other);

    const auto loaded = store.load(id);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(AvatarSpecFileStoreTest, RejectsExternalHardLinksForBothStoredCopies) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto id = AvatarId::create("hero").value();
    const fs::path outside = root_ / "outside.json";
    write(outside, AvatarSpecCodec{}.toJson(validSpec()).dump());
    fs::create_directories(primary().parent_path());
    std::error_code error;
    fs::create_hard_link(outside, primary(), error);
    ASSERT_FALSE(error) << error.message();
    fs::create_hard_link(outside, backup(), error);
    ASSERT_FALSE(error) << error.message();

    const auto loaded = store.load(id);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(AvatarSpecFileStoreTest, UnsafePrimaryHardLinkDoesNotFallBackToValidLastGood) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto id = AvatarId::create("hero").value();
    ASSERT_TRUE(store.save(validSpec()).hasValue());
    const fs::path outside = root_ / "outside.json";
    write(outside, AvatarSpecCodec{}.toJson(validSpec()).dump());
    ASSERT_TRUE(fs::remove(primary()));
    std::error_code error;
    fs::create_hard_link(outside, primary(), error);
    ASSERT_FALSE(error) << error.message();

    const auto loaded = store.load(id);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(AvatarSpecFileStoreTest, RejectsCaseAliasBeforeCreatingAvatarDirectory) {
    ASSERT_TRUE(fs::create_directory(root_ / "avatars" / "Hero"));
    AvatarSpecFileStore store{root_ / "avatars"};

    const auto saved = store.save(validSpec("hero", "Hero"));

    ASSERT_FALSE(saved.hasValue());
    EXPECT_EQ(saved.error().code(), ErrorCode::InvalidArgument);
    std::vector<std::string> children;
    for (const auto& entry : fs::directory_iterator(root_ / "avatars")) {
        children.push_back(entry.path().filename().string());
    }
    EXPECT_EQ(children, std::vector<std::string>{"Hero"});
}

TEST_F(AvatarSpecFileStoreTest, OversizedSavePreservesPriorPrimaryAndLastGood) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto original = validSpec("hero", "Original");
    ASSERT_TRUE(store.save(original).hasValue());
    auto oversizedDraft = validSpec("hero", "Oversized").values();
    for (std::size_t index = 0; index < 48'000U; ++index) {
        oversizedDraft.palette.emplace(
            "color-" + std::to_string(index) + std::string(170U, 'x'),
            creator::avatar::ColorRgba{.red = 0.1F, .green = 0.2F,
                                       .blue = 0.3F, .alpha = 1.0F});
    }
    const auto oversized = AvatarSpec::create(std::move(oversizedDraft));
    ASSERT_TRUE(oversized.hasValue()) << oversized.error().message();

    const auto saved = store.save(oversized.value());

    ASSERT_FALSE(saved.hasValue());
    EXPECT_EQ(saved.error().code(), ErrorCode::InvalidArgument);
    const auto loaded = store.load(original.avatarId());
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value().displayName(), "Original");
}

TEST_F(AvatarSpecFileStoreTest, RejectsExternalSymlinksForBothStoredCopies) {
    AvatarSpecFileStore store{root_ / "avatars"};
    const auto id = AvatarId::create("hero").value();
    const fs::path outside = root_ / "outside.json";
    write(outside, AvatarSpecCodec{}.toJson(validSpec()).dump());
    fs::create_directories(primary().parent_path());
    std::error_code error;
    fs::create_symlink(outside, primary(), error);
    if (error) GTEST_SKIP() << "file symlink unavailable: " << error.message();
    fs::create_symlink(outside, backup(), error);
    ASSERT_FALSE(error) << error.message();

    const auto loaded = store.load(id);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(AvatarSpecFileStoreTest, FailedSavePreservesPreviouslyReadableRevisionAndLeavesNoPartFile) {
    AvatarSpecFileStore store{root_ / "avatars"};
    ASSERT_TRUE(store.save(validSpec("hero", "Original")).hasValue());
    ASSERT_TRUE(fs::remove(primary()));
    ASSERT_TRUE(fs::create_directory(primary()));

    const auto result = store.save(validSpec("hero", "Changed"));

    ASSERT_FALSE(result.hasValue());
    const auto loaded = store.load(AvatarId::create("hero").value());
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value().displayName(), "Original");
    for (const auto& entry : fs::directory_iterator{primary().parent_path()}) {
        EXPECT_EQ(entry.path().filename().string().find(".part-"), std::string::npos);
    }
}

}  // namespace
