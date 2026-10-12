// AlphaWorldMotion: wind-swayed leaves and drifting clouds for Cube World Alpha
// (32-bit, Direct3D 9).
//
// Leaves: when a terrain chunk is drawn for the first time, a copy of its vertex
// buffer is built (a few quads per frame) with an extra per-vertex sway weight
// for foliage corners. Later draws of that chunk use the copy together with a
// patched version of the native vertex shader (see ShaderWind.h) that adds
// weight * wind offset to each position. The world itself is never modified.
//
// Clouds: draws of the cloud model are translated by a slowly changing offset.
//
// AlphaShadows calls ReplayAlphaMotion() so shadows match the moved geometry,
// and QueryAlphaMotionOrigin() to keep cloud shadows aligned. F6 toggles motion.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "MotionRules.h"
#include "ShaderWind.h"

namespace {

// ===========================================================================
// Constants
// ===========================================================================

// IDirect3DDevice9 vtable slots that are hooked.
constexpr unsigned SLOT_RESET = 16, SLOT_PRESENT = 17, SLOT_DRAW_INDEXED_PRIMITIVE = 82;

// Native vertex-shader constant registers.
constexpr UINT NATIVE_WORLD_VIEW_PROJ = 20; // 4 registers.
constexpr UINT NATIVE_WORLD = 27;           // 3 registers.

// Cube World Alpha memory layout (offsets from the game object + `layout`).
constexpr DWORD GAME_PLAYER = 0x8006c8;
constexpr DWORD GAME_CHUNK_CACHE_WIDTH = 0x2d4;
constexpr DWORD GAME_REGIONS = 0x398;
constexpr DWORD REGION_ZONES = 0x10018;
constexpr DWORD ZONE_COORDINATES = 0x60;
constexpr DWORD ZONE_COLUMNS = 0xa8;
// Model array entries and model fields.
constexpr DWORD CLOUD_MODEL_INDEX = 0xa08;
constexpr DWORD PROBE_MODEL_INDEX = 0x988; // A 16x16x25 model used to find the array.
constexpr DWORD MODEL_VERTEX_BUFFER = 0x34;
constexpr DWORD MODEL_DIMENSIONS = 0x44;
constexpr DWORD MODEL_DEVICE = 0x58;

constexpr size_t MAX_SHADERS = 32, MAX_MESHES = 64;
constexpr ULONGLONG MESH_EXPIRY_MS = 3000; // Drop copies not drawn for this long.

// ===========================================================================
// State
// ===========================================================================

HMODULE module;
IDirect3DDevice9 *device;
void **vtable;
DWORD models; // The game's model array.
DWORD game;   // The game object, once found.
DWORD layout; // 8 or 0 depending on the executable build.
bool enabled = true, useClouds = true, useLeaves = true;
// Settings (see AlphaWorldMotion.ini).
float amplitude = .12f, range = 96, cloudDistance = 32, speed = 1, cloudSpeed = 2;
ULONGLONG started, lastRefresh, lastStats, frame;
motion::Offset leafOffset{}, cloudOffset{}; // Current wind and cloud displacement.
// Statistics logged once a minute.
unsigned builtThisFrame, animatedDraws, cloudDraws, builds;
size_t cachedBytes;
constexpr size_t cacheLimit = 24 * 1024 * 1024;

using Dip = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using Present = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND,
                                  const RGNDATA *);
using Reset = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
Dip originalDip;
Present originalPresent;
Reset originalReset;

// ===========================================================================
// Utilities
// ===========================================================================

template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
std::wstring directory() {
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, std::size(path));
    std::wstring result(path);
    return result.substr(0, result.find_last_of(L"\\/"));
}
void log(const char *message) {
    FILE *file = nullptr;
    _wfopen_s(&file, (directory() + L"\\AlphaWorldMotion.log").c_str(), L"a");
    if (file) {
        fprintf(file, "%llu %s\n", GetTickCount64(), message);
        fclose(file);
    }
}
// Safe read of game memory (fails instead of faulting on bad pointers).
bool read(DWORD address, void *out, size_t bytes) {
    SIZE_T got = 0;
    return address > 0x10000 && bytes <= 16 * 1024 * 1024 &&
           ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)address, out, bytes, &got) &&
           got == bytes;
}
template <class T> T value(DWORD address) {
    T out{};
    read(address, &out, sizeof(out));
    return out;
}

