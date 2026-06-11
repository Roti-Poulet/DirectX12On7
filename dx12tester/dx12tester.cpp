/*
 * dx12test.cpp — A simple program to test DirectX12
 * MinGW64 :
 *   g++ dx12test.cpp -o dx12test.exe -ldxgi -luuid -lole32 -lgdi32 -luser32 -lwinmm -static-libgcc -static-libstdc++ -std=c++17
 *
 * Usage :
 *   dx12test.exe           -> d3d12 functions test
 *   dx12test.exe --window  -> opens a new window with DirectX12
 *   dx12test.exe --all     -> both
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <winerror.h>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod (winmm)


#define INITGUID
#include <guiddef.h>

#include <dxgi1_4.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <string>

struct FeatureResult {
    bool  dx12DllFound   = false;
    bool  dxgiDllFound   = false;
    DWORD loadError      = 0;
    int   maxFL          = -1;         // 0=error 110=FL11.0 111=FL11.1 120=FL12.0
    char  adapterName[128] = "(Something isn't right...)";
};
static FeatureResult g_feat;

static std::string hr_to_string(HRESULT hr)
{
    char buf[64];
    switch (hr) {
        case S_OK:                    return "S_OK (0x0)";
        case E_FAIL:                  return "E_FAIL (0x80004005)";
        case E_INVALIDARG:            return "E_INVALIDARG (0x80070057)";
        case E_OUTOFMEMORY:           return "E_OUTOFMEMORY (0x8007000E)";
        case DXGI_ERROR_UNSUPPORTED:  return "DXGI_ERROR_UNSUPPORTED (0x887A0004)";
        case DXGI_ERROR_NOT_FOUND:    return "DXGI_ERROR_NOT_FOUND (0x887A0002)";
        case E_NOINTERFACE:           return "E_NOINTERFACE (0x80004002)";
        default:
            snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
            return buf;
    }
}

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(
    IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12GetDebugInterface)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);

static PFN_D3D12CreateDevice     g_D3D12CreateDevice     = nullptr;
static PFN_D3D12GetDebugInterface g_D3D12GetDebugInterface = nullptr;
static PFN_CreateDXGIFactory1    g_CreateDXGIFactory1    = nullptr;
static PFN_CreateDXGIFactory2    g_CreateDXGIFactory2    = nullptr;

static HMODULE g_hD3D12 = nullptr;
static HMODULE g_hDXGI  = nullptr;

static bool LoadDX12(DWORD &outErr)
{
    outErr = 0;

    g_hDXGI = LoadLibraryW(L"dxgi.dll");
    if (!g_hDXGI) { outErr = GetLastError(); return false; }

    g_hD3D12 = LoadLibraryW(L"d3d12.dll");
    if (!g_hD3D12) { outErr = GetLastError(); return false; }

    g_D3D12CreateDevice = (PFN_D3D12CreateDevice)
        GetProcAddress(g_hD3D12, "D3D12CreateDevice");
    g_D3D12GetDebugInterface = (PFN_D3D12GetDebugInterface)
        GetProcAddress(g_hD3D12, "D3D12GetDebugInterface");
    g_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)
        GetProcAddress(g_hDXGI, "CreateDXGIFactory1");
    g_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)
        GetProcAddress(g_hDXGI, "CreateDXGIFactory2");

    if (!g_D3D12CreateDevice) {
        outErr = ERROR_PROC_NOT_FOUND; // 127
        return false;
    }
    return true;
}

static void TestDeviceCreation()
{
    printf("\n=== [1] Attempt for device creation with DirectX 12 ===\n");

    // Adaptateur enumere via DXGI
    IDXGIFactory4 *pFactory = nullptr;
    HRESULT hr = E_FAIL;

    if (g_CreateDXGIFactory2)
        hr = g_CreateDXGIFactory2(0, IID_IDXGIFactory4, (void**)&pFactory);
    else if (g_CreateDXGIFactory1)
        hr = g_CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&pFactory);

    if (FAILED(hr) || !pFactory) {
        printf("  ECHEC CreateDXGIFactory : %s\n", hr_to_string(hr).c_str());
        printf("  -> DXGI not available.\n");
        return;
    }
    printf("  IDXGIFactory4 created : OK\n");

    IDXGIAdapter1 *pAdapter = nullptr;
    UINT adapterIdx = 0;
    while (pFactory->EnumAdapters1(adapterIdx, &pAdapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        pAdapter->GetDesc1(&desc);
        char nameA[128];
        WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
        printf("  GPU detected %u : %s  (VRAM=%llu MB)\n",
               adapterIdx, nameA,
               (unsigned long long)desc.DedicatedVideoMemory / (1024*1024));

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        for (auto fl : levels) {
            ID3D12Device *pDevice = nullptr;
            hr = g_D3D12CreateDevice(pAdapter, fl, IID_ID3D12Device, (void**)&pDevice);
            if (SUCCEEDED(hr) && pDevice) {
                const char *flStr =
                    (fl == D3D_FEATURE_LEVEL_12_0) ? "12_0" :
                    (fl == D3D_FEATURE_LEVEL_11_1) ? "11_1" : "11_0";
                printf("    D3D12CreateDevice FL_%s : SUCCESS\n", flStr);
                pDevice->Release();
                break;
            } else {
                const char *flStr =
                    (fl == D3D_FEATURE_LEVEL_12_0) ? "12_0" :
                    (fl == D3D_FEATURE_LEVEL_11_1) ? "11_1" : "11_0";
                printf("    D3D12CreateDevice FL_%s : ECHEC %s\n", flStr, hr_to_string(hr).c_str());
            }
        }
        pAdapter->Release();
        ++adapterIdx;
    }

    if (adapterIdx == 0)
        printf("  Aucun adaptateur GPU trouve via DXGI.\n");

    pFactory->Release();
}

static void TestFeatureLevelFunctions()
{
    printf("\n=== [2] Functions test ===\n");

    IDXGIFactory4 *pFactory = nullptr;
    HRESULT hr = E_FAIL;
    if (g_CreateDXGIFactory2)
        hr = g_CreateDXGIFactory2(0, IID_IDXGIFactory4, (void**)&pFactory);
    else if (g_CreateDXGIFactory1)
        hr = g_CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&pFactory);

    if (FAILED(hr) || !pFactory) {
        printf("  DXGI indisponible, test annule.\n");
        return;
    }

    IDXGIAdapter1 *pAdapter = nullptr;
    if (pFactory->EnumAdapters1(0, &pAdapter) == DXGI_ERROR_NOT_FOUND) {
        printf("  Aucun adaptateur, test annule.\n");
        pFactory->Release();
        return;
    }

    // --- Feature Level 11.0 ---
    printf("\n  -- Feature Level 11.0 --\n");
    {
        ID3D12Device *dev = nullptr;
        hr = g_D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, (void**)&dev);
        if (SUCCEEDED(hr) && dev) {
            // CreateCommandAllocator (present depuis FL11.0)
            ID3D12CommandAllocator *alloc = nullptr;
            hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_ID3D12CommandAllocator, (void**)&alloc);
            printf("    CreateCommandAllocator : %s\n", SUCCEEDED(hr) ? "OK" : hr_to_string(hr).c_str());
            if (alloc) alloc->Release();

            // CreateDescriptorHeap
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.NumDescriptors = 16;
            heapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ID3D12DescriptorHeap *heap = nullptr;
            hr = dev->CreateDescriptorHeap(&heapDesc, IID_ID3D12DescriptorHeap, (void**)&heap);
            printf("    CreateDescriptorHeap   : %s\n", SUCCEEDED(hr) ? "OK" : hr_to_string(hr).c_str());
            if (heap) heap->Release();

            // GetDescriptorHandleIncrementSize
            UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            printf("    GetDescriptorHandleIncrementSize(RTV) : %u\n", incr);

            dev->Release();
        } else {
            printf("    Device FL11.0 not created : %s\n", hr_to_string(hr).c_str());
        }
    }

    // --- Feature Level 11.1 ---
    printf("\n  -- Feature Level 11.1 --\n");
    {
        ID3D12Device *dev = nullptr;
        hr = g_D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_1, IID_ID3D12Device, (void**)&dev);
        if (SUCCEEDED(hr) && dev) {
            // CheckFeatureSupport : D3D12_FEATURE_D3D12_OPTIONS
            D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
            hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
            printf("    CheckFeatureSupport(OPTIONS)       : %s\n", SUCCEEDED(hr) ? "OK" : hr_to_string(hr).c_str());
            if (SUCCEEDED(hr)) {
                printf("      TiledResourcesTier : %d\n", (int)opts.TiledResourcesTier);
                printf("      ResourceBindingTier: %d\n", (int)opts.ResourceBindingTier);
            }

            // CreateFence
            ID3D12Fence *fence = nullptr;
            hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&fence);
            printf("    CreateFence                        : %s\n", SUCCEEDED(hr) ? "OK" : hr_to_string(hr).c_str());
            if (fence) fence->Release();

            dev->Release();
        } else {
            printf("    Device FL11.1 non cree : %s\n", hr_to_string(hr).c_str());
        }
    }

    // --- Feature Level 12.0 ---
    printf("\n  -- Feature Level 12.0 --\n");
    {
        ID3D12Device *dev = nullptr;
        hr = g_D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_12_0, IID_ID3D12Device, (void**)&dev);
        if (SUCCEEDED(hr) && dev) {
            // CheckFeatureSupport : D3D12_FEATURE_SHADER_MODEL
            D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_0 };
            hr = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
            printf("    CheckFeatureSupport(SHADER_MODEL)  : %s\n", SUCCEEDED(hr) ? "OK" : hr_to_string(hr).c_str());
            if (SUCCEEDED(hr))
                printf("      HighestShaderModel : 0x%x\n", (unsigned)sm.HighestShaderModel);

            // CreateCommandQueue
            D3D12_COMMAND_QUEUE_DESC qDesc = {};
            qDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
            qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            ID3D12CommandQueue *queue = nullptr;
            hr = dev->CreateCommandQueue(&qDesc, IID_ID3D12CommandQueue, (void**)&queue);
            printf("    CreateCommandQueue                 : %s\n", SUCCEEDED(hr) ? "OK" : hr_to_string(hr).c_str());
            if (queue) queue->Release();

            dev->Release();
        } else {
            printf("    Device FL12.0 non cree : %s\n", hr_to_string(hr).c_str());
        }
    }

    pAdapter->Release();
    pFactory->Release();
}

static void DetectFeatureLevel()
{
    g_feat.dx12DllFound  = (g_hD3D12 != nullptr);
    g_feat.dxgiDllFound  = (g_hDXGI  != nullptr);
    g_feat.maxFL         = 0; // erreur par defaut

    if (!g_D3D12CreateDevice || !g_CreateDXGIFactory1) return;

    IDXGIFactory4 *pFactory = nullptr;
    HRESULT hr = E_FAIL;
    if (g_CreateDXGIFactory2)
        hr = g_CreateDXGIFactory2(0, IID_IDXGIFactory4, (void**)&pFactory);
    else if (g_CreateDXGIFactory1)
        hr = g_CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) return;

    IDXGIAdapter1 *pAdapter = nullptr;
    if (pFactory->EnumAdapters1(0, &pAdapter) == DXGI_ERROR_NOT_FOUND) {
        pFactory->Release(); return;
    }

    DXGI_ADAPTER_DESC1 desc = {};
    pAdapter->GetDesc1(&desc);
    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1,
                        g_feat.adapterName, sizeof(g_feat.adapterName), nullptr, nullptr);

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    const int codes[] = { 120, 111, 110 };

    for (int i = 0; i < 3; ++i) {
        ID3D12Device *dev = nullptr;
        hr = g_D3D12CreateDevice(pAdapter, levels[i], IID_ID3D12Device, (void**)&dev);
        if (SUCCEEDED(hr) && dev) {
            g_feat.maxFL = codes[i];
            dev->Release();
            break;
        }
    }

    pAdapter->Release();
    pFactory->Release();
}

// Window test

struct WndState {
    ULONGLONG frameCount = 0;
    DWORD     fps        = 0;
    DWORD     fpsTimer   = 0;
    DWORD     frameInSec = 0;
};

static const char* FeatureLevelStr()
{
    if (!g_feat.dx12DllFound) return "d3d12.dll MISSING (err 126)";
    if (!g_feat.dxgiDllFound) return "dxgi.dll MISSING (err 126)";
    switch (g_feat.maxFL) {
        case 120: return "DirectX 12  (Feature Level 12.0)  OK";
        case 111: return "DirectX 12  (Feature Level 11.1)  OK";
        case 110: return "DirectX 12  (Feature Level 11.0)  OK";
        case 0:   return "DirectX 12  NOT SUPPORTED (device creation failed)";
        default:  return "Not tested";
    }
}

// color, depending of the max feature level
static COLORREF FeatureLevelColor()
{
    switch (g_feat.maxFL) {
        case 120: return RGB(0, 180, 60);    // green  — FL 12.0
        case 111: return RGB(0, 160, 100);   // — FL 11.1
        case 110: return RGB(0, 130, 160);   // blue — FL 11.0
        default:  return RGB(160, 30, 30);   // red     — error
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WndState *st = reinterpret_cast<WndState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);

            HBRUSH bg = CreateSolidBrush(FeatureLevelColor());
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            HFONT hFontBig = CreateFontA(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, "Arial");
            HFONT hFontSm  = CreateFontA(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, "Arial");

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));

            RECT r1 = { rc.left, rc.top + 40, rc.right, rc.top + 80 };
            SelectObject(hdc, hFontBig);
            DrawTextA(hdc, FeatureLevelStr(), -1, &r1, DT_CENTER | DT_SINGLELINE);

            char line2[160];
            snprintf(line2, sizeof(line2), "GPU : %s", g_feat.adapterName);
            RECT r2 = { rc.left, rc.top + 100, rc.right, rc.top + 130 };
            SelectObject(hdc, hFontSm);
            DrawTextA(hdc, line2, -1, &r2, DT_CENTER | DT_SINGLELINE);

            if (st) {
                char line3[64];
                snprintf(line3, sizeof(line3), "%lu FPS   frame #%llu",
                         (unsigned long)st->fps,
                         (unsigned long long)st->frameCount);
                RECT r3 = { rc.left, rc.top + 140, rc.right, rc.top + 165 };
                DrawTextA(hdc, line3, -1, &r3, DT_CENTER | DT_SINGLELINE);
            }

            if (g_feat.loadError) {
                char line4[64];
                snprintf(line4, sizeof(line4), "Error when loading DLL : %lu", g_feat.loadError);
                RECT r4 = { rc.left, rc.top + 180, rc.right, rc.top + 205 };
                DrawTextA(hdc, line4, -1, &r4, DT_CENTER | DT_SINGLELINE);
            }

            RECT rf = { rc.left, rc.bottom - 30, rc.right, rc.bottom };
            DrawTextA(hdc, "Echap pour fermer", -1, &rf, DT_CENTER | DT_SINGLELINE);

            DeleteObject(hFontBig);
            DeleteObject(hFontSm);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenGreenWindow()
{
    printf("\n=== [3] Green window [60 FPS] (Press escape to exit) ===\n");
    printf("  Feature level detected : %s\n", FeatureLevelStr());
    printf("  GPU : %s\n", g_feat.adapterName);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX12TestWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"DX12TestWnd", L"dx12test — green window",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        800, 480, nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        printf("  ECHEC CreateWindowEx : error Windows %lu\n", GetLastError());
        return;
    }

    WndState state;
    state.fpsTimer = GetTickCount();
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&state);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    timeBeginPeriod(1);

    printf("  Window open. Press escape or close it to continue.\n");

    const DWORD TARGET_MS = 1000 / 60; // ~16 ms per frame
    MSG msg;
    while (true) {
        DWORD frameStart = GetTickCount();

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        state.frameCount++;
        state.frameInSec++;
        DWORD now = GetTickCount();
        if (now - state.fpsTimer >= 1000) {
            state.fps       = state.frameInSec;
            state.frameInSec = 0;
            state.fpsTimer  = now;
        }

        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);

        DWORD elapsed = GetTickCount() - frameStart;
        if (elapsed < TARGET_MS)
            Sleep(TARGET_MS - elapsed);
    }
done:
    timeEndPeriod(1);
    printf("  Window closed. %llu frames rended.\n",
           (unsigned long long)state.frameCount);
}

// ---------------------------------------------------------------
// main
// ---------------------------------------------------------------
int main(int argc, char *argv[])
{
    bool doWindow = false;
    bool doTests  = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--window") == 0) { doWindow = true; doTests = false; }
        else if (strcmp(argv[i], "--all")    == 0) { doWindow = true; doTests = true;  }
        else if (strcmp(argv[i], "--help")   == 0) {
            printf(
                "Usage :\n"
                "  dx12test.exe           => test device + functions DX12\n"
                "  dx12test.exe --window  => green window only\n"
                "  dx12test.exe --all     => test DX12 + green window\n"
                "  dx12test.exe --help    => help\n"
            );
            return 0;
        }
    }

    DWORD loadErr = 0;
    bool loaded = LoadDX12(loadErr);

    printf("\n--- Loading DLLs ---\n");
    if (g_hDXGI)
        printf("  dxgi.dll  : loaded\n");
    else
        printf("  dxgi.dll  : NOT FOUND (error %lu)\n", loadErr);

    if (g_hD3D12)
        printf("  d3d12.dll : loaded\n");
    else
        printf("  d3d12.dll : NOT FOUND (error %lu)\n", loadErr);

    if (!loaded) {
        printf("\n  RESULTAT : DX12 not available on this system.\n");
        if (loadErr == ERROR_MOD_NOT_FOUND)          // 126
            printf("  Error 126 : DLL not found.\n");
        else if (loadErr == ERROR_PROC_NOT_FOUND)    // 127
            printf("  Error 127 : DLL found but D3D12CreateDevice is missing.\n");
        else
            printf("  Error : %lu\n", loadErr);

        g_feat.loadError    = loadErr;
        g_feat.dx12DllFound = (g_hD3D12 != nullptr);
        g_feat.dxgiDllFound = (g_hDXGI  != nullptr);
        if (doWindow) OpenGreenWindow();
        printf("\nFin du test. Press Enter...\n");
        getchar();
        return (int)loadErr;
    }

    printf("  D3D12CreateDevice      : %s\n", g_D3D12CreateDevice     ? "OK" : "MISSING");
    printf("  D3D12GetDebugInterface : %s\n", g_D3D12GetDebugInterface ? "OK" : "MISSING");
    printf("  CreateDXGIFactory2     : %s\n", g_CreateDXGIFactory2     ? "OK" : "MISSING");

    if (doTests) {
        TestDeviceCreation();
        TestFeatureLevelFunctions();
    }

    DetectFeatureLevel();

    if (doWindow) {
        OpenGreenWindow();
    }

    if (g_hD3D12) FreeLibrary(g_hD3D12);
    if (g_hDXGI)  FreeLibrary(g_hDXGI);

    printf("\n=== End of the test. Press Enter... ===\n");
    getchar();
    return 0;
}
