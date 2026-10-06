#pragma once
#include <cstdint>
#include <vector>
namespace AnyFSE::Tools::XboxStartup
{
    using Bytes = std::vector<std::uint8_t>;
    enum class Target { GameMode, Settings, Shell };
    enum class State { Original, Patched };
    struct Site { std::size_t offset; Bytes replacement; State state; };
    std::vector<Site> BuildPlan(Target target, const Bytes& image);
    Bytes PatchImage(Target target, const Bytes& image);
}