// ===========================================================================
// Cached GPU objects
// ===========================================================================

// A native vertex shader and its wind-patched twin.
struct Shader {
    IDirect3DVertexShader9 *original = nullptr, *animated = nullptr;
    unsigned constant = 0; // Register that receives the wind offset.
    ~Shader() {
        drop(original);
        drop(animated);
    }
};
// A native terrain chunk vertex buffer and its sway-weighted copy (if it has foliage).
struct Mesh {
    IDirect3DVertexBuffer9 *original = nullptr, *animated = nullptr;
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    ULONGLONG seen = 0;
    size_t bytes = 0;
    int x = 0, y = 0, z = 0; // World block position of the chunk origin.
    ~Mesh() {
        drop(original);
        drop(animated);
        drop(declaration);
    }
};
std::map<IDirect3DVertexShader9 *, std::unique_ptr<Shader>> shaders;
std::map<IDirect3DVertexBuffer9 *, std::unique_ptr<Mesh>> meshes; // Keyed by native buffer.
std::map<IDirect3DVertexBuffer9 *, Mesh *> animatedBuffers;       // Keyed by our copy.
IDirect3DVertexBuffer9 *cloudBuffer;
std::array<double, 3> player{};
bool playerKnown;
std::vector<DWORD> gameCandidates; // Pointers that may be the game object.
size_t candidateCursor;

// ===========================================================================
// Game discovery
// ===========================================================================

// Reads the player position from candidate game object `candidate` and
// validates it. Updates `player` and returns true on success.
bool updatePlayer(DWORD candidate, DWORD candidateLayout) {
    DWORD entity = value<DWORD>(candidate + candidateLayout + GAME_PLAYER);
    long long fixed[3]{};
    float dimensions[3]{};
    DWORD width = value<DWORD>(candidate + candidateLayout + GAME_CHUNK_CACHE_WIDTH);
    DWORD exe = DWORD(uintptr_t(GetModuleHandleW(nullptr)));
    DWORD entityVtable = value<DWORD>(entity);
    if (entityVtable < exe + 0x2f0000 || entityVtable >= exe + 0x360000 ||
        !read(entity + 0x10, fixed, sizeof(fixed)) ||
        !read(entity + 0x80, dimensions, sizeof(dimensions)) || width < 1 || width > 64)
        return false;
    std::array<double, 3> position{};
    for (int i = 0; i < 3; ++i) {
        position[i] = fixed[i] / 65536.0;
        if (!std::isfinite(dimensions[i]) || dimensions[i] < .1f || dimensions[i] > 20)
            return false;
    }
    if (position[0] <= 0 || position[1] <= 0 || position[0] >= 16777216 ||
        position[1] >= 16777216 || std::abs(position[2]) > 10000)
        return false;
    player = position;
    return true;
}

// Accepts `candidate` as the game object if either build layout validates.
bool tryGame(DWORD candidate) {
    for (DWORD candidateLayout : {8u, 0u})
        if (updatePlayer(candidate, candidateLayout)) {
            game = candidate;
            layout = candidateLayout;
            return true;
        }
    return false;
}

// Twice a second: keep looking for the game object (64 candidates per call),
// refresh the cloud buffer pointer and expire unused mesh copies.
void refresh() {
    ULONGLONG now = GetTickCount64();
    if (now - lastRefresh < 500)
        return;
    lastRefresh = now;
    if (!game && !gameCandidates.empty())
        for (unsigned i = 0; i < 64; ++i)
            if (tryGame(gameCandidates[candidateCursor++ % gameCandidates.size()]))
                break;
    DWORD model = value<DWORD>(models + CLOUD_MODEL_INDEX * 4);
    cloudBuffer = (IDirect3DVertexBuffer9 *)(uintptr_t)value<DWORD>(model + MODEL_VERTEX_BUFFER);
    for (auto it = meshes.begin(); it != meshes.end();) {
        if (now - it->second->seen > MESH_EXPIRY_MS) {
            cachedBytes -= it->second->bytes;
            animatedBuffers.erase(it->second->animated);
            it = meshes.erase(it);
        } else
            ++it;
    }
}

