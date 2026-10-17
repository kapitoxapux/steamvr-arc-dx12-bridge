// steamvr-arc-dx12-bridge: openvr_api.dll proxy that forwards everything to
// openvr_api.orig.dll, but re-submits D3D11 eye textures to IVRCompositor::Submit as
// D3D12 textures. On Intel Arc the SteamVR D3D11 submit path hangs, the D3D12 one works.
//
// Install next to the game's original: rename it to openvr_api.orig.dll, drop this
// DLL in as openvr_api.dll. Config: openvr_proxy.ini ([proxy] convert=1, mirror=0),
// log: openvr_proxy.log, both next to the DLL.
//
// SPDX-License-Identifier: MIT
// https://gitverse.ru/kapitoxapux/steamvr-arc-dx12-bridge
// https://github.com/kapitoxapux/steamvr-arc-dx12-bridge (mirror)

#define BRIDGE_NAME "steamvr-arc-dx12-bridge"
#define BRIDGE_VERSION "0.1.0"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// ---------------------------------------------------------------- OpenVR 2.0.10 subset

namespace vr {
enum ETextureType { TextureType_DirectX = 0, TextureType_DirectX12 = 4 };
enum EColorSpace { ColorSpace_Auto = 0 };
struct HmdMatrix34_t { float m[3][4]; };
struct HmdMatrix44_t { float m[4][4]; };
struct HmdVector2_t { float v[2]; };
struct Texture_t {
    void* handle;
    ETextureType eType;
    EColorSpace eColorSpace;
};
struct VRTextureBounds_t { float uMin, vMin, uMax, vMax; };
struct VRTextureWithPose_t : Texture_t { HmdMatrix34_t mDeviceToAbsoluteTracking; };
struct VRTextureDepthInfo_t {
    void* handle;
    HmdMatrix44_t mProjection;
    HmdVector2_t vRange;
};
struct VRTextureWithDepth_t : Texture_t { VRTextureDepthInfo_t depth; };
struct VRTextureWithPoseAndDepth_t : VRTextureWithPose_t { VRTextureDepthInfo_t depth; };
struct D3D12TextureData_t {
    ID3D12Resource* m_pResource;
    ID3D12CommandQueue* m_pCommandQueue;
    uint32_t m_nNodeMask;
};
enum { Submit_TextureWithPose = 0x08, Submit_TextureWithDepth = 0x10 };
enum { VRCompositorError_None = 0, VRCompositorError_RequestFailed = 1 };
}  // namespace vr

static const int kSubmitIndex = 5;               // IVRCompositor_027::Submit
static const int kGetMirrorTextureD3D11Index = 33;  // IVRCompositor_027::GetMirrorTextureD3D11

// ---------------------------------------------------------------- logging / config

static wchar_t g_dir[MAX_PATH];
static std::mutex g_logMtx;
static FILE* g_log;
static int g_convert = 1;
static int g_mirror = 0;

