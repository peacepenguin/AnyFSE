// Adapted from XboxStartupEnabler, Copyright (c) 2026 Victor Jimenez (MIT).
// See THIRD_PARTY_NOTICES/XboxStartupEnabler.txt.
#include "XboxStartupPlan.hpp"
#include "App/Constants.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace AnyFSE::Tools::XboxStartup
{
    namespace
    {
        namespace c = App::Constants;
        void Require(bool valid, const char *message)
        {
            if (!valid) throw std::runtime_error(message);
        }
        class Pe
        {
            const Bytes& data;
            std::size_t sections, count;
            std::uint32_t exportRva, exportSize;
            std::uint32_t exceptionRva = 0, exceptionSize = 0;
            std::uint32_t importRva = 0, importSize = 0, delayRva = 0, delaySize = 0;
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
                const auto directories = U32(optional + 108);
                Require(directories > 0 && directories <= (U16(pe + 20) - 112u) / 8, "Invalid PE directory count");
                exportRva = U32(optional + 112);
                exportSize = U32(optional + 116);
                if (directories > 1)
                {
                    importRva = U32(optional + 120);
                    importSize = U32(optional + 124);
                }
                if (directories > 3)
                {
                    exceptionRva = U32(optional + 136);
                    exceptionSize = U32(optional + 140);
                }
                if (directories > 13)
                {
                    delayRva = U32(optional + 216);
                    delaySize = U32(optional + 220);
                }
                checksum = optional + 64;
                count = U16(pe + 6);
                sections = optional + U16(pe + 20);
                Range(sections, count * 40);
                Require(count > 0, "Missing PE sections");
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto section = sections + i * 40;
                    const auto raw = U32(section + 20), size = U32(section + 16), rva = U32(section + 12);
                    if (!size) continue;
                    Range(raw, size);
                    Require(raw >= sections + count * 40, "Section overlaps PE headers");
                    Require(std::uint64_t(rva) + size <= (std::uint64_t{1} << 32), "Section RVA overflow");
                    for (std::size_t j = 0; j < i; ++j)
                    {
                        const auto other = sections + j * 40;
                        const auto otherRaw = U32(other + 20), otherSize = U32(other + 16), otherRva = U32(other + 12);
                        if (!otherSize) continue;
                        Require(std::uint64_t(raw) + size <= otherRaw || std::uint64_t(otherRaw) + otherSize <= raw,
                            "Overlapping raw PE sections");
                        Require(std::uint64_t(rva) + size <= otherRva || std::uint64_t(otherRva) + otherSize <= rva,
                            "Overlapping PE section RVAs");
                    }
                }
            }
            std::size_t Offset(std::uint32_t rva, std::size_t length = 1) const
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto section = sections + i * 40;
                    const auto start = U32(section + 12), size = U32(section + 16);
                    if (rva >= start && std::uint64_t(rva) - start < size)
                    {
                        Require(length <= size - (rva - start), "RVA span crosses section boundary");
                        const std::size_t off = std::size_t(U32(section + 20)) + (rva - start);
                        Range(off, length);
                        return off;
                    }
                }
                throw std::runtime_error("RVA is not backed by section data");
            }
            std::uint32_t Rva(std::size_t off) const
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto section = sections + i * 40;
                    const auto start = U32(section + 20), size = U32(section + 16);
                    if (off >= start && off - start < size)
                        return static_cast<std::uint32_t>(U32(section + 12) + (off - start));
                }
                throw std::runtime_error("File offset is not backed by section data");
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
                const auto exp = Offset(exportRva, exportSize);
                const auto namesCount = U32(exp + 24), functionCount = U32(exp + 20);
                const auto funcs = Offset(U32(exp + 28), std::size_t(functionCount) * 4);
                const auto names = Offset(U32(exp + 32), std::size_t(namesCount) * 4);
                const auto ords = Offset(U32(exp + 36), std::size_t(namesCount) * 2);
                for (std::size_t i = 0; i < namesCount; ++i)
                {
                    const auto name = Offset(U32(names + i * 4));
                    auto end = name;
                    while (end < data.size() && data[end]) ++end;
                    Require(end < data.size(), "Unterminated export name");
                    Offset(U32(names + i * 4), end - name + 1);
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
            std::string Name(std::uint32_t rva) const
            {
                const auto name = Offset(rva);
                auto end = name;
                while (end < data.size() && data[end]) ++end;
                Require(end < data.size(), "Unterminated import name");
                return std::string(data.begin() + name, data.begin() + end);
            }
            // RVAs of the IAT slots that import `wanted` by name, from both the regular and the delay-load import tables.
            std::vector<std::uint32_t> ImportSlots(const std::string& wanted) const
            {
                std::vector<std::uint32_t> slots;
                const auto scan = [&](std::uint32_t lookup, std::uint32_t iat)
                {
                    if (!lookup) lookup = iat;
                    for (std::uint32_t i = 0; i < 0x10000; ++i)
                    {
                        const auto entry = Offset(lookup + i * 8, 8);
                        const std::uint64_t thunk = U32(entry) | (std::uint64_t(U32(entry + 4)) << 32);
                        if (!thunk) return;
                        if (!(thunk >> 63) && Name(static_cast<std::uint32_t>(thunk & 0x7FFFFFFF) + 2) == wanted) slots.push_back(iat + i * 8);
                    }
                };
                if (importRva && importSize >= 20)
                {
                    const auto table = Offset(importRva, 20);
                    for (std::size_t d = table; U32(d + 12) && d + 20 <= table + std::size_t(importSize); d += 20) scan(U32(d), U32(d + 16));
                }
                if (delayRva && delaySize >= 32)
                {
                    const auto table = Offset(delayRva, 32);
                    for (std::size_t d = table; U32(d + 4) && d + 32 <= table + std::size_t(delaySize); d += 32) scan(U32(d + 16), U32(d + 12));
                }
                return slots;
            }
            // File-offset bounds of the RUNTIME_FUNCTION containing `off`, or {0, 0} when none does.
            std::pair<std::size_t, std::size_t> Function(std::size_t off) const
            {
                if (!exceptionRva || !exceptionSize || exceptionSize % 12) return {0, 0};
                const auto rva = Rva(off);
                const auto table = Offset(exceptionRva, exceptionSize);
                for (std::size_t i = 0; i < exceptionSize; i += 12)
                {
                    const auto begin = U32(table + i), end = U32(table + i + 4);
                    if (rva >= begin && rva < end) return {Offset(begin, end - begin), Offset(begin, end - begin) + (end - begin)};
                }
                return {0, 0};
            }
            std::size_t FunctionEnd(std::size_t entry) const
            {
                // x64 RUNTIME_FUNCTION entries give an exclusive end RVA. Never scan into a neighboring function.
                Require(exceptionRva && exceptionSize && exceptionSize % 12 == 0, "Missing or malformed x64 function table");
                const auto table = Offset(exceptionRva, exceptionSize);
                std::size_t endOffset = 0;
                std::uint32_t previousEnd = 0;
                for (std::size_t i = 0; i < exceptionSize; i += 12)
                {
                    const auto begin = U32(table + i), end = U32(table + i + 4);
                    Require(begin < end && begin >= previousEnd, "Unordered or overlapping x64 functions");
                    previousEnd = end;
                    const auto off = Offset(begin, end - begin);
                    Require(IsCode(off, end - begin), "x64 function is outside executable code");
                    if (off == entry) endOffset = off + (end - begin);
                }
                Require(endOffset != 0, "Gaming setter has no x64 function boundary");
                return endOffset;
            }
        };
        bool Match(const Bytes& bytes, std::size_t off, const std::vector<int>& pattern)
        {
            if (off > bytes.size() || pattern.size() > bytes.size() - off) return false;
            for (std::size_t i = 0; i < pattern.size(); ++i)
                if (pattern[i] >= 0 && bytes[off + i] != pattern[i]) return false;
            return true;
        }
        // Hex-dumps the bytes actually present at `off`, "??" past the end of the image. Diagnostic only: this is what makes a
        // refusal on an unlisted Windows build actionable. New patterns still require disassembly and behavior validation.
        std::string Dump(const Bytes& bytes, std::size_t off, std::size_t length)
        {
            static const char *hex = "0123456789abcdef";
            std::string result;
            for (std::size_t i = 0; i < length; ++i)
            {
                if (i) result += ' ';
                if (off + i < bytes.size()) { const auto b = bytes[off + i]; result += hex[b >> 4]; result += hex[b & 15]; }
                else result += "??";
            }
            return result;
        }
        std::string DumpPattern(const std::vector<int>& pattern)
        {
            static const char *hex = "0123456789abcdef";
            std::string result;
            for (std::size_t i = 0; i < pattern.size(); ++i)
            {
                if (i) result += ' ';
                if (pattern[i] < 0) result += "??";
                else { result += hex[(pattern[i] >> 4) & 15]; result += hex[pattern[i] & 15]; }
            }
            return result;
        }
        Site Resolve(const Bytes& image, std::size_t off, const std::vector<int>& original, const Bytes& replacement)
        {
            if (off <= image.size() && replacement.size() <= image.size() - off
                && std::equal(replacement.begin(), replacement.end(), image.begin() + off))
                return {off, replacement, State::Patched};
            if (!Match(image, off, original))
                throw std::runtime_error("Patch bytes do not match a supported layout at +0x" + [&] {
                    char hexOffset[2 * sizeof(std::size_t) + 1] = {};
                    std::snprintf(hexOffset, sizeof(hexOffset), "%zx", off);
                    return std::string(hexOffset);
                }() + ": expected [" + DumpPattern(original) + "] or the patched replacement, found [" + Dump(image, off, original.size()) + "]");
            return {off, replacement, State::Original};
        }
        std::string Hex(std::size_t value)
        {
            char text[2 * sizeof(std::size_t) + 1] = {};
            std::snprintf(text, sizeof(text), "%zx", value);
            return text;
        }
        // Overwriting an entry with `mov eax,1; ret` is safe for any prologue when the entry starts a real x64 function that is
        // long enough and no relative branch lands inside the overwritten bytes. Branch decoding is byte-level, so a false match
        // can only refuse a patch, never allow one.
        bool SafeEntryStub(const Pe& pe, const Bytes& image, std::size_t off, std::size_t length)
        {
            const auto function = pe.Function(off);
            if (!function.second) return false;
            if (function.first != off && !(function.first + 4 == off && Match(image, function.first, {0xF3, 0x0F, 0x1E, 0xFA}))) return false;
            if (function.second - off < length) return false;
            const std::int64_t entry = pe.Rva(off);
            for (const auto& range : pe.Code())
            {
                const std::int64_t base = pe.Rva(range.first);
                const auto end = range.first + range.second;
                for (std::size_t i = range.first; i < end; ++i)
                {
                    const auto op = image[i];
                    std::size_t size = 0;
                    if (op == 0xE8 || op == 0xE9) size = 5;
                    else if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) size = 2;
                    else if (op == 0x0F && i + 1 < end && image[i + 1] >= 0x80 && image[i + 1] <= 0x8F) size = 6;
                    else continue;
                    if (i + size > end) continue;
                    const std::int64_t relative = size == 2 ? std::int64_t(std::int8_t(image[i + 1]))
                        : std::int64_t(std::int32_t(pe.U32(i + size - 4)));
                    const auto destination = base + std::int64_t(i - range.first) + std::int64_t(size) + relative;
                    if (destination > entry && destination < entry + std::int64_t(length)) return false;
                }
            }
            return true;
        }
        Site ResolveExport(const Pe& pe, const Bytes& image, const char *name, const std::vector<int>& original, const Bytes& replacement,
            const std::vector<int>& alternative = {})
        {
            auto off = pe.Export(name);
            // Preserve ENDBR64 if a newer toolchain emits an indirect-branch landing pad.
            if (Match(image, off, {0xF3, 0x0F, 0x1E, 0xFA})) off += 4;
            Require(pe.IsCode(off, replacement.size()), "Export patch crosses executable section boundary");
            if (!alternative.empty() && pe.IsCode(off, alternative.size()) && Match(image, off, alternative))
                return Resolve(image, off, alternative, replacement);
            const bool patched = std::equal(replacement.begin(), replacement.end(), image.begin() + off);
            if (!patched && !Match(image, off, original) && SafeEntryStub(pe, image, off, replacement.size()))
                return {off, replacement, State::Original, false, std::string(name) + " +0x" + Hex(off) + ": unrecognized prologue ["
                    + Dump(image, off, replacement.size()) + "] -> mov eax,1; ret"};
            return Resolve(image, off, original, replacement);
        }
        // Stack slot named by a ModRM memory operand: [rsp+disp] through SIB 0x24, or [rbp+disp].
        struct Slot { bool rbp = false; std::int32_t disp = 0; };
        bool Operand(const Bytes& image, std::size_t modrm, Slot& slot, std::size_t& length)
        {
            if (modrm >= image.size()) return false;
            const auto mod = image[modrm] >> 6, rm = image[modrm] & 7;
            if (mod != 1 && mod != 2) return false;
            auto at = modrm + 1;
            if (rm == 4)
            {
                if (at >= image.size() || image[at] != 0x24) return false;
                slot.rbp = false;
                ++at;
            }
            else if (rm == 5) slot.rbp = true;
            else return false;
            if (mod == 1)
            {
                if (at >= image.size()) return false;
                slot.disp = std::int8_t(image[at++]);
            }
            else
            {
                if (at + 4 > image.size()) return false;
                slot.disp = std::int32_t(image[at] | (std::uint32_t(image[at + 1]) << 8) | (std::uint32_t(image[at + 2]) << 16)
                    | (std::uint32_t(image[at + 3]) << 24));
                at += 4;
            }
            length = at - modrm;
            return true;
        }
        std::string Describe(const Slot& slot)
        {
            const auto magnitude = slot.disp < 0 ? std::size_t(-std::int64_t(slot.disp)) : std::size_t(slot.disp);
            return std::string(slot.rbp ? "[rbp" : "[rsp") + (slot.disp < 0 ? "-0x" : "+0x") + Hex(magnitude) + "]";
        }
        // Follows one RtlGetDeviceFamilyInfoEnum call to the compare of its device-form output against the handheld form.
        void FollowFormCall(const Pe& pe, const Bytes& image, std::size_t call, std::size_t callEnd, std::vector<Finding>& findings)
        {
            const auto function = pe.Function(call);
            if (!function.second) return;
            // The form output is the third argument: find the closest `lea r8,[slot]` and require that r8 is not rewritten after it.
            Slot slot;
            std::size_t leaEnd = 0;
            const auto backLimit = (std::max)(function.first, call > 0x40 ? call - 0x40 : std::size_t{0});
            for (std::size_t i = call; i-- > backLimit;)
            {
                std::size_t length = 0;
                if (image[i] == 0x4C && image[i + 1] == 0x8D && (image[i + 2] & 0x38) == 0 && Operand(image, i + 2, slot, length)
                    && i + 2 + length <= call) { leaEnd = i + 2 + length; break; }
            }
            if (!leaEnd) return;
            for (std::size_t i = leaEnd; i + 3 <= call; ++i)
                if (((image[i] == 0x45 || image[i] == 0x4D) && image[i + 1] == 0x33 && image[i + 2] == 0xC0)
                    || ((image[i] == 0x4C || image[i] == 0x4D) && image[i + 1] == 0x8B && (image[i + 2] & 0xF8) == 0xC0)) return;
            if (Match(image, callEnd, {0x0F, 0x1F, 0x44, 0x00, 0x00})) callEnd += 5;
            const auto windowEnd = (std::min)(function.second, callEnd + 0x40);
            for (std::size_t i = callEnd; i + 3 <= windowEnd; ++i)
            {
                Slot read;
                std::size_t length = 0, compare = 0, compareLength = 0;
                std::string form;
                if (image[i] == 0x83 && (image[i + 1] & 0x38) == 0x38 && Operand(image, i + 1, read, length) && read.rbp == slot.rbp
                    && read.disp == slot.disp && i + 1 + length < windowEnd && image[i + 1 + length] == 0x2E)
                {
                    compare = i;
                    compareLength = 2 + length;
                    form = "cmp dword " + Describe(slot) + ",0x2E";
                }
                else if (image[i] == 0x8B && Operand(image, i + 1, read, length) && read.rbp == slot.rbp && read.disp == slot.disp)
                {
                    const auto reg = (image[i + 1] >> 3) & 7;
                    const auto next = i + 1 + length;
                    if (next + 3 > windowEnd || image[next] != 0x83 || image[next + 1] != 0xF8 + reg || image[next + 2] != 0x2E) continue;
                    compare = next;
                    compareLength = 3;
                    form = "mov r32," + Describe(slot) + "; cmp r32,0x2E";
                }
                else continue;
                const auto use = compare + compareLength;
                Finding finding{compare, false, "+0x" + Hex(compare) + ": " + form};
                const auto jump = [&](std::size_t size)
                {
                    const std::int64_t relative = size == 2 ? std::int64_t(std::int8_t(image[use + 1]))
                        : std::int64_t(std::int32_t(pe.U32(use + 2)));
                    const auto destination = std::int64_t(pe.Rva(use)) + std::int64_t(size) + relative;
                    const auto total = compareLength + size;
                    if (destination < 0 || destination > (std::numeric_limits<std::uint32_t>::max)()
                        || !pe.IsCode(pe.Offset(static_cast<std::uint32_t>(destination)), 1)) return;
                    const auto fromShort = destination - (std::int64_t(pe.Rva(compare)) + 2);
                    const auto fromNear = destination - (std::int64_t(pe.Rva(compare)) + 5);
                    if (fromShort >= -128 && fromShort <= 127)
                        finding.replacement = {0xEB, static_cast<std::uint8_t>(fromShort)};
                    else if (total >= 5)
                        finding.replacement = {0xE9, static_cast<std::uint8_t>(fromNear), static_cast<std::uint8_t>(fromNear >> 8),
                            static_cast<std::uint8_t>(fromNear >> 16), static_cast<std::uint8_t>(fromNear >> 24)};
                    else return;
                    finding.replacement.resize(total, 0x90);
                    finding.patchable = true;
                    finding.description += "; je -> jmp (always take the handheld branch)";
                };
                if (use + 3 <= windowEnd && image[use] == 0x0F && (image[use + 1] == 0x94 || image[use + 1] == 0x95)
                    && (image[use + 2] & 0xF8) == 0xC0)
                {
                    const auto reg = image[use + 2] & 7;
                    const bool equal = image[use + 1] == 0x94;
                    finding.replacement = {static_cast<std::uint8_t>(0xB0 + reg), static_cast<std::uint8_t>(equal ? 1 : 0)};
                    finding.replacement.resize(compareLength + 3, 0x90);
                    finding.patchable = true;
                    finding.description += equal ? "; sete -> mov reg8,1" : "; setne -> mov reg8,0";
                    finding.known = Match(image, compare, {0x83, 0x7C, 0x24, -1, 0x2E, 0x0F, 0x94, -1});
                }
                else if (use + 2 <= windowEnd && image[use] == 0x74) jump(2);
                else if (use + 6 <= windowEnd && image[use] == 0x0F && image[use + 1] == 0x84) jump(6);
                else if ((use + 2 <= windowEnd && image[use] == 0x75) || (use + 6 <= windowEnd && image[use] == 0x0F && image[use + 1] == 0x85))
                {
                    finding.replacement.assign(compareLength + (image[use] == 0x75 ? 2 : 6), 0x90);
                    finding.patchable = true;
                    finding.description += "; jne -> nop (fall through to the handheld path)";
                }
                if (!finding.patchable) finding.description += "; result used by [" + Dump(image, use, 4) + "], unsupported shape, not patched";
                findings.push_back(finding);
                return;
            }
        }
        std::int64_t RelativeDestination(const Pe& pe, std::size_t instruction, std::size_t displacementOffset, std::size_t length)
        {
            const auto relative = pe.U32(instruction + displacementOffset);
            const auto displacement = std::int64_t(relative) - ((relative & 0x80000000u) ? (std::int64_t{1} << 32) : 0);
            return std::int64_t(pe.Rva(instruction)) + static_cast<std::int64_t>(length) + displacement;
        }
        void Checksum(Bytes& image, std::size_t offset)
        {
            std::fill_n(image.begin() + offset, 4, std::uint8_t{0});
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
    std::vector<Finding> DiscoverHandheldChecks(const Bytes& image)
    {
        Require(image.size() <= (std::numeric_limits<std::uint32_t>::max)(), "Oversized PE image");
        const Pe pe(image);
        std::vector<Finding> findings;
        const auto slots = pe.ImportSlots(c::XboxStartupDeviceFormExport);
        if (slots.empty()) return findings;
        for (const auto& range : pe.Code())
        {
            const std::int64_t base = pe.Rva(range.first);
            for (std::size_t off = range.first; off + 6 <= range.first + range.second; ++off)
            {
                if (image[off] != 0xFF || image[off + 1] != 0x15) continue;
                const auto target = base + std::int64_t(off - range.first) + 6 + std::int64_t(std::int32_t(pe.U32(off + 2)));
                if (std::find(slots.begin(), slots.end(), target) == slots.end()) continue;
                FollowFormCall(pe, image, off > range.first && image[off - 1] == 0x48 ? off - 1 : off, off + 6, findings);
            }
        }
        return findings;
    }
    std::vector<Site> BuildPlan(Target target, const Bytes& image)
    {
        Require(image.size() <= (std::numeric_limits<std::uint32_t>::max)(), "Oversized PE image");
        const Pe pe(image);
        std::vector<Site> sites;
        if (target == Target::GameMode)
        {
            const Bytes stub{0xB8, 1, 0, 0, 0, 0xC3};
            // 26100.9278 uses a small stack frame and RIP-relative feature-state lookup at this named BOOL export.
            sites.push_back(ResolveExport(pe, image, c::XboxStartupSupportedExport, {0x48, 0x89, 0x5C, 0x24, 8, 0x48}, stub,
                {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74, -1}));
            sites.push_back(ResolveExport(pe, image, c::XboxStartupCanSetExport, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57}, stub));
            const auto setter = pe.Export(c::XboxStartupSetExport);
            const auto functionEnd = pe.FunctionEnd(setter);
            const auto scanEnd = (std::min)(functionEnd, setter + 0x80);
            const auto supportedRva = pe.Rva(pe.Export(c::XboxStartupSupportedExport));
            std::vector<Site> supportGates;
            for (std::size_t i = setter; i + 8 <= scanEnd; ++i)
            {
                const bool byteTest = Match(image, i, {0x84, 0xC0});
                const bool boolTest = Match(image, i, {0x85, 0xC0});
                if (!pe.IsCode(i, 8) || (!byteTest && !boolTest)) continue;
                // BOOL is tested as EAX in the alternate layout. Bind it to the named support export,
                // not an arbitrary HRESULT/boolean test elsewhere in the setter.
                if (boolTest && (i < setter + 5 || image[i - 5] != 0xE8
                    || RelativeDestination(pe, i - 5, 1, 5) != supportedRva)) continue;
                const bool original = Match(image, i + 2, {0x0F, 0x84, -1, -1, -1, -1});
                if (original)
                {
                    const auto destination = RelativeDestination(pe, i + 2, 2, 6);
                    Require(destination >= 0 && destination <= (std::numeric_limits<std::uint32_t>::max)(), "Invalid branch destination RVA");
                    // Cold blocks and shared epilogues may have separate RUNTIME_FUNCTION entries.
                    Require(pe.IsCode(pe.Offset(static_cast<std::uint32_t>(destination)), 1), "Gaming setter branch targets nonexecutable data");
                }
                if (original || Match(image, i + 2, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}))
                {
                    auto& candidates = boolTest ? supportGates : sites;
                    candidates.push_back(Resolve(image, i + 2, {0x0F, 0x84, -1, -1, -1, -1}, Bytes(6, 0x90)));
                }
            }
            // Preserve the original home-app patch when present. Only use the verified support gate
            // for layouts without it, keeping detection/restoration of previously patched DLLs stable.
            if (sites.size() == 2) sites.insert(sites.end(), supportGates.begin(), supportGates.end());
            if (sites.size() != 3)
                throw std::runtime_error("Missing or ambiguous gaming setter branch: found " + std::to_string(sites.size()) + " of 3 expected sites");
        }
        else
        {
            Require(target == Target::Settings || target == Target::Shell || target == Target::SettingsEnvironment, "Unknown patch target");
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
            // Device-form checks in layouts the byte pattern does not recognize are added as unverified sites.
            for (const auto& finding : DiscoverHandheldChecks(image))
            {
                if (!finding.patchable || finding.known) continue;
                const bool overlaps = std::any_of(sites.begin(), sites.end(), [&](const Site& site)
                    { return finding.offset < site.offset + site.replacement.size() && site.offset < finding.offset + finding.replacement.size(); });
                if (!overlaps) sites.push_back({finding.offset, finding.replacement, State::Original, false, finding.description});
            }
            // As in upstream, compiler inlining may change the number of identical checks between builds.
            // Every site must still match the complete original/replacement sequence in executable code.
            // Newer builds gate the Settings Gaming Posture page in SettingsEnvironment.Desktop.dll; older builds have no check there.
            Require(!sites.empty() || target == Target::SettingsEnvironment, "No recognized handheld checks in executable code");
        }
        auto ordered = sites;
        std::sort(ordered.begin(), ordered.end(), [](const Site& a, const Site& b) { return a.offset < b.offset; });
        for (std::size_t i = 1; i < ordered.size(); ++i)
            Require(ordered[i - 1].offset + ordered[i - 1].replacement.size() <= ordered[i].offset, "Overlapping patch sites");
        return sites;
    }
    Bytes PatchImage(Target target, const Bytes& image, bool includeUnverified)
    {
        Bytes result = image;
        for (const auto& site : BuildPlan(target, image))
            if (site.verified || includeUnverified) std::copy(site.replacement.begin(), site.replacement.end(), result.begin() + site.offset);
        Checksum(result, Pe(image).checksum);
        return result;
    }
}
