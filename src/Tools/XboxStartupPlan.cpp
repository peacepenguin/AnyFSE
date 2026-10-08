// Adapted from XboxStartupEnabler, Copyright (c) 2026 Victor Jimenez (MIT).
// See THIRD_PARTY_NOTICES/XboxStartupEnabler.txt.
#include "XboxStartupPlan.hpp"
#include "App/Constants.hpp"
#include "zydis/Zydis.h"
#include <algorithm>
#include <array>
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
            // File-offset [begin, end) of every RUNTIME_FUNCTION, in table order. These are the only ranges holding code; a linear
            // decode started at each true function entry stays aligned, where a sweep from a section base could desync on padding.
            std::vector<std::pair<std::size_t, std::size_t>> Functions() const
            {
                std::vector<std::pair<std::size_t, std::size_t>> result;
                if (!exceptionRva || !exceptionSize || exceptionSize % 12) return result;
                const auto table = Offset(exceptionRva, exceptionSize);
                for (std::size_t i = 0; i < exceptionSize; i += 12)
                {
                    const auto begin = U32(table + i), end = U32(table + i + 4);
                    if (begin >= end) continue;
                    const auto off = Offset(begin, end - begin);
                    if (IsCode(off, end - begin)) result.emplace_back(off, off + (end - begin));
                }
                return result;
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
        // One decoded instruction with its file offset and runtime (RVA-based) address.
        struct Insn
        {
            std::size_t offset = 0;
            ZyanU64 address = 0;
            ZydisDecodedInstruction instruction{};
            std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        };
        const ZydisDecoder& Decoder()
        {
            static const ZydisDecoder decoder = [] {
                ZydisDecoder value;
                ZydisDecoderInit(&value, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
                return value;
            }();
            return decoder;
        }
        // Linear decode of one x64 function. Started at a real RUNTIME_FUNCTION entry, every instruction is aligned; a failed
        // decode (padding or embedded data) ends the walk rather than resyncing, so nothing downstream sees a misaligned stream.
        std::vector<Insn> Decode(const Pe& pe, const Bytes& image, std::size_t begin, std::size_t end)
        {
            std::vector<Insn> result;
            for (std::size_t off = begin; off < end;)
            {
                Insn insn;
                insn.offset = off;
                insn.address = pe.Rva(off);
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), image.data() + off, end - off,
                    &insn.instruction, insn.operands.data()))) break;
                off += insn.instruction.length;
                result.push_back(insn);
            }
            return result;
        }
        bool WritesRegister(const Insn& insn, ZydisRegister reg)
        {
            const auto enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
            for (ZyanU8 i = 0; i < insn.instruction.operand_count; ++i)
            {
                const auto& op = insn.operands[i];
                if (op.type == ZYDIS_OPERAND_TYPE_REGISTER && (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)
                    && ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, op.reg.value) == enclosing)
                    return true;
            }
            return false;
        }
        // Encodes `instruction` at `address`, then fills the rest of `length` bytes with NOPs. Used so a rewrite is the exact
        // encoding the assembler would emit for any register or branch distance, not a hand-built byte string.
        bool Encode(ZydisEncoderRequest& request, ZyanU64 address, std::size_t length, Bytes& out)
        {
            std::array<ZyanU8, ZYDIS_MAX_INSTRUCTION_LENGTH> buffer{};
            ZyanUSize size = buffer.size();
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&request, buffer.data(), &size, address)) || size > length)
                return false;
            out.assign(buffer.begin(), buffer.begin() + size);
            out.resize(length, 0x90);
            if (length > size && !ZYAN_SUCCESS(ZydisEncoderNopFill(out.data() + size, length - size))) return false;
            return true;
        }
        bool EncodeMovImm8(ZydisRegister reg, std::uint8_t value, ZyanU64 address, std::size_t length, Bytes& out)
        {
            ZydisEncoderRequest request{};
            request.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
            request.mnemonic = ZYDIS_MNEMONIC_MOV;
            request.operand_count = 2;
            request.operands[0].type = ZYDIS_OPERAND_TYPE_REGISTER;
            request.operands[0].reg.value = reg;
            request.operands[1].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            request.operands[1].imm.u = value;
            return Encode(request, address, length, out);
        }
        bool EncodeJmp(ZyanU64 target, ZyanU64 address, std::size_t length, Bytes& out)
        {
            // Prefer the shortest branch that reaches the target and fits the region (rel8, then rel32), as the assembler would.
            for (const auto width : {ZYDIS_BRANCH_WIDTH_8, ZYDIS_BRANCH_WIDTH_32})
            {
                ZydisEncoderRequest request{};
                request.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
                request.mnemonic = ZYDIS_MNEMONIC_JMP;
                request.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
                request.branch_width = width;
                request.operand_count = 1;
                request.operands[0].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
                request.operands[0].imm.u = target;
                if (Encode(request, address, length, out)) return true;
            }
            return false;
        }
        std::string Describe(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands, ZyanU64 address)
        {
            char text[96] = {};
            static const ZydisFormatter& formatter = [] {
                static ZydisFormatter value;
                ZydisFormatterInit(&value, ZYDIS_FORMATTER_STYLE_INTEL);
                return value;
            }();
            if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                instruction.operand_count_visible, text, sizeof(text), address, nullptr)))
                return "<unformattable>";
            return text;
        }
        // Overwriting an export entry with `mov eax,1; ret` is safe only when the entry is a real function entry, the function is
        // long enough, and no instruction in the image branches into the bytes being overwritten. Branch targets are resolved by
        // the disassembler, so a misread cannot approve an unsafe stub.
        bool SafeEntryStub(const Pe& pe, const Bytes& image, std::size_t off, std::size_t length)
        {
            const auto function = pe.Function(off);
            if (!function.second) return false;
            if (function.first != off && !(function.first + 4 == off && Match(image, function.first, {0xF3, 0x0F, 0x1E, 0xFA}))) return false;
            if (function.second - off < length) return false;
            const ZyanU64 entry = pe.Rva(off);
            for (const auto& range : pe.Functions())
                for (const auto& insn : Decode(pe, image, range.first, range.second))
                {
                    if (!(insn.instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE)) continue;
                    for (ZyanU8 i = 0; i < insn.instruction.operand_count; ++i)
                    {
                        if (insn.operands[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !insn.operands[i].imm.is_relative) continue;
                        ZyanU64 destination = 0;
                        if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn.instruction, &insn.operands[i], insn.address, &destination))
                            && destination > entry && destination < entry + length)
                            return false;
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
        bool IsStackMemory(const ZydisDecodedOperand& op)
        {
            return op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.index == ZYDIS_REGISTER_NONE
                && (op.mem.base == ZYDIS_REGISTER_RSP || op.mem.base == ZYDIS_REGISTER_RBP);
        }
        bool SameStackSlot(const ZydisDecodedOperand& a, const ZydisDecodedOperand& b)
        {
            // Same stack location is base + displacement; access size differs (the LEA that takes its address versus the later
            // dword compare), so size is deliberately not compared.
            return IsStackMemory(a) && IsStackMemory(b) && a.mem.base == b.mem.base && a.mem.disp.value == b.mem.disp.value;
        }
        // Follows one RtlGetDeviceFamilyInfoEnum call to the compare of its device-form output against the handheld form, then to
        // the instruction that consumes the compare. All register and memory tracking is on decoded operands, so register choice,
        // addressing mode and displacement width do not matter; only the data flow does.
        void FollowFormCall(const Pe& pe, const Bytes& image, const std::vector<Insn>& function, std::size_t callIndex,
            std::vector<Finding>& findings)
        {
            // The device-form output is the third integer argument (R8): a pointer the callee writes through. Require the nearest
            // preceding write to R8 to be `lea r8, [stack slot]`, and that R8 is not rewritten again before the call.
            ZydisDecodedOperand slot{};
            for (std::size_t i = callIndex; i-- > 0;)
            {
                if (!WritesRegister(function[i], ZYDIS_REGISTER_R8)) continue;
                if (function[i].instruction.mnemonic != ZYDIS_MNEMONIC_LEA || !IsStackMemory(function[i].operands[1])) return;
                slot = function[i].operands[1];
                for (std::size_t j = i + 1; j < callIndex; ++j)
                    if (WritesRegister(function[j], ZYDIS_REGISTER_R8)) return;
                break;
            }
            if (slot.type != ZYDIS_OPERAND_TYPE_MEMORY) return;

            // Registers currently holding a copy of the form value, seeded with the memory slot itself.
            std::vector<ZydisRegister> holders;
            for (std::size_t i = callIndex + 1; i < function.size(); ++i)
            {
                const auto& insn = function[i];
                const auto& mn = insn.instruction.mnemonic;
                const auto held = [&](const ZydisDecodedOperand& op) {
                    if (SameStackSlot(op, slot)) return true;
                    if (op.type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
                    const auto enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, op.reg.value);
                    return std::find(holders.begin(), holders.end(), enclosing) != holders.end();
                };
                // A store into the slot (other than the call's own write) means the value is gone; stop.
                if (insn.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && SameStackSlot(insn.operands[0], slot)
                    && (insn.operands[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) return;
                // Copy of the form value into a register: track it.
                if ((mn == ZYDIS_MNEMONIC_MOV || mn == ZYDIS_MNEMONIC_MOVZX || mn == ZYDIS_MNEMONIC_MOVSX)
                    && insn.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && held(insn.operands[1]))
                {
                    holders.push_back(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, insn.operands[0].reg.value));
                    continue;
                }
                if (mn == ZYDIS_MNEMONIC_CMP && insn.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
                    && insn.operands[1].imm.value.u == c::HandheldDeviceForm && held(insn.operands[0]))
                {
                    const auto consumer = i + 1 < function.size() ? &function[i + 1] : nullptr;
                    Finding finding{insn.offset, false, "+0x" + Hex(insn.offset) + ": "
                        + Describe(insn.instruction, insn.operands.data(), insn.address)};
                    const auto region = consumer ? consumer->offset + consumer->instruction.length - insn.offset : insn.instruction.length;
                    if (consumer && (consumer->instruction.mnemonic == ZYDIS_MNEMONIC_SETZ
                        || consumer->instruction.mnemonic == ZYDIS_MNEMONIC_SETNZ)
                        && consumer->operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
                    {
                        const bool equal = consumer->instruction.mnemonic == ZYDIS_MNEMONIC_SETZ;
                        finding.patchable = EncodeMovImm8(consumer->operands[0].reg.value, equal ? 1 : 0, insn.address, region,
                            finding.replacement);
                        finding.description += "; " + Describe(consumer->instruction, consumer->operands.data(), consumer->address)
                            + (equal ? " -> mov reg,1" : " -> mov reg,0");
                        // The validated layout: cmp dword [rsp+disp],2Eh ; sete r8. Keep it verified so it needs no confirmation.
                        finding.known = Match(image, insn.offset, {0x83, 0x7C, 0x24, -1, 0x2E, 0x0F, 0x94, -1});
                    }
                    else if (consumer && consumer->instruction.mnemonic == ZYDIS_MNEMONIC_JZ
                        && consumer->operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && consumer->operands[0].imm.is_relative)
                    {
                        ZyanU64 destination = 0;
                        if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&consumer->instruction, &consumer->operands[0], consumer->address,
                            &destination)) && destination <= (std::numeric_limits<std::uint32_t>::max)()
                            && pe.IsCode(pe.Offset(static_cast<std::uint32_t>(destination)), 1))
                        {
                            finding.patchable = EncodeJmp(destination, insn.address, region, finding.replacement);
                            finding.description += "; jz -> jmp (always take the handheld branch)";
                        }
                    }
                    else if (consumer && consumer->instruction.mnemonic == ZYDIS_MNEMONIC_JNZ)
                    {
                        finding.replacement.assign(region, 0x90);
                        finding.patchable = true;
                        finding.description += "; jnz -> nop (fall through to the handheld path)";
                    }
                    if (!finding.patchable)
                        finding.description += consumer ? "; result used by " + Describe(consumer->instruction, consumer->operands.data(),
                            consumer->address) + ", unsupported shape, not patched" : "; no consumer found, not patched";
                    findings.push_back(finding);
                    return;
                }
                // The form value's register was overwritten by something other than a tracked copy: drop it.
                if (insn.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && (insn.operands[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
                {
                    const auto enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, insn.operands[0].reg.value);
                    holders.erase(std::remove(holders.begin(), holders.end(), enclosing), holders.end());
                }
                if (mn == ZYDIS_MNEMONIC_RET || mn == ZYDIS_MNEMONIC_INT3) return;
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
        for (const auto& range : pe.Functions())
        {
            const auto function = Decode(pe, image, range.first, range.second);
            for (std::size_t i = 0; i < function.size(); ++i)
            {
                const auto& insn = function[i];
                if (insn.instruction.mnemonic != ZYDIS_MNEMONIC_CALL || insn.operands[0].type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
                ZyanU64 target = 0;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn.instruction, &insn.operands[0], insn.address, &target))) continue;
                if (std::find(slots.begin(), slots.end(), static_cast<std::uint32_t>(target)) == slots.end()) continue;
                FollowFormCall(pe, image, function, i, findings);
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