static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (!g_log) {
        std::wstring p = std::wstring(g_dir) + L"\\openvr_proxy.log";
        g_log = _wfopen(p.c_str(), L"a");
        if (!g_log) return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- original DLL

static HMODULE g_orig;
extern "C" {
// Raw forwards for exports whose signatures we don't care about (jumped to from naked stubs).
__attribute__((used)) void* g_fwd[5];
}

typedef uint32_t (*PFN_VR_InitInternal)(int*, int);
typedef uint32_t (*PFN_VR_InitInternal2)(int*, int, const char*);
typedef void (*PFN_VR_ShutdownInternal)();
typedef bool (*PFN_VR_IsHmdPresent)();
typedef void* (*PFN_VR_GetGenericInterface)(const char*, int*);
typedef bool (*PFN_VR_IsRuntimeInstalled)();
typedef const char* (*PFN_VR_RuntimePath)();
typedef bool (*PFN_VR_GetRuntimePath)(char*, uint32_t, uint32_t*);
typedef const char* (*PFN_VR_ErrorString)(int);
typedef bool (*PFN_VR_IsInterfaceVersionValid)(const char*);
typedef uint32_t (*PFN_VR_GetInitToken)();

static PFN_VR_InitInternal o_InitInternal;
static PFN_VR_InitInternal2 o_InitInternal2;
static PFN_VR_ShutdownInternal o_ShutdownInternal;
static PFN_VR_IsHmdPresent o_IsHmdPresent;
static PFN_VR_GetGenericInterface o_GetGenericInterface;
static PFN_VR_IsRuntimeInstalled o_IsRuntimeInstalled;
static PFN_VR_RuntimePath o_RuntimePath;
static PFN_VR_GetRuntimePath o_GetRuntimePath;
static PFN_VR_ErrorString o_ErrorAsSymbol, o_ErrorAsEnglish, o_StringForHmdError;
static PFN_VR_IsInterfaceVersionValid o_IsInterfaceVersionValid;
static PFN_VR_GetInitToken o_GetInitToken;

static bool LoadOriginal(HMODULE self) {
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = 0;
    std::wstring ini = std::wstring(g_dir) + L"\\openvr_proxy.ini";
    g_convert = GetPrivateProfileIntW(L"proxy", L"convert", 1, ini.c_str());
    g_mirror = GetPrivateProfileIntW(L"proxy", L"mirror", 0, ini.c_str());
    std::wstring orig = std::wstring(g_dir) + L"\\openvr_api.orig.dll";
    g_orig = LoadLibraryExW(orig.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_orig) {
        Log("FATAL: cannot load openvr_api.orig.dll (error %lu)", GetLastError());
        return false;
    }
#define RESOLVE(var, name) var = (decltype(var))GetProcAddress(g_orig, name)
    RESOLVE(o_InitInternal, "VR_InitInternal");
    RESOLVE(o_InitInternal2, "VR_InitInternal2");
    RESOLVE(o_ShutdownInternal, "VR_ShutdownInternal");
    RESOLVE(o_IsHmdPresent, "VR_IsHmdPresent");
    RESOLVE(o_GetGenericInterface, "VR_GetGenericInterface");
    RESOLVE(o_IsRuntimeInstalled, "VR_IsRuntimeInstalled");
    RESOLVE(o_RuntimePath, "VR_RuntimePath");
    RESOLVE(o_GetRuntimePath, "VR_GetRuntimePath");
    RESOLVE(o_ErrorAsSymbol, "VR_GetVRInitErrorAsSymbol");
    RESOLVE(o_ErrorAsEnglish, "VR_GetVRInitErrorAsEnglishDescription");
    RESOLVE(o_StringForHmdError, "VR_GetStringForHmdError");
    RESOLVE(o_IsInterfaceVersionValid, "VR_IsInterfaceVersionValid");
    RESOLVE(o_GetInitToken, "VR_GetInitToken");
#undef RESOLVE
    const char* raw[5] = {"LiquidVR", "VRControlPanel", "VRHeadsetView", "VRPaths", "VRVirtualDisplay"};
    for (int i = 0; i < 5; i++) g_fwd[i] = (void*)GetProcAddress(g_orig, raw[i]);
    Log("==== " BRIDGE_NAME " " BRIDGE_VERSION " loaded: convert=%d mirror=%d", g_convert, g_mirror);
    return true;
}

// ---------------------------------------------------------------- D3D11 -> D3D12 bridge

static const int kSlots = 3;

struct Slot {
    ID3D12Resource* res12 = nullptr;
    ID3D11Texture2D* tex11 = nullptr;
    vr::D3D12TextureData_t data{};
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* pre = nullptr;   // COMMON -> PIXEL_SHADER_RESOURCE
    ID3D12GraphicsCommandList* post = nullptr;  // PIXEL_SHADER_RESOURCE -> COMMON
    UINT64 freeAt = 0;                          // value of fence12 when the slot can be rewritten
};

struct Bridge {
    bool ready = false, failed = false;
    ID3D11Device* dev11 = nullptr;
    ID3D11Device1* dev11_1 = nullptr;
    ID3D11Device5* dev11_5 = nullptr;
    ID3D11DeviceContext* ctx11 = nullptr;
    ID3D11DeviceContext4* ctx11_4 = nullptr;
    ID3D11Multithread* mt = nullptr;
    ID3D12Device* dev12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence11to12 = nullptr;  // signalled by D3D11 after the copy
    ID3D12Fence* fence12 = nullptr;      // signalled by D3D12 when a slot is free again
    ID3D11Fence* fence11to12_11 = nullptr;
    ID3D11Fence* fence12_11 = nullptr;
    UINT64 v11 = 0, v12 = 0;
    HANDLE cpuEvent = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;       // source format
    DXGI_FORMAT shareFmt = DXGI_FORMAT_UNKNOWN;  // format of the shared slots
    Slot slots[2][kSlots];
    int next[2] = {0, 0};
    uint64_t submits = 0, errors = 0;
};

static Bridge g_br;
static std::mutex g_brMtx;

#define CHECK(expr, what)                                                       \
    do {                                                                        \
        HRESULT _hr = (expr);                                                   \
        if (FAILED(_hr)) {                                                      \
            Log("bridge init: %s failed hr=0x%08lx", what, (unsigned long)_hr); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static DXGI_FORMAT TypedFor(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return f;
    }
}

// The shared texture can be born on either side; drivers differ in what they accept, so try both,
// first with the game's own (often TYPELESS) format, then with a typed one.
enum SharePath { Share_None = -1, Share_12to11 = 0, Share_11to12 = 1 };
static const char* kShareNames[] = {"d3d12->d3d11", "d3d11->d3d12"};

static HRESULT CreateSharedPair(Bridge& b, int path, DXGI_FORMAT fmt, ID3D12Resource** res12, ID3D11Texture2D** tex11,
                                const char** step) {
    *res12 = nullptr;
    *tex11 = nullptr;
    HANDLE hnd = nullptr;
    HRESULT hr;
    if (path == Share_12to11) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = b.w;
        rd.Height = b.h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        *step = "CreateCommittedResource(shared)";
        hr = b.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              __uuidof(ID3D12Resource), (void**)res12);
        if (FAILED(hr)) return hr;
        *step = "ID3D12Device::CreateSharedHandle";
        hr = b.dev12->CreateSharedHandle(*res12, nullptr, GENERIC_ALL, nullptr, &hnd);
        if (SUCCEEDED(hr)) {
            *step = "OpenSharedResource1";
            hr = b.dev11_1->OpenSharedResource1(hnd, __uuidof(ID3D11Texture2D), (void**)tex11);
            CloseHandle(hnd);
        }
    } else {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = b.w;
        td.Height = b.h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        *step = "CreateTexture2D(shared nt)";
        hr = b.dev11->CreateTexture2D(&td, nullptr, tex11);
        if (FAILED(hr)) return hr;
        IDXGIResource1* dr = nullptr;
        *step = "QI IDXGIResource1";
        hr = (*tex11)->QueryInterface(__uuidof(IDXGIResource1), (void**)&dr);
        if (SUCCEEDED(hr)) {
            *step = "IDXGIResource1::CreateSharedHandle";
            hr = dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                        &hnd);
            dr->Release();
        }
        if (SUCCEEDED(hr)) {
            *step = "ID3D12Device::OpenSharedHandle";
            hr = b.dev12->OpenSharedHandle(hnd, __uuidof(ID3D12Resource), (void**)res12);
            CloseHandle(hnd);
        }
    }
    if (FAILED(hr)) {
        if (*tex11) (*tex11)->Release();
        if (*res12) (*res12)->Release();
        *tex11 = nullptr;
        *res12 = nullptr;
    }
    return hr;
}