// ===========================================================================
// Terrain sampling
// ===========================================================================

// Snapshot individual columns once while constructing a mesh. No world writes,
// whole-world walk, per-frame voxel scan or color-based leaf classification.
struct Column {
    int base = 0;
    std::vector<unsigned char> cells;
};
struct Samples {
    std::map<std::pair<int, int>, Column> columns;

    // Reads one column of the world (its 32-byte header and cells).
    static Column load(int x, int y) {
        Column column;
        int zx = x / 256, zy = y / 256;
        DWORD region =
            value<DWORD>(game + layout + GAME_REGIONS + ((zx / 64) * 1024 + zy / 64) * 4);
        DWORD zone =
            region ? value<DWORD>(region + REGION_ZONES + ((zx % 64) * 64 + zy % 64) * 4) : 0;
        int coords[2]{};
        DWORD data = 0, count = 0, cells = 0;
        if (zone && read(zone + ZONE_COORDINATES, coords, sizeof(coords)) && coords[0] == zx &&
            coords[1] == zy) {
            cells = value<DWORD>(zone + ZONE_COLUMNS);
            DWORD address = cells + (x % 256 + y % 256 * 256) * 32;
            DWORD header[8]{};
            if (cells && read(address, header, sizeof(header))) {
                column.base = int(header[4]);
                data = header[6];
                count = header[7];
                if (data && count && count <= 2048) {
                    column.cells.resize(count * 4);
                    // Re-check the header so a column rebuilt mid-read is discarded.
                    if (!read(data, column.cells.data(), column.cells.size()) ||
                        value<DWORD>(address + 24) != data || value<DWORD>(address + 28) != count)
                        column.cells.clear();
                }
            }
        }
        return column;
    }

    // Material of the block at world (x, y, z): 0 above the column, -1 if unknown.
    int type(int x, int y, int z) {
        if (x < 0 || y < 0 || x >= 16777216 || y >= 16777216)
            return -1;
        auto key = std::make_pair(x, y);
        auto found = columns.find(key);
        if (found == columns.end())
            found = columns.emplace(key, load(x, y)).first;
        auto &column = found->second;
        if (column.cells.empty())
            return -1;
        int at = z - column.base;
        if (at < 0)
            return -1;
        if (size_t(at) >= column.cells.size() / 4)
            return 0;
        return column.cells[at * 4 + 3] & 31;
    }
};

// Returns the cached entry for a native shader, creating its patched twin on
// first use (nullptr once MAX_SHADERS shaders are cached).
Shader *shader(IDirect3DVertexShader9 *original) {
    auto found = shaders.find(original);
    if (found != shaders.end())
        return found->second.get();
    if (shaders.size() >= MAX_SHADERS)
        return nullptr;
    auto entry = std::make_unique<Shader>();
    entry->original = original;
    original->AddRef();
    UINT bytes = 0;
    original->GetFunction(nullptr, &bytes);
    if (bytes >= 8 && bytes <= 65536 && bytes % 4 == 0) {
        std::vector<DWORD> code(bytes / 4);
        if (SUCCEEDED(original->GetFunction(code.data(), &bytes))) {
            D3DCAPS9 caps{};
            device->GetDeviceCaps(&caps);
            auto patched = shaderWind::patch(code, caps.MaxVertexShaderConst);
            if (!patched.code.empty() &&
                SUCCEEDED(device->CreateVertexShader(patched.code.data(), &entry->animated)))
                entry->constant = patched.constant;
        }
    }
    auto result = entry.get();
    shaders.emplace(original, std::move(entry));
    return result;
}

// ===========================================================================
// Animated mesh construction
// ===========================================================================

// Layout of our vertex-buffer copy: float4 position, colour, sway weight.
struct Vertex {
    float p[4];
    DWORD color;
    float weight;
};
// A mesh copy being built. Native vertices are 8 bytes: UBYTE4 position
// (w = face id 0..5) followed by a D3DCOLOR.
struct Pending {
    std::unique_ptr<Mesh> mesh;
    std::vector<unsigned char> source;
    std::vector<Vertex> vertices;
    Samples samples;
    size_t next = 0;     // Next vertex to process (multiple of 4: one quad).
    unsigned moving = 0; // Vertices with a non-zero sway weight.
};
std::unique_ptr<Pending> pending;

