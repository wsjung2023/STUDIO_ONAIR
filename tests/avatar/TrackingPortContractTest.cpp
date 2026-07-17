#include "avatar/ITrackingProvider.h"

#include <memory>
#include <type_traits>

namespace {

using creator::avatar::AvatarProviderId;
using creator::avatar::ITrackingProvider;
using creator::avatar::TrackingResult;

// Pure interface, nothing to call at runtime, but the contract is worth
// pinning at compile time the same way capture/PortContractTest.cpp pins
// ICaptureSource et al. Same discipline applies here: is_copy_constructible_v
// is vacuous on an abstract type (see that file's long comment) because
// constructing ANY object of an abstract type is what the language forbids,
// regardless of whether copy/move are explicitly deleted. Assignment does not
// have that problem, since it acts on an already-existing object rather than
// constructing one.
static_assert(!std::is_copy_assignable_v<ITrackingProvider>,
              "tracking providers own engine/model resources and must not be copied");
static_assert(!std::is_move_assignable_v<ITrackingProvider>);

// A concrete stub answers the constructor-deletion question a bare abstract
// type cannot: the base's deleted copy constructor propagates, leaving the
// stub's own implicitly deleted too.
class StubTrackingProvider final : public ITrackingProvider {
public:
    [[nodiscard]] AvatarProviderId providerId() const override {
        return AvatarProviderId::create("stub").value();
    }
    [[nodiscard]] creator::core::Result<TrackingResult> process(
        const creator::media::VideoFrame&) override {
        return TrackingResult{};
    }
};

static_assert(!std::is_abstract_v<StubTrackingProvider>,
              "the stub must be concrete or it proves nothing");
static_assert(!std::is_copy_constructible_v<StubTrackingProvider>,
              "a copied tracking provider would double-manage its engine resources");
static_assert(!std::is_move_constructible_v<StubTrackingProvider>);

// Destroyed through a base pointer (held as unique_ptr<ITrackingProvider>), so
// the destructor must be virtual or the derived engine's resources never get
// released.
static_assert(std::has_virtual_destructor_v<ITrackingProvider>);

// Not instantiable directly - this is a port, not a class.
static_assert(std::is_abstract_v<ITrackingProvider>);

// process() must report failure through Result, not silently return a default
// TrackingResult or throw across the port boundary.
static_assert(std::is_same_v<decltype(std::declval<ITrackingProvider&>().process(
                                 std::declval<const creator::media::VideoFrame&>())),
                             creator::core::Result<TrackingResult>>,
              "process() must report why it failed, not just that it did");

// Compiles only if the port is usable the way callers actually hold it:
// unique_ptr<T>'s destructor instantiates default_delete<T>, which requires T
// to have an accessible destructor. There is nothing further to assert about
// this at runtime.
std::unique_ptr<ITrackingProvider> gPortContractProvider;

}  // namespace
