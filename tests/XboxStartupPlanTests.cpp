#include "Tools/XboxStartupPlan.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <string>

namespace x = AnyFSE::Tools::XboxStartup;
void Put16(x::Bytes& data, std::size_t off, unsigned value)
{
    data[off] = static_cast<std::uint8_t>(value);
    data[off + 1] = static_cast<std::uint8_t>(value >> 8);
}
void Put32(x::Bytes& data, std::size_t off, unsigned value)
{
    Put16(data, off, value);
    Put16(data, off + 2, value >> 16);
}
void Put(x::Bytes& data, std::size_t off, std::initializer_list<std::uint8_t> bytes)
{
    std::copy(bytes.begin(), bytes.end(), data.begin() + off);
}
x::Bytes Image()
{
    x::Bytes data(4096);
    Put16(data, 0, 0x5A4D);
    Put32(data, 0x3C, 0x80);
    Put32(data, 0x80, 0x4550);
    Put16(data, 0x84, 0x8664);
    Put16(data, 0x86, 2);
    Put16(data, 0x94, 0xF0);
    Put16(data, 0x98, 0x20B);
    Put32(data, 0x98 + 108, 16);
    Put32(data, 0x98 + 112, 0x2000);
    Put32(data, 0x98 + 116, 0x60);
    const auto text = 0x188, other = text + 40;
    Put32(data, text + 12, 0x1000);
    Put32(data, text + 16, 0x600);
    Put32(data, text + 20, 0x200);
    Put32(data, text + 36, 0x60000020);
    Put32(data, other + 12, 0x2000);
    Put32(data, other + 16, 0x600);
    Put32(data, other + 20, 0x800);
    Put32(data, other + 36, 0x40000040);
    Put32(data, 0x800 + 20, 3);
    Put32(data, 0x800 + 24, 3);
    Put32(data, 0x800 + 28, 0x20A0);
    Put32(data, 0x800 + 32, 0x2080);
    Put32(data, 0x800 + 36, 0x2090);
    const char *names[] = {"IsGamingFullScreenExperienceSupported", "CanSetGamingFullScreenExperience", "SetGamingFullScreenExperience"};
    const unsigned functions[] = {0x1020, 0x1040, 0x1080};
    std::size_t name = 0x900;
    for (unsigned i = 0; i < 3; ++i)
    {
        Put32(data, 0x880 + i * 4, static_cast<unsigned>(0x2000 + name - 0x800));
        Put16(data, 0x890 + i * 2, i);
        Put32(data, 0x8A0 + i * 4, functions[i]);
        const std::string textName = names[i];
        std::copy(textName.begin(), textName.end(), data.begin() + name);
        name += textName.size() + 1;
    }
    return data;
}
void Check(bool valid, const char *name)
{
    if (!valid) throw std::runtime_error(name);
}
void Reject(const std::function<void()>& operation, const char *name)
{
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, name);
}
int main()
{
    try
    {
        auto game = Image();
        Put(game, 0x220, {0x48, 0x89, 0x5C, 0x24, 8, 0x48});
        Put(game, 0x240, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57});
        Put(game, 0x290, {0x84, 0xC0, 0x0F, 0x84, 0x12, 0x34, 0, 0});
        const auto plan = x::BuildPlan(x::Target::GameMode, game);
        Check(plan.size() == 3, "Resolve all three gaming gates");
        const auto patched = x::PatchImage(x::Target::GameMode, game);
        Check(game[0x220] == 0x48 && patched[0x220] == 0xB8 && patched[0x225] == 0xC3, "Patch without changing input");
        Check(patched[0x292] == 0x90 && patched[0x297] == 0x90, "Remove home-app branch");
        Check(x::PatchImage(x::Target::GameMode, patched) == patched, "Apply is idempotent");
        const auto patchedPlan = x::BuildPlan(x::Target::GameMode, patched);
        Check(std::all_of(patchedPlan.begin(), patchedPlan.end(), [](const x::Site& s) { return s.state == x::State::Patched; }), "Detect enabled state");
        for (std::size_t i = 0; i < game.size(); ++i)
        {
            bool edited = i >= 0xD8 && i < 0xDC; // PE checksum
            for (const auto& site : plan) edited |= i >= site.offset && i < site.offset + site.replacement.size();
            if (!edited) Check(game[i] == patched[i], "Preserve all unrelated image bytes");
        }
        auto drift = game;
        drift[0x220] = 0xCC;
        Reject([&] { x::PatchImage(x::Target::GameMode, drift); }, "Reject unknown export prologue");
        auto ambiguous = game;
        Put(ambiguous, 0x2B0, {0x84, 0xC0, 0x0F, 0x84, 0, 0, 0, 0});
        Reject([&] { x::BuildPlan(x::Target::GameMode, ambiguous); }, "Reject ambiguous setter gate");
        auto forwarded = game;
        Put32(forwarded, 0x8A0, 0x2000);
        Reject([&] { x::BuildPlan(x::Target::GameMode, forwarded); }, "Reject forwarded export");
        auto badOrdinal = game;
        Put16(badOrdinal, 0x890, 20);
        Reject([&] { x::BuildPlan(x::Target::GameMode, badOrdinal); }, "Reject invalid export ordinal");
        auto wrongMachine = game;
        Put16(wrongMachine, 0x84, 0x14C);
        Reject([&] { x::BuildPlan(x::Target::GameMode, wrongMachine); }, "Reject x86 image");
        auto settings = Image();
        Put(settings, 0x320, {0x83, 0x7C, 0x24, 0x40, 0x2E, 0x0F, 0x94, 0xC1});
        Put(settings, 0xA00, {0x83, 0x7C, 0x24, 0x40, 0x2E, 0x0F, 0x94, 0xC0});
        const auto patchedSettings = x::PatchImage(x::Target::Shell, settings);
        Check(patchedSettings[0x320] == 0xB1 && patchedSettings[0x321] == 1, "Preserve selected byte register");
        Check(patchedSettings[0xA00] == settings[0xA00], "Ignore matching bytes in nonexecutable sections");
        Check(x::PatchImage(x::Target::Shell, patchedSettings) == patchedSettings, "Handheld patch idempotence");
        auto shell = settings;
        Put(shell, 0x360, {0x83, 0x7C, 0x24, 0x38, 0x2E, 0x0F, 0x94, 0xC2});
        Check(x::BuildPlan(x::Target::Settings, shell).size() == 2, "Resolve Settings checks");
        Reject([&] { x::BuildPlan(x::Target::Settings, settings); }, "Reject unexpected Settings-site count");
        for (const std::size_t length : {0u, 1u, 63u, 127u, 255u, 511u})
        {
            const x::Bytes truncated(game.begin(), game.begin() + length);
            Reject([&] { x::BuildPlan(x::Target::GameMode, truncated); }, "Reject truncated PE");
        }
        std::cout << "Xbox startup patch-plan regression checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