// Creates the GPU buffer and declaration for a finished copy. Leaves
// mesh.animated null if nothing moves or creation fails.
void uploadAnimatedMesh(Pending &job) {
    auto &mesh = *job.mesh;
    auto bytes = UINT(job.vertices.size() * sizeof(Vertex));
    if (SUCCEEDED(device->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                             &mesh.animated, nullptr))) {
        void *target = nullptr;
        if (SUCCEEDED(mesh.animated->Lock(0, bytes, &target, 0))) {
            memcpy(target, job.vertices.data(), bytes);
            mesh.animated->Unlock();
            mesh.bytes = bytes;
        } else
            drop(mesh.animated);
    }
    if (mesh.animated) {
        D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            {0, 20, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7},
            D3DDECL_END()};
        if (FAILED(device->CreateVertexDeclaration(elements, &mesh.declaration))) {
            drop(mesh.animated);
            mesh.bytes = 0;
        }
    }
}

// Mesh preparation is incremental: at most 256 quads per frame, plus one
// immutable VB read/upload on admission. No animated VB uploads after creation.
void advance() {
    if (!pending)
        return;
    auto &job = *pending;
    auto &mesh = *job.mesh;
    size_t end = std::min(job.next + 1024, job.vertices.size());
    LARGE_INTEGER begin{}, frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    const int normals[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (; job.next < end; job.next += 4) {
        // Never spend more than 1 ms per frame.
        if (job.next % 64 == 0) {
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - begin.QuadPart) * 1000 >= frequency.QuadPart)
                break;
        }
        size_t at = job.next;
        unsigned normal = job.source[at * 8 + 3];
        float center[3]{};
        bool quad = normal < 6; // All four vertices must share one valid face id.
        for (size_t j = 0; j < 4; ++j) {
            Vertex &v = job.vertices[at + j];
            for (int axis = 0; axis < 4; ++axis)
                v.p[axis] = job.source[(at + j) * 8 + axis];
            memcpy(&v.color, &job.source[(at + j) * 8 + 4], 4);
            v.weight = 0;
            quad &= job.source[(at + j) * 8 + 3] == normal;
            for (int axis = 0; axis < 3; ++axis)
                center[axis] += v.p[axis] * .25f;
        }
        if (!quad)
            continue;
        // The block behind the face must be foliage.
        int x = int(std::floor(center[0] - normals[normal][0] * .25f));
        int y = int(std::floor(center[1] - normals[normal][1] * .25f));
        int z = mesh.z + int(std::floor(center[2] - normals[normal][2] * .25f));
        if (!motion::foliage(job.samples.type(mesh.x + x, mesh.y + y, z)))
            continue;
        for (size_t j = 0; j < 4; ++j) {
            Vertex &v = job.vertices[at + j];
            v.weight = motion::corner(int(v.p[0]), int(v.p[1]), mesh.z + int(v.p[2]),
                                      [&](int cx, int cy, int cz) {
                                          return job.samples.type(mesh.x + cx, mesh.y + cy, cz);
                                      });
            job.moving += v.weight > 0;
        }
    }
    if (job.next < job.vertices.size())
        return;
    // Finished: upload the copy (only if something moves) and cache the mesh.
    if (job.moving)
        uploadAnimatedMesh(job);
    cachedBytes += mesh.bytes;
    mesh.seen = GetTickCount64();
    if (mesh.animated)
        animatedBuffers.emplace(mesh.animated, &mesh);
    meshes.emplace(mesh.original, std::move(job.mesh));
    pending.reset();
    ++builds;
}

