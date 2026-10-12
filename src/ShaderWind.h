#pragma once
#include <d3d9.h>
#include <algorithm>
#include <cstring>
#include <array>
#include <vector>

// Adds wind displacement to an existing D3D9 vertex shader (vs_2_x / vs_3_0)
// by editing its bytecode:
//
//   dcl_texcoord7 vN            ; per-vertex sway weight (from our vertex buffer)
//   mov rT, v<position>         ; copy the original position input ...
//   mad rT.xyz, vN, cK, rT      ; ... and add weight * wind offset (constant cK)
//
// and every later read of the position input is redirected to rT. The
// caller sets cK to the wind offset before drawing.
namespace shaderWind {
struct Patched {
    std::vector<DWORD> code; // Empty when the shader cannot be patched safely.
    unsigned constant = 0;   // Register index of the wind constant.
};

// Register type of a parameter token (split across two bit fields).
inline unsigned type(DWORD token) {
    return ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
           ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
}
// Builds a parameter token for register `number` of type `kind`.
inline DWORD reg(unsigned kind, unsigned number) {
    return 0x80000000u | number | ((kind << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) |
           ((kind << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2);
}

// Number of DWORDs that follow the instruction token at `token`.
inline unsigned operandCount(DWORD token) {
    unsigned op = token & D3DSI_OPCODE_MASK;
    return op == D3DSIO_COMMENT ? (token >> 16) & 0x7fff
                                : (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
}
inline bool isDefinition(unsigned op) {
    return op == D3DSIO_DEF || op == D3DSIO_DEFI || op == D3DSIO_DEFB;
}
// Comments, declarations and constant definitions precede the executable body.
inline bool isHeader(unsigned op) {
    return op == D3DSIO_COMMENT || op == D3DSIO_DCL || isDefinition(op);
}
inline bool isDeclaration(const std::vector<DWORD> &source, size_t at, unsigned count,
                          unsigned usage, unsigned usageIndex) {
    unsigned op = source[at] & D3DSI_OPCODE_MASK;
    return op == D3DSIO_DCL && count == 2 && (source[at + 1] & D3DSP_DCL_USAGE_MASK) == usage &&
           ((source[at + 1] & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT) ==
               usageIndex;
}

static constexpr DWORD CTAB_TAG = 0x42415443; // "CTAB"

// Marks every float-constant range described by the shader's constant table
// (CTAB) as used, including whole arrays that may be indexed dynamically.
// Returns false for malformed bytecode or tables.
inline bool reserveTableConstants(const std::vector<DWORD> &source,
                                  std::array<bool, 256> &constants, bool &constantTable) {
    for (size_t at = 1; at < source.size();) {
        unsigned op = source[at] & D3DSI_OPCODE_MASK;
        if (op == D3DSIO_END)
            break;
        unsigned count = operandCount(source[at]);
        if (at + count >= source.size())
            return false;
        if (op == D3DSIO_COMMENT && count >= 8 && source[at + 1] == CTAB_TAG) {
            const unsigned char *table =
                reinterpret_cast<const unsigned char *>(source.data() + at + 2);
            size_t bytes = (count - 1) * 4;
            DWORD entries = 0, offset = 0;
            memcpy(&entries, table + 12, 4);
            memcpy(&offset, table + 16, 4);
            if (entries > 200 || offset > bytes || entries * 20 > bytes - offset)
                return false;
            // D3DXSHADER_CONSTANTINFO: RegisterSet at +4, RegisterIndex +6, RegisterCount +8.
            for (DWORD i = 0; i < entries; ++i) {
                WORD set = 0, first = 0, length = 0;
                memcpy(&set, table + offset + i * 20 + 4, 2);
                memcpy(&first, table + offset + i * 20 + 6, 2);
                memcpy(&length, table + offset + i * 20 + 8, 2);
                if (set == 2) { // D3DXRS_FLOAT4
                    if (first + length > constants.size())
                        return false;
                    for (unsigned c = first; c < unsigned(first + length); ++c)
                        constants[c] = true;
                }
            }
            constantTable = true;
        }
        at += count + 1;
    }
    return true;
}

// Registers used by the shader and where the extra instructions go.
struct Usage {
    std::array<bool, 32> temps{};
    std::array<bool, 16> inputs{};
    std::array<bool, 256> constants{};
    bool constantTable = false;
    unsigned position = 16; // Input register holding POSITION0 (16 = not found).
    size_t insertion = 1;   // Index just after the last leading header instruction.
};

// Records every register the shader touches. Returns false when patching would
// be unsafe: a TEXCOORD7 input already exists, relative addressing is used on
// anything but CTAB-described constants, or the bytecode is malformed/truncated.
inline bool scanRegisters(const std::vector<DWORD> &source, Usage &usage) {
    bool body = false, end = false;
    for (size_t at = 1; at < source.size();) {
        unsigned op = source[at] & D3DSI_OPCODE_MASK;
        if (op == D3DSIO_END) {
            end = at + 1 == source.size();
            break;
        }
        unsigned count = operandCount(source[at]);
        if (at + count >= source.size())
            return false;
        if (op != D3DSIO_COMMENT) {
            if (isDeclaration(source, at, count, D3DDECLUSAGE_TEXCOORD, 7))
                return false;
            if (isDeclaration(source, at, count, D3DDECLUSAGE_POSITION, 0) &&
                type(source[at + 2]) == D3DSPR_INPUT)
                usage.position = source[at + 2] & D3DSP_REGNUM_MASK;
            // Relative addressing is safe only for CTAB-described float arrays.
            for (unsigned i = 1; i <= count; ++i) {
                if (op == D3DSIO_DCL && i == 1)
                    continue; // Usage token, not a register.
                DWORD token = source[at + i];
                if (!(token & 0x80000000u))
                    continue;
                if (!isDefinition(op) && (token & D3DSHADER_ADDRESSMODE_MASK) &&
                    (!usage.constantTable || type(token) != D3DSPR_CONST))
                    return false;
                if (isDefinition(op) && i > 1)
                    continue; // Literal values.
                unsigned n = token & D3DSP_REGNUM_MASK, t = type(token);
                if (t == D3DSPR_TEMP && n < usage.temps.size())
                    usage.temps[n] = true;
                if (t == D3DSPR_INPUT && n < usage.inputs.size())
                    usage.inputs[n] = true;
                if (t == D3DSPR_CONST && n < usage.constants.size())
                    usage.constants[n] = true;
            }
        }
        if (!body && isHeader(op))
            usage.insertion = at + count + 1;
        else
            body = true;
        at += count + 1;
    }
    return end && usage.position < 16;
}

// Highest index in [low, limit) that is not marked used, or -1.
template <size_t N> inline int highestFree(const std::array<bool, N> &used, int limit, int low) {
    for (int i = limit - 1; i >= low; --i)
        if (!used[i])
            return i;
    return -1;
}

// Returns the patched shader, or an empty result if it cannot be patched safely.
// `maxConstants` is the device's vertex-shader constant limit; the wind constant
// is placed in the highest free register at or above c40.
inline Patched patch(const std::vector<DWORD> &source, unsigned maxConstants = 256) {
    Patched result;
    // Only vertex shaders (0xfffe) of version 2.x or 3.x.
    if (source.size() < 3 || D3DSHADER_VERSION_MAJOR(source[0]) < 2 ||
        D3DSHADER_VERSION_MAJOR(source[0]) > 3 || (source[0] >> 16) != 0xfffe)
        return result;
    Usage usage;
    if (!reserveTableConstants(source, usage.constants, usage.constantTable) ||
        !scanRegisters(source, usage))
        return {};
    int input = highestFree(usage.inputs, 16, 0);
    int tempLimit = D3DSHADER_VERSION_MAJOR(source[0]) == 3 ? 32 : 12;
    int temp = highestFree(usage.temps, tempLimit, 0);
    int constant = highestFree(usage.constants, int(std::min(maxConstants, 256u)), 40);
    if (input < 0 || temp < 0 || constant < 0)
        return {};
    unsigned position = usage.position;

    // Redirect position reads in executable instructions to the temp register.
    result.code = source;
    for (size_t at = 1; at < source.size();) {
        unsigned op = source[at] & D3DSI_OPCODE_MASK;
        if (op == D3DSIO_END)
            break;
        unsigned count = operandCount(source[at]);
        if (!isHeader(op))
            for (unsigned i = 1; i <= count; ++i) {
                DWORD token = source[at + i];
                if (type(token) == D3DSPR_INPUT && (token & D3DSP_REGNUM_MASK) == position)
                    result.code[at + i] =
                        (token & ~(D3DSP_REGTYPE_MASK | D3DSP_REGTYPE_MASK2 | D3DSP_REGNUM_MASK)) |
                        reg(D3DSPR_TEMP, temp);
            }
        at += count + 1;
    }

    const DWORD instructions[] = {
        // dcl_texcoord7 v<input>
        D3DSIO_DCL | (2u << 24),
        0x80000000u | D3DDECLUSAGE_TEXCOORD | (7u << 16),
        reg(D3DSPR_INPUT, input) | D3DSP_WRITEMASK_ALL,
        // mov r<temp>, v<position>
        D3DSIO_MOV | (2u << 24),
        reg(D3DSPR_TEMP, temp) | D3DSP_WRITEMASK_ALL,
        reg(D3DSPR_INPUT, position) | D3DSP_NOSWIZZLE,
        // mad r<temp>.xyz, v<input>, c<constant>, r<temp>
        D3DSIO_MAD | (4u << 24),
        reg(D3DSPR_TEMP, temp) | D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 | D3DSP_WRITEMASK_2,
        reg(D3DSPR_INPUT, input),
        reg(D3DSPR_CONST, constant) | D3DSP_NOSWIZZLE,
        reg(D3DSPR_TEMP, temp) | D3DSP_NOSWIZZLE,
    };
    result.code.insert(result.code.begin() + usage.insertion, std::begin(instructions),
                       std::end(instructions));
    result.constant = constant;
    return result;
}
} // namespace shaderWind