static bool CreateSlots(Bridge& b, const D3D11_TEXTURE2D_DESC& src) {
    b.w = src.Width;
    b.h = src.Height;
    b.fmt = src.Format;
    // pick a working (path, format) combination on the first slot, reuse it for the rest
    int path = Share_None;
    DXGI_FORMAT fmt = src.Format;
    {
        const DXGI_FORMAT fmts[2] = {src.Format, TypedFor(src.Format)};
        for (int p = 0; p < 2 && path == Share_None; p++)
            for (int f = 0; f < 2 && path == Share_None; f++) {
                if (f == 1 && fmts[1] == fmts[0]) continue;
                const char* step = "";
                HRESULT hr = CreateSharedPair(b, p, fmts[f], &b.slots[0][0].res12, &b.slots[0][0].tex11, &step);
                if (SUCCEEDED(hr)) {
                    path = p;
                    fmt = fmts[f];
                } else {
                    Log("bridge: share %s fmt=%d failed at %s hr=0x%08lx", kShareNames[p], (int)fmts[f], step,
                        (unsigned long)hr);
                }
            }
    }
    if (path == Share_None) {
        Log("bridge init: no way to share a texture between D3D11 and D3D12");
        return false;
    }
    b.shareFmt = fmt;
    Log("bridge: sharing via %s, slot format %d (source %d)", kShareNames[path], (int)fmt, (int)src.Format);
    for (int eye = 0; eye < 2; eye++)
        for (int i = 0; i < kSlots; i++) {
            Slot& s = b.slots[eye][i];
            if (!s.res12) {
                const char* step = "";
                HRESULT hr = CreateSharedPair(b, path, fmt, &s.res12, &s.tex11, &step);
                if (FAILED(hr)) {
                    Log("bridge init: slot %d/%d failed at %s hr=0x%08lx", eye, i, step, (unsigned long)hr);
                    return false;
                }
            }
            s.data.m_pResource = s.res12;
            s.data.m_pCommandQueue = b.queue;
            s.data.m_nNodeMask = 0;
            CHECK(b.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                  (void**)&s.alloc),
                  "CreateCommandAllocator");
            for (int k = 0; k < 2; k++) {
                ID3D12GraphicsCommandList* cl = nullptr;
                CHECK(b.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr,
                                                 __uuidof(ID3D12GraphicsCommandList), (void**)&cl),
                      "CreateCommandList");
                D3D12_RESOURCE_BARRIER bar{};
                bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bar.Transition.pResource = s.res12;
                bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bar.Transition.StateBefore = k == 0 ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bar.Transition.StateAfter = k == 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
                cl->ResourceBarrier(1, &bar);
                CHECK(cl->Close(), "CommandList Close");
                (k == 0 ? s.pre : s.post) = cl;
            }
        }
    Log("bridge: slots ready %ux%u fmt=%d", b.w, b.h, (int)b.shareFmt);
    return true;
}

