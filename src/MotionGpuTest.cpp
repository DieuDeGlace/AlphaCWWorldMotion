#include "AlphaWorldMotion.cpp"
#include <d3dcompiler.h>
#include <cassert>

using Compile = decltype(&D3DCompile);
static unsigned cloudChecks;
static HRESULT WINAPI cloudSpy(IDirect3DDevice9 *d, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
    float world[12]{}, projection[16]{};
    d->GetVertexShaderConstantF(27, world, 3);
    d->GetVertexShaderConstantF(20, projection, 4);
    assert(world[3] == 13 && world[7] == 18);
    assert(projection[3] == 1.5f && projection[7] == -.5f);
    ++cloudChecks;
    return D3D_OK;
}
static void cloudTransformTest() {
    IDirect3DVertexBuffer9 *buffer = nullptr;
    assert(SUCCEEDED(device->CreateVertexBuffer(32, 0, 0, D3DPOOL_MANAGED, &buffer, nullptr)));
    device->SetStreamSource(0, buffer, 0, 8);
    float world[] = {2, 0, 0, 10, 0, 4, 0, 20, 0, 0, 1, 0};
    float projection[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    device->SetVertexShaderConstantF(27, world, 3);
    device->SetVertexShaderConstantF(20, projection, 4);
    cloudBuffer = buffer;
    cloudOffset = {3, -2};
    enabled = useClouds = true;
    originalDip = cloudSpy;
    assert(SUCCEEDED(dip(device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2)) && cloudChecks == 1);
    float restoredWorld[12]{}, restoredProjection[16]{};
    device->GetVertexShaderConstantF(27, restoredWorld, 3);
    device->GetVertexShaderConstantF(20, restoredProjection, 4);
    assert(!memcmp(world, restoredWorld, sizeof(world)) &&
           !memcmp(projection, restoredProjection, sizeof(projection)));
    cloudBuffer = nullptr;
    device->SetStreamSource(0, nullptr, 0, 0);
    drop(buffer);
    puts("PASS: cloud drift through a scaled transform and exact native constant restoration.");
}
static IDirect3DVertexShader9 *compileVertex(Compile compile, const char *profile) {
    const char *code = R"HLSL(
float4 worldViewProjMatrix[4]:register(c20);
float4 worldMatrix[3]:register(c27);
float4 lightDirection:register(c33);
struct V { float4 p:POSITION; float4 c:COLOR0; float weight:TEXCOORD7; };
struct O { float4 p:POSITION; float4 c:COLOR0; };
O main(V v) {
 O o; float4 p=float4(v.p.xyz,1);
 o.p=float4(dot(worldViewProjMatrix[0],p),dot(worldViewProjMatrix[1],p),dot(worldViewProjMatrix[2],p),dot(worldViewProjMatrix[3],p));
 o.c=float4(v.c.rgb,1); o.c.b=dot(worldMatrix[0].xyz,float3(v.p.w,0,0))/5;
 return o;
})HLSL";
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT result = compile(code, strlen(code), nullptr, nullptr, nullptr, "main", profile,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(result) && errors)
        puts((const char *)errors->GetBufferPointer());
    assert(SUCCEEDED(result));
    IDirect3DVertexShader9 *shader = nullptr;
    assert(SUCCEEDED(device->CreateVertexShader((DWORD *)blob->GetBufferPointer(), &shader)));
    drop(blob);
    drop(errors);
    return shader;
}
static DWORD pixel(IDirect3DSurface9 *target, int x, int y) {
    IDirect3DSurface9 *cpu = nullptr;
    assert(SUCCEEDED(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                         &cpu, nullptr)));
    assert(SUCCEEDED(device->GetRenderTargetData(target, cpu)));
    D3DLOCKED_RECT rect{};
    assert(SUCCEEDED(cpu->LockRect(&rect, nullptr, D3DLOCK_READONLY)));
    DWORD result = *(DWORD *)((BYTE *)rect.pBits + y * rect.Pitch + x * 4);
    cpu->UnlockRect();
    drop(cpu);
    return result;
}
static void meshPreparationTest() {
    std::vector<DWORD> root(0x800700 / 4), region(0x10020 / 4), zone(0xac / 4), entity(0x100 / 4);
    std::vector<DWORD> columns(256 * 256 * 8);
    std::array<DWORD, 32> cells{};
    for (auto &cell : cells)
        cell = 0x08008040;
    auto address = [](auto &data) { return DWORD(uintptr_t(data.data())); };
    game = address(root);
    layout = 8;
    root[(layout + 0x398) / 4] = address(region);
    region[0x10018 / 4] = address(zone);
    zone[0xa8 / 4] = address(columns);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            size_t at = (x + y * 256) * 8;
            columns[at + 4] = 0;
            columns[at + 6] = address(cells);
            columns[at + 7] = 32;
        }
    root[(layout + 0x8006c8) / 4] = address(entity);
    root[(layout + 0x2d4) / 4] = 16;
    entity[0] = DWORD(uintptr_t(GetModuleHandleW(nullptr))) + 0x2fc8cc;
    long long position[] = {8 * 65536, 8 * 65536, 16 * 65536};
    memcpy((BYTE *)entity.data() + 0x10, position, sizeof(position));
    float dimensions[] = {1, 1, 2};
    memcpy((BYTE *)entity.data() + 0x80, dimensions, sizeof(dimensions));
    assert(updatePlayer(game, layout));
    playerKnown = true;
    unsigned char nativeVertices[] = {8,  8,   16, 4,  10,  20, 30, 255, 16, 8,  16,
                                      4,  10,  20, 30, 255, 16, 16, 16,  4,  10, 20,
                                      30, 255, 8,  16, 16,  4,  10, 20,  30, 255};
    IDirect3DVertexBuffer9 *buffer = nullptr;
    void *data = nullptr;
    assert(SUCCEEDED(device->CreateVertexBuffer(sizeof(nativeVertices), 0, 0, D3DPOOL_MANAGED,
                                                &buffer, nullptr)));
    assert(SUCCEEDED(buffer->Lock(0, 0, &data, 0)));
    memcpy(data, nativeVertices, sizeof(nativeVertices));
    buffer->Unlock();
    IDirect3DVertexDeclaration9 *decl = nullptr;
    D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_POSITION, 0},
                                    {0, 4, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
                                    D3DDECL_END()};
    assert(SUCCEEDED(device->CreateVertexDeclaration(elements, &decl)));
    float world[] = {1, 0, 0, -8, 0, 1, 0, -8, 0, 0, 1, 0};
    admit(buffer, decl, world);
    assert(pending);
    for (int i = 0; i < 100 && pending; ++i)
        advance();
    assert(!pending && meshes.count(buffer));
    auto &mesh = *meshes.at(buffer);
    assert(mesh.animated && mesh.bytes == 4 * sizeof(Vertex));
    assert(SUCCEEDED(mesh.animated->Lock(0, 0, &data, D3DLOCK_READONLY)));
    for (int i = 0; i < 4; ++i) {
        auto &v = ((Vertex *)data)[i];
        assert(v.weight == 1);
        for (int axis = 0; axis < 4; ++axis)
            assert(v.p[axis] == nativeVertices[i * 8 + axis]);
        assert(!memcmp(&v.color, nativeVertices + i * 8 + 4, 4));
    }
    mesh.animated->Unlock();
    drop(decl);
    drop(buffer);
    cleanup();
    game = 0;
    playerKnown = false;
    puts(
        "PASS: native column selection, player-relative chunk alignment, immutable leaf buffer, colors and normals preserved.");
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    module = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = module;
    wc.lpszClassName = L"AlphaMotionHiddenTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 160, 120, nullptr,
                                nullptr, module, nullptr);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    assert(d3d);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferWidth = 128;
    parameters.BackBufferHeight = 96;
    parameters.EnableAutoDepthStencil = TRUE;
    parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
    D3DDISPLAYMODE mode{};
    d3d->GetAdapterDisplayMode(0, &mode);
    parameters.BackBufferFormat = mode.Format;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &parameters, &device);
    if (FAILED(hr)) {
        printf(
            "Hardware vertex device unavailable (%08lx); validating through HAL/software vertex processing.\n",
            hr);
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    }
    if (FAILED(hr)) {
        printf("CreateDevice failed: %08lx\n", hr);
        return 1;
    }
    auto compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = (Compile)GetProcAddress(compiler, "D3DCompile");
    assert(compile);
    meshPreparationTest();
    cloudTransformTest();
    IDirect3DPixelShader9 *ps = nullptr;
    ID3DBlob *blob = nullptr;
    const char *pixelCode = "float4 main(float4 c:COLOR0):COLOR0{return c;}";
    assert(SUCCEEDED(compile(pixelCode, strlen(pixelCode), nullptr, nullptr, nullptr, "main",
                             "ps_2_0", 0, 0, &blob, nullptr)));
    assert(SUCCEEDED(device->CreatePixelShader((DWORD *)blob->GetBufferPointer(), &ps)));
    drop(blob);
    for (const char *profile : {"vs_2_0", "vs_3_0"}) {
        if (ps) {
            device->SetPixelShader(nullptr);
            drop(ps);
        }
        assert(SUCCEEDED(compile(pixelCode, strlen(pixelCode), nullptr, nullptr, nullptr, "main",
                                 strcmp(profile, "vs_3_0") == 0 ? "ps_3_0" : "ps_2_0", 0, 0, &blob,
                                 nullptr)));
        assert(SUCCEEDED(device->CreatePixelShader((DWORD *)blob->GetBufferPointer(), &ps)));
        drop(blob);
        IDirect3DVertexShader9 *native = compileVertex(compile, profile);
        Shader *entry = shader(native);
        if (!entry || !entry->animated) {
            UINT size = 0;
            native->GetFunction(nullptr, &size);
            std::vector<DWORD> code(size / 4);
            native->GetFunction(code.data(), &size);
            auto patched = shaderWind::patch(code);
            printf("Patch result: profile=%s words=%zu\n", profile, patched.code.size());
            auto disassemble =
                (decltype(&D3DDisassemble))GetProcAddress(compiler, "D3DDisassemble");
            ID3DBlob *assembly = nullptr;
            disassemble(code.data(), size, 0, nullptr, &assembly);
            puts((const char *)assembly->GetBufferPointer());
            drop(assembly);
            if (!patched.code.empty()) {
                disassemble(patched.code.data(), patched.code.size() * 4, 0, nullptr, &assembly);
                if (assembly) {
                    puts((const char *)assembly->GetBufferPointer());
                    drop(assembly);
                }
                IDirect3DVertexShader9 *debug = nullptr;
                printf("Create patched shader HRESULT=%08lx\n",
                       device->CreateVertexShader(patched.code.data(), &debug));
                drop(debug);
            }
        }
        assert(entry && entry->animated);
        // Native shader comments/CTAB survive patching, preserving TAA/shadow recognition.
        UINT bytes = 0;
        native->GetFunction(nullptr, &bytes);
        std::vector<DWORD> original(bytes / 4);
        native->GetFunction(original.data(), &bytes);
        auto patched = shaderWind::patch(original);
        assert(!patched.code.empty());
        const char *raw = (const char *)patched.code.data();
        assert(std::search(raw, raw + patched.code.size() * 4, "worldViewProjMatrix",
                           "worldViewProjMatrix" + 18) != raw + patched.code.size() * 4);
        auto mesh = std::make_unique<Mesh>();
        Vertex vertices[] = {{{8, 8, 0, 4}, 0xffff0000, 0},
                             {{16, 8, 0, 4}, 0xffff0000, 0},
                             {{16, 16, 0, 4}, 0xffff0000, 1},
                             {{8, 16, 0, 4}, 0xffff0000, 1}};
        assert(SUCCEEDED(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_WRITEONLY, 0,
                                                    D3DPOOL_DEFAULT, &mesh->animated, nullptr)));
        void *data = nullptr;
        assert(SUCCEEDED(mesh->animated->Lock(0, 0, &data, 0)));
        memcpy(data, vertices, sizeof(vertices));
        mesh->animated->Unlock();
        D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
                                        {0, 16, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
                                        {0, 20, D3DDECLTYPE_FLOAT1, 0, D3DDECLUSAGE_TEXCOORD, 7},
                                        D3DDECL_END()};
        assert(SUCCEEDED(device->CreateVertexDeclaration(elements, &mesh->declaration)));
        IDirect3DIndexBuffer9 *indices = nullptr;
        WORD index[] = {0, 1, 2, 0, 2, 3};
        assert(SUCCEEDED(device->CreateIndexBuffer(sizeof(index), 0, D3DFMT_INDEX16,
                                                   D3DPOOL_DEFAULT, &indices, nullptr)));
        assert(SUCCEEDED(indices->Lock(0, 0, &data, 0)));
        memcpy(data, index, sizeof(index));
        indices->Unlock();
        IDirect3DSurface9 *target = nullptr;
        assert(SUCCEEDED(device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                                    FALSE, &target, nullptr)));
        device->SetRenderTarget(0, target);
        device->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 viewport{0, 0, 64, 64, 0, 1};
        device->SetViewport(&viewport);
        float wvp[] = {1.f / 16, 0, 0, -1, 0, -1.f / 16, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1};
        float world[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        device->SetVertexShaderConstantF(20, wvp, 4);
        device->SetVertexShaderConstantF(27, world, 3);
        device->SetVertexShader(native);
        device->SetPixelShader(ps);
        device->SetVertexDeclaration(mesh->declaration);
        device->SetStreamSource(0, mesh->animated, 0, sizeof(Vertex));
        device->SetIndices(indices);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        originalDip = (Dip)(*(void ***)device)[82];
        enabled = useLeaves = true;
        cloudBuffer = nullptr;
        leafOffset = {4, 0};
        range = 96;
        animatedBuffers.emplace(mesh->animated, mesh.get());
        float sentinel[] = {.33f, .44f, .55f, .66f};
        device->SetVertexShaderConstantF(entry->constant, sentinel, 1);
        assert(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0)));
        assert(SUCCEEDED(device->BeginScene()));
        assert(SUCCEEDED(dip(device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2)));
        assert(SUCCEEDED(device->EndScene()));
        // Top corners shifted four world units, lower anchored corners stayed still.
        DWORD shifted = pixel(target, 36, 30), anchored = pixel(target, 17, 17),
              untouched = pixel(target, 36, 17);
        printf("Pixels %s: shifted=%08lx anchor=%08lx untouched=%08lx\n", profile, shifted,
               anchored, untouched);
        if (((shifted >> 16) & 255) < 240) {
            auto disassemble =
                (decltype(&D3DDisassemble))GetProcAddress(compiler, "D3DDisassemble");
            ID3DBlob *assembly = nullptr;
            disassemble(patched.code.data(), patched.code.size() * 4, 0, nullptr, &assembly);
            if (assembly) {
                puts((const char *)assembly->GetBufferPointer());
                drop(assembly);
            }
        }
        assert(((shifted >> 16) & 255) > 240 && (shifted & 255) > 195 && (shifted & 255) < 215);
        assert(((anchored >> 16) & 255) > 240 && ((untouched >> 16) & 255) == 0);
        IDirect3DVertexShader9 *restored = nullptr;
        device->GetVertexShader(&restored);
        assert(restored == native);
        drop(restored);
        IDirect3DVertexBuffer9 *restoredVB = nullptr;
        UINT off = 0, stride = 0;
        device->GetStreamSource(0, &restoredVB, &off, &stride);
        assert(restoredVB == mesh->animated && stride == sizeof(Vertex));
        drop(restoredVB);
        float restoredConstant[4];
        device->GetVertexShaderConstantF(entry->constant, restoredConstant, 1);
        assert(!memcmp(sentinel, restoredConstant, sizeof(sentinel)));
        animatedBuffers.clear();
        mesh.reset();
        drop(indices);
        device->SetRenderTarget(0, nullptr);
        drop(target);
        device->SetIndices(nullptr);
        device->SetStreamSource(0, nullptr, 0, 0);
        device->SetVertexShader(nullptr);
        drop(native);
        shaders.clear();
        printf(
            "PASS: %s native bytecode/CTAB, GPU sway, stationary anchors, face ID and draw-state restoration.\n",
            profile);
    }
    device->SetPixelShader(nullptr);
    drop(ps);
    for (int i = 0; i < 3; ++i) {
        cleanup();
        assert(SUCCEEDED(device->Reset(&parameters)));
    }
    puts("PASS: three device resets with all motion resources released.");
    drop(device);
    drop(d3d);
    DestroyWindow(window);
    return 0;
}
