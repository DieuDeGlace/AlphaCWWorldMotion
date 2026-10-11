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
HMODULE module;
IDirect3DDevice9 *device;
void **vtable;
DWORD models, game, layout;
bool enabled = true, useClouds = true, useLeaves = true;
float amplitude = .12f, range = 96, cloudDistance = 32, speed = 1;
ULONGLONG started, lastRefresh, lastStats, frame;
motion::Offset leafOffset{}, cloudOffset{};
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
struct Shader {
    IDirect3DVertexShader9 *original = nullptr, *animated = nullptr;
    unsigned constant = 0;
    ~Shader() {
        drop(original);
        drop(animated);
    }
};
struct Mesh {
    IDirect3DVertexBuffer9 *original = nullptr, *animated = nullptr;
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    ULONGLONG seen = 0;
    size_t bytes = 0;
    int x = 0, y = 0, z = 0;
    ~Mesh() {
        drop(original);
        drop(animated);
        drop(declaration);
    }
};
std::map<IDirect3DVertexShader9 *, std::unique_ptr<Shader>> shaders;
std::map<IDirect3DVertexBuffer9 *, std::unique_ptr<Mesh>> meshes;
std::map<IDirect3DVertexBuffer9 *, Mesh *> animatedBuffers;
IDirect3DVertexBuffer9 *cloudBuffer;
std::array<double, 3> player{};
bool playerKnown;
std::vector<DWORD> gameCandidates;
size_t candidateCursor;