// Starts building a copy of a terrain chunk's vertex buffer (one per frame, and
// only one in progress at a time).
void admit(IDirect3DVertexBuffer9 *buffer, IDirect3DVertexDeclaration9 *decl,
           const float world[12]) {
    if (pending || builtThisFrame || meshes.size() >= MAX_MESHES || !playerKnown ||
        !updatePlayer(game, layout))
        return;
    // Only Alpha's terrain format: UBYTE4 position at 0, D3DCOLOR at 4.
    D3DVERTEXELEMENT9 elements[64]{};
    UINT count = 64;
    if (FAILED(decl->GetDeclaration(elements, &count)) || count != 3 ||
        elements[0].Type != D3DDECLTYPE_UBYTE4 || elements[0].Stream != 0 ||
        elements[0].Offset != 0 || elements[0].Usage != D3DDECLUSAGE_POSITION ||
        elements[1].Type != D3DDECLTYPE_D3DCOLOR || elements[1].Stream != 0 ||
        elements[1].Offset != 4 || elements[1].Usage != D3DDECLUSAGE_COLOR)
        return;
    D3DVERTEXBUFFER_DESC desc{};
    // Our copy is 3x the size of the native buffer (24 vs 8 bytes per vertex).
    if (FAILED(buffer->GetDesc(&desc)) || !desc.Size || desc.Size % 32 || desc.Size > 512 * 1024 ||
        desc.Size * 3 > cacheLimit - cachedBytes)
        return;
    // Alpha renders terrain in player-relative X/Y, keeping absolute terrain Z.
    // Require exact chunk alignment; unfamiliar transforms remain untouched.
    double gx = player[0] + world[3], gy = player[1] + world[7];
    int x = int(std::round(gx / 32)) * 32, y = int(std::round(gy / 32)) * 32;
    if (std::abs(gx - x) > .03 || std::abs(gy - y) > .03 ||
        std::abs(world[11] - std::round(world[11])) > .001f)
        return;
    auto job = std::make_unique<Pending>();
    job->mesh = std::make_unique<Mesh>();
    job->mesh->original = buffer;
    buffer->AddRef();
    job->mesh->x = x;
    job->mesh->y = y;
    job->mesh->z = int(std::round(world[11]));
    job->source.resize(desc.Size);
    job->vertices.resize(desc.Size / 8);
    void *source = nullptr;
    if (FAILED(buffer->Lock(0, desc.Size, &source, D3DLOCK_READONLY)))
        return;
    memcpy(job->source.data(), source, desc.Size);
    buffer->Unlock();
    pending = std::move(job);
    ++builtThisFrame;
}

void cleanup() {
    pending.reset();
    animatedBuffers.clear();
    meshes.clear();
    shaders.clear();
    cachedBytes = 0;
}

// Terrain chunks have an identity rotation/scale (translation only).
bool terrain(const float w[12]) {
    return std::abs(w[0] - 1) + std::abs(w[5] - 1) + std::abs(w[10] - 1) + std::abs(w[1]) +
               std::abs(w[2]) + std::abs(w[4]) + std::abs(w[6]) + std::abs(w[8]) + std::abs(w[9]) <
           .00001f;
}

// ===========================================================================
// Drawing
// ===========================================================================

// Translates a cloud draw by cloudOffset: adjusts both the world matrix and
// the world-view-projection (solving the local translation from the affine
// world basis). Returns false when the basis is degenerate.
bool shiftCloud(IDirect3DDevice9 *d, const float world[12], const float wvp[16]) {
    float det = world[0] * world[5] - world[1] * world[4];
    if (!(std::abs(det) > 1e-8f))
        return false;
    float dx = (cloudOffset.x * world[5] - cloudOffset.y * world[1]) / det;
    float dy = (cloudOffset.y * world[0] - cloudOffset.x * world[4]) / det;
    float shifted[16];
    std::copy(wvp, wvp + 16, shifted);
    for (int r = 0; r < 4; ++r)
        shifted[r * 4 + 3] += wvp[r * 4] * dx + wvp[r * 4 + 1] * dy;
    float translated[12];
    std::copy(world, world + 12, translated);
    translated[3] += cloudOffset.x;
    translated[7] += cloudOffset.y;
    d->SetVertexShaderConstantF(NATIVE_WORLD_VIEW_PROJ, shifted, 4);
    d->SetVertexShaderConstantF(NATIVE_WORLD, translated, 3);
    ++cloudDraws;
    return true;
}