static bool InitBridge(Bridge& b, ID3D11Texture2D* t11) {
    t11->GetDevice(&b.dev11);
    CHECK(b.dev11->QueryInterface(__uuidof(ID3D11Device1), (void**)&b.dev11_1), "QI ID3D11Device1");
    CHECK(b.dev11->QueryInterface(__uuidof(ID3D11Device5), (void**)&b.dev11_5), "QI ID3D11Device5");
    b.dev11->GetImmediateContext(&b.ctx11);
    CHECK(b.ctx11->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&b.ctx11_4), "QI ID3D11DeviceContext4");
    if (SUCCEEDED(b.ctx11->QueryInterface(__uuidof(ID3D11Multithread), (void**)&b.mt))) {
        BOOL was = b.mt->SetMultithreadProtected(TRUE);
        Log("bridge: D3D11 multithread protection was %d, now on", (int)was);
    }
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    CHECK(b.dev11->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev), "QI IDXGIDevice");
    HRESULT hr = dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    CHECK(hr, "GetAdapter");
    DXGI_ADAPTER_DESC ad;
    adapter->GetDesc(&ad);
    Log("bridge: adapter '%ls'", ad.Description);
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&b.dev12);
    adapter->Release();
    CHECK(hr, "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(b.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&b.queue), "CreateCommandQueue");
    for (int k = 0; k < 2; k++) {
        ID3D12Fence*& f12 = k == 0 ? b.fence11to12 : b.fence12;
        ID3D11Fence*& f11 = k == 0 ? b.fence11to12_11 : b.fence12_11;
        CHECK(b.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void**)&f12), "CreateFence");
        HANDLE hnd = nullptr;
        CHECK(b.dev12->CreateSharedHandle(f12, nullptr, GENERIC_ALL, nullptr, &hnd), "CreateSharedHandle(fence)");
        hr = b.dev11_5->OpenSharedFence(hnd, __uuidof(ID3D11Fence), (void**)&f11);
        CloseHandle(hnd);
        CHECK(hr, "OpenSharedFence");
    }
    b.cpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D11_TEXTURE2D_DESC d;
    t11->GetDesc(&d);
    Log("bridge: first texture %ux%u fmt=%d array=%u mips=%u samples=%u bind=0x%x misc=0x%x", d.Width, d.Height,
        (int)d.Format, d.ArraySize, d.MipLevels, d.SampleDesc.Count, d.BindFlags, d.MiscFlags);
    if (!CreateSlots(b, d)) return false;
    return true;
}

