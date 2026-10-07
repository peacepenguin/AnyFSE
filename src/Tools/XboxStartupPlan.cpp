// Adapted from XboxStartupEnabler, Copyright (c) 2026 Victor Jimenez (MIT).
// See THIRD_PARTY_NOTICES/XboxStartupEnabler.txt.
#include "XboxStartupPlan.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace AnyFSE::Tools::XboxStartup
{
    namespace
    {
        void Require(bool valid, const char *message)
        {
            if (!valid) throw std::runtime_error(message);
        }
        class Pe
        {
            const Bytes& data;
            std::size_t sections, count;
            std::uint32_t exportRva, exportSize;
        public:
            std::size_t checksum;
            void Range(std::size_t off, std::size_t length) const
            {
                Require(off <= data.size() && length <= data.size() - off, "Truncated PE image");
            }
            std::uint16_t U16(std::size_t off) const
            {
                Range(off, 2);
                return static_cast<std::uint16_t>(data[off] | (std::uint16_t(data[off + 1]) << 8));
            }
            std::uint32_t U32(std::size_t off) const
            {
                Range(off, 4);
                return data[off] | (std::uint32_t(data[off + 1]) << 8)
                    | (std::uint32_t(data[off + 2]) << 16) | (std::uint32_t(data[off + 3]) << 24);
            }
            explicit Pe(const Bytes& bytes) : data(bytes)
            {
                Require(U16(0) == 0x5A4D, "Missing DOS header");
                const std::size_t pe = U32(0x3C), optional = pe + 24;
                Require(U32(pe) == 0x4550 && U16(pe + 4) == 0x8664, "Expected x64 PE image");
                Require(U16(optional) == 0x20B && U16(pe + 20) >= 120, "Expected PE32+ optional header");
                Range(optional, U16(pe + 20));
                Require(U32(optional + 108) > 0, "Missing export directory slot");
                exportRva = U32(optional + 112);
                exportSize = U32(optional + 116);
                checksum = optional + 64;
                count = U16(pe + 6);
                sections = optional + U16(pe + 20);
                Range(sections, count * 40);
            }
            std::size_t Offset(std::uint32_t rva) const
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto section = sections + i * 40;
                    const auto start = U32(section + 12), size = U32(section + 16);
                    if (rva >= start && std::uint64_t(rva) - start < size)
                    {
                        const std::size_t off = std::size_t(U32(section + 20)) + (rva - start);
                        Range(off, 1);
                        return off;
                    }
                }
                throw std::runtime_error("RVA is not backed by section data");
            }
            std::vector<std::pair<std::size_t, std::size_t>> Code() const
            {
                std::vector<std::pair<std::size_t, std::size_t>> result;
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto section = sections + i * 40;
                    if (!(U32(section + 36) & 0x20000000)) continue;
                    const std::size_t start = U32(section + 20), length = U32(section + 16);
                    Range(start, length);
                    result.emplace_back(start, length);
                }
                Require(!result.empty(), "No executable sections");
                return result;
            }
            bool IsCode(std::size_t off, std::size_t length) const
            {
                for (const auto& range : Code())
                    if (off >= range.first && off - range.first <= range.second
                        && length <= range.second - (off - range.first)) return true;
                return false;
            }
            std::size_t Export(const std::string& wanted) const
            {
                Require(exportRva && exportSize >= 40, "Missing export table");
                const auto exp = Offset(exportRva);
                const auto namesCount = U32(exp + 24), functionCount = U32(exp + 20);
                const auto funcs = Offset(U32(exp + 28)), names = Offset(U32(exp + 32)), ords = Offset(U32(exp + 36));
                Range(names, std::size_t(namesCount) * 4);
                Range(ords, std::size_t(namesCount) * 2);
                Range(funcs, std::size_t(functionCount) * 4);
                for (std::size_t i = 0; i < namesCount; ++i)
                {
                    const auto name = Offset(U32(names + i * 4));
                    auto end = name;
                    while (end < data.size() && data[end]) ++end;
                    Require(end < data.size(), "Unterminated export name");
                    if (std::string(data.begin() + name, data.begin() + end) != wanted) continue;
                    const auto ordinal = U16(ords + i * 2);
                    Require(ordinal < functionCount, "Invalid export ordinal");
                    const auto rva = U32(funcs + std::size_t(ordinal) * 4);
                    Require(rva < exportRva || std::uint64_t(rva) >= std::uint64_t(exportRva) + exportSize, "Forwarded export unsupported");
                    const auto off = Offset(rva);
                    Require(IsCode(off, 6), "Export is outside executable code");
                    return off;
                }
                throw std::runtime_error("Required gaming export is missing");
            }
        };
        bool Match(const Bytes& bytes, std::size_t off, const std::vector<int>& pattern)
        {
            if (off > bytes.size() || pattern.size() > bytes.size() - off) return false;
            for (std::size_t i = 0; i < pattern.size(); ++i)
                if (pattern[i] >= 0 && bytes[off + i] != pattern[i]) return false;
            return true;
        }
        Site Resolve(const Bytes& image, std::size_t off, const std::vector<int>& original, const Bytes& replacement)
        {
            if (off <= image.size() && replacement.size() <= image.size() - off
                && std::equal(replacement.begin(), replacement.end(), image.begin() + off))
                return {off, replacement, State::Patched};
            Require(Match(image, off, original), "Patch bytes do not match a supported layout");
            return {off, replacement, State::Original};
        }
        void Checksum(Bytes& image, std::size_t offset)
        {
            std::fill_n(image.begin() + offset, 4, 0);
            std::uint32_t sum = 0;
            for (std::size_t i = 0; i < image.size(); i += 2)
            {
                sum += image[i] | (i + 1 < image.size() ? std::uint32_t(image[i + 1]) << 8 : 0);
                sum = (sum & 0xFFFF) + (sum >> 16);
            }
            sum = ((sum & 0xFFFF) + (sum >> 16)) & 0xFFFF;
            sum += static_cast<std::uint32_t>(image.size());
            for (int i = 0; i < 4; ++i) image[offset + i] = static_cast<std::uint8_t>(sum >> (i * 8));
        }
    }
    std::vector<Site> BuildPlan(Target target, const Bytes& image)
    {
        Require(image.size() <= (std::numeric_limits<std::uint32_t>::max)(), "Oversized PE image");
        const Pe pe(image);
        std::vector<Site> sites;
        if (target == Target::GameMode)
        {
            const Bytes stub{0xB8, 1, 0, 0, 0, 0xC3};
            sites.push_back(Resolve(image, pe.Export("IsGamingFullScreenExperienceSupported"), {0x48, 0x89, 0x5C, 0x24, 8, 0x48}, stub));
            sites.push_back(Resolve(image, pe.Export("CanSetGamingFullScreenExperience"), {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57}, stub));
            const auto setter = pe.Export("SetGamingFullScreenExperience");
            for (std::size_t i = setter; i < setter + 0x80 && i + 8 <= image.size(); ++i)
            {
                if (!pe.IsCode(i, 8) || !Match(image, i, {0x84, 0xC0})) continue;
                if (Match(image, i + 2, {0x0F, 0x84, -1, -1, -1, -1}) || Match(image, i + 2, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}))
                    sites.push_back(Resolve(image, i + 2, {0x0F, 0x84, -1, -1, -1, -1}, Bytes(6, 0x90)));
            }
            Require(sites.size() == 3, "Missing or ambiguous home-app branch");
        }
        else
        {
            for (const auto& range : pe.Code())
                for (std::size_t off = range.first; off + 8 <= range.first + range.second; ++off)
                {
                    const bool original = Match(image, off, {0x83, 0x7C, 0x24, -1, 0x2E, 0x0F, 0x94, -1})
                        && image[off + 7] >= 0xC0 && image[off + 7] <= 0xC7;
                    const bool patched = image[off] >= 0xB0 && image[off] <= 0xB7
                        && Match(image, off + 1, {1, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
                    if (!original && !patched) continue;
                    const auto reg = (original ? image[off + 7] : image[off]) & 7;
                    sites.push_back(Resolve(image, off, {0x83, 0x7C, 0x24, -1, 0x2E, 0x0F, 0x94, 0xC0 | reg},
                        {static_cast<std::uint8_t>(0xB0 + reg), 1, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}));
                    off += 7;
                }
            // Documented six-site layout: three gaming, two Settings, one shell check.
            Require(sites.size() == (target == Target::Settings ? 2u : 1u), "Unsupported or ambiguous handheld-check count");
        }
        return sites;
    }
    Bytes PatchImage(Target target, const Bytes& image)
    {
        Bytes result = image;
        for (const auto& site : BuildPlan(target, image))
            std::copy(site.replacement.begin(), site.replacement.end(), result.begin() + site.offset);
        Checksum(result, Pe(image).checksum);
        return result;
    }
}
