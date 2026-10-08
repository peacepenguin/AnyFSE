#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace AnyFSE::Tools::XboxStartup
{
    using Bytes = std::vector<std::uint8_t>;
    // SettingsEnvironment is optional: builds without its handheld check resolve to an empty (no-op) plan.
    enum class Target { GameMode, Settings, Shell, SettingsEnvironment };
    enum class State { Original, Patched };
    // Verified sites match a layout validated on real Windows builds. Unverified sites were found structurally on an unknown
    // layout and are only applied after the user explicitly confirms them.
    struct Site { std::size_t offset; Bytes replacement; State state; bool verified = true; std::string description; };
    // A device-form check located by following RtlGetDeviceFamilyInfoEnum's form output to its compare against the handheld value.
    struct Finding { std::size_t offset; bool patchable; std::string description; Bytes replacement; bool known = false; };
    std::vector<Finding> DiscoverHandheldChecks(const Bytes& image);
    std::vector<Site> BuildPlan(Target target, const Bytes& image);
    Bytes PatchImage(Target target, const Bytes& image, bool includeUnverified = false);
}