// Shared by the game's own draws (dip) and AlphaShadows' replays
// (ReplayAlphaMotion, replayPass = true). Swaps in the animated mesh/shader or
// the shifted cloud transform around `forward`, then restores the native state.
HRESULT drawMotion(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT min, UINT count,
                   UINT start, UINT primitives, bool replayPass, Dip forward) {
    if (!enabled || type != D3DPT_TRIANGLELIST)
        return forward(d, type, base, min, count, start, primitives);
    IDirect3DVertexBuffer9 *buffer = nullptr;
    UINT offset = 0, stride = 0;
    d->GetStreamSource(0, &buffer, &offset, &stride);
    if (!buffer)
        return forward(d, type, base, min, count, start, primitives);
    // `cached`: a native chunk we already processed. `animated`: the buffer is
    // one of our copies (bound by AlphaShadows when it captured the draw).
    auto cached = meshes.find(buffer);
    auto animated = animatedBuffers.find(buffer);
    bool isCached = cached != meshes.end(), isAnimated = animated != animatedBuffers.end();
    if ((replayPass && !isCached && !isAnimated) ||
        (isCached && !cached->second->animated && buffer != cloudBuffer)) {
        // Nothing moves here.
        if (isCached)
            cached->second->seen = GetTickCount64();
        drop(buffer);
        return forward(d, type, base, min, count, start, primitives);
    }
    float world[12]{};
    // Shadow captures already contain cloud drift; do not translate them twice.
    bool changedCloud = !replayPass && useClouds && buffer == cloudBuffer;
    bool leafCandidate = useLeaves && (stride == 8 || isAnimated);
    if (!changedCloud && !leafCandidate) {
        drop(buffer);
        return forward(d, type, base, min, count, start, primitives);
    }
    d->GetVertexShaderConstantF(NATIVE_WORLD, world, 3);
    float wvp[16]{};
    if (changedCloud) {
        d->GetVertexShaderConstantF(NATIVE_WORLD_VIEW_PROJ, wvp, 4);
        changedCloud = shiftCloud(d, world, wvp);
    }

    // Find (or start building) the animated copy of a nearby terrain chunk.
    Mesh *mesh = nullptr;
    if (isAnimated)
        mesh = animated->second;
    else if (!changedCloud && leafCandidate && offset == 0 && terrain(world) &&
             std::hypot(world[3], world[7]) <= range + 32) {
        if (isCached) {
            mesh = cached->second.get();
            mesh->seen = GetTickCount64();
        } else if (!replayPass) {
            IDirect3DVertexDeclaration9 *decl = nullptr;
            d->GetVertexDeclaration(&decl);
            if (decl)
                admit(buffer, decl, world);
            drop(decl);
        }
    }

    // Bind the animated copy with the wind-patched shader.
    IDirect3DVertexShader9 *nativeShader = nullptr;
    IDirect3DVertexDeclaration9 *nativeDecl = nullptr;
    float oldConstant[4]{};
    Shader *patched = nullptr;
    if (mesh && mesh->animated) {
        d->GetVertexShader(&nativeShader);
        if (nativeShader)
            patched = shader(nativeShader);
        if (patched && patched->animated) {
            d->GetVertexDeclaration(&nativeDecl);
            // Sway fades out over the last 32 blocks of the leaf range.
            float fade = std::clamp((range + 32 - std::hypot(world[3], world[7])) / 32.f, 0.f, 1.f);
            float wind[4] = {leafOffset.x * fade, leafOffset.y * fade, 0, 0};
            d->GetVertexShaderConstantF(patched->constant, oldConstant, 1);
            d->SetVertexShaderConstantF(patched->constant, wind, 1);
            d->SetVertexShader(patched->animated);
            d->SetVertexDeclaration(mesh->declaration);
            d->SetStreamSource(0, mesh->animated, 0, sizeof(Vertex));
            ++animatedDraws;
        }
    }
    HRESULT result = forward(d, type, base, min, count, start, primitives);

    // Restore the native state.
    if (nativeDecl) {
        d->SetStreamSource(0, buffer, offset, stride);
        d->SetVertexDeclaration(nativeDecl);
        d->SetVertexShader(nativeShader);
        d->SetVertexShaderConstantF(patched->constant, oldConstant, 1);
    }
    if (changedCloud) {
        d->SetVertexShaderConstantF(NATIVE_WORLD_VIEW_PROJ, wvp, 4);
        d->SetVertexShaderConstantF(NATIVE_WORLD, world, 3);
    }
    drop(nativeDecl);
    drop(nativeShader);
    drop(buffer);
    return result;
}