// Copies the eye texture into a free shared slot and makes the D3D12 queue wait for it.
// Returns the slot to submit, or nullptr on failure.
static Slot* PrepareEye(Bridge& b, int eye, ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    if (d.Width != b.w || d.Height != b.h || d.Format != b.fmt) {
        Log("bridge: eye texture changed to %ux%u fmt=%d, unsupported for now", d.Width, d.Height, (int)d.Format);
        return nullptr;
    }
    Slot& s = b.slots[eye][b.next[eye]];
    b.next[eye] = (b.next[eye] + 1) % kSlots;
    if (b.fence12->GetCompletedValue() < s.freeAt) {
        b.fence12->SetEventOnCompletion(s.freeAt, b.cpuEvent);
        if (WaitForSingleObject(b.cpuEvent, 200) == WAIT_TIMEOUT) Log("bridge: slot wait timed out (eye %d)", eye);
    }
    if (b.mt) b.mt->Enter();
    b.ctx11_4->Wait(b.fence12_11, s.freeAt);
    const UINT sub = D3D11CalcSubresource(0, d.ArraySize > 1 ? (UINT)eye : 0, d.MipLevels);
    if (d.SampleDesc.Count > 1)
        b.ctx11->ResolveSubresource(s.tex11, 0, src, sub, TypedFor(d.Format));
    else
        b.ctx11->CopySubresourceRegion(s.tex11, 0, 0, 0, 0, src, sub, nullptr);
    b.ctx11_4->Signal(b.fence11to12_11, ++b.v11);
    b.ctx11->Flush();
    if (b.mt) b.mt->Leave();
    b.queue->Wait(b.fence11to12, b.v11);
    ID3D12CommandList* pre = s.pre;
    b.queue->ExecuteCommandLists(1, &pre);
    return &s;
}

static void FinishEye(Bridge& b, Slot* s) {
    ID3D12CommandList* post = s->post;
    b.queue->ExecuteCommandLists(1, &post);
    s->freeAt = ++b.v12;
    b.queue->Signal(b.fence12, s->freeAt);
}

// ---------------------------------------------------------------- IVRCompositor hooks

typedef int (*PFN_Submit)(void* self, int eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds,
                          int flags);
typedef int (*PFN_GetMirrorTextureD3D11)(void* self, int eye, void* devOrRes, void** srv);
static PFN_Submit o_Submit;
static PFN_GetMirrorTextureD3D11 o_GetMirrorTextureD3D11;

