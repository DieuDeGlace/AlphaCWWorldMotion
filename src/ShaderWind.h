#pragma once
#include <d3d9.h>
#include <algorithm>
#include <cstring>
#include <array>
#include <vector>

namespace shaderWind {
struct Patched {
    std::vector<DWORD> code;
    unsigned constant = 0;
};
inline unsigned type(DWORD token) {
    return ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
           ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
}
inline DWORD reg(unsigned kind, unsigned number) {
    return 0x80000000u | number | ((kind << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) |
           ((kind << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2);
}
// Preserve native bytecode, declarations, CTAB, lighting and face-normal IDs.
// Only position input references in executable instructions are redirected.
// The inserted two ALU instructions use genuinely unused registers.
inline Patched patch(const std::vector<DWORD> &source, unsigned maxConstants = 256) {
    Patched result;
    if (source.size() < 3 || D3DSHADER_VERSION_MAJOR(source[0]) < 2 ||
        D3DSHADER_VERSION_MAJOR(source[0]) > 3 || (source[0] >> 16) != 0xfffe)
        return result;
    std::array<bool, 32> temps{};
    std::array<bool, 16> inputs{};
    std::array<bool, 256> constants{};
    bool constantTable = false;
    // Reserve complete CTAB array ranges, including dynamically indexed lights.
    for (size_t at = 1; at < source.size();) {
        unsigned op = source[at] & D3DSI_OPCODE_MASK;
        if (op == D3DSIO_END)
            break;
        unsigned count = op == D3DSIO_COMMENT
                             ? (source[at] >> 16) & 0x7fff
                             : (source[at] & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        if (at + count >= source.size())
            return {};
        if (op == D3DSIO_COMMENT && count >= 8 && source[at + 1] == 0x42415443) {
            const unsigned char *table =
                reinterpret_cast<const unsigned char *>(source.data() + at + 2);
            size_t bytes = (count - 1) * 4;
            DWORD entries = 0, offset = 0;
            memcpy(&entries, table + 12, 4);
            memcpy(&offset, table + 16, 4);
            if (entries > 200 || offset > bytes || entries * 20 > bytes - offset)
                return {};
            for (DWORD i = 0; i < entries; ++i) {
                WORD set = 0, first = 0, length = 0;
                memcpy(&set, table + offset + i * 20 + 4, 2);
                memcpy(&first, table + offset + i * 20 + 6, 2);
                memcpy(&length, table + offset + i * 20 + 8, 2);
                if (set == 2) {
                    if (first + length > constants.size())
                        return {};
                    for (unsigned c = first; c < unsigned(first + length); ++c)
                        constants[c] = true;
                }
            }
            constantTable = true;
        }
        at += count + 1;
    }
    unsigned position = 16;
    size_t insertion = 1;
    bool body = false, end = false;
    for (size_t at = 1; at < source.size();) {
        unsigned op = source[at] & D3DSI_OPCODE_MASK;
        if (op == D3DSIO_END) {
            end = at + 1 == source.size();
            break;
        }
        unsigned count = op == D3DSIO_COMMENT
                             ? (source[at] >> 16) & 0x7fff
                             : (source[at] & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        if (at + count >= source.size())
            return {};
        if (op != D3DSIO_COMMENT) {
            if (op == D3DSIO_DCL && count == 2 &&
                (source[at + 1] & D3DSP_DCL_USAGE_MASK) == D3DDECLUSAGE_TEXCOORD &&
                ((source[at + 1] & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT) == 7)
                return {};
            if (op == D3DSIO_DCL && count == 2 &&
                (source[at + 1] & D3DSP_DCL_USAGE_MASK) == D3DDECLUSAGE_POSITION &&
                ((source[at + 1] & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT) == 0 &&
                type(source[at + 2]) == D3DSPR_INPUT)
                position = source[at + 2] & D3DSP_REGNUM_MASK;
            // Relative addressing is safe only for CTAB-described float arrays.
            for (unsigned i = 1; i <= count; ++i) {
                if (op == D3DSIO_DCL && i == 1)
                    continue;
                DWORD token = source[at + i];
                if (!(token & 0x80000000u))
                    continue;
                if (op != D3DSIO_DEF && op != D3DSIO_DEFI && op != D3DSIO_DEFB &&
                    (token & D3DSHADER_ADDRESSMODE_MASK) &&
                    (!constantTable || type(token) != D3DSPR_CONST))
                    return {};
                if ((op == D3DSIO_DEF || op == D3DSIO_DEFI || op == D3DSIO_DEFB) && i > 1)
                    continue;
                unsigned n = token & D3DSP_REGNUM_MASK, t = type(token);
                if (t == D3DSPR_TEMP && n < temps.size())
                    temps[n] = true;
                if (t == D3DSPR_INPUT && n < inputs.size())
                    inputs[n] = true;
                if (t == D3DSPR_CONST && n < constants.size())
                    constants[n] = true;
            }
        }
        bool header = op == D3DSIO_COMMENT || op == D3DSIO_DCL || op == D3DSIO_DEF ||
                      op == D3DSIO_DEFI || op == D3DSIO_DEFB;
        if (!body && header)
            insertion = at + count + 1;
        else
            body = true;
        at += count + 1;
    }
    if (!end || position >= 16)
        return {};
    int input = -1, temp = -1, constant = -1;
    for (int i = 15; i >= 0; --i)
        if (!inputs[i]) {
            input = i;
            break;
        }
    int tempLimit = D3DSHADER_VERSION_MAJOR(source[0]) == 3 ? 32 : 12;
    for (int i = tempLimit - 1; i >= 0; --i)
        if (!temps[i]) {
            temp = i;
            break;
        }
    for (int i = int(std::min(maxConstants, 256u)) - 1; i >= 40; --i)
        if (!constants[i]) {
            constant = i;
            break;
        }
    if (input < 0 || temp < 0 || constant < 0)
        return {};
    result.code = source;
    for (size_t at = 1; at < source.size();) {
        unsigned op = source[at] & D3DSI_OPCODE_MASK;
        if (op == D3DSIO_END)
            break;
        unsigned count = op == D3DSIO_COMMENT
                             ? (source[at] >> 16) & 0x7fff
                             : (source[at] & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        if (op != D3DSIO_COMMENT && op != D3DSIO_DCL && op != D3DSIO_DEF && op != D3DSIO_DEFI &&
            op != D3DSIO_DEFB)
            for (unsigned i = 1; i <= count; ++i) {
                DWORD token = source[at + i];
                if (type(token) == D3DSPR_INPUT && (token & D3DSP_REGNUM_MASK) == position)
                    result.code[at + i] =
                        (token & ~(D3DSP_REGTYPE_MASK | D3DSP_REGTYPE_MASK2 | D3DSP_REGNUM_MASK)) |
                        reg(D3DSPR_TEMP, temp);
            }
        at += count + 1;
    }
    const DWORD instructions[] = {D3DSIO_DCL | (2u << 24),
                                  0x80000000u | D3DDECLUSAGE_TEXCOORD | (7u << 16),
                                  reg(D3DSPR_INPUT, input) | D3DSP_WRITEMASK_ALL,
                                  D3DSIO_MOV | (2u << 24),
                                  reg(D3DSPR_TEMP, temp) | D3DSP_WRITEMASK_ALL,
                                  reg(D3DSPR_INPUT, position) | D3DSP_NOSWIZZLE,
                                  D3DSIO_MAD | (4u << 24),
                                  reg(D3DSPR_TEMP, temp) | D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                      D3DSP_WRITEMASK_2,
                                  reg(D3DSPR_INPUT, input),
                                  reg(D3DSPR_CONST, constant) | D3DSP_NOSWIZZLE,
                                  reg(D3DSPR_TEMP, temp) | D3DSP_NOSWIZZLE};
    result.code.insert(result.code.begin() + insertion, std::begin(instructions),
                       std::end(instructions));
    result.constant = constant;
    return result;
}
} // namespace shaderWind