// ===========================================================================
// Device hooks
// ===========================================================================

HRESULT WINAPI dip(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT min, UINT count,
                   UINT start, UINT primitives) {
    return drawMotion(d, type, base, min, count, start, primitives, false, originalDip);
}

HRESULT WINAPI present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w,
                       const RGNDATA *r) {
    // Present executes outside the native scene. Prepare only one bounded batch.
    if (enabled) {
        refresh();
        playerKnown = game && updatePlayer(game, layout);
        advance();
    }
    double seconds = (GetTickCount64() - started) * .001 * speed;
    leafOffset = motion::leaves(seconds, amplitude);
    cloudOffset = motion::clouds(seconds * cloudSpeed, cloudDistance);
    builtThisFrame = 0;
    ++frame;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    d->GetCreationParameters(&creation);
    if (GetForegroundWindow() == creation.hFocusWindow && (GetAsyncKeyState(VK_F6) & 1))
        enabled = !enabled;
    if (GetTickCount64() - lastStats >= 60000) {
        char text[256];
        sprintf_s(text,
                  "draws leaves=%u clouds=%u; meshes=%zu; cache=%zu bytes; completed builds=%u",
                  animatedDraws, cloudDraws, meshes.size(), cachedBytes, builds);
        log(text);
        lastStats = GetTickCount64();
        animatedDraws = cloudDraws = builds = 0;
    }
    return originalPresent(d, a, b, w, r);
}

HRESULT WINAPI reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *p) {
    cleanup();
    lastRefresh = 0;
    return originalReset(d, p);
}

// Atomically replaces one device vtable entry, returning the previous function.
void *exchange(unsigned slot, void *replacement) {
    DWORD oldProtection = 0;
    if (!VirtualProtect(vtable + slot, sizeof(void *), PAGE_READWRITE, &oldProtection))
        return nullptr;
    void *old = InterlockedExchangePointer(vtable + slot, replacement);
    VirtualProtect(vtable + slot, sizeof(void *), oldProtection, &oldProtection);
    return old;
}

// ===========================================================================
// Startup
// ===========================================================================

void loadSettings() {
    auto ini = directory() + L"\\AlphaWorldMotion.ini";
    auto option = [&](const wchar_t *name, unsigned fallback, unsigned lo, unsigned hi) {
        return std::clamp(GetPrivateProfileIntW(L"Motion", name, fallback, ini.c_str()), lo, hi);
    };
    enabled = option(L"Enabled", 1, 0, 1) != 0;
    useClouds = option(L"Clouds", 1, 0, 1) != 0;
    useLeaves = option(L"Leaves", 1, 0, 1) != 0;
    amplitude = option(L"LeafAmplitudePercent", 12, 0, 25) * .01f;
    range = float(option(L"LeafRange", 96, 32, 160));
    cloudDistance = float(option(L"CloudDriftDistance", 32, 0, 64));
    speed = option(L"WindSpeedPercent", 100, 10, 200) * .01f;
    cloudSpeed = option(L"CloudSpeedPercent", 200, 25, 500) * .01f;
}

// Collects plausible pointers from the executable's .data section and one
// level below it.
std::set<DWORD> pointerCandidates(DWORD base, const IMAGE_NT_HEADERS32 &nt, DWORD sections) {
    std::set<DWORD> candidates;
    auto add = [&](DWORD address, size_t bytes) {
        if (bytes > 16 * 1024 * 1024)
            return;
        std::vector<DWORD> data(bytes / 4);
        if (read(address, data.data(), data.size() * 4))
            for (DWORD p : data)
                if (p > 0x10000 && p < 0x7fff0000 && p % 4 == 0)
                    candidates.insert(p);
    };
    for (int i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (read(sections + i * sizeof(section), &section, sizeof(section)) &&
            !memcmp(section.Name, ".data", 5))
            add(base + section.VirtualAddress, section.Misc.VirtualSize);
    }
    auto roots = candidates;
    for (DWORD p : roots)
        add(p, 16384);
    return candidates;
}