static int Hook_Submit(void* self, int eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds,
                       int flags) {
    static std::atomic<int> logged{0};
    if (logged < 4) {
        logged++;
        Log("Submit eye=%d tex=%p type=%d cs=%d flags=0x%x bounds=%s", eye, tex ? tex->handle : nullptr,
            tex ? (int)tex->eType : -1, tex ? (int)tex->eColorSpace : -1, flags, bounds ? "yes" : "null");
        if (bounds) Log("  bounds u=%.3f..%.3f v=%.3f..%.3f", bounds->uMin, bounds->uMax, bounds->vMin, bounds->vMax);
    }
    if (!g_convert || !tex || tex->eType != vr::TextureType_DirectX || !tex->handle || (eye != 0 && eye != 1))
        return o_Submit(self, eye, tex, bounds, flags);

    std::lock_guard<std::mutex> lk(g_brMtx);
    Bridge& b = g_br;
    if (b.failed) return o_Submit(self, eye, tex, bounds, flags);
    if (!b.ready) {
        if (!InitBridge(b, (ID3D11Texture2D*)tex->handle)) {
            b.failed = true;
            Log("bridge: init failed, falling back to plain D3D11 submits");
            return o_Submit(self, eye, tex, bounds, flags);
        }
        b.ready = true;
    }
    Slot* s = PrepareEye(b, eye, (ID3D11Texture2D*)tex->handle);
    if (!s) return o_Submit(self, eye, tex, bounds, flags);

    // Re-pack the texture struct, keeping the pose if present; depth (a D3D11 handle) is dropped.
    vr::VRTextureWithPose_t out{};
    const bool withPose = (flags & vr::Submit_TextureWithPose) != 0;
    if (withPose)
        out = *(const vr::VRTextureWithPose_t*)tex;
    else
        *(vr::Texture_t*)&out = *tex;
    out.handle = &s->data;
    out.eType = vr::TextureType_DirectX12;
    const int outFlags = flags & ~vr::Submit_TextureWithDepth;
    int err = o_Submit(self, eye, &out, bounds, outFlags);
    FinishEye(b, s);
    b.submits++;
    if (err != 0) {
        b.errors++;
        if (b.errors <= 10 || b.errors % 500 == 0) Log("Submit(D3D12) eye=%d returned %d (errors=%llu)", eye, err,
                                                       (unsigned long long)b.errors);
    }
    if (b.submits == 2 || b.submits % 1000 == 0)
        Log("stats: submits=%llu errors=%llu v11=%llu v12=%llu completed12=%llu", (unsigned long long)b.submits,
            (unsigned long long)b.errors, (unsigned long long)b.v11, (unsigned long long)b.v12,
            (unsigned long long)b.fence12->GetCompletedValue());
    return err;
}

static int Hook_GetMirrorTextureD3D11(void* self, int eye, void* devOrRes, void** srv) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) Log("GetMirrorTextureD3D11 called (mirror=%d)", g_mirror);
    if (g_mirror) return o_GetMirrorTextureD3D11(self, eye, devOrRes, srv);
    if (srv) *srv = nullptr;
    return vr::VRCompositorError_RequestFailed;
}

static void PatchVtable(void** vtbl, int index, void* hook, void** orig) {
    if (vtbl[index] == hook) return;
    DWORD old;
    if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old)) {
        Log("VirtualProtect failed for vtable index %d (error %lu)", index, GetLastError());
        return;
    }
    *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
}

static void HookCompositor(void* compositor) {
    void** vtbl = *(void***)compositor;
    PatchVtable(vtbl, kSubmitIndex, (void*)Hook_Submit, (void**)&o_Submit);
    PatchVtable(vtbl, kGetMirrorTextureD3D11Index, (void*)Hook_GetMirrorTextureD3D11, (void**)&o_GetMirrorTextureD3D11);
    Log("IVRCompositor hooked: vtable=%p Submit=%p", vtbl, (void*)o_Submit);
}

