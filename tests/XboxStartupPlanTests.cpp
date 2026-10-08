#include "Tools/XboxStartupPlan.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <string>
#include <fstream>
#include <iterator>

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
    Put32(data, 0x98 + 136, 0x2300);
    Put32(data, 0x98 + 140, 12);
    Put32(data, 0xB00, 0x1080);
    Put32(data, 0xB04, 0x1100);
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
// Adds an ntdll import of RtlGetDeviceFamilyInfoEnum (IAT slot RVA 0x2450) and a function at RVA 0x1400 that queries the device
// form into [rsp+30h], compares it with the handheld form and consumes the result with `consumer`.
x::Bytes FormImage(std::initializer_list<std::uint8_t> consumer, std::initializer_list<std::uint8_t> argumentSetup = {0x33, 0xD2, 0x33, 0xC9})
{
    auto data = Image();
    Put32(data, 0x98 + 120, 0x2400);
    Put32(data, 0x98 + 124, 40);
    Put32(data, 0xC00, 0x2440);
    Put32(data, 0xC00 + 12, 0x2470);
    Put32(data, 0xC00 + 16, 0x2450);
    Put32(data, 0xC40, 0x2480);
    Put32(data, 0xC50, 0x2480);
    const std::string library = "ntdll.dll", function = "RtlGetDeviceFamilyInfoEnum";
    std::copy(library.begin(), library.end(), data.begin() + 0xC70);
    std::copy(function.begin(), function.end(), data.begin() + 0xC82);
    Put32(data, 0x98 + 140, 24);
    Put32(data, 0xB0C, 0x1400);
    Put32(data, 0xB10, 0x1440);
    Put(data, 0x600, {0x4C, 0x8D, 0x44, 0x24, 0x30});
    Put(data, 0x605, argumentSetup);
    Put(data, 0x609, {0x48, 0xFF, 0x15, 0x40, 0x10, 0, 0, 0x0F, 0x1F, 0x44, 0, 0, 0x83, 0x7C, 0x24, 0x30, 0x2E});
    Put(data, 0x61A, consumer);
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
int main(int argc, char **argv)
{
    try
    {
        // Optional read-only compatibility probe: pass GameMode, Settings and Shell DLL paths in that order.
        if (argc != 1)
        {
            Check(argc == 4, "Expected three DLL paths: gamemode, SettingsHandlers_Gaming, twinui.pcshell");
            const x::Target targets[] = {x::Target::GameMode, x::Target::Settings, x::Target::Shell};
            bool supported = true;
            for (int i = 1; i < argc; ++i)
            {
                try
                {
                    std::ifstream stream(argv[i], std::ios::binary);
                    Check(stream.good(), "Cannot open DLL");
                    const x::Bytes image((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                    Check(!stream.bad(), "Cannot read DLL");
                    const auto sites = x::BuildPlan(targets[i - 1], image);
                    std::cout << argv[i] << ": " << sites.size() << " recognized sites\n";
                    for (const auto& site : sites)
                        std::cout << "  +0x" << std::hex << site.offset << std::dec
                            << (site.state == x::State::Original ? " original" : " patched")
                            << (site.verified ? "" : " UNVERIFIED ") << site.description << '\n';
                    const auto patchedImage = x::PatchImage(targets[i - 1], image);
                    const auto patchedSites = x::BuildPlan(targets[i - 1], patchedImage);
                    Check(std::all_of(patchedSites.begin(), patchedSites.end(), [](const x::Site& site) { return site.state == x::State::Patched; }),
                        "In-memory patch must be recognized as fully patched");
                    Check(x::PatchImage(targets[i - 1], patchedImage) == patchedImage, "In-memory patch must be idempotent");
                    std::cout << "  in-memory apply/detect/reapply passed (file unchanged)\n";
                }
                catch (const std::exception& error)
                {
                    supported = false;
                    std::cerr << argv[i] << ": " << error.what() << '\n';
                }
            }
            return supported ? 0 : 1;
        }
        auto game = Image();
        Put(game, 0x220, {0x48, 0x89, 0x5C, 0x24, 8, 0x48});
        Put(game, 0x240, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57});
        Put(game, 0x290, {0x84, 0xC0, 0x0F, 0x84, 0x08, 0, 0, 0});
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
        auto landingPads = game;
        for (const std::size_t off : {0x220u, 0x240u})
        {
            std::copy_backward(landingPads.begin() + off, landingPads.begin() + off + 6, landingPads.begin() + off + 10);
            Put(landingPads, off, {0xF3, 0x0F, 0x1E, 0xFA});
        }
        const auto patchedPads = x::PatchImage(x::Target::GameMode, landingPads);
        for (const std::size_t off : {0x220u, 0x240u})
        {
            Check(std::equal(landingPads.begin() + off, landingPads.begin() + off + 4, patchedPads.begin() + off), "Preserve ENDBR64");
            Check(patchedPads[off + 4] == 0xB8 && patchedPads[off + 9] == 0xC3, "Patch after ENDBR64");
        }
        Check(x::PatchImage(x::Target::GameMode, patchedPads) == patchedPads, "Landing pad patch idempotence");
        auto boolGate = game;
        Put(boolGate, 0x220, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x0D, 0, 0, 0, 0,
            0xE8, 0, 0, 0, 0, 0x84, 0xC0, 0x74, 0x2B});
        Put(boolGate, 0x28B, {0xE8, 0x90, 0xFF, 0xFF, 0xFF, 0x85, 0xC0});
        const auto boolPlan = x::BuildPlan(x::Target::GameMode, boolGate);
        Check(boolPlan.size() == 3 && boolPlan[2].offset == 0x292, "Recognize support-export BOOL gate");
        const auto patchedBool = x::PatchImage(x::Target::GameMode, boolGate);
        Check(patchedBool[0x220] == 0xB8 && patchedBool[0x292] == 0x90, "Patch alternate prologue and BOOL gate");
        Check(x::PatchImage(x::Target::GameMode, patchedBool) == patchedBool, "Alternate layout patch is idempotent");
        const auto patchedBoolPlan = x::BuildPlan(x::Target::GameMode, patchedBool);
        Check(std::all_of(patchedBoolPlan.begin(), patchedBoolPlan.end(), [](const x::Site& s) { return s.state == x::State::Patched; }),
            "Detect alternate layout as fully patched");
        auto wrongCall = boolGate;
        Put32(wrongCall, 0x28C, 0);
        Reject([&] { x::BuildPlan(x::Target::GameMode, wrongCall); }, "Reject BOOL test of unrelated call");
        wrongCall = boolGate;
        wrongCall[0x28B] = 0x90;
        Reject([&] { x::BuildPlan(x::Target::GameMode, wrongCall); }, "Reject BOOL test without direct call");
        auto unknownSmallFrame = boolGate;
        unknownSmallFrame[0x22B] = 0xCC;
        Reject([&] { x::BuildPlan(x::Target::GameMode, unknownSmallFrame); }, "Do not accept arbitrary small-stack prologues");
        auto ambiguousBool = boolGate;
        Put(ambiguousBool, 0x2AB, {0xE8, 0x70, 0xFF, 0xFF, 0xFF, 0x85, 0xC0, 0x0F, 0x84, 0, 0, 0, 0});
        Reject([&] { x::BuildPlan(x::Target::GameMode, ambiguousBool); }, "Reject multiple support-export gates");
        auto bothGates = game;
        Put(bothGates, 0x2AB, {0xE8, 0x70, 0xFF, 0xFF, 0xFF, 0x85, 0xC0, 0x0F, 0x84, 0, 0, 0, 0});
        Check(x::BuildPlan(x::Target::GameMode, bothGates)[2].offset == 0x292, "Prefer existing home-app gate when both exist");
        Check(x::PatchImage(x::Target::GameMode, bothGates)[0x2B2] == 0x0F, "Leave support gate intact in legacy layout");
        auto neighbor = game;
        Put32(neighbor, 0xB04, 0x1090);
        Reject([&] { x::BuildPlan(x::Target::GameMode, neighbor); }, "Never patch a neighboring function's branch");
        auto escapingBranch = game;
        Put32(escapingBranch, 0x294, 0x100);
        Check(x::BuildPlan(x::Target::GameMode, escapingBranch).size() == 3, "Allow branch to separate executable cold block");
        Put32(escapingBranch, 0x294, 0xFFFFFF00);
        Reject([&] { x::BuildPlan(x::Target::GameMode, escapingBranch); }, "Reject branch to unmapped RVA");
        Put32(escapingBranch, 0x294, 0xF68);
        Reject([&] { x::BuildPlan(x::Target::GameMode, escapingBranch); }, "Reject branch to nonexecutable section");
        auto crossing = game;
        Put32(crossing, 0xB04, 0x1094);
        Reject([&] { x::BuildPlan(x::Target::GameMode, crossing); }, "Never patch across function end");
        auto shortFunction = game;
        Put32(shortFunction, 0xB04, 0x10A8);
        Put(shortFunction, 0x2B0, {0x84, 0xC0, 0x0F, 0x84, 0, 0, 0, 0});
        Check(x::BuildPlan(x::Target::GameMode, shortFunction).size() == 3, "Ignore neighboring function's extra branch");
        auto missingFunctions = game;
        Put32(missingFunctions, 0x98 + 140, 0);
        Reject([&] { x::BuildPlan(x::Target::GameMode, missingFunctions); }, "Require setter function metadata");
        auto malformedFunctions = game;
        Put32(malformedFunctions, 0x98 + 140, 13);
        Reject([&] { x::BuildPlan(x::Target::GameMode, malformedFunctions); }, "Reject malformed function table size");
        auto overlappingFunctions = game;
        Put32(overlappingFunctions, 0x98 + 140, 24);
        Put32(overlappingFunctions, 0xB0C, 0x1090);
        Put32(overlappingFunctions, 0xB10, 0x1110);
        Reject([&] { x::BuildPlan(x::Target::GameMode, overlappingFunctions); }, "Reject overlapping functions");
        auto badDirectories = game;
        Put32(badDirectories, 0x98 + 108, 17);
        Reject([&] { x::BuildPlan(x::Target::GameMode, badDirectories); }, "Reject directories outside optional header");
        auto overlappingSections = game;
        Put32(overlappingSections, 0x1B0 + 20, 0x700);
        Reject([&] { x::BuildPlan(x::Target::GameMode, overlappingSections); }, "Reject overlapping raw sections");
        auto overlappingRvas = game;
        Put32(overlappingRvas, 0x1B0 + 12, 0x1100);
        Reject([&] { x::BuildPlan(x::Target::GameMode, overlappingRvas); }, "Reject overlapping section RVAs");
        auto crossingTable = game;
        Put32(crossingTable, 0x800 + 28, 0x25FC);
        Reject([&] { x::BuildPlan(x::Target::GameMode, crossingTable); }, "Reject export array crossing raw section end");
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
        Check(x::BuildPlan(x::Target::Settings, settings).size() == 1, "Accept one recognized Settings check as upstream does");
        Check(x::BuildPlan(x::Target::Shell, shell).size() == 2, "Accept multiple recognized shell checks as upstream does");
        Put(shell, 0x390, {0x83, 0x7C, 0x24, 0x48, 0x2E, 0x0F, 0x94, 0xC3});
        const auto variableCount = x::PatchImage(x::Target::Settings, shell);
        Check(x::BuildPlan(x::Target::Settings, variableCount).size() == 3, "Accept changed compiler inlining count");
        Check(x::PatchImage(x::Target::Settings, variableCount) == variableCount, "Variable check count remains idempotent");
        const auto noChecks = Image();
        Reject([&] { x::BuildPlan(x::Target::Settings, noChecks); }, "Reject absence of recognized checks");
        Check(x::BuildPlan(x::Target::SettingsEnvironment, noChecks).empty(), "Optional Settings environment check may be absent");
        Check(x::PatchImage(x::Target::SettingsEnvironment, settings)[0x320] == 0xB1, "Patch Settings environment handheld check");

        // Import-anchored discovery follows the r8 form output, independent of the byte pattern.
        const auto jne = FormImage({0x75, 0x05, 0xB0, 0x00, 0xC3, 0, 0, 0xB0, 0x01, 0xC3});
        const auto jnePlan = x::BuildPlan(x::Target::Settings, jne);
        Check(jnePlan.size() == 1 && jnePlan[0].offset == 0x615 && !jnePlan[0].verified, "Discover jne device-form check as unverified");
        Check(jnePlan[0].replacement == x::Bytes(7, 0x90), "Fall through jne to the handheld path");
        Check(x::PatchImage(x::Target::Settings, jne)[0x615] == 0x83, "Never apply unverified sites by default");
        Check(x::PatchImage(x::Target::Settings, jne, true)[0x61A] == 0x90, "Apply unverified sites after confirmation");
        const auto je = FormImage({0x74, 0x05, 0xB0, 0x00, 0xC3, 0, 0, 0xB0, 0x01, 0xC3});
        const auto jePlan = x::BuildPlan(x::Target::Settings, je);
        // cmp at file 0x615 (RVA 0x1415); jz +5 at 0x61A targets RVA 0x1421. Accept any valid jmp encoding (short or near) to
        // that same target with NOP padding across the cmp+jz region, rather than one hard-coded encoder byte choice.
        Check(jePlan.size() == 1 && jePlan[0].offset == 0x615 && jePlan[0].replacement.size() == 7, "Rewrite je across the cmp+jz region");
        const auto& jb = jePlan[0].replacement;
        const long long jmpTarget = jb[0] == 0xEB ? 0x1415 + 2 + static_cast<signed char>(jb[1])
            : jb[0] == 0xE9 ? 0x1415 + 5 + static_cast<int>(jb[1] | (jb[2] << 8) | (jb[3] << 16) | (jb[4] << 24)) : -1;
        Check(jmpTarget == 0x1421, "je rewrite is an unconditional jump to the original handheld target");
        const auto knownShape = FormImage({0x0F, 0x94, 0xC0, 0xC3});
        const auto knownPlan = x::BuildPlan(x::Target::Settings, knownShape);
        Check(knownPlan.size() == 1 && knownPlan[0].verified, "Keep the validated sete layout verified and avoid duplicates");
        Check(x::DiscoverHandheldChecks(knownShape).at(0).known, "Discovery recognizes the validated layout");
        const auto familyOnly = FormImage({0x75, 0x05, 0xB0, 0x00, 0xC3, 0, 0, 0xB0, 0x01, 0xC3}, {0x45, 0x33, 0xC0, 0x90});
        Check(x::DiscoverHandheldChecks(familyOnly).empty(), "Ignore calls whose form output register is overwritten");
        const auto conditionalMove = FormImage({0x48, 0x0F, 0x44, 0xC1, 0xC3});
        const auto reportOnly = x::DiscoverHandheldChecks(conditionalMove);
        Check(reportOnly.size() == 1 && !reportOnly[0].patchable, "Report unsupported consumers without patching them");
        Check(x::BuildPlan(x::Target::SettingsEnvironment, conditionalMove).empty(), "Unsupported consumers never enter the plan");

        // An unrecognized export prologue is stubbed only at a real function entry that nothing branches into. Three ordered
        // RUNTIME_FUNCTION entries cover the exports: IsSupported [0x1020,0x1040), CanSet [0x1040,0x1080), Set [0x1080,0x1100).
        auto prologue = game;
        Put32(prologue, 0x98 + 140, 36);
        Put32(prologue, 0xB00, 0x1020); Put32(prologue, 0xB04, 0x1040); Put32(prologue, 0xB08, 0);
        Put32(prologue, 0xB0C, 0x1040); Put32(prologue, 0xB10, 0x1080); Put32(prologue, 0xB14, 0);
        Put32(prologue, 0xB18, 0x1080); Put32(prologue, 0xB1C, 0x1100); Put32(prologue, 0xB20, 0);
        Put(prologue, 0x220, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20});
        const auto prologuePlan = x::BuildPlan(x::Target::GameMode, prologue);
        Check(!prologuePlan[0].verified && prologuePlan[1].verified && prologuePlan[2].verified, "Stub unknown prologue only as unverified");
        Check(x::PatchImage(x::Target::GameMode, prologue)[0x220] == 0x40, "Keep unknown prologue intact without confirmation");
        Check(x::PatchImage(x::Target::GameMode, prologue, true)[0x220] == 0xB8, "Stub unknown prologue after confirmation");
        // A near jmp inside the Set function (RVA 0x10F0) whose target lands in the IsSupported stub bytes (RVA 0x1022).
        auto branchedInto = prologue;
        Put(branchedInto, 0x2F0, {0xE9, 0x2D, 0xFF, 0xFF, 0xFF});
        Reject([&] { x::BuildPlan(x::Target::GameMode, branchedInto); }, "Reject stub when a branch lands inside the entry bytes");
        auto notEntry = prologue;
        Put32(notEntry, 0xB00, 0x101C);
        Reject([&] { x::BuildPlan(x::Target::GameMode, notEntry); }, "Reject stub away from an x64 function entry");
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
