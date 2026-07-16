#include "capture/ICaptureSource.h"
#include "capture/IPullCaptureSource.h"
#include "project_store/IProjectStore.h"
#include "recorder/IRecorder.h"

#include <memory>
#include <string>
#include <type_traits>

namespace {

using creator::capture::ICaptureSource;
using creator::capture::IPullCaptureSource;
using creator::project_store::IProjectStore;
using creator::recorder::IRecorder;

// Pure interfaces have nothing to call, but they do carry contracts worth
// pinning down. If one of these breaks, the build should stop rather than a
// reviewer having to notice.

// Implementations own OS handles: a display capture holds a stream, a recorder
// holds file descriptors, a project store holds a handle to the file it is
// writing. Copying one would double-close the handle. They are always held by
// unique_ptr, never by value.
//
// is_copy_constructible_v/is_move_constructible_v are NOT the right trait to
// pin this down, even though `!is_copy_constructible_v<ICaptureSource>` reads
// naturally. Both ask "can a T be constructed", and constructing any object of
// an abstract type is what the language forbids (a class stays constructible
// as a base subobject of a concrete derived class either way) - so both report
// false for an abstract class regardless of whether copy/move are explicitly
// deleted. Verified independently: an abstract class with a pure virtual and a
// protected default ctor, but with copy/move left untouched (nothing deleted),
// still reports is_copy_constructible_v/is_move_constructible_v as false. The
// assertion would hold even if the `= delete` lines below were never written.
//
// is_copy_assignable_v/is_move_assignable_v do not have this problem:
// assignment acts through a reference to an already-existing object rather
// than constructing a new one, so it is never disqualified by abstractness.
// The same experiment confirms it tracks the actual `= delete`: the
// untouched-copy/move class reports assignable == true, and only the class
// that explicitly deletes the operator reports false. These are the traits
// that actually pin the design decision.
static_assert(!std::is_copy_assignable_v<ICaptureSource>,
              "capture sources own OS handles and must not be copied");
static_assert(!std::is_move_assignable_v<ICaptureSource>);
static_assert(!std::is_copy_assignable_v<IRecorder>,
              "recorders own file handles and must not be copied");
static_assert(!std::is_move_assignable_v<IRecorder>);
static_assert(!std::is_copy_assignable_v<IProjectStore>,
              "project stores own file handles and must not be copied");
static_assert(!std::is_move_assignable_v<IProjectStore>);

// Assignment and construction are different special members: deleting one
// does not guarantee the other is gone, and "a copy double-closes the OS
// handle" is triggered by copy-construction at least as much as by
// assignment. The abstract types themselves cannot answer this -
// is_copy_constructible_v is false for any abstract class whether or not the
// constructor is deleted, which is what made the original assertions here
// vacuous (see above). A concrete stub can answer it - the base's deleted
// copy constructor propagates, leaving the stub's own implicitly deleted.
class StubCaptureSource final : public creator::capture::ICaptureSource {
public:
    [[nodiscard]] creator::domain::SourceId id() const override {
        return creator::domain::SourceId::create("stub").value();
    }
    [[nodiscard]] std::string displayName() const override { return "stub"; }
    [[nodiscard]] creator::core::Result<void> start(
        const creator::capture::CaptureConfig&) override {
        return creator::core::ok();
    }
    [[nodiscard]] creator::core::Result<void> stop() override { return creator::core::ok(); }
    [[nodiscard]] creator::capture::CaptureStats stats() const noexcept override { return {}; }
};

static_assert(!std::is_abstract_v<StubCaptureSource>,
              "the stub must be concrete or it proves nothing");
static_assert(!std::is_copy_constructible_v<StubCaptureSource>,
              "a copied capture source would double-close its OS handle");
static_assert(!std::is_move_constructible_v<StubCaptureSource>);

// Destroyed through a base pointer, so the destructor must be virtual or the
// derived class's handles never get released.
static_assert(std::has_virtual_destructor_v<ICaptureSource>);
static_assert(std::has_virtual_destructor_v<IRecorder>);
static_assert(std::has_virtual_destructor_v<IProjectStore>);

// Nothing may be instantiated directly - these are ports, not classes.
static_assert(std::is_abstract_v<ICaptureSource>);
static_assert(std::is_abstract_v<IPullCaptureSource>);
static_assert(std::is_abstract_v<IRecorder>);
static_assert(std::is_abstract_v<IProjectStore>);

// A pull source is a capture source: the application can hold one and still
// start/stop it through the base port, with no downcast anywhere. That needs
// is_convertible_v<Derived*, Base*>, not is_base_of_v: is_base_of_v is true
// for ANY inheritance relationship - public, protected or private alike - so
// it would not notice if IPullCaptureSource inherited privately, which would
// make the very upcast this comment claims works fail to compile everywhere
// outside the class itself. is_convertible_v on the pointer types only holds
// when the base is publicly (accessibly) reachable, which is what "with no
// downcast anywhere" actually depends on.
static_assert(std::is_convertible_v<IPullCaptureSource*, ICaptureSource*>);
static_assert(std::has_virtual_destructor_v<IPullCaptureSource>);

// start() returning Result is only worth anything if dropping it is an error.
// [[nodiscard]] lives on Result itself, so this checks the return type is the
// one that carries it rather than a bool that silently discards why it failed.
static_assert(std::is_same_v<decltype(std::declval<ICaptureSource&>().start(
                                 std::declval<const creator::capture::CaptureConfig&>())),
                             creator::core::Result<void>>,
              "start() must report why it failed, not just that it did");
static_assert(std::is_same_v<decltype(std::declval<ICaptureSource&>().stop()),
                             creator::core::Result<void>>);
static_assert(std::is_same_v<decltype(std::declval<IPullCaptureSource&>().tick()),
                             creator::core::Result<creator::media::VideoFrame>>);

// Compiles only if the interfaces are usable the way the app uses them:
// unique_ptr<T>'s destructor instantiates default_delete<T>, which requires T
// to have an accessible destructor. That instantiation is the actual check;
// there is nothing to assert about it at runtime; a default-constructed
// unique_ptr is null by definition, and the four EXPECT_EQ(..., nullptr)
// calls previously here asserted exactly that and nothing more (the comment
// justifying their existence - "the suite needs one runtime test or the file
// links nothing into cs_tests" - was also false: a .cpp file listed directly
// on add_executable compiles and links whether or not it defines a TEST(), as
// MediaTypesTest.cpp in this same repo already correctly notes).
std::unique_ptr<ICaptureSource> gPortContractSource;
std::unique_ptr<IPullCaptureSource> gPortContractPullSource;
std::unique_ptr<IRecorder> gPortContractRecorder;
std::unique_ptr<IProjectStore> gPortContractStore;

}  // namespace