static void ReleaseBridge() {
    std::lock_guard<std::mutex> lk(g_brMtx);
    Bridge& b = g_br;
    if (!b.ready) return;
    if (b.fence12 && b.v12 && b.fence12->GetCompletedValue() < b.v12) {
        b.fence12->SetEventOnCompletion(b.v12, b.cpuEvent);
        WaitForSingleObject(b.cpuEvent, 1000);
    }
    for (auto& eyeSlots : b.slots)
        for (auto& s : eyeSlots) {
            if (s.pre) s.pre->Release();
            if (s.post) s.post->Release();
            if (s.alloc) s.alloc->Release();
            if (s.tex11) s.tex11->Release();
            if (s.res12) s.res12->Release();
        }
    IUnknown* objs[] = {b.fence12_11, b.fence11to12_11, b.fence12, b.fence11to12, b.queue, b.dev12, b.mt,
                        b.ctx11_4,    b.ctx11,          b.dev11_5, b.dev11_1,  b.dev11};
    for (auto* o : objs)
        if (o) o->Release();
    if (b.cpuEvent) CloseHandle(b.cpuEvent);
    Log("bridge released after %llu submits, %llu errors", (unsigned long long)b.submits,
        (unsigned long long)b.errors);
    b = Bridge();
}

// ---------------------------------------------------------------- exports

extern "C" {

__declspec(dllexport) uint32_t VR_InitInternal(int* err, int type) { return o_InitInternal(err, type); }

__declspec(dllexport) uint32_t VR_InitInternal2(int* err, int type, const char* startup) {
    uint32_t r = o_InitInternal2(err, type, startup);
    Log("VR_InitInternal2 type=%d -> token=%u err=%d", type, r, err ? *err : -1);
    return r;
}

__declspec(dllexport) void VR_ShutdownInternal() {
    Log("VR_ShutdownInternal");
    o_ShutdownInternal();
    ReleaseBridge();
}

__declspec(dllexport) bool VR_IsHmdPresent() { return o_IsHmdPresent(); }

__declspec(dllexport) void* VR_GetGenericInterface(const char* ver, int* err) {
    void* r = o_GetGenericInterface(ver, err);
    if (r && ver && strncmp(ver, "IVRCompositor_", 14) == 0) {
        if (strcmp(ver, "IVRCompositor_027") == 0)
            HookCompositor(r);
        else
            Log("WARNING: %s requested, hook offsets are for IVRCompositor_027; not hooked", ver);
    }
    if (r && ver && strncmp(ver, "FnTable:IVRCompositor_", 22) == 0) Log("WARNING: %s (C table) not hooked", ver);
    return r;
}

__declspec(dllexport) bool VR_IsRuntimeInstalled() { return o_IsRuntimeInstalled(); }
__declspec(dllexport) const char* VR_RuntimePath() { return o_RuntimePath(); }
__declspec(dllexport) bool VR_GetRuntimePath(char* buf, uint32_t size, uint32_t* req) {
    return o_GetRuntimePath(buf, size, req);
}
__declspec(dllexport) const char* VR_GetVRInitErrorAsSymbol(int e) { return o_ErrorAsSymbol(e); }
__declspec(dllexport) const char* VR_GetVRInitErrorAsEnglishDescription(int e) { return o_ErrorAsEnglish(e); }
__declspec(dllexport) const char* VR_GetStringForHmdError(int e) { return o_StringForHmdError(e); }
__declspec(dllexport) bool VR_IsInterfaceVersionValid(const char* v) { return o_IsInterfaceVersionValid(v); }
__declspec(dllexport) uint32_t VR_GetInitToken() { return o_GetInitToken(); }

#define RAW_FORWARD(name, idx) \
    __declspec(dllexport) __attribute__((naked)) void name() { __asm__ volatile("jmpq *g_fwd+" #idx "(%rip)"); }
RAW_FORWARD(LiquidVR, 0)
RAW_FORWARD(VRControlPanel, 8)
RAW_FORWARD(VRHeadsetView, 16)
RAW_FORWARD(VRPaths, 24)
RAW_FORWARD(VRVirtualDisplay, 32)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        return LoadOriginal(inst) ? TRUE : FALSE;
    }
    return TRUE;
}

}  // extern "C"