bool updatePlayer(DWORD candidate, DWORD candidateLayout) {
    DWORD entity = value<DWORD>(candidate + candidateLayout + 0x8006c8);
    long long fixed[3]{};
    float dimensions[3]{};
    DWORD width = value<DWORD>(candidate + candidateLayout + 0x2d4);
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
void refresh() {
    ULONGLONG now = GetTickCount64();
    if (now - lastRefresh < 500)
        return;
    lastRefresh = now;
    if (!game && !gameCandidates.empty()) {
        for (unsigned i = 0; i < 64; ++i) {
            DWORD candidate = gameCandidates[candidateCursor++ % gameCandidates.size()];
            if (updatePlayer(candidate, 8)) {
                game = candidate;
                layout = 8;
                break;
            }
            if (updatePlayer(candidate, 0)) {
                game = candidate;
                layout = 0;
                break;
            }
        }
    }
    DWORD model = value<DWORD>(models + 0xa08 * 4);
    cloudBuffer = (IDirect3DVertexBuffer9 *)(uintptr_t)value<DWORD>(model + 0x34);
    for (auto it = meshes.begin(); it != meshes.end();) {
        if (now - it->second->seen > 3000) {
            cachedBytes -= it->second->bytes;
            animatedBuffers.erase(it->second->animated);
            it = meshes.erase(it);
        } else
            ++it;
    }
}
// Snapshot individual columns once while constructing a mesh. No world writes,
// whole-world walk, per-frame voxel scan or color-based leaf classification.
struct Column {
    int base = 0;
    std::vector<unsigned char> cells;
};
struct Samples {
    std::map<std::pair<int, int>, Column> columns;
    int type(int x, int y, int z) {
        if (x < 0 || y < 0 || x >= 16777216 || y >= 16777216)
            return -1;
        auto key = std::make_pair(x, y);
        auto found = columns.find(key);
        if (found == columns.end()) {
            Column column;
            int zx = x / 256, zy = y / 256;
            DWORD region = value<DWORD>(game + layout + 0x398 + ((zx / 64) * 1024 + zy / 64) * 4);
            DWORD zone =
                region ? value<DWORD>(region + 0x10018 + ((zx % 64) * 64 + zy % 64) * 4) : 0;
            int coords[2]{};
            DWORD data = 0, count = 0, cells = 0;
            if (zone && read(zone + 0x60, coords, sizeof(coords)) && coords[0] == zx &&
                coords[1] == zy) {
                cells = value<DWORD>(zone + 0xa8);
                DWORD address = cells + (x % 256 + y % 256 * 256) * 32;
                DWORD header[8]{};
                if (cells && read(address, header, sizeof(header))) {
                    column.base = int(header[4]);
                    data = header[6];
                    count = header[7];
                    if (data && count && count <= 2048) {
                        column.cells.resize(count * 4);
                        if (!read(data, column.cells.data(), column.cells.size()) ||
                            value<DWORD>(address + 24) != data ||
                            value<DWORD>(address + 28) != count)
                            column.cells.clear();
                    }
                }
            }
            found = columns.emplace(key, std::move(column)).first;
        }
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
Shader *shader(IDirect3DVertexShader9 *original) {
    auto found = shaders.find(original);
    if (found != shaders.end())
        return found->second.get();
    if (shaders.size() >= 32)
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
struct Vertex {
    float p[4];
    DWORD color;
    float weight;
};
struct Pending {
    std::unique_ptr<Mesh> mesh;
    std::vector<unsigned char> source;
    std::vector<Vertex> vertices;
    Samples samples;
    size_t next = 0;
    unsigned moving = 0;
};
std::unique_ptr<Pending> pending;
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
        if (job.next % 64 == 0) {
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - begin.QuadPart) * 1000 >= frequency.QuadPart)
                break;
        }
        size_t at = job.next;
        unsigned normal = job.source[at * 8 + 3];
        float center[3]{};
        bool quad = normal < 6;
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
    if (job.moving) {
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
    cachedBytes += mesh.bytes;
    mesh.seen = GetTickCount64();
    if (mesh.animated)
        animatedBuffers.emplace(mesh.animated, &mesh);
    meshes.emplace(mesh.original, std::move(job.mesh));
    pending.reset();
    ++builds;
}
void admit(IDirect3DVertexBuffer9 *buffer, IDirect3DVertexDeclaration9 *decl,
           const float world[12]) {
    if (pending || builtThisFrame || meshes.size() >= 64 || !playerKnown ||
        !updatePlayer(game, layout))
        return;
    D3DVERTEXELEMENT9 elements[64]{};
    UINT count = 64;
    if (FAILED(decl->GetDeclaration(elements, &count)) || count != 3 ||
        elements[0].Type != D3DDECLTYPE_UBYTE4 || elements[0].Stream != 0 ||
        elements[0].Offset != 0 || elements[0].Usage != D3DDECLUSAGE_POSITION ||
        elements[1].Type != D3DDECLTYPE_D3DCOLOR || elements[1].Stream != 0 ||
        elements[1].Offset != 4 || elements[1].Usage != D3DDECLUSAGE_COLOR)
        return;
    D3DVERTEXBUFFER_DESC desc{};
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
bool terrain(const float w[12]) {
    return std::abs(w[0] - 1) + std::abs(w[5] - 1) + std::abs(w[10] - 1) + std::abs(w[1]) +
               std::abs(w[2]) + std::abs(w[4]) + std::abs(w[6]) + std::abs(w[8]) + std::abs(w[9]) <
           .00001f;
}
HRESULT WINAPI dip(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT min, UINT count,
                   UINT start, UINT primitives) {
    if (!enabled || type != D3DPT_TRIANGLELIST)
        return originalDip(d, type, base, min, count, start, primitives);
    IDirect3DVertexBuffer9 *buffer = nullptr;
    UINT offset = 0, stride = 0;
    d->GetStreamSource(0, &buffer, &offset, &stride);
    if (!buffer)
        return originalDip(d, type, base, min, count, start, primitives);
    float world[12]{};
    bool changedCloud = useClouds && buffer == cloudBuffer;
    bool leafCandidate = useLeaves && (stride == 8 || animatedBuffers.count(buffer));
    if (!changedCloud && !leafCandidate) {
        drop(buffer);
        return originalDip(d, type, base, min, count, start, primitives);
    }
    d->GetVertexShaderConstantF(27, world, 3);
    float wvp[16]{};
    if (changedCloud) {
        d->GetVertexShaderConstantF(20, wvp, 4);
        // Solve local horizontal translation using the affine world basis.
        float det = world[0] * world[5] - world[1] * world[4];
        if (std::abs(det) > 1e-8f) {
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
            d->SetVertexShaderConstantF(20, shifted, 4);
            d->SetVertexShaderConstantF(27, translated, 3);
            ++cloudDraws;
        } else
            changedCloud = false;
    }
    Mesh *mesh = nullptr;
    auto replay = animatedBuffers.find(buffer);
    if (replay != animatedBuffers.end())
        mesh = replay->second;
    else if (!changedCloud && leafCandidate && offset == 0 && terrain(world) &&
             std::hypot(world[3], world[7]) <= range + 32) {
        auto found = meshes.find(buffer);
        if (found != meshes.end()) {
            mesh = found->second.get();
            mesh->seen = GetTickCount64();
        } else {
            IDirect3DVertexDeclaration9 *decl = nullptr;
            d->GetVertexDeclaration(&decl);
            if (decl)
                admit(buffer, decl, world);
            drop(decl);
        }
    }
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
    HRESULT result = originalDip(d, type, base, min, count, start, primitives);
    if (nativeDecl) {
        d->SetStreamSource(0, buffer, offset, stride);
        d->SetVertexDeclaration(nativeDecl);
        d->SetVertexShader(nativeShader);
        d->SetVertexShaderConstantF(patched->constant, oldConstant, 1);
    }
    if (changedCloud) {
        d->SetVertexShaderConstantF(20, wvp, 4);
        d->SetVertexShaderConstantF(27, world, 3);
    }
    drop(nativeDecl);
    drop(nativeShader);
    drop(buffer);
    return result;
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
    cloudOffset = motion::clouds(seconds, cloudDistance);
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
void *exchange(unsigned slot, void *replacement) {
    DWORD oldProtection = 0;
    if (!VirtualProtect(vtable + slot, sizeof(void *), PAGE_READWRITE, &oldProtection))
        return nullptr;
    void *old = InterlockedExchangePointer(vtable + slot, replacement);
    VirtualProtect(vtable + slot, sizeof(void *), oldProtection, &oldProtection);
    return old;
}
DWORD WINAPI worker(void *) {
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
    DWORD base = DWORD(uintptr_t(GetModuleHandleW(nullptr)));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!read(base, &dos, sizeof(dos)) || !read(base + dos.e_lfanew, &nt, sizeof(nt)))
        return 0;
    DWORD sections = base + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    for (int attempt = 0; attempt < 180; ++attempt) {
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
        for (DWORD p : candidates)
            if (updatePlayer(p, 8)) {
                game = p;
                layout = 8;
                break;
            } else if (updatePlayer(p, 0)) {
                game = p;
                layout = 0;
                break;
            }
        for (DWORD p : candidates) {
            DWORD model = value<DWORD>(p + 0x988 * 4), dims[3]{};
            if (!read(model + 0x44, dims, 12) || dims[0] != 16 || dims[1] != 16 || dims[2] != 25)
                continue;
            device = (IDirect3DDevice9 *)(uintptr_t)value<DWORD>(model + 0x58);
            vtable = (void **)(uintptr_t)value<DWORD>(DWORD(uintptr_t(device)));
            if (!vtable)
                continue;
            // Remain outermost: shadows must capture the animated mesh/declaration.
            HMODULE shadows = GetModuleHandleW(L"AlphaShadows.dll"),
                    taa = GetModuleHandleW(L"AlphaTAA.dll");
            MEMORY_BASIC_INFORMATION owner{};
            VirtualQuery(vtable[82], &owner, sizeof(owner));
            if ((shadows && owner.AllocationBase != shadows) ||
                (!shadows && taa && owner.AllocationBase != taa))
                continue;
            models = p;
            gameCandidates.assign(candidates.begin(), candidates.end());
            started = GetTickCount64();
            lastStats = started;
            originalDip = (Dip)vtable[82];
            originalPresent = (Present)vtable[17];
            originalReset = (Reset)vtable[16];
            exchange(82, (void *)dip);
            exchange(17, (void *)present);
            exchange(16, (void *)reset);
            log("AlphaWorldMotion attached: cached foliage weights, GPU displacement, bounded cloud drift; F6 toggles motion.");
            return 0;
        }
        Sleep(1000);
    }
    log("Supported Alpha render device not found; motion disabled.");
    return 0;
}
} // namespace
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