// Finds the model array (and through it the D3D9 device) and installs the hooks.
bool hookDevice(const std::set<DWORD> &candidates) {
    for (DWORD p : candidates) {
        DWORD model = value<DWORD>(p + PROBE_MODEL_INDEX * 4), dims[3]{};
        if (!read(model + MODEL_DIMENSIONS, dims, 12) || dims[0] != 16 || dims[1] != 16 ||
            dims[2] != 25)
            continue;
        device = (IDirect3DDevice9 *)(uintptr_t)value<DWORD>(model + MODEL_DEVICE);
        vtable = (void **)(uintptr_t)value<DWORD>(DWORD(uintptr_t(device)));
        if (!vtable)
            continue;
        // Remain outermost: shadows must capture the animated mesh/declaration.
        HMODULE shadows = GetModuleHandleW(L"AlphaShadows.dll"),
                taa = GetModuleHandleW(L"AlphaTAA.dll");
        MEMORY_BASIC_INFORMATION owner{};
        VirtualQuery(vtable[SLOT_DRAW_INDEXED_PRIMITIVE], &owner, sizeof(owner));
        if ((shadows && owner.AllocationBase != shadows) ||
            (!shadows && taa && owner.AllocationBase != taa))
            continue;
        models = p;
        gameCandidates.assign(candidates.begin(), candidates.end());
        started = GetTickCount64();
        lastStats = started;
        originalDip = (Dip)vtable[SLOT_DRAW_INDEXED_PRIMITIVE];
        originalPresent = (Present)vtable[SLOT_PRESENT];
        originalReset = (Reset)vtable[SLOT_RESET];
        exchange(SLOT_DRAW_INDEXED_PRIMITIVE, (void *)dip);
        exchange(SLOT_PRESENT, (void *)present);
        exchange(SLOT_RESET, (void *)reset);
        log("AlphaWorldMotion attached: cached foliage weights, GPU displacement, bounded cloud drift; F6 toggles motion.");
        return true;
    }
    return false;
}

DWORD WINAPI worker(void *) {
    loadSettings();
    DWORD base = DWORD(uintptr_t(GetModuleHandleW(nullptr)));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!read(base, &dos, sizeof(dos)) || !read(base + dos.e_lfanew, &nt, sizeof(nt)))
        return 0;
    DWORD sections = base + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    // Retry for up to three minutes while the game creates its device.
    for (int attempt = 0; attempt < 180; ++attempt) {
        std::set<DWORD> candidates = pointerCandidates(base, nt, sections);
        for (DWORD p : candidates)
            if (tryGame(p))
                break;
        if (hookDevice(candidates))
            return 0;
        Sleep(1000);
    }
    log("Supported Alpha render device not found; motion disabled.");
    return 0;
}
} // namespace

// ===========================================================================
// Exports
// ===========================================================================

// Explicit entry point for shadow draws that intentionally bypass the hook chain.
// The caller supplies its native draw function; no shadow-hook recursion is needed.
extern "C" __declspec(dllexport) HRESULT WINAPI ReplayAlphaMotion(IDirect3DDevice9 *d,
                                                                  D3DPRIMITIVETYPE type, INT base,
                                                                  UINT min, UINT count, UINT start,
                                                                  UINT primitives, Dip forward) {
    if (!forward)
        return D3DERR_INVALIDCALL;
    if (d != device)
        return forward(d, type, base, min, count, start, primitives);
    return drawMotion(d, type, base, min, count, start, primitives, true, forward);
}

// Reports the player's current world X/Y (the origin clouds drift relative to).
extern "C" __declspec(dllexport) BOOL WINAPI QueryAlphaMotionOrigin(double *xy) {
    if (!xy || !game || !updatePlayer(game, layout))
        return FALSE;
    xy[0] = player[0];
    xy[1] = player[1];
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = self;
        DisableThreadLibraryCalls(self);
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, std::size(path));
        auto name = wcsrchr(path, L'\\');
        if (name && !_wcsicmp(name + 1, L"Cube.exe")) {
            HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
            if (thread)
                CloseHandle(thread);
        }
    }
    return TRUE;
}
