/*
 * dxgi_win7.c — DXGI wrapper for Windows 7 x64
 *
 * Provides D3D12on7 support via ID3D12CommandQueueDownlevel, allowing
 * DirectX 12 applications to run on Windows 7.
 *
 * Compiled with MinGW64: x86_64-w64-mingw32-gcc -m64 -shared -O2 -std=c11 -o dxgw.dll dxgi_win791.c dxgi_win7.def -ldxgi -ld2d1 -loleaut32 -static -lgdi32
 * Single source + dxgi_win7.def
 */

/* =========================================================
 * Includes / base types
 * ========================================================= */
#define INITGUID

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601   /* Windows 7 */
#define WINVER       0x0601
#include <windows.h>
#include <unknwn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <winternl.h>   /* Provides NTSTATUS, PVOID, ULONG, PULONG */
#include <dxgi1_2.h>    /* Provides DXGI_ADAPTER_DESC, DXGI_ADAPTER_DESC1, and DXGI_ERROR_* */

typedef unsigned char      undefined;
typedef unsigned char      undefined1;
typedef unsigned short     undefined2;
typedef unsigned int       undefined4;
typedef unsigned long long undefined8;
typedef unsigned char      uchar;
typedef unsigned short     ushort;
typedef unsigned int       uint;
typedef unsigned long long ulonglong;
typedef long long          longlong;

/* =========================================================
 * D3D11 definitions missing from MinGW64 with _WIN32_WINNT=0x0601
 * ========================================================= */
typedef enum D3D11_MAP {
    D3D11_MAP_READ                 = 1,
    D3D11_MAP_WRITE                = 2,
    D3D11_MAP_READ_WRITE           = 3,
    D3D11_MAP_WRITE_DISCARD        = 4,
    D3D11_MAP_WRITE_NO_OVERWRITE   = 5
} D3D11_MAP;

typedef struct D3D11_MAPPED_SUBRESOURCE {
    void    *pData;
    UINT    RowPitch;
    UINT    DepthPitch;
} D3D11_MAPPED_SUBRESOURCE;

/* D3D11_USAGE */
#define D3D11_USAGE_DEFAULT         0
#define D3D11_USAGE_IMMUTABLE       1
#define D3D11_USAGE_DYNAMIC         2
#define D3D11_USAGE_STAGING         3

/* D3D11_CPU_ACCESS_FLAG */
#define D3D11_CPU_ACCESS_WRITE      0x00010000L
#define D3D11_CPU_ACCESS_READ       0x00020000L

/* D3D11_RESOURCE_MISC_FLAG */
#define D3D11_RESOURCE_MISC_SHARED                  0x2
#define D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX       0x4
#define D3D11_RESOURCE_MISC_SHARED_NTHANDLE         0x800

/* D3D11_BIND_FLAG */
#define D3D11_BIND_RENDER_TARGET        0x20
#define D3D11_BIND_SHADER_RESOURCE      0x8

/* =========================================================
 * Logging
 * ========================================================= */
#define LOG_BUF 512

/* D3D12 vtable offsets for command list and downlevel present */
#define D3D12_COMMAND_ALLOCATOR_RESET_OFF         0x40   /* ID3D12CommandAllocator::Reset */
#define D3D12_GRAPHICS_COMMAND_LIST_RESET_OFF     0x50   /* ID3D12GraphicsCommandList::Reset */
#define D3D12_COMMAND_QUEUE_DOWNLEVEL_PRESENT_OFF 0x18   /* ID3D12CommandQueueDownlevel::Present */

#ifndef DXGI_SCALING_STRETCH
#define DXGI_SCALING_STRETCH 0
#endif
#ifndef DXGI_SWAP_EFFECT_DISCARD
#define DXGI_SWAP_EFFECT_DISCARD 0
#endif
#ifndef DXGI_ALPHA_MODE_UNSPECIFIED
#define DXGI_ALPHA_MODE_UNSPECIFIED 0
#endif

static HANDLE g_hLogFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCS;
static BOOL g_logInit = FALSE;

/* Config — read from dxgw.ini (auto-created next to dxgi.dll on first run)
 * [dxgi_win7]
 * FPS_Overlay=0     ; set to 1 to enable the FPS counter overlay
 * OutputLogFile=0   ; set to 1 to write dxgw.log next to dxgi.dll
 */
static BOOL g_ovEnabled      = FALSE;
static BOOL g_logFileEnabled = FALSE;

static void DXGILog_Init(void)
{
    char dllPath[MAX_PATH];
    HMODULE hSelf = NULL;

    if (g_logInit) return;
    g_logInit = TRUE;
    InitializeCriticalSection(&g_logCS);

    /* Resolve directory of dxgi.dll itself (not the exe) to avoid
     * write-permission issues in game install directories.           */
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)DXGILog_Init, &hSelf);
    if (hSelf)
        GetModuleFileNameA(hSelf, dllPath, sizeof(dllPath));
    else
        GetModuleFileNameA(NULL, dllPath, sizeof(dllPath));

    /* Strip filename, keep trailing backslash */
    {
        char *last = strrchr(dllPath, '\\');
        if (last) *(last + 1) = '\0';
        else      dllPath[0]  = '\0';
    }

    /* ---- dxgw.ini : read or create with defaults ----
     * [dxgi_win7]
     * FPS_Overlay=0     ; 1 = show FPS counter in-game
     * OutputLogFile=0   ; 1 = write dxgw.log next to dxgi.dll
     */
    {
        char iniPath[MAX_PATH];
        HANDLE hIni;
        _snprintf(iniPath, sizeof(iniPath) - 1, "%sdxgw.ini", dllPath);
        iniPath[sizeof(iniPath) - 1] = '\0';

        /* Create with defaults only if the file is absent */
        hIni = CreateFileA(iniPath, GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hIni != INVALID_HANDLE_VALUE) {
            const char *defaults =
                "[dxgi_win7]\r\n"
                "FPS_Overlay=0\r\n"
                "OutputLogFile=0\r\n";
            DWORD written;
            WriteFile(hIni, defaults, (DWORD)strlen(defaults), &written, NULL);
            CloseHandle(hIni);
        }

        g_ovEnabled      = (BOOL)GetPrivateProfileIntA("dxgi_win7", "FPS_Overlay",   0, iniPath);
        g_logFileEnabled = (BOOL)GetPrivateProfileIntA("dxgi_win7", "OutputLogFile", 0, iniPath);
    }

    /* ---- Open dxgw.log only if enabled ---- */
    if (g_logFileEnabled) {
        char logPath[MAX_PATH];
        _snprintf(logPath, sizeof(logPath) - 1, "%sdxgw.log", dllPath);
        logPath[sizeof(logPath) - 1] = '\0';

        g_hLogFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ,
                                 NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                 NULL);
        /* Fallback to current directory if DLL directory is read-only */
        if (g_hLogFile == INVALID_HANDLE_VALUE)
            g_hLogFile = CreateFileA("dxgw.log", GENERIC_WRITE, FILE_SHARE_READ,
                                     NULL, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                     NULL);
    }
}

static void DXGILog(const char *fmt, ...)
{
    char buf[LOG_BUF + 2]; /* +2 for \r\n */
    int  len;
    va_list va;

    if (!g_logInit) DXGILog_Init();

    va_start(va, fmt);
    len = _vsnprintf(buf, LOG_BUF - 1, fmt, va);
    va_end(va);
    if (len < 0) len = LOG_BUF - 1;
    buf[len] = '\0';

    /* Append Windows line ending */
    buf[len]     = '\r';
    buf[len + 1] = '\n';
    buf[len + 2] = '\0';

    /* Write to log file only when OutputLogFile=1 in dxgw.ini */
    if (g_logFileEnabled && g_hLogFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        EnterCriticalSection(&g_logCS);
        WriteFile(g_hLogFile, buf, (DWORD)(len + 2), &written, NULL);
        LeaveCriticalSection(&g_logCS);
    }
    /* OutputDebugString always active — zero overhead when no debugger attached */
    OutputDebugStringA(buf);
}

/* Logging verbosity control.
 * g_logVerbose is TRUE during init (before the first Present).
 * After the first Present it is set to FALSE so per-frame paths
 * produce zero output — no file I/O, no OutputDebugString overhead.
 * Errors (LOG_ERR) are always emitted regardless of this flag.     */
static volatile LONG g_logVerbose = 1;

/* Convenience macros */
/* LOG      : init-time info, silenced after first Present          */
/* LOG_ERR  : always emitted (errors and unexpected states)         */
/* LOG_FRAME: per-frame hot-path — compiled out completely          */
/* LOG_ENTER/LOG_LEAVE/LOG_VOID: init-time entry/exit tracing       */
#define LOG(...)          do { if (g_logVerbose) DXGILog("[dxgi_win7] " __VA_ARGS__); } while(0)
#define LOG_ERR(...)      DXGILog("[dxgi_win7] ERR: " __VA_ARGS__)
#define LOG_FRAME(...)    /* per-frame: no-op */
#define LOG_ENTER(fn)     do { if (g_logVerbose) DXGILog("[dxgi_win7] --> " fn); } while(0)
#define LOG_LEAVE(fn,hr)  do { if (g_logVerbose) DXGILog("[dxgi_win7] <-- " fn " = 0x%08X", (unsigned)(hr)); } while(0)
#define LOG_VOID(fn)      do { if (g_logVerbose) DXGILog("[dxgi_win7] <-- " fn " (void)"); } while(0)

/* =========================================================
 * SEH emulation (__try/__except) for MinGW
 *
 * MinGW-w64 GCC does NOT support Microsoft's __try/__except syntax.
 * We emulate it using AddVectoredExceptionHandler + setjmp/longjmp.
 * ========================================================= */
#include <setjmp.h>

/* __thread (GCC native extension) instead of __declspec(thread) */
static __thread jmp_buf  g_SehJmpBuf;
static __thread BOOL     g_SehArmed = FALSE;
static __thread DWORD    g_SehLastExceptionCode = 0;
static PVOID g_SehVehHandle = NULL;

static LONG CALLBACK SehVectoredHandler(EXCEPTION_POINTERS *pInfo)
{
    if (g_SehArmed) {
        g_SehArmed = FALSE;
        g_SehLastExceptionCode = pInfo->ExceptionRecord->ExceptionCode;
        longjmp(g_SehJmpBuf, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void SehInit(void)
{
    if (!g_SehVehHandle)
        g_SehVehHandle = AddVectoredExceptionHandler(1 /* CALL_FIRST */, SehVectoredHandler);
}

/*
 * SEH_TRY / SEH_EXCEPT(code_var) — emulates __try { ... } __except(...) { ... }
 *
 * Usage:
 *   DWORD exCode;
 *   SEH_TRY {
 *       ... risky code ...
 *   } SEH_EXCEPT(exCode) {
 *       ... exception handling ...
 *   }
 */
#define SEH_TRY \
    SehInit(); \
    g_SehLastExceptionCode = 0; \
    g_SehArmed = TRUE; \
    if (setjmp(g_SehJmpBuf) == 0) { \
        do

#define SEH_EXCEPT(codevar) \
        while(0); \
        g_SehArmed = FALSE; \
    } else { \
        (codevar) = g_SehLastExceptionCode; \
        do

#define SEH_END \
        while(0); \
    }

/* =========================================================
 * HRESULT / DXGI error codes
 * ========================================================= */
#ifndef S_OK
#define S_OK ((HRESULT)0)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057L)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#endif
#ifndef E_NOTIMPL
#define E_NOTIMPL ((HRESULT)0x80004001L)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif

#define DXGI_CREATE_FACTORY_DEBUG  0x1u

/*
 * NtStatus to HRESULT conversion
 */
static HRESULT NtStatusToDxgiHR(NTSTATUS st)
{
    switch ((ULONG)st) {
    case 0:           return S_OK;
    case 0xc0000022:  return 0x80070005L; /* ACCESS_DENIED */
    case 0xc0000001:  return 0x80004005L; /* UNSUCCESSFUL */
    case 0x803e0006:  return 0x88980800L; /* GDI_HANDLE_ERROR */
    case 0xc0000002:  return E_NOTIMPL;
    case 0xc0000008:  return 0x80070006L; /* INVALID_HANDLE */
    case 0xc000000d:  return E_INVALIDARG;
    case 0xc0000017:  return E_OUTOFMEMORY;
    case 0xc0000024:  return 0x80070006L;
    case 0xc00000bb:  return E_INVALIDARG;
    case 0xc0000510:  return DXGI_ERROR_ALREADY_EXISTS;
    default:
        if ((LONG)st < 0) return (HRESULT)(st | 0x10000000L);
        return S_OK;
    }
}

/* =========================================================
 * DXGI GUIDs (public Microsoft SDK GUIDs, defined here to
 * avoid dependency on dxgi.h / initguid.h)
 * ========================================================= */
#define MAKE_GUID(name,l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) \
    static const GUID name = {l,w1,w2,{b1,b2,b3,b4,b5,b6,b7,b8}}

MAKE_GUID(IID_IUnknown_,         0x00000000,0x0000,0x0000,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);

MAKE_GUID(IID_IDXGIDeviceSubObj, 0x3D3E0379,0xF9DE,0x4D58,0xBB,0x6C,0x18,0xD6,0x29,0x92,0xF1,0xA6);

MAKE_GUID(IID_IDXGIAdapter3,     0x645967A4,0x1392,0x4310,0xA7,0x98,0x80,0x53,0xCE,0x3E,0x93,0xFD);
MAKE_GUID(IID_IDXGIAdapter4,     0x3C8D99D1,0x4FBF,0x4181,0xA8,0x2C,0xAF,0x66,0xBF,0x7B,0xD2,0x4E);

MAKE_GUID(IID_IDXGIOutput2,      0x595E39D1,0x2724,0x4663,0x99,0xB1,0xDA,0x96,0x9D,0xE2,0x83,0x64);
MAKE_GUID(IID_IDXGIOutput3,      0x8A6BB301,0x7E7E,0x41F4,0xA8,0xE0,0x5B,0x32,0xF7,0xF9,0x9B,0x18);
MAKE_GUID(IID_IDXGIOutput4,      0xDC7DCA35,0x2196,0x414D,0x9F,0x53,0x61,0x78,0x84,0x03,0x2A,0x60);
MAKE_GUID(IID_IDXGIOutput5,      0x80A07424,0xAB52,0x42EB,0x83,0x3C,0x0C,0x42,0xFD,0x28,0x2D,0x98);
MAKE_GUID(IID_IDXGIOutput6,      0x068346E8,0xAAEC,0x4B84,0xAD,0xD7,0x13,0x7F,0x51,0x3F,0x77,0xA1);

MAKE_GUID(IID_IDXGISwapChain2,   0xA8BE2AC4,0x199F,0x4946,0xB3,0x31,0x79,0x59,0x9F,0xB9,0x8D,0xE7);
MAKE_GUID(IID_IDXGISwapChain3,   0x94D99BDB,0xF1F8,0x4AB0,0xB2,0x36,0x7D,0xA0,0x17,0x0E,0xDA,0xB1);
MAKE_GUID(IID_IDXGISwapChain4,   0x3D585D5A,0xBD4A,0x489E,0xB1,0xF4,0x3D,0xBC,0xB6,0x45,0x2F,0xFB);

MAKE_GUID(IID_IDXGIFactory3,     0x25483823,0xCD46,0x4C7D,0x86,0xCA,0x47,0xAA,0x95,0xB8,0x37,0xBD);
MAKE_GUID(IID_IDXGIFactory4,     0x1BC6EA02,0xEF36,0x464F,0xBF,0x0C,0x21,0xCA,0x39,0xE5,0x16,0x8A);
MAKE_GUID(IID_IDXGIFactory5,     0x7632E1F5,0xEE65,0x4DCA,0x87,0xFD,0x84,0xCD,0x75,0xF8,0x83,0x8D);
MAKE_GUID(IID_IDXGIFactory6,     0xC1B6694F,0xFF09,0x44A9,0xB0,0x3C,0x77,0x90,0x0A,0x0A,0x1D,0x17);
MAKE_GUID(IID_IDXGIFactory7,     0xA4966EED,0x76DB,0x44DA,0x84,0xC1,0xEE,0x9A,0x7A,0xFB,0x20,0xA8);

MAKE_GUID(IID_IDXGIDevice3,      0x6007896C,0x3244,0x4AFD,0xBF,0x18,0xA6,0xD3,0xBE,0xDA,0x50,0x23);
MAKE_GUID(IID_IDXGIDevice4,      0x95B4F95F,0xD8DA,0x4CA4,0x9E,0xE6,0x3B,0x76,0xD5,0x96,0x8A,0x10);

/* =========================================================
 * D3D11 / D3D12 GUIDs
 * ========================================================= */
MAKE_GUID(IID_ID3D11Device,        0xDB6F6DDB,0xAC77,0x4E88,0x82,0x53,0x81,0x9D,0xF9,0xBB,0xF1,0x40);
MAKE_GUID(IID_ID3D11Device1,       0xA04BFB29,0x08EF,0x43D6,0xA4,0x9C,0xA9,0xBD,0xBD,0xCB,0xE6,0x86);
MAKE_GUID(IID_ID3D11Texture2D,     0x6F15AAF2,0xD208,0x4E89,0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C);
MAKE_GUID(IID_ID3D11Resource,      0xDC8E63F3,0xD12B,0x4952,0xB4,0x7B,0x5E,0x45,0x02,0x6A,0x86,0x2D);
MAKE_GUID(IID_ID3D12Device,        0x189819F1,0x1DB6,0x4B57,0xBE,0x54,0x18,0x21,0x33,0x9B,0x85,0xF7);
MAKE_GUID(IID_ID3D12CommandQueue,  0x0EC870A6,0x5D7E,0x4C22,0x8C,0xFC,0x5B,0xAA,0xE0,0x76,0x16,0xED);
/* D3D12on7 — official Microsoft API for presenting D3D12 content on Windows 7
 * (Direct3D12Downlevel package). */
MAKE_GUID(IID_ID3D12CommandQueueDownlevel, 0x38A8C5EF,0x7CCB,0x4E81,0x91,0x4F,0xA6,0xE9,0xD0,0x72,0xC4,0x94);
typedef enum { D3D12_DOWNLEVEL_PRESENT_FLAG_NONE_ = 0 } D3D12_DOWNLEVEL_PRESENT_FLAGS_;
typedef enum { D3D12_COMMAND_LIST_TYPE_DIRECT_ = 0 } D3D12_COMMAND_LIST_TYPE_;
MAKE_GUID(IID_ID3D12InfoQueue,     0x0742A90B,0xC387,0x483F,0xB9,0x46,0x30,0xA7,0xE4,0xE6,0x14,0x58);
MAKE_GUID(IID_ID3D12Resource,      0x696442BE,0xA72E,0x4059,0xBC,0x79,0x5B,0x5C,0x98,0x04,0x0F,0xAD);
MAKE_GUID(IID_ID3D12Fence,         0x0A753DCF,0xC4D8,0x4B91,0xAD,0xF6,0xBE,0x5A,0x60,0xD9,0x5A,0x76);
MAKE_GUID(IID_ID3D12CommandAllocator,    0x6102DEE4,0xAF59,0x4B09,0xB9,0x99,0xB4,0x4D,0x73,0xF0,0x9B,0x24);
MAKE_GUID(IID_ID3D12GraphicsCommandList, 0x5B160D0F,0xAC1B,0x4185,0x8B,0xA8,0xB3,0xAE,0x42,0xA5,0xA4,0x55);

typedef struct _D3D12_MESSAGE_MIN {
    UINT Category;
    UINT Severity;
    UINT ID;
    LPCSTR pDescription;
    SIZE_T DescriptionByteLength;
} D3D12_MESSAGE_MIN;

#define D3D12IQ_GetMessage_OFF          0x28
#define D3D12IQ_GetNumStoredMessages_OFF 0x40

static void D3D12_DumpInfoQueue(void *pIQ);  /* cached ID3D12InfoQueue* or NULL */

typedef enum { D3D_DRIVER_TYPE_UNKNOWN_=0, D3D_DRIVER_TYPE_HARDWARE_=1 } D3D_DRIVER_TYPE_;
typedef UINT D3D_FEATURE_LEVEL_;
#define D3D_FEATURE_LEVEL_11_0_  0xb000
#define D3D_FEATURE_LEVEL_11_1_  0xb100
#define D3D11_SDK_VERSION_ 7

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    IUnknown *pAdapter, D3D_DRIVER_TYPE_ DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL_ *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    void **ppDevice, D3D_FEATURE_LEVEL_ *pFeatureLevel, void **ppImmediateContext);

typedef struct _D3D11_TEXTURE2D_DESC_MIN {
    UINT Width, Height;
    UINT MipLevels, ArraySize;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
} D3D11_TEXTURE2D_DESC_MIN;

/* =========================================================
 * Minimal D3D12 structures
 * ========================================================= */
typedef enum {
    D3D12_HEAP_TYPE_DEFAULT_  = 1,
} D3D12_HEAP_TYPE_;

typedef enum {
    D3D12_CPU_PAGE_PROPERTY_UNKNOWN_ = 0,
} D3D12_CPU_PAGE_PROPERTY_;

typedef enum {
    D3D12_MEMORY_POOL_UNKNOWN_ = 0,
} D3D12_MEMORY_POOL_;

typedef struct _D3D12_HEAP_PROPERTIES_MIN {
    D3D12_HEAP_TYPE_ Type;
    D3D12_CPU_PAGE_PROPERTY_ CPUPageProperty;
    D3D12_MEMORY_POOL_ MemoryPoolPreference;
    UINT CreationNodeMask;
    UINT VisibleNodeMask;
} D3D12_HEAP_PROPERTIES_MIN;

typedef enum {
    D3D12_HEAP_FLAG_NONE_   = 0,
    D3D12_HEAP_FLAG_SHARED_ = 0x1,
} D3D12_HEAP_FLAGS_;

typedef enum {
    D3D12_RESOURCE_DIMENSION_TEXTURE2D_ = 3,
} D3D12_RESOURCE_DIMENSION_;

typedef enum {
    D3D12_TEXTURE_LAYOUT_UNKNOWN_   = 0,
    D3D12_TEXTURE_LAYOUT_ROW_MAJOR_ = 1,
} D3D12_TEXTURE_LAYOUT_;

typedef enum {
    D3D12_RESOURCE_FLAG_NONE_                  = 0,
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET_   = 0x1,
    D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS_ = 0x20,
} D3D12_RESOURCE_FLAGS_;

typedef struct _D3D12_RESOURCE_DESC_MIN {
    D3D12_RESOURCE_DIMENSION_ Dimension;
    UINT64 Alignment;
    UINT64 Width;
    UINT   Height;
    UINT16 DepthOrArraySize;
    UINT16 MipLevels;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D12_TEXTURE_LAYOUT_ Layout;
    D3D12_RESOURCE_FLAGS_ Flags;
} D3D12_RESOURCE_DESC_MIN;

/* Note: D3D12_RESOURCE_STATES are separate from D3D12_RESOURCE_FLAGS_ */
typedef enum {
    D3D12_RESOURCE_STATE_COMMON_                 = 0,
    D3D12_RESOURCE_STATE_RENDER_TARGET_          = 0x4,
    D3D12_RESOURCE_STATE_PRESENT_                = 0,
    D3D12_RESOURCE_STATE_COPY_DEST_              = 0x400,
    D3D12_RESOURCE_STATE_COPY_SOURCE_            = 0x800,
} D3D12_RESOURCE_STATES_;

/* D3D12_CLEAR_VALUE — union of Color[4] and DepthStencil */
typedef struct _D3D12_CLEAR_VALUE_MIN {
    DXGI_FORMAT Format;
    FLOAT Color[4];
} D3D12_CLEAR_VALUE_MIN;

/* D3D12 device vtable offsets */
#define D3D12DEV_OpenSharedHandle_OFF        0x100
#define D3D12DEV_CreateFence_OFF             0x120
#define D3D12DEV_CreateCommandAllocator_OFF  0x48
#define D3D12DEV_CreateCommandList_OFF       0x60
#define D3D12DEV_CreateCommittedResource_OFF 0xD8
#define D3D12DEV_CreateGraphicsPSO_OFF       0x68  /* CreateGraphicsPipelineState */
#define D3D12DEV_CreateRootSig_OFF           0x40  /* CreateRootSignature */
#define D3D12DEV_CreateDescHeap_OFF          0x78  /* CreateDescriptorHeap */
#define D3D12DEV_CreateCBV_SRV_UAV_OFF       0x80  /* CreateShaderResourceView */
#define D3D12DEV_GetDescIncSize_OFF          0x50  /* GetDescriptorHandleIncrementSize */
#define D3D12DEV_CreateUploadBuf_OFF         0xD8  /* same as CommittedResource */
#define D3D12CQ_GetDevice_OFF   0x38
#define D3D12CQ_Signal_OFF      0x70

/* ID3D12Fence vtable offsets (inherits from ID3D12DeviceChild) */
#define D3D12FENCE_GetCompletedValue_OFF     0x40
#define D3D12FENCE_SetEventOnCompletion_OFF  0x48
#define D3D12FENCE_Signal_OFF                0x50


/* =========================================================
 * Diagnostic helper: validates a function pointer before calling
 * ========================================================= */
static BOOL IsLikelyValidCodePtr(void *p, const char *label)
{
    if (!p) { LOG("%s: NULL pointer", label); return FALSE; }

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
        LOG("%s: VirtualQuery failed on %p (GetLastError=%lu)",
            label, p, GetLastError());
        return FALSE;
    }
    BOOL execOk = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;

    HMODULE hMod = NULL;
    char modName[MAX_PATH] = "?unknown?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)p, &hMod)) {
        GetModuleFileNameA(hMod, modName, MAX_PATH);
    }

    LOG("%s: ptr=%p exec=%d module=%s", label, p, execOk, modName);
    return execOk;
}

/* Validates an object pointer (data, not code) */
static BOOL IsLikelyValidObjectPtr(void *p, const char *label)
{
    if (!p) { LOG("%s: NULL pointer", label); return FALSE; }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
        LOG("%s: VirtualQuery failed on %p (GetLastError=%lu)",
            label, p, GetLastError());
        return FALSE;
    }
    BOOL readable = (mbi.State == MEM_COMMIT) &&
        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_WRITECOPY)) != 0;
    LOG("%s: ptr=%p committed=%d readable=%d", label, p,
        mbi.State == MEM_COMMIT, readable);
    return readable;
}

static BOOL GuidEq(REFIID a, const GUID *b)
{
    return (a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3
        && ((const UINT64*)a->Data4)[0] == ((const UINT64*)b->Data4)[0]);
}

/* GUID to string for logging */
static const char* GuidName(REFIID riid)
{
#define G(n) if(GuidEq(riid,&IID_##n)) return #n
    G(IUnknown_); G(IDXGIObject); G(IDXGIAdapter); G(IDXGIAdapter1);
    G(IDXGIAdapter2); G(IDXGIAdapter3); G(IDXGIAdapter4);
    G(IDXGIOutput); G(IDXGIOutput1); G(IDXGIOutput2); G(IDXGIOutput3);
    G(IDXGIOutput4); G(IDXGIOutput5); G(IDXGIOutput6);
    G(IDXGISwapChain); G(IDXGISwapChain1); G(IDXGISwapChain2);
    G(IDXGISwapChain3); G(IDXGISwapChain4);
    G(IDXGIFactory); G(IDXGIFactory1); G(IDXGIFactory2); G(IDXGIFactory3);
    G(IDXGIFactory4); G(IDXGIFactory5); G(IDXGIFactory6); G(IDXGIFactory7);
    G(IDXGIDevice); G(IDXGIDevice1); G(IDXGIDevice2); G(IDXGIDevice3); G(IDXGIDevice4);
    G(IDXGISurface); G(IDXGISurface1); G(IDXGISurface2);
    G(IDXGIResource); G(IDXGIResource1); G(IDXGIKeyedMutex);
#undef G
    return "?Unknown?";
}

static const char* GuidNameRaw(REFIID riid)
{
    static char buf[160];
    if(!riid) { return "(null riid)"; }
    _snprintf(buf,sizeof(buf)-1,
        "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} (%s)",
        riid->Data1, riid->Data2, riid->Data3,
        riid->Data4[0], riid->Data4[1],
        riid->Data4[2], riid->Data4[3], riid->Data4[4],
        riid->Data4[5], riid->Data4[6], riid->Data4[7],
        GuidName(riid));
    buf[sizeof(buf)-1]=0;
    return buf;
}
#define LOG_GUID(label,riid) LOG("%s riid=%s",(label),GuidNameRaw(riid))

/* =========================================================
 * Function pointer types — system dxgi.dll
 * ========================================================= */
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory )(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT,   REFIID, void**);

/* =========================================================
 * D3DKMT function pointers (gdi32.dll)
 * ========================================================= */
typedef UINT D3DKMT_HANDLE;
typedef struct { UINT Value; } D3DKMT_SETPROCESSDEVICEREMOVALSUPPORT;
typedef struct { D3DKMT_HANDLE hAdapter; UINT VidPnSourceId; } D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP;
typedef struct { D3DKMT_HANDLE hAdapter; UINT VidPnSourceId; } D3DKMT_WAITFORVERTICALBLANKEVENT;
typedef struct { D3DKMT_HANDLE hAdapter; UINT VidPnSourceId; UINT NumObjects; HANDLE Handles[8]; } D3DKMT_WAITFORVERTICALBLANKEVENT2;
typedef NTSTATUS (APIENTRY *PFN_D3DKMTSetProcessDeviceRemovalSupport)(const D3DKMT_SETPROCESSDEVICEREMOVALSUPPORT*);
typedef NTSTATUS (APIENTRY *PFN_D3DKMTCheckVidPnExclusiveOwnership)(const D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP*);
typedef NTSTATUS (APIENTRY *PFN_D3DKMTWaitForVerticalBlankEvent)(const D3DKMT_WAITFORVERTICALBLANKEVENT*);
typedef NTSTATUS (APIENTRY *PFN_D3DKMTWaitForVerticalBlankEvent2)(const D3DKMT_WAITFORVERTICALBLANKEVENT2*);

/* WNF (Windows Notification Facility) */
typedef ULONG64 WNF_STATE_NAME;
typedef NTSTATUS (NTAPI *WNF_USER_CALLBACK)(WNF_STATE_NAME,ULONG,PVOID,PVOID,PVOID,ULONG);
typedef PVOID WNF_SUBSCRIPTION_HANDLE;
typedef NTSTATUS (NTAPI *PFN_RtlSubscribeWnf)(WNF_SUBSCRIPTION_HANDLE*,WNF_STATE_NAME,ULONG,PVOID,WNF_USER_CALLBACK,PVOID,ULONG,ULONG);
typedef NTSTATUS (NTAPI *PFN_RtlUnsubscribeWnf)(WNF_SUBSCRIPTION_HANDLE);
typedef NTSTATUS (NTAPI *PFN_RtlQueryWnfStateData)(PULONG,WNF_STATE_NAME,WNF_USER_CALLBACK,PVOID,PVOID);
typedef NTSTATUS (NTAPI *PFN_RtlUnsubscribeWnfWait)(WNF_SUBSCRIPTION_HANDLE);

/* =========================================================
 * Global state
 * ========================================================= */

static HMODULE               g_hDxgiSystem   = NULL;
static PFN_CreateDXGIFactory  g_pfnFactory    = NULL;
static PFN_CreateDXGIFactory1 g_pfnFactory1   = NULL;
static PFN_CreateDXGIFactory2 g_pfnFactory2   = NULL;
/* Reentrancy guards for CreateDXGIFactory* — set to 1 while we are inside
 * the wrapper so that recursive calls (PIX/DXCapture/UE4) bypass wrapping
 * and go straight to the system dxgi.dll.  Plain globals (not TLS) are
 * sufficient here: reentrancy always happens on the same thread that entered
 * the function, so a second thread racing in will briefly see the flag set,
 * but will immediately call the system function directly which is safe. */
static volatile LONG g_inCreateFactory  = 0;
static volatile LONG g_inCreateFactory1 = 0;
static volatile LONG g_inCreateFactory2 = 0;

static PFN_D3DKMTSetProcessDeviceRemovalSupport g_D3DKMTSetProcessDeviceRemovalSupport = NULL;
static PFN_D3DKMTCheckVidPnExclusiveOwnership   g_D3DKMTCheckVidPnExclusiveOwnership   = NULL;
static PFN_D3DKMTWaitForVerticalBlankEvent      g_D3DKMTWaitForVerticalBlankEvent      = NULL;
static PFN_D3DKMTWaitForVerticalBlankEvent2     g_D3DKMTWaitForVerticalBlankEvent2     = NULL;

static PFN_RtlSubscribeWnf        g_pfnRtlSubscribeWnf        = NULL;
static PFN_RtlUnsubscribeWnf      g_pfnRtlUnsubscribeWnf      = NULL;
static PFN_RtlQueryWnfStateData   g_pfnRtlQueryWnfStateData   = NULL;
static PFN_RtlUnsubscribeWnfWait  g_pfnRtlUnsubscribeWnfWait  = NULL;

static LPWSTR g_pExePath = NULL;
static LPWSTR g_pExeDir  = NULL;

static volatile PVOID g_pCompatBase   = NULL;
static volatile PVOID g_pCompatSecond = NULL;
#define COMPAT_BUF_SZ 512
static char             g_szCompatBuf[COMPAT_BUF_SZ];
static SIZE_T           g_cbCompatBuf  = 0;
static BOOL             g_bCompatBuilt = FALSE;
static CRITICAL_SECTION g_csCompat;

#define JOURNAL_N    0x40
#define JOURNAL_SZ   0x78
typedef struct { UINT type; UINT arg1; UINT arg2; UINT arg3; UINT64 ts; char msg[0x60]; } JOURNAL_ENTRY;
static JOURNAL_ENTRY g_Journal[JOURNAL_N];
static volatile int  g_JournalHead = 0;

static CRITICAL_SECTION g_csHMD;
static volatile PVOID   g_pActiveFactory = NULL;

static INIT_ONCE g_InitOnceCompat = INIT_ONCE_STATIC_INIT;
static BOOL  g_bCompatResQuirkSet = FALSE;
static DWORD g_dwNativeW = 0, g_dwNativeH = 0;
static DWORD g_dwLogicW  = 0, g_dwLogicH  = 0;

static IUnknown* g_pFallbackFactory = NULL;
/* =========================================================
 * Private Data Store (for COM wrappers)
 * ========================================================= */
typedef struct _PRIVENTRY {
    struct _PRIVENTRY *pNext;
    GUID               Guid;
    UINT               DataSz;
    BOOL               bIsUnk;
    union { BYTE *pData; IUnknown *pUnk; };
} PRIVENTRY;

static void PrivData_Free(PRIVENTRY *p)
{
    if (!p) return;
    if (p->bIsUnk) { if (p->pUnk) p->pUnk->lpVtbl->Release(p->pUnk); }
    else           { if (p->pData) HeapFree(GetProcessHeap(),0,p->pData); }
    HeapFree(GetProcessHeap(),0,p);
}

static PRIVENTRY** PrivData_Find(PRIVENTRY **ppHead, REFIID g)
{
    PRIVENTRY **pp = ppHead;
    while (*pp) {
        if (GuidEq(&(*pp)->Guid, g)) return pp;
        pp = &(*pp)->pNext;
    }
    return NULL;
}

static HRESULT PrivData_Set(PRIVENTRY **ppHead, CRITICAL_SECTION *pcs,
                             REFIID guid, UINT sz, const void *pData)
{
    PRIVENTRY **pp, *e;
    EnterCriticalSection(pcs);
    pp = PrivData_Find(ppHead, guid);
    if (!sz || !pData) {
        if (pp) { e=*pp; *pp=e->pNext; PrivData_Free(e); }
        LeaveCriticalSection(pcs); return S_OK;
    }
    if (sz && !pData) { LeaveCriticalSection(pcs); return DXGI_ERROR_INVALID_CALL; }
    if (pp) {
        e = *pp;
        if (e->bIsUnk && e->pUnk) e->pUnk->lpVtbl->Release(e->pUnk);
        else if (e->pData) HeapFree(GetProcessHeap(),0,e->pData);
        e->pData=NULL; e->bIsUnk=FALSE;
    } else {
        e = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*e));
        if (!e) { LeaveCriticalSection(pcs); return E_OUTOFMEMORY; }
        memcpy(&e->Guid,guid,sizeof(GUID));
        e->pNext=*ppHead; *ppHead=e;
    }
    e->pData = HeapAlloc(GetProcessHeap(),0,sz);
    if (!e->pData) {
        if (!pp) { *ppHead=e->pNext; HeapFree(GetProcessHeap(),0,e); }
        LeaveCriticalSection(pcs); return E_OUTOFMEMORY;
    }
    memcpy(e->pData,pData,sz); e->DataSz=sz; e->bIsUnk=FALSE;
    LeaveCriticalSection(pcs); return S_OK;
}

static HRESULT PrivData_SetUnk(PRIVENTRY **ppHead, CRITICAL_SECTION *pcs,
                                REFIID guid, const IUnknown *pUnk)
{
    PRIVENTRY **pp, *e;
    EnterCriticalSection(pcs);
    pp = PrivData_Find(ppHead, guid);
    if (!pUnk) {
        if (pp) { e=*pp; *pp=e->pNext; PrivData_Free(e); }
        LeaveCriticalSection(pcs); return S_OK;
    }
    if (pp) {
        e = *pp;
        if (e->bIsUnk && e->pUnk) e->pUnk->lpVtbl->Release(e->pUnk);
        else if (e->pData) HeapFree(GetProcessHeap(),0,e->pData);
        e->pData=NULL;
    } else {
        e = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*e));
        if (!e) { LeaveCriticalSection(pcs); return E_OUTOFMEMORY; }
        memcpy(&e->Guid,guid,sizeof(GUID));
        e->pNext=*ppHead; *ppHead=e;
    }
    ((IUnknown*)pUnk)->lpVtbl->AddRef((IUnknown*)pUnk);
    e->pUnk=((IUnknown*)pUnk); e->DataSz=sizeof(IUnknown*); e->bIsUnk=TRUE;
    LeaveCriticalSection(pcs); return S_OK;
}

static HRESULT PrivData_Get(PRIVENTRY **ppHead, CRITICAL_SECTION *pcs,
                             REFIID guid, UINT *pSz, void *pDst)
{
    PRIVENTRY **pp; HRESULT hr=DXGI_ERROR_NOT_FOUND;
    EnterCriticalSection(pcs);
    pp = PrivData_Find(ppHead, guid);
    if (pp) {
        PRIVENTRY *e=*pp;
        UINT need = e->bIsUnk ? sizeof(IUnknown*) : e->DataSz;
        if (!pDst) { *pSz=need; hr=S_OK; }
        else if (*pSz < need) { *pSz=need; hr=DXGI_ERROR_MORE_DATA; }
        else if (e->bIsUnk) {
            e->pUnk->lpVtbl->AddRef(e->pUnk);
            *(IUnknown**)pDst=e->pUnk; *pSz=sizeof(IUnknown*); hr=S_OK;
        } else {
            memcpy(pDst,e->pData,e->DataSz); *pSz=e->DataSz; hr=S_OK;
        }
    }
    LeaveCriticalSection(pcs); return hr;
}

static void PrivData_DestroyAll(PRIVENTRY **ppHead, CRITICAL_SECTION *pcs)
{
    PRIVENTRY *p, *n;
    EnterCriticalSection(pcs);
    p=*ppHead; *ppHead=NULL;
    LeaveCriticalSection(pcs);
    while (p) { n=p->pNext; PrivData_Free(p); p=n; }
}

/* =========================================================
 * Vtable dispatch macro
 * ========================================================= */
#define VTBL(obj)           (*(void***)(obj))
#define VS(obj,off)         (VTBL(obj)[(off)/8])

/* Dump and clear the D3D12 info queue.
 * pIQ is the cached ID3D12InfoQueue* from D3D12Interop.pInfoQueue.
 * Passing NULL is a no-op (debug layer not active -- common case).
 * No QI performed here: the pointer is resolved once at device creation. */
static void D3D12_DumpInfoQueue(void *pIQ)
{
    typedef UINT64(STDMETHODCALLTYPE*PFN_GetNum)(void*);
    typedef HRESULT(STDMETHODCALLTYPE*PFN_GetMsg)(void*,UINT64,D3D12_MESSAGE_MIN*,SIZE_T*);
    typedef void(STDMETHODCALLTYPE*PFN_Clear)(void*);
    UINT64 n, i;
    if (!pIQ) return;
    n = ((PFN_GetNum)VS(pIQ, D3D12IQ_GetNumStoredMessages_OFF))(pIQ);
    if (n == 0) {
        LOG("  D3D12_DumpInfoQueue: 0 messages stored");
    } else {
        LOG("  D3D12_DumpInfoQueue: *** %llu message(s) stored ***", (unsigned long long)n);
        for (i = 0; i < n; i++) {
            SIZE_T len = 0;
            D3D12_MESSAGE_MIN *m;
            ((PFN_GetMsg)VS(pIQ,D3D12IQ_GetMessage_OFF))(pIQ,i,NULL,&len);
            if (len == 0) continue;
            m = (D3D12_MESSAGE_MIN*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
            if (!m) continue;
            if (SUCCEEDED(((PFN_GetMsg)VS(pIQ,D3D12IQ_GetMessage_OFF))(pIQ,i,m,&len))) {
                static const char* const sevNames[] = {
                    "CORRUPTION","ERROR","WARNING","INFO","MESSAGE"
                };
                const char *sev = (m->Severity < 5) ? sevNames[m->Severity] : "UNKNOWN";
                LOG("  D3D12 MSG[%llu] [%s] cat=%u id=%u: %s",
                    (unsigned long long)i, sev, m->Category, m->ID,
                    m->pDescription ? m->pDescription : "(null)");
            }
            HeapFree(GetProcessHeap(), 0, m);
        }
    }
    ((PFN_Clear)VS(pIQ, 4*8))(pIQ);
}


/* =========================================================
 * ==================== COM WRAPPERS =======================
 * ========================================================= */
typedef struct _DXGIBASE {
    void              *pVtbl;
    volatile LONG      lRef;
    IUnknown          *pReal;
    PRIVENTRY         *pPriv;
    CRITICAL_SECTION   csPriv;
} DXGIBASE;

static void Base_Init(DXGIBASE *b, void *vtbl, IUnknown *pReal)
{
    b->pVtbl = vtbl;
    b->lRef  = 1;
    b->pReal = pReal;
    b->pPriv = NULL;
    InitializeCriticalSection(&b->csPriv);
}
static void Base_Dtor(DXGIBASE *b)
{
    PrivData_DestroyAll(&b->pPriv,&b->csPriv);
    DeleteCriticalSection(&b->csPriv);
    if (b->pReal) { b->pReal->lpVtbl->Release(b->pReal); b->pReal=NULL; }
}
static ULONG Base_AddRef(DXGIBASE *b) { return (ULONG)InterlockedIncrement(&b->lRef); }
static ULONG Base_Release(DXGIBASE *b, void(*dtor)(DXGIBASE*))
{
    LONG n = InterlockedDecrement(&b->lRef);
    if (!n) { Base_Dtor(b); if(dtor) dtor(b); HeapFree(GetProcessHeap(),0,b); return 0; }
    return (ULONG)n;
}

static HRESULT WrapFactory_Create(IUnknown *pReal, UINT uFlags, REFIID riid, void **ppOut);

/* =========================================================
 * ======== SWAPCHAIN WRAPPER ========
 * ========================================================= */
#define D3D12_INTEROP_MAX_BUFFERS 8

/* Cached function pointer types used in the Present hot-path */
typedef HRESULT (STDMETHODCALLTYPE *PFN_AllocReset_t)(void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ListReset_t)(void*, void*, void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DLPresent_t)(void*, void*, void*, HWND, D3D12_DOWNLEVEL_PRESENT_FLAGS_);
typedef UINT64  (STDMETHODCALLTYPE *PFN_FenceGetVal_t)(void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_FenceWait_t)(void*, UINT64, HANDLE);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CQSignal_t)(void*, void*, UINT64);

typedef struct _D3D12Interop {
    /*
     * D3D12on7 architecture (ID3D12CommandQueueDownlevel) -- official
     * Microsoft API for presenting D3D12 content on Windows 7. No
     * hidden D3D11 device/swapchain, no manual cross-API copy.
     */
    void *pD3D12Dev;        /* ID3D12Device* (kept ref) */
    void *pD3D12CQ;         /* ID3D12CommandQueue* (kept ref) */
    void *pDownlevel;       /* ID3D12CommandQueueDownlevel* (QI from pD3D12CQ) */
    void *pCmdAlloc;        /* ID3D12CommandAllocator* (ours, for Present) */
    void *pCmdList;         /* ID3D12GraphicsCommandList* (ours, for Present) */
    UINT  BufferCount;
    UINT  Width, Height;
    DXGI_FORMAT Format;
    UINT  presentIndex;
    HWND  hwnd;
    /* D3D12 "back buffer" resources, exposed via GetBuffer.
     * No sharing, no MISC_SHARED -- just standard D3D12 render targets. */
    void *pD3D12RenderRes[D3D12_INTEROP_MAX_BUFFERS];

    /* ---- Cached vtable pointers (resolved once at Create, used every Present) ---- */
    PFN_AllocReset_t  pfnAllocReset;  /* ID3D12CommandAllocator::Reset       */
    PFN_ListReset_t   pfnListReset;   /* ID3D12GraphicsCommandList::Reset     */
    PFN_DLPresent_t   pfnDLPresent;   /* ID3D12CommandQueueDownlevel::Present */
    /* ---- Hot-path precomputed values ---- */
    UINT  bufIdxMask;   /* BufferCount-1 when pow2, else 0 (falls back to %) */
    void *pInfoQueue;   /* ID3D12InfoQueue* cached once; NULL if unavailable  */

    /* ---- Frame-latency fence (emulates IDXGISwapChain2::MaxFrameLatency=1) ---- */
    void   *pFence;           /* ID3D12Fence*                                    */
    HANDLE  hFenceEvent;      /* Win32 event signaled when frame completes        */
    UINT64  fenceValue;       /* monotonically increasing signal value            */
    UINT    maxFrameLatency;  /* currently requested latency (default 1)          */
    PFN_FenceGetVal_t pfnFenceGetVal;  /* ID3D12Fence::GetCompletedValue          */
    PFN_FenceWait_t   pfnFenceWait;    /* ID3D12Fence::SetEventOnCompletion       */
    PFN_CQSignal_t    pfnCQSignal;     /* ID3D12CommandQueue::Signal              */

    /* ---- FPS overlay ---- */
    void *ovPSO;            /* ID3D12PipelineState* for overlay quads (future)   */
    void *ovRootSig;        /* ID3D12RootSignature*                 (future)      */
    void *ovVBuf;           /* ID3D12Resource* vertex buffer        (future)      */
    void *ovTex;            /* ID3D12Resource* glyph atlas          (future)      */
    void *ovDescHeap;       /* ID3D12DescriptorHeap*                (future)      */
    BOOL  ovReady;          /* TRUE once overlay is initialised                   */
    /* GDI resources -- created once in Ov_Init, reused every frame */
    HDC     ovMemDC;        /* off-screen DC for double-buffering                 */
    HBITMAP ovBmp;          /* bitmap selected into ovMemDC                       */
    HBITMAP ovOldBmp;       /* original bitmap to restore on cleanup              */
    HFONT   ovFont;         /* bold Courier New 14pt                              */
    HFONT   ovOldFont;      /* original font to restore on cleanup                */
    /* FPS measurement */
    LARGE_INTEGER ovQpcFreq;
    LARGE_INTEGER ovQpcLast;
    UINT          ovFpsSamples[8];   /* rolling window -- 8 frames ~= 0.13s at 60fps */
    UINT          ovFpsSampleIdx;
    UINT          ovFpsCurrent;
} D3D12Interop;

typedef struct _WrapSC {
    DXGIBASE base;
    void    *pReal1;
    void    *pReal2;
    void    *pReal3;
    void    *pReal4;
    D3D12Interop *interop;
} WrapSC;

static D3D12Interop* D3D12Interop_Create(IUnknown *pD3D12CQ, HWND hwnd,
    const void *pDesc, IUnknown **ppRealSC);
static void D3D12Interop_Destroy(D3D12Interop *it);
static HRESULT D3D12Interop_GetBuffer(WrapSC *w, UINT Buf, REFIID riid, void **pp);
static HRESULT D3D12Interop_Present(WrapSC *w, UINT SyncInterval, UINT Flags, HRESULT *outHr);

static HRESULT STDMETHODCALLTYPE SC_QI      (WrapSC*,REFIID,void**);
static ULONG   STDMETHODCALLTYPE SC_AddRef  (WrapSC*);
static ULONG   STDMETHODCALLTYPE SC_Release (WrapSC*);
static HRESULT STDMETHODCALLTYPE SC_SetPriv (WrapSC*,REFGUID,UINT,const void*);
static HRESULT STDMETHODCALLTYPE SC_SetPrivUnk(WrapSC*,REFGUID,const IUnknown*);
static HRESULT STDMETHODCALLTYPE SC_GetPriv (WrapSC*,REFGUID,UINT*,void*);
static HRESULT STDMETHODCALLTYPE SC_GetParent(WrapSC*,REFIID,void**);
static HRESULT STDMETHODCALLTYPE SC_Present           (WrapSC*,UINT,UINT);
static HRESULT STDMETHODCALLTYPE SC_GetBuffer         (WrapSC*,UINT,REFIID,void**);
static HRESULT STDMETHODCALLTYPE SC_SetFullscreenState(WrapSC*,BOOL,void*);
static HRESULT STDMETHODCALLTYPE SC_GetFullscreenState(WrapSC*,BOOL*,void**);
static HRESULT STDMETHODCALLTYPE SC_GetDesc           (WrapSC*,void*);
static HRESULT STDMETHODCALLTYPE SC_ResizeBuffers     (WrapSC*,UINT,UINT,UINT,UINT,UINT);
static HRESULT STDMETHODCALLTYPE SC_ResizeTarget      (WrapSC*,const void*);
static HRESULT STDMETHODCALLTYPE SC_GetContainingOutput(WrapSC*,void**);
static HRESULT STDMETHODCALLTYPE SC_GetFrameStatistics(WrapSC*,void*);
static HRESULT STDMETHODCALLTYPE SC_GetLastPresentCount(WrapSC*,UINT*);
static HRESULT STDMETHODCALLTYPE SC_GetDesc1             (WrapSC*,void*);
static HRESULT STDMETHODCALLTYPE SC_GetFullscreenDesc    (WrapSC*,void*);
static HRESULT STDMETHODCALLTYPE SC_GetHwnd              (WrapSC*,HWND*);
static HRESULT STDMETHODCALLTYPE SC_GetCoreWindow        (WrapSC*,REFIID,void**);
static HRESULT STDMETHODCALLTYPE SC_Present1             (WrapSC*,UINT,UINT,const void*);
static BOOL    STDMETHODCALLTYPE SC_IsTemporaryMonoSupported(WrapSC*);
static HRESULT STDMETHODCALLTYPE SC_GetRestrictToOutput  (WrapSC*,void**);
static HRESULT STDMETHODCALLTYPE SC_SetBackgroundColor   (WrapSC*,const void*);
static HRESULT STDMETHODCALLTYPE SC_GetBackgroundColor   (WrapSC*,void*);
static HRESULT STDMETHODCALLTYPE SC_SetRotation          (WrapSC*,UINT);
static HRESULT STDMETHODCALLTYPE SC_GetRotation          (WrapSC*,UINT*);
static HRESULT STDMETHODCALLTYPE SC_SetSourceSize        (WrapSC*,UINT,UINT);
static HRESULT STDMETHODCALLTYPE SC_GetSourceSize        (WrapSC*,UINT*,UINT*);
static HRESULT STDMETHODCALLTYPE SC_SetMaximumFrameLatency(WrapSC*,UINT);
static HRESULT STDMETHODCALLTYPE SC_GetMaximumFrameLatency(WrapSC*,UINT*);
static HANDLE  STDMETHODCALLTYPE SC_GetFrameLatencyWaitableObject(WrapSC*);
static HRESULT STDMETHODCALLTYPE SC_SetMatrixTransform   (WrapSC*,const void*);
static HRESULT STDMETHODCALLTYPE SC_GetMatrixTransform   (WrapSC*,void*);
static UINT    STDMETHODCALLTYPE SC_GetCurrentBackBufferIndex(WrapSC*);
static HRESULT STDMETHODCALLTYPE SC_CheckColorSpaceSupport(WrapSC*,UINT,UINT*);
static HRESULT STDMETHODCALLTYPE SC_SetColorSpace1       (WrapSC*,UINT);
static HRESULT STDMETHODCALLTYPE SC_ResizeBuffers1       (WrapSC*,UINT,UINT,UINT,UINT,UINT,const UINT*,IUnknown*const*);
static HRESULT STDMETHODCALLTYPE SC_SetHDRMetaData       (WrapSC*,UINT,UINT,void*);

static const void* g_SC_Vtbl[] = {
    SC_QI, SC_AddRef, SC_Release,
    SC_SetPriv, SC_SetPrivUnk, SC_GetPriv, SC_GetParent,
    NULL,
    SC_Present, SC_GetBuffer, SC_SetFullscreenState, SC_GetFullscreenState,
    SC_GetDesc, SC_ResizeBuffers, SC_ResizeTarget, SC_GetContainingOutput,
    SC_GetFrameStatistics, SC_GetLastPresentCount,
    SC_GetDesc1, SC_GetFullscreenDesc, SC_GetHwnd, SC_GetCoreWindow,
    SC_Present1, SC_IsTemporaryMonoSupported, SC_GetRestrictToOutput,
    SC_SetBackgroundColor, SC_GetBackgroundColor, SC_SetRotation, SC_GetRotation,
    SC_SetSourceSize, SC_GetSourceSize, SC_SetMaximumFrameLatency,
    SC_GetMaximumFrameLatency, SC_GetFrameLatencyWaitableObject,
    SC_SetMatrixTransform, SC_GetMatrixTransform,
    SC_GetCurrentBackBufferIndex, SC_CheckColorSpaceSupport,
    SC_SetColorSpace1, SC_ResizeBuffers1,
    SC_SetHDRMetaData,
};

static void SC_Dtor(DXGIBASE *b)
{
    WrapSC *w=(WrapSC*)b;
    if(w->interop) { D3D12Interop_Destroy(w->interop); w->interop=NULL; }
    if(w->pReal4) { ((IUnknown*)w->pReal4)->lpVtbl->Release((IUnknown*)w->pReal4); w->pReal4=NULL; }
    if(w->pReal3) { ((IUnknown*)w->pReal3)->lpVtbl->Release((IUnknown*)w->pReal3); w->pReal3=NULL; }
    if(w->pReal2) { ((IUnknown*)w->pReal2)->lpVtbl->Release((IUnknown*)w->pReal2); w->pReal2=NULL; }
    if(w->pReal1) { ((IUnknown*)w->pReal1)->lpVtbl->Release((IUnknown*)w->pReal1); w->pReal1=NULL; }
}

static HRESULT WrapSC_Create2(IUnknown *pReal, REFIID riid, void **ppOut, WrapSC **ppWrapOut, D3D12Interop *pInterop)
{
    WrapSC *w; HRESULT hr;
    LOG("WrapSC_Create riid=%s", GuidName(riid));
    if (!ppOut) return E_INVALIDARG;
    *ppOut=NULL;
    /* pReal may be NULL: "ghost" wrapper for D3D12on7 where there is no
     * real IDXGISwapChain1 underneath. */
    w = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*w));
    if (!w) return E_OUTOFMEMORY;
    if (pReal) {
        pReal->lpVtbl->AddRef(pReal);
        Base_Init(&w->base,(void*)g_SC_Vtbl,pReal);
        pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain1,&w->pReal1);
        pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain2,&w->pReal2);
        pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain3,&w->pReal3);
        pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain4,&w->pReal4);
    } else {
        Base_Init(&w->base,(void*)g_SC_Vtbl,NULL);
    }
    /*
     * CRITICAL: w->interop MUST be assigned BEFORE calling SC_QI below.
     * SC_QI checks (w->pReal1 || w->interop) to accept IDXGISwapChain1.
     */
    w->interop = pInterop;
    hr = SC_QI(w,riid,ppOut);
    if(ppWrapOut) *ppWrapOut = SUCCEEDED(hr) ? w : NULL;
    SC_Release(w);
    return hr;
}
static HRESULT WrapSC_Create(IUnknown *pReal, REFIID riid, void **ppOut)
{
    return WrapSC_Create2(pReal,riid,ppOut,NULL,NULL);
}

static HRESULT STDMETHODCALLTYPE SC_QI(WrapSC *w, REFIID riid, void **ppv)
{
    LOG_FRAME("IDXGISwapChain::QI(%s)", GuidName(riid));
    if (!ppv) return E_INVALIDARG; *ppv=NULL;
    
    if (GuidEq(riid,&IID_IUnknown_)      || GuidEq(riid,&IID_IDXGIObject)    ||
        GuidEq(riid,&IID_IDXGIDeviceSubObj)||GuidEq(riid,&IID_IDXGISwapChain))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    if (GuidEq(riid,&IID_IDXGISwapChain1) && (w->pReal1 || w->interop))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    if (GuidEq(riid,&IID_IDXGISwapChain2) && (w->pReal2 || w->interop))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    /* IDXGISwapChain3: always return wrapper, even if pReal3 is NULL */
    if (GuidEq(riid,&IID_IDXGISwapChain3))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    if (GuidEq(riid,&IID_IDXGISwapChain4) && (w->pReal4 || w->interop))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    
    LOG("IDXGISwapChain::QI unknown GUID {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} → E_NOINTERFACE",
        riid->Data1, riid->Data2, riid->Data3,
        riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3],
        riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7]);
    
    return E_NOINTERFACE;
}

/* =========================================================
 * Memory validation (prevents crashes on invalid parameters)
 * ========================================================= */
static BOOL IsReadablePtr(const void *p, SIZE_T size)
{
    if (!p) return FALSE;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
        return FALSE;
    if (mbi.RegionSize < size) return FALSE;
    return TRUE;
}

static BOOL IsWriteablePtr(void *p, SIZE_T size)
{
    if (!p) return FALSE;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return FALSE;
    if (mbi.RegionSize < size) return FALSE;
    return TRUE;
}

static ULONG STDMETHODCALLTYPE SC_AddRef (WrapSC *w){ return Base_AddRef(&w->base); }
static ULONG STDMETHODCALLTYPE SC_Release(WrapSC *w){ return Base_Release(&w->base,SC_Dtor); }
static HRESULT STDMETHODCALLTYPE SC_SetPriv(WrapSC*w,REFGUID g,UINT sz,const void*p)
{ return PrivData_Set(&w->base.pPriv,&w->base.csPriv,g,sz,p); }
static HRESULT STDMETHODCALLTYPE SC_SetPrivUnk(WrapSC*w,REFGUID g,const IUnknown*p)
{ return PrivData_SetUnk(&w->base.pPriv,&w->base.csPriv,g,p); }
static HRESULT STDMETHODCALLTYPE SC_GetPriv(WrapSC*w,REFGUID g,UINT*sz,void*p)
{ return PrivData_Get(&w->base.pPriv,&w->base.csPriv,g,sz,p); }
static HRESULT STDMETHODCALLTYPE SC_GetParent(WrapSC*w,REFIID riid,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,REFIID,void**);
    LOG("IDXGISwapChain::GetParent(%s)", GuidName(riid));
    if (!pp) return E_INVALIDARG; *pp=NULL;
    if (w->interop && !w->base.pReal) {
        LOG("IDXGISwapChain::GetParent: D3D12on7 mode, no pReal -> DXGI_ERROR_UNSUPPORTED");
        return DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT hr = ((PFN)VS(w->base.pReal,0x30))(w->base.pReal,riid,pp);
    LOG_LEAVE("IDXGISwapChain::GetParent",hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE SC_Present(WrapSC*w,UINT SyncInterval,UINT Flags)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT);
    HRESULT hr;
    /* Silence verbose logging after the first Present -- init is done.
     * Guard with a read first: InterlockedExchange issues a full memory
     * barrier (bus-lock on x86) every call; the branch costs nothing.  */
    if (g_logVerbose) InterlockedExchange(&g_logVerbose, 0);
    LOG_FRAME("IDXGISwapChain::Present(sync=%u flags=0x%X)",SyncInterval,Flags);
    if(w->interop) { D3D12Interop_Present(w,SyncInterval,Flags,&hr); return hr; }
    hr=((PFN)VS(w->base.pReal,0x40))(w->base.pReal,SyncInterval,Flags);
    return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetBuffer(WrapSC*w,UINT Buf,REFIID riid,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,REFIID,void**);
    HRESULT hr;
    LOG_FRAME("IDXGISwapChain::GetBuffer(%u,%s)",Buf,GuidNameRaw(riid));
    if(!pp) return E_INVALIDARG; *pp=NULL;

    if(w->interop) {
        hr=D3D12Interop_GetBuffer(w,Buf,riid,pp);
        return hr;
    }

    /* If the GUID is a standard D3D11/DXGI GUID, pass through to pReal */
    if(GuidEq(riid, &IID_ID3D11Texture2D) || GuidEq(riid, &IID_IDXGISurface) ||
       GuidEq(riid, &IID_IDXGIResource) || GuidEq(riid, &IID_ID3D11Resource) ||
       GuidEq(riid, &IID_IDXGISurface1) || GuidEq(riid, &IID_IDXGISurface2)) {
        hr = ((PFN)VS(w->base.pReal,0x48))(w->base.pReal,Buf,riid,pp);
        return hr;
    }

    /* For unknown GUIDs, get texture as IUnknown and return it directly */
    void *pTex = NULL;
    hr = ((PFN)VS(w->base.pReal,0x48))(w->base.pReal, Buf, &IID_IUnknown_, &pTex);
    if(SUCCEEDED(hr) && pTex) {
        *pp = pTex;
        return S_OK;
    }
    
    /* Fallback: delegate directly */
    hr = ((PFN)VS(w->base.pReal,0x48))(w->base.pReal,Buf,riid,pp);
    LOG_LEAVE("IDXGISwapChain::GetBuffer",hr); 
    return hr;
}


static HRESULT STDMETHODCALLTYPE SC_SetFullscreenState(WrapSC*w,BOOL fs,void*pTarget)
{
    LOG("IDXGISwapChain::SetFullscreenState(%d)", fs);
    if (w->interop) {
        // Le mode plein écran n'est pas supporté en D3D12on7
        LOG_LEAVE("IDXGISwapChain::SetFullscreenState[interop]", DXGI_ERROR_UNSUPPORTED);
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!w->base.pReal) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,BOOL,void*);
    HRESULT hr=((PFN)VS(w->base.pReal,0x50))(w->base.pReal,fs,pTarget);
    LOG_LEAVE("IDXGISwapChain::SetFullscreenState",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetFullscreenState(WrapSC*w,BOOL*pFs,void**ppTarget)
{
    LOG("IDXGISwapChain::GetFullscreenState");
    if (w->interop) {
        if (pFs) *pFs = FALSE;
        if (ppTarget) *ppTarget = NULL;
        LOG_LEAVE("IDXGISwapChain::GetFullscreenState[interop]", S_OK);
        return S_OK;
    }
    if (!w->base.pReal) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,BOOL*,void**);
    HRESULT hr=((PFN)VS(w->base.pReal,0x58))(w->base.pReal,pFs,ppTarget);
    LOG_LEAVE("IDXGISwapChain::GetFullscreenState",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetDesc(WrapSC*w,void*pDesc)
{
    LOG_FRAME("IDXGISwapChain::GetDesc");
    if (w->interop) {
        DXGI_SWAP_CHAIN_DESC *desc = (DXGI_SWAP_CHAIN_DESC*)pDesc;
        if (!desc) return E_INVALIDARG;
        ZeroMemory(desc, sizeof(DXGI_SWAP_CHAIN_DESC));
        desc->BufferDesc.Width = w->interop->Width;
        desc->BufferDesc.Height = w->interop->Height;
        desc->BufferDesc.Format = w->interop->Format;
        desc->BufferDesc.RefreshRate.Numerator = 60;
        desc->BufferDesc.RefreshRate.Denominator = 1;
        desc->BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        desc->BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        desc->SampleDesc.Count = 1;
        desc->SampleDesc.Quality = 0;
        desc->BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc->BufferCount = w->interop->BufferCount;
        desc->OutputWindow = w->interop->hwnd;
        desc->Windowed = TRUE;
        desc->SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc->Flags = 0;
        LOG_FRAME("IDXGISwapChain::GetDesc[interop] OK");
        return S_OK;
    }
    if (!w->base.pReal) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    HRESULT hr=((PFN)VS(w->base.pReal,0x60))(w->base.pReal,pDesc);
    LOG_FRAME("IDXGISwapChain::GetDesc done"); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_ResizeBuffers(WrapSC*w,UINT C,UINT W,UINT H,UINT F,UINT Fl)
{
    LOG("IDXGISwapChain::ResizeBuffers(%u,%u,%u,fmt=%u,flags=%u)", C, W, H, F, Fl);
    if (w->interop) {
        D3D12Interop *it = w->interop;
        // Libérer les anciennes ressources
        for (UINT i = 0; i < it->BufferCount; i++) {
            if (it->pD3D12RenderRes[i]) {
                ((IUnknown*)it->pD3D12RenderRes[i])->lpVtbl->Release((IUnknown*)it->pD3D12RenderRes[i]);
                it->pD3D12RenderRes[i] = NULL;
            }
        }
        // Mettre à jour les dimensions
        it->Width = W;
        it->Height = H;
        it->Format = F;
        it->BufferCount = C;
        // Recréer les ressources avec les nouvelles dimensions
        for (UINT i = 0; i < C; i++) {
            typedef HRESULT(STDMETHODCALLTYPE*PFN_CCR)(void*, const D3D12_HEAP_PROPERTIES_MIN*,
                D3D12_HEAP_FLAGS_, const D3D12_RESOURCE_DESC_MIN*, D3D12_RESOURCE_STATES_, const void*, REFIID, void**);
            D3D12_HEAP_PROPERTIES_MIN hp; ZeroMemory(&hp, sizeof(hp));
            hp.Type = D3D12_HEAP_TYPE_DEFAULT_;
            D3D12_RESOURCE_DESC_MIN rd; ZeroMemory(&rd, sizeof(rd));
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D_;
            rd.Width = W; rd.Height = H;
            rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = F; rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN_;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET_;
            D3D12_CLEAR_VALUE_MIN cv; ZeroMemory(&cv, sizeof(cv));
            cv.Format = F;
            void *pRes = NULL;
            void *pFn = VS(it->pD3D12Dev, D3D12DEV_CreateCommittedResource_OFF);
            HRESULT hr = ((PFN_CCR)pFn)(
                it->pD3D12Dev, &hp, D3D12_HEAP_FLAG_NONE_, &rd,
                D3D12_RESOURCE_STATE_COMMON_, &cv, &IID_ID3D12Resource, &pRes);
            if (FAILED(hr) || !pRes) {
                LOG("SC_ResizeBuffers: CreateCommittedResource failed for buffer %u", i);
                // Nettoyer les ressources déjà créées
                for (UINT j = 0; j < i; j++) {
                    if (it->pD3D12RenderRes[j]) {
                        ((IUnknown*)it->pD3D12RenderRes[j])->lpVtbl->Release((IUnknown*)it->pD3D12RenderRes[j]);
                        it->pD3D12RenderRes[j] = NULL;
                    }
                }
                return hr;
            }
            it->pD3D12RenderRes[i] = pRes;
        }
        it->BufferCount = C;
        // Réinitialiser l'index de présentation (optionnel)
        // it->presentIndex = 0;
        LOG("SC_ResizeBuffers[interop]: resized to %ux%u, %u buffers", W, H, C);
        return S_OK;
    }
    // Si pReal existe, déléguer
    if (w->base.pReal) {
        typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,UINT,UINT,UINT);
        HRESULT hr = ((PFN)VS(w->base.pReal,0x68))(w->base.pReal,C,W,H,F,Fl);
        LOG_LEAVE("IDXGISwapChain::ResizeBuffers",hr); return hr;
    }
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE SC_ResizeTarget(WrapSC*w,const void*pDesc)
{
    LOG("IDXGISwapChain::ResizeTarget");
    if (w->interop) {
        // Non supporté en D3D12on7
        LOG_LEAVE("IDXGISwapChain::ResizeTarget[interop]", DXGI_ERROR_UNSUPPORTED);
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!w->base.pReal) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,const void*);
    HRESULT hr=((PFN)VS(w->base.pReal,0x70))(w->base.pReal,pDesc);
    LOG_LEAVE("IDXGISwapChain::ResizeTarget",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetContainingOutput(WrapSC*w,void**pp)
{
    LOG("IDXGISwapChain::GetContainingOutput");
    if (!pp) return E_INVALIDARG;
    *pp = NULL;
    if (w->interop) {
        LOG_LEAVE("IDXGISwapChain::GetContainingOutput[interop]", DXGI_ERROR_UNSUPPORTED);
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!w->base.pReal) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void**);
    HRESULT hr=((PFN)VS(w->base.pReal,0x78))(w->base.pReal,pp);
    LOG_LEAVE("IDXGISwapChain::GetContainingOutput",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetFrameStatistics(WrapSC*w,void*p)
{
    LOG("IDXGISwapChain::GetFrameStatistics");
    if (w->interop) {
        // Stub : retourner S_OK avec des zéros
        if (p) ZeroMemory(p, sizeof(DXGI_FRAME_STATISTICS));
        LOG_LEAVE("IDXGISwapChain::GetFrameStatistics[interop]", S_OK);
        return S_OK;
    }
    if (!w->base.pReal) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    HRESULT hr=((PFN)VS(w->base.pReal,0x80))(w->base.pReal,p);
    LOG_LEAVE("IDXGISwapChain::GetFrameStatistics",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetLastPresentCount(WrapSC*w,UINT*p)
{
    LOG("IDXGISwapChain::GetLastPresentCount");
    if (w->interop) {
        if (p) *p = w->interop->presentIndex;
        LOG_LEAVE("IDXGISwapChain::GetLastPresentCount[interop]", S_OK);
        return S_OK;
    }
    if (!w->base.pReal) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT*);
    return ((PFN)VS(w->base.pReal,0x88))(w->base.pReal,p);
}

#define SC1_CALL_1(off,a) do { \
    if (!w->pReal1) return E_NOTIMPL; \
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*, void*); \
    return ((PFN)VS(w->pReal1,(off)))(w->pReal1, (void*)(a)); } while(0)

static HRESULT STDMETHODCALLTYPE SC_GetDesc1(WrapSC*w,void*p)
{
    LOG_FRAME("IDXGISwapChain1::GetDesc1");
    if (w->interop) {
        DXGI_SWAP_CHAIN_DESC1 *desc = (DXGI_SWAP_CHAIN_DESC1*)p;
        if (!desc) return E_INVALIDARG;
        ZeroMemory(desc, sizeof(DXGI_SWAP_CHAIN_DESC1));
        desc->Width = w->interop->Width;
        desc->Height = w->interop->Height;
        desc->Format = w->interop->Format;
        desc->SampleDesc.Count = 1;
        desc->SampleDesc.Quality = 0;
        desc->BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc->BufferCount = w->interop->BufferCount;
        desc->Flags = 0;
        desc->SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc->AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc->Scaling = DXGI_SCALING_STRETCH;
        desc->Stereo = FALSE;
        LOG_FRAME("IDXGISwapChain1::GetDesc1[interop] OK");
        return S_OK;
    }
    if (!w->pReal1) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*, void*);
    return ((PFN)VS(w->pReal1, 0x90))(w->pReal1, p);
}

static HRESULT STDMETHODCALLTYPE SC_GetFullscreenDesc(WrapSC*w,void*p)  { SC1_CALL_1(0x98,p); }

static HRESULT STDMETHODCALLTYPE SC_GetHwnd(WrapSC*w,HWND*p)
{
    LOG("IDXGISwapChain1::GetHwnd");
    if (!p) return E_INVALIDARG;
    if (w->interop) {
        *p = w->interop->hwnd;
        LOG_LEAVE("IDXGISwapChain1::GetHwnd[interop]", S_OK);
        return S_OK;
    }
    if (!w->pReal1) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HWND*);
    HRESULT hr=((PFN)VS(w->pReal1,0xa0))(w->pReal1,p);
    LOG_LEAVE("IDXGISwapChain1::GetHwnd",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetCoreWindow(WrapSC*w,REFIID r,void**pp)
{
    if(!w->pReal1||!pp) return E_NOTIMPL; *pp=NULL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,REFIID,void**);
    return ((PFN)VS(w->pReal1,0xa8))(w->pReal1,r,pp);
}
static HRESULT STDMETHODCALLTYPE SC_Present1(WrapSC*w,UINT si,UINT fl,const void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,const void*);
    HRESULT hr;
    InterlockedExchange(&g_logVerbose, 0);
    LOG_FRAME("IDXGISwapChain1::Present1(sync=%u,flags=0x%X)",si,fl);
    if(w->interop) { D3D12Interop_Present(w,si,fl,&hr); return hr; }
    if(!w->pReal1) return SC_Present(w,si,fl);
    hr=((PFN)VS(w->pReal1,0xb0))(w->pReal1,si,fl,p);
    return hr;
}
static BOOL    STDMETHODCALLTYPE SC_IsTemporaryMonoSupported(WrapSC*w)
{
    if(!w->pReal1) return FALSE;
    typedef BOOL(STDMETHODCALLTYPE*PFN)(void*);
    return ((PFN)VS(w->pReal1,0xb8))(w->pReal1);
}
static HRESULT STDMETHODCALLTYPE SC_GetRestrictToOutput(WrapSC*w,void**pp)
{
    if(!w->pReal1||!pp) return E_NOTIMPL; *pp=NULL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void**);
    return ((PFN)VS(w->pReal1,0xc0))(w->pReal1,pp);
}
static HRESULT STDMETHODCALLTYPE SC_SetBackgroundColor(WrapSC*w,const void*p) { SC1_CALL_1(0xc8,p); }
static HRESULT STDMETHODCALLTYPE SC_GetBackgroundColor(WrapSC*w,void*p)       { SC1_CALL_1(0xd0,p); }
static HRESULT STDMETHODCALLTYPE SC_SetRotation(WrapSC*w,UINT r)
{
    if(!w->pReal1) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT);
    return ((PFN)VS(w->pReal1,0xd8))(w->pReal1,r);
}
static HRESULT STDMETHODCALLTYPE SC_GetRotation(WrapSC*w,UINT*r)
{
    if(!w->pReal1||!r) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT*);
    return ((PFN)VS(w->pReal1,0xe0))(w->pReal1,r);
}

static HRESULT STDMETHODCALLTYPE SC_SetSourceSize(WrapSC*w,UINT W,UINT H)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT);
    LOG("IDXGISwapChain2::SetSourceSize(%u,%u)",W,H);
    if(!w->pReal2) return DXGI_ERROR_UNSUPPORTED;
    HRESULT hr=((PFN)VS(w->pReal2,0xe8))(w->pReal2,W,H);
    LOG_LEAVE("IDXGISwapChain2::SetSourceSize",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE SC_GetSourceSize(WrapSC*w,UINT*pW,UINT*pH)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT*,UINT*);
    if(!w->pReal2) return DXGI_ERROR_UNSUPPORTED;
    return ((PFN)VS(w->pReal2,0xf0))(w->pReal2,pW,pH);
}
static HRESULT STDMETHODCALLTYPE SC_SetMaximumFrameLatency(WrapSC*w,UINT ml)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT);
    if(!w->pReal2) return DXGI_ERROR_UNSUPPORTED;
    return ((PFN)VS(w->pReal2,0xf8))(w->pReal2,ml);
}
static HRESULT STDMETHODCALLTYPE SC_GetMaximumFrameLatency(WrapSC*w,UINT*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT*);
    if(!w->pReal2||!p) return DXGI_ERROR_UNSUPPORTED;
    return ((PFN)VS(w->pReal2,0x100))(w->pReal2,p);
}
static HANDLE STDMETHODCALLTYPE SC_GetFrameLatencyWaitableObject(WrapSC*w)
{
    typedef HANDLE(STDMETHODCALLTYPE*PFN)(void*);
    LOG("IDXGISwapChain2::GetFrameLatencyWaitableObject");
    if(!w->pReal2) return NULL;
    return ((PFN)VS(w->pReal2,0x108))(w->pReal2);
}
static HRESULT STDMETHODCALLTYPE SC_SetMatrixTransform(WrapSC*w,const void*m)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,const void*);
    if(!w->pReal2) return DXGI_ERROR_UNSUPPORTED;
    return ((PFN)VS(w->pReal2,0x110))(w->pReal2,m);
}
static HRESULT STDMETHODCALLTYPE SC_GetMatrixTransform(WrapSC*w,void*m)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    if(!w->pReal2) return DXGI_ERROR_UNSUPPORTED;
    return ((PFN)VS(w->pReal2,0x118))(w->pReal2,m);
}

static UINT STDMETHODCALLTYPE SC_GetCurrentBackBufferIndex(WrapSC*w)
{
    typedef UINT(STDMETHODCALLTYPE*PFN)(void*);
    LOG_FRAME("IDXGISwapChain3::GetCurrentBackBufferIndex");
    if(w->interop) {
        D3D12Interop *it = w->interop;
        return it->bufIdxMask
               ? (it->presentIndex & it->bufIdxMask)
               : (it->presentIndex % it->BufferCount);
    }
    if(!w->pReal3) return 0;
    return ((PFN)VS(w->pReal3,0x120))(w->pReal3);
}
static HRESULT STDMETHODCALLTYPE SC_CheckColorSpaceSupport(WrapSC*w,UINT cs,UINT*pFlags)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT*);
    LOG("IDXGISwapChain3::CheckColorSpaceSupport(%u)",cs);
    if(!w->pReal3||!pFlags) return E_NOTIMPL;
    return ((PFN)VS(w->pReal3,0x128))(w->pReal3,cs,pFlags);
}
static HRESULT STDMETHODCALLTYPE SC_SetColorSpace1(WrapSC*w,UINT cs)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT);
    LOG("IDXGISwapChain3::SetColorSpace1(%u)",cs);
    if(!w->pReal3) return DXGI_ERROR_UNSUPPORTED;
    return ((PFN)VS(w->pReal3,0x130))(w->pReal3,cs);
}
static HRESULT STDMETHODCALLTYPE SC_ResizeBuffers1(WrapSC*w,UINT bc,UINT W,UINT H,UINT F,UINT Fl,const UINT*pND,IUnknown*const*ppQueues)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,UINT,UINT,UINT,const UINT*,IUnknown*const*);
    LOG("IDXGISwapChain3::ResizeBuffers1(%u,%u,%u)",bc,W,H);
    if(!w->pReal3) return SC_ResizeBuffers(w,bc,W,H,F,Fl);
    return ((PFN)VS(w->pReal3,0x138))(w->pReal3,bc,W,H,F,Fl,pND,ppQueues);
}
static HRESULT STDMETHODCALLTYPE SC_SetHDRMetaData(WrapSC*w,UINT t,UINT sz,void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,void*);
    LOG("IDXGISwapChain4::SetHDRMetaData(type=%u,sz=%u)",t,sz);
    if(!w->pReal4) return DXGI_ERROR_UNSUPPORTED;
    return ((PFN)VS(w->pReal4,0x140))(w->pReal4,t,sz,p);
}

/* =========================================================
 * ======== ADAPTER WRAPPER ========
 * ========================================================= */
typedef struct _WrapAdapter {
    DXGIBASE base;
    void *pReal1, *pReal2, *pReal3, *pReal4;
} WrapAdapter;

static HRESULT STDMETHODCALLTYPE WA_QI(WrapAdapter*,REFIID,void**);
static ULONG   STDMETHODCALLTYPE WA_AddRef(WrapAdapter*);
static ULONG   STDMETHODCALLTYPE WA_Release(WrapAdapter*);

static void WA_Dtor(DXGIBASE *b)
{
    WrapAdapter *w=(WrapAdapter*)b;
    if(w->pReal4){((IUnknown*)w->pReal4)->lpVtbl->Release((IUnknown*)w->pReal4);w->pReal4=NULL;}
    if(w->pReal3){((IUnknown*)w->pReal3)->lpVtbl->Release((IUnknown*)w->pReal3);w->pReal3=NULL;}
    if(w->pReal2){((IUnknown*)w->pReal2)->lpVtbl->Release((IUnknown*)w->pReal2);w->pReal2=NULL;}
    if(w->pReal1){((IUnknown*)w->pReal1)->lpVtbl->Release((IUnknown*)w->pReal1);w->pReal1=NULL;}
}

static HRESULT STDMETHODCALLTYPE WA_SetPriv(WrapAdapter*w,REFGUID g,UINT sz,const void*p)
{ return PrivData_Set(&w->base.pPriv,&w->base.csPriv,g,sz,p); }
static HRESULT STDMETHODCALLTYPE WA_SetPrivUnk(WrapAdapter*w,REFGUID g,const IUnknown*p)
{ return PrivData_SetUnk(&w->base.pPriv,&w->base.csPriv,g,p); }
static HRESULT STDMETHODCALLTYPE WA_GetPriv(WrapAdapter*w,REFGUID g,UINT*sz,void*p)
{ return PrivData_Get(&w->base.pPriv,&w->base.csPriv,g,sz,p); }
static HRESULT STDMETHODCALLTYPE WA_GetParent(WrapAdapter*w,REFIID r,void**pp)
{
    LOG("IDXGIAdapter::GetParent(%s)",GuidName(r));
    if (!pp) return E_INVALIDARG; *pp=NULL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,REFIID,void**);
    void *pRealParent=NULL;
    HRESULT hr=((PFN)VS(w->base.pReal,0x30))(w->base.pReal,r,&pRealParent);
    if(SUCCEEDED(hr) && pRealParent) {
        hr = WrapFactory_Create((IUnknown*)pRealParent,0,r,pp);
        ((IUnknown*)pRealParent)->lpVtbl->Release((IUnknown*)pRealParent);
    }
    LOG_LEAVE("IDXGIAdapter::GetParent",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_EnumOutputs(WrapAdapter*w,UINT i,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,void**);
    LOG("IDXGIAdapter::EnumOutputs(%u)",i);
    if(!pp) return E_INVALIDARG; *pp=NULL;
    HRESULT hr=((PFN)VS(w->base.pReal,0x38))(w->base.pReal,i,pp);
    LOG_LEAVE("IDXGIAdapter::EnumOutputs",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_GetDesc(WrapAdapter*w,void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    LOG_FRAME("IDXGIAdapter::GetDesc");
    HRESULT hr=((PFN)VS(w->base.pReal,0x40))(w->base.pReal,p);
    LOG_FRAME("IDXGIAdapter::GetDesc done"); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_CheckInterfaceSupport(WrapAdapter*w,REFGUID n,void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,REFGUID,void*);
    LOG("IDXGIAdapter::CheckInterfaceSupport(%s)",GuidName(n));
    HRESULT hr=((PFN)VS(w->base.pReal,0x48))(w->base.pReal,n,p);
    LOG_LEAVE("IDXGIAdapter::CheckInterfaceSupport",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_GetDesc1(WrapAdapter*w,void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    LOG_FRAME("IDXGIAdapter1::GetDesc1");
    if(!w->pReal1) return E_NOTIMPL;
    HRESULT hr=((PFN)VS(w->pReal1,0x50))(w->pReal1,p);
    LOG_FRAME("IDXGIAdapter1::GetDesc1 done"); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_GetDesc2(WrapAdapter*w,void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    LOG_FRAME("IDXGIAdapter2::GetDesc2");
    if(!w->pReal2) return DXGI_ERROR_UNSUPPORTED;
    HRESULT hr=((PFN)VS(w->pReal2,0x58))(w->pReal2,p);
    LOG_FRAME("IDXGIAdapter2::GetDesc2 done"); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_RegisterVideoMemoryBudgetChangeNotification(WrapAdapter*w,HANDLE h,DWORD*p)
{
    LOG_FRAME("IDXGIAdapter3::RegisterVideoMemoryBudgetChangeNotification (stub)");
    if(!w->pReal3) {
        /* Win7: no budget change notifications, silently succeed */
        if(p) *p = 0;
        return S_OK;
    }
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HANDLE,DWORD*);
    return ((PFN)VS(w->pReal3,0x60))(w->pReal3,h,p);
}
static void STDMETHODCALLTYPE WA_UnregisterVideoMemoryBudgetChangeNotification(WrapAdapter*w,DWORD c)
{
    LOG_FRAME("IDXGIAdapter3::UnregisterVideoMemoryBudgetChangeNotification (stub)");
    if(!w->pReal3) return; /* Win7: no-op */
    typedef void(STDMETHODCALLTYPE*PFN)(void*,DWORD);
    ((PFN)VS(w->pReal3,0x68))(w->pReal3,c);
}
static HRESULT STDMETHODCALLTYPE WA_QueryVideoMemoryInfo(WrapAdapter*w,UINT node,UINT sg,void*p)
{
    /* DXGI_QUERY_VIDEO_MEMORY_INFO layout:
     *   UINT64 Budget, CurrentUsage, AvailableForReservation, CurrentReservation */
    typedef struct { UINT64 Budget,CurrentUsage,AvailableForReservation,CurrentReservation; } VMINFO;
    LOG_FRAME("IDXGIAdapter3::QueryVideoMemoryInfo(node=%u,seg=%u)",node,sg);
    if(!w->pReal3) {
        /* Win7 fallback: try D3DKMTQueryVideoMemoryInfo first */
        typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,void*);
        HRESULT (*pfn)(void*,UINT,UINT,void*) = NULL;
        HMODULE hGdi = GetModuleHandleA("gdi32.dll");
        if(hGdi) *(FARPROC*)&pfn = GetProcAddress(hGdi,"D3DKMTQueryVideoMemoryInfo");
        if(pfn) return pfn(w->base.pReal,node,sg,p);
        /* Last resort: return synthetic values so UE4 doesn't crash.
         * Report 2GB budget for local (sg=0) and 512MB for non-local (sg=1). */
        if(p) {
            VMINFO *info = (VMINFO*)p;
            info->Budget                = (sg == 0) ? (UINT64)2048*1024*1024 : (UINT64)512*1024*1024;
            info->CurrentUsage          = 0;
            info->AvailableForReservation = info->Budget;
            info->CurrentReservation    = 0;
        }
        return S_OK;
    }
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,void*);
    return ((PFN)VS(w->pReal3,0x70))(w->pReal3,node,sg,p);
}
static HRESULT STDMETHODCALLTYPE WA_SetVideoMemoryReservation(WrapAdapter*w,UINT node,UINT sg,UINT64 r)
{
    LOG_FRAME("IDXGIAdapter3::SetVideoMemoryReservation(node=%u,seg=%u,res=%llu)",node,sg,(unsigned long long)r);
    if(!w->pReal3) return S_OK; /* Win7: no-op, silently succeed */
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,UINT64);
    return ((PFN)VS(w->pReal3,0x78))(w->pReal3,node,sg,r);
}
static HRESULT STDMETHODCALLTYPE WA_GetDesc3(WrapAdapter*w,void*p)
{
    LOG_FRAME("IDXGIAdapter4::GetDesc3");
    if(!w->pReal4) {
        /* Win7: synthesize GetDesc3 from GetDesc1 */
        /* DXGI_ADAPTER_DESC3 starts with same fields as DXGI_ADAPTER_DESC1
         * (Description[128], VendorId, DeviceId, SubSysId, Revision,
         *  DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory, AdapterLuid)
         * followed by Flags (DXGI_ADAPTER_FLAG3) and GraphicsPreemptionGranularity/
         * ComputePreemptionGranularity — zero those out. */
        if(p && w->pReal1) {
            typedef HRESULT(STDMETHODCALLTYPE*PFN1)(void*,void*);
            /* GetDesc1 vtable offset = 0x50 on IDXGIAdapter1 */
            memset(p, 0, 128*2+6*8); /* zero the whole desc3 */
            ((PFN1)VS(w->pReal1,0x50))(w->pReal1,p);
            return S_OK;
        }
        return DXGI_ERROR_UNSUPPORTED;
    }
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    return ((PFN)VS(w->pReal4,0x80))(w->pReal4,p);
}

static const void* g_WA_Vtbl[] = {
    WA_QI, WA_AddRef, WA_Release,
    WA_SetPriv, WA_SetPrivUnk, WA_GetPriv, WA_GetParent,
    WA_EnumOutputs, WA_GetDesc, WA_CheckInterfaceSupport,
    WA_GetDesc1,
    WA_GetDesc2,
    WA_RegisterVideoMemoryBudgetChangeNotification,
    WA_UnregisterVideoMemoryBudgetChangeNotification,
    WA_QueryVideoMemoryInfo,
    WA_SetVideoMemoryReservation,
    WA_GetDesc3,
};

static HRESULT STDMETHODCALLTYPE WA_QI(WrapAdapter *w, REFIID riid, void **ppv)
{
    LOG("IDXGIAdapter::QI(%s)",GuidName(riid));
    if(!ppv) return E_INVALIDARG; *ppv=NULL;
    if(GuidEq(riid,&IID_IUnknown_)||GuidEq(riid,&IID_IDXGIObject)||GuidEq(riid,&IID_IDXGIAdapter))
        { *ppv=w; WA_AddRef(w); return S_OK; }
    /* IDXGIAdapter1/2/3/4: always claim support — the wrapper vtable implements
     * all methods (with stubs for the ones Win7 drivers don't expose).
     * Do NOT gate on w->pReal1/2/3/4 being non-NULL: on Win7 those QIs fail
     * because the system adapter only supports IDXGIAdapter1, but UE4 and
     * other D3D12 apps do a hard QueryInterface for IDXGIAdapter3 and treat
     * E_NOINTERFACE as a fatal error. */
    if(GuidEq(riid,&IID_IDXGIAdapter1)){ *ppv=w; WA_AddRef(w); return S_OK; }
    if(GuidEq(riid,&IID_IDXGIAdapter2)){ *ppv=w; WA_AddRef(w); return S_OK; }
    if(GuidEq(riid,&IID_IDXGIAdapter3)){ *ppv=w; WA_AddRef(w); return S_OK; }
    if(GuidEq(riid,&IID_IDXGIAdapter4)){ *ppv=w; WA_AddRef(w); return S_OK; }
    LOG("IDXGIAdapter::QI unknown → passthrough");
    return w->base.pReal->lpVtbl->QueryInterface(w->base.pReal, riid, ppv);
}
static ULONG STDMETHODCALLTYPE WA_AddRef (WrapAdapter*w){ return Base_AddRef(&w->base); }
static ULONG STDMETHODCALLTYPE WA_Release(WrapAdapter*w){ return Base_Release(&w->base,WA_Dtor); }

static HRESULT WrapAdapter_Create(IUnknown *pReal, REFIID riid, void **ppOut)
{
    WrapAdapter *w; HRESULT hr;
    LOG("WrapAdapter_Create riid=%s",GuidName(riid));
    if(!ppOut) return E_INVALIDARG; *ppOut=NULL;
    if(!pReal) return E_INVALIDARG;
    w = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*w));
    if(!w) return E_OUTOFMEMORY;
    pReal->lpVtbl->AddRef(pReal);
    Base_Init(&w->base,(void*)g_WA_Vtbl,pReal);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGIAdapter1,&w->pReal1);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGIAdapter2,&w->pReal2);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGIAdapter3,&w->pReal3);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGIAdapter4,&w->pReal4);
    hr = WA_QI(w,riid,ppOut);
    WA_Release(w);
    return hr;
}

/* =========================================================
 * ======== FACTORY WRAPPER ========
 * ========================================================= */
typedef struct _WrapFactory {
    DXGIBASE base;
    void *pRealF;
    void *pRealF1;
    void *pRealF2;
    UINT  uFlags;
    DWORD dwAdapChgCookie;
} WrapFactory;

static HRESULT STDMETHODCALLTYPE WF_QI      (WrapFactory*,REFIID,void**);
static ULONG   STDMETHODCALLTYPE WF_AddRef  (WrapFactory*);
static ULONG   STDMETHODCALLTYPE WF_Release (WrapFactory*);
static HRESULT STDMETHODCALLTYPE WF_SetPriv (WrapFactory*,REFGUID,UINT,const void*);
static HRESULT STDMETHODCALLTYPE WF_SetPrivUnk(WrapFactory*,REFGUID,const IUnknown*);
static HRESULT STDMETHODCALLTYPE WF_GetPriv (WrapFactory*,REFGUID,UINT*,void*);
static HRESULT STDMETHODCALLTYPE WF_GetParent(WrapFactory*,REFIID,void**);
static HRESULT STDMETHODCALLTYPE WF_EnumAdapters(WrapFactory*,UINT,void**);
static HRESULT STDMETHODCALLTYPE WF_MakeWindowAssociation(WrapFactory*,HWND,UINT);
static HRESULT STDMETHODCALLTYPE WF_GetWindowAssociation(WrapFactory*,HWND*);
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChain(WrapFactory*,IUnknown*,void*,void**);
static HRESULT STDMETHODCALLTYPE WF_CreateSoftwareAdapter(WrapFactory*,HMODULE,void**);
static HRESULT STDMETHODCALLTYPE WF_EnumAdapters1(WrapFactory*,UINT,void**);
static BOOL    STDMETHODCALLTYPE WF_IsCurrent(WrapFactory*);
static BOOL    STDMETHODCALLTYPE WF_IsWindowedStereoEnabled(WrapFactory*);
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChainForHwnd(WrapFactory*,IUnknown*,HWND,const void*,const void*,void*,void**);
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChainForCoreWindow(WrapFactory*,IUnknown*,IUnknown*,const void*,void*,void**);
static HRESULT STDMETHODCALLTYPE WF_GetSharedResourceAdapterLuid(WrapFactory*,HANDLE,LUID*);
static HRESULT STDMETHODCALLTYPE WF_RegisterStereoStatusWindow(WrapFactory*,HWND,UINT,DWORD*);
static HRESULT STDMETHODCALLTYPE WF_RegisterStereoStatusEvent(WrapFactory*,HANDLE,DWORD*);
static void    STDMETHODCALLTYPE WF_UnregisterStereoStatus(WrapFactory*,DWORD);
static HRESULT STDMETHODCALLTYPE WF_RegisterOcclusionStatusWindow(WrapFactory*,HWND,UINT,DWORD*);
static HRESULT STDMETHODCALLTYPE WF_RegisterOcclusionStatusEvent(WrapFactory*,HANDLE,DWORD*);
static void    STDMETHODCALLTYPE WF_UnregisterOcclusionStatus(WrapFactory*,DWORD);
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChainForComposition(WrapFactory*,IUnknown*,const void*,void*,void**);
static UINT    STDMETHODCALLTYPE WF_GetCreationFlags(WrapFactory*);
static HRESULT STDMETHODCALLTYPE WF_EnumAdapterByLuid(WrapFactory*,LUID,REFIID,void**);
static HRESULT STDMETHODCALLTYPE WF_EnumWarpAdapter(WrapFactory*,REFIID,void**);
static HRESULT STDMETHODCALLTYPE WF_CheckFeatureSupport(WrapFactory*,UINT,void*,UINT);
static HRESULT STDMETHODCALLTYPE WF_EnumAdapterByGpuPreference(WrapFactory*,UINT,UINT,REFIID,void**);
static HRESULT STDMETHODCALLTYPE WF_RegisterAdaptersChangedEvent(WrapFactory*,HANDLE,DWORD*);
static HRESULT STDMETHODCALLTYPE WF_UnregisterAdaptersChangedEvent(WrapFactory*,DWORD);

static const void* g_WF_Vtbl[] = {
    WF_QI, WF_AddRef, WF_Release,
    WF_SetPriv, WF_SetPrivUnk, WF_GetPriv, WF_GetParent,
    WF_EnumAdapters, WF_MakeWindowAssociation, WF_GetWindowAssociation,
    WF_CreateSwapChain, WF_CreateSoftwareAdapter,
    WF_EnumAdapters1, WF_IsCurrent,
    WF_IsWindowedStereoEnabled,
    WF_CreateSwapChainForHwnd, WF_CreateSwapChainForCoreWindow,
    WF_GetSharedResourceAdapterLuid,
    WF_RegisterStereoStatusWindow, WF_RegisterStereoStatusEvent, WF_UnregisterStereoStatus,
    WF_RegisterOcclusionStatusWindow, WF_RegisterOcclusionStatusEvent, WF_UnregisterOcclusionStatus,
    WF_CreateSwapChainForComposition,
    WF_GetCreationFlags,
    WF_EnumAdapterByLuid, WF_EnumWarpAdapter,
    WF_CheckFeatureSupport,
    WF_EnumAdapterByGpuPreference,
    WF_RegisterAdaptersChangedEvent,
    WF_UnregisterAdaptersChangedEvent,
};

static void WF_Dtor(DXGIBASE *b)
{
    WrapFactory *w=(WrapFactory*)b;
    if(w->pRealF2){((IUnknown*)w->pRealF2)->lpVtbl->Release((IUnknown*)w->pRealF2);w->pRealF2=NULL;}
    if(w->pRealF1){((IUnknown*)w->pRealF1)->lpVtbl->Release((IUnknown*)w->pRealF1);w->pRealF1=NULL;}
    InterlockedCompareExchangePointer(&g_pActiveFactory, NULL, w);
}

static HRESULT WrapFactory_Create(IUnknown *pReal, UINT uFlags, REFIID riid, void **ppOut)
{
    WrapFactory *w; HRESULT hr;
    LOG("WrapFactory_Create flags=0x%X pReal=%p", uFlags, (void*)pReal);
    LOG_GUID("WrapFactory_Create",riid);
    if(!ppOut) return E_INVALIDARG; *ppOut=NULL;
    if(!pReal) return E_INVALIDARG;

    w = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*w));
    if(!w) return E_OUTOFMEMORY;

    void *pF=NULL, *pF1=NULL, *pF2=NULL;
    HRESULT hrF =pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGIFactory, &pF);
    HRESULT hrF1=pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGIFactory1,&pF1);
    HRESULT hrF2=pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGIFactory2,&pF2);
    LOG("WrapFactory_Create: QI(IDXGIFactory)=0x%08X pF=%p | QI(IDXGIFactory1)=0x%08X pF1=%p | QI(IDXGIFactory2)=0x%08X pF2=%p",
        (unsigned)hrF,pF,(unsigned)hrF1,pF1,(unsigned)hrF2,pF2);

    if(!pF) {
        LOG("WrapFactory_Create: pF==NULL -> E_NOINTERFACE");
        if(pF1) ((IUnknown*)pF1)->lpVtbl->Release((IUnknown*)pF1);
        if(pF2) ((IUnknown*)pF2)->lpVtbl->Release((IUnknown*)pF2);
        HeapFree(GetProcessHeap(),0,w);
        return E_NOINTERFACE;
    }

    Base_Init(&w->base,(void*)g_WF_Vtbl,(IUnknown*)pF);
    w->pRealF  = pF;
    w->pRealF1 = pF1;
    w->pRealF2 = pF2;
    w->uFlags  = uFlags;

    InterlockedExchangePointer(&g_pActiveFactory, w);

    LOG("WrapFactory_Create: --> WF_QI");
    hr = WF_QI(w, riid, ppOut);
    LOG("WrapFactory_Create: <-- WF_QI = 0x%08X *ppOut=%p",(unsigned)hr,*ppOut);
    WF_Release(w);
    LOG_LEAVE("WrapFactory_Create", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE WF_QI(WrapFactory *w, REFIID riid, void **ppv)
{
    LOG_GUID("IDXGIFactory::QI",riid);
    if(!ppv) return E_INVALIDARG; *ppv=NULL;
    BOOL ok =
        GuidEq(riid,&IID_IUnknown_)    || GuidEq(riid,&IID_IDXGIObject)   ||
        GuidEq(riid,&IID_IDXGIFactory) || GuidEq(riid,&IID_IDXGIFactory1) ||
        GuidEq(riid,&IID_IDXGIFactory2)|| GuidEq(riid,&IID_IDXGIFactory3) ||
        GuidEq(riid,&IID_IDXGIFactory4)|| GuidEq(riid,&IID_IDXGIFactory5) ||
        GuidEq(riid,&IID_IDXGIFactory6)|| GuidEq(riid,&IID_IDXGIFactory7);
    if(ok){ *ppv=w; WF_AddRef(w); return S_OK; }
    LOG_GUID("IDXGIFactory::QI unknown -> passthrough to pReal",riid);
    HRESULT hrp = w->base.pReal->lpVtbl->QueryInterface(w->base.pReal, riid, ppv);
    LOG("IDXGIFactory::QI passthrough = 0x%08X *ppv=%p",(unsigned)hrp,*ppv);
    return hrp;
}
static ULONG STDMETHODCALLTYPE WF_AddRef (WrapFactory*w){ return Base_AddRef(&w->base); }
static ULONG STDMETHODCALLTYPE WF_Release(WrapFactory*w){ return Base_Release(&w->base,WF_Dtor); }

static HRESULT STDMETHODCALLTYPE WF_SetPriv(WrapFactory*w,REFGUID g,UINT sz,const void*p)
{ return PrivData_Set(&w->base.pPriv,&w->base.csPriv,g,sz,p); }
static HRESULT STDMETHODCALLTYPE WF_SetPrivUnk(WrapFactory*w,REFGUID g,const IUnknown*p)
{ return PrivData_SetUnk(&w->base.pPriv,&w->base.csPriv,g,p); }
static HRESULT STDMETHODCALLTYPE WF_GetPriv(WrapFactory*w,REFGUID g,UINT*sz,void*p)
{ return PrivData_Get(&w->base.pPriv,&w->base.csPriv,g,sz,p); }
static HRESULT STDMETHODCALLTYPE WF_GetParent(WrapFactory*w,REFIID r,void**pp)
{
    (void)w; (void)r;
    LOG("IDXGIFactory::GetParent → E_NOINTERFACE");
    if(pp) *pp=NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE WF_EnumAdapters(WrapFactory*w,UINT i,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,void**);
    LOG("IDXGIFactory::EnumAdapters(%u)",i);
    if(!pp) return E_INVALIDARG; *pp=NULL;
    void *pA=NULL;
    HRESULT hr=((PFN)VS(w->pRealF,0x38))(w->pRealF,i,&pA);
    if(SUCCEEDED(hr)) hr=WrapAdapter_Create((IUnknown*)pA,&IID_IDXGIAdapter,pp);
    if(pA) { ((IUnknown*)pA)->lpVtbl->Release((IUnknown*)pA); pA=NULL; }
    LOG_LEAVE("IDXGIFactory::EnumAdapters",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WF_MakeWindowAssociation(WrapFactory*w,HWND h,UINT f)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HWND,UINT);
    LOG("IDXGIFactory::MakeWindowAssociation(hwnd=%p,flags=0x%X)",(void*)h,f);
    HRESULT hr=((PFN)VS(w->pRealF,0x40))(w->pRealF,h,f);
    LOG_LEAVE("IDXGIFactory::MakeWindowAssociation",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WF_GetWindowAssociation(WrapFactory*w,HWND*ph)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HWND*);
    LOG("IDXGIFactory::GetWindowAssociation");
    if(!ph) return E_INVALIDARG;
    HRESULT hr=((PFN)VS(w->pRealF,0x48))(w->pRealF,ph);
    LOG_LEAVE("IDXGIFactory::GetWindowAssociation",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChain(WrapFactory*w, IUnknown*pDev, void*pDesc, void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,IUnknown*,void*,void**);
    LOG("IDXGIFactory::CreateSwapChain");
    if (!pp) return E_INVALIDARG; *pp = NULL;
    if (!pDev || !pDesc) return E_INVALIDARG;

    // Check if pDev is an ID3D12CommandQueue (or ID3D12Device)
    void *pCQ = NULL;
    HRESULT hrQI = pDev->lpVtbl->QueryInterface(pDev, &IID_ID3D12CommandQueue, &pCQ);
    if (SUCCEEDED(hrQI) && pCQ) {
        // This is a D3D12 device → use our interop
        LOG("  IDXGIFactory::CreateSwapChain: D3D12 device detected -> D3D12on7");

        DXGI_SWAP_CHAIN_DESC *pDesc1 = (DXGI_SWAP_CHAIN_DESC*)pDesc;
        HWND hwnd = pDesc1->OutputWindow;

        // Convert DXGI_SWAP_CHAIN_DESC → DXGI_SWAP_CHAIN_DESC1
        DXGI_SWAP_CHAIN_DESC1 desc1 = {0};
        desc1.Width = pDesc1->BufferDesc.Width;
        desc1.Height = pDesc1->BufferDesc.Height;
        desc1.Format = pDesc1->BufferDesc.Format;
        desc1.Stereo = FALSE;
        desc1.SampleDesc = pDesc1->SampleDesc;
        desc1.BufferUsage = pDesc1->BufferUsage;
        desc1.BufferCount = pDesc1->BufferCount;
        desc1.Scaling = DXGI_SCALING_STRETCH;
        desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc1.Flags = pDesc1->Flags;

        // Create D3D12 interop
        IUnknown *pRealSC = NULL;
        D3D12Interop *it = D3D12Interop_Create((IUnknown*)pCQ, hwnd, &desc1, &pRealSC);
        ((IUnknown*)pCQ)->lpVtbl->Release((IUnknown*)pCQ);
        if (!it) {
            LOG("  IDXGIFactory::CreateSwapChain: D3D12Interop_Create failed");
            return E_FAIL;
        }

        // Create swapchain wrapper (without pReal)
        WrapSC *wsc = NULL;
        HRESULT hr2 = WrapSC_Create2(NULL, &IID_IDXGISwapChain1, pp, &wsc, it);
        if (FAILED(hr2) || !wsc) {
            LOG("  IDXGIFactory::CreateSwapChain: WrapSC_Create2 = 0x%08X", (unsigned)hr2);
            D3D12Interop_Destroy(it);
            return FAILED(hr2) ? hr2 : E_FAIL;
        }
        LOG_LEAVE("IDXGIFactory::CreateSwapChain", S_OK);
        return S_OK;
    }

    // Not a D3D12 device → delegate to real factory
    if (pCQ) ((IUnknown*)pCQ)->lpVtbl->Release((IUnknown*)pCQ);

    void *pSC = NULL;
    HRESULT hr = ((PFN)VS(w->pRealF, 0x50))(w->pRealF, pDev, pDesc, &pSC);
    if (SUCCEEDED(hr)) hr = WrapSC_Create((IUnknown*)pSC, &IID_IDXGISwapChain, pp);
    if (pSC) { ((IUnknown*)pSC)->lpVtbl->Release((IUnknown*)pSC); pSC = NULL; }
    LOG_LEAVE("IDXGIFactory::CreateSwapChain", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE WF_CreateSoftwareAdapter(WrapFactory*w,HMODULE hm,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HMODULE,void**);
    LOG("IDXGIFactory::CreateSoftwareAdapter");
    if(!pp) return E_INVALIDARG; *pp=NULL;
    void *pA=NULL;
    HRESULT hr=((PFN)VS(w->pRealF,0x58))(w->pRealF,hm,&pA);
    if(SUCCEEDED(hr)) hr=WrapAdapter_Create((IUnknown*)pA,&IID_IDXGIAdapter,pp);
    if(pA) { ((IUnknown*)pA)->lpVtbl->Release((IUnknown*)pA); pA=NULL; }
    LOG_LEAVE("IDXGIFactory::CreateSoftwareAdapter",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WF_EnumAdapters1(WrapFactory*w,UINT i,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,void**);
    LOG("IDXGIFactory1::EnumAdapters1(%u)",i);
    if(!pp) return E_INVALIDARG; *pp=NULL;
    void *pSrc = w->pRealF1 ? w->pRealF1 : w->pRealF;
    UINT off   = w->pRealF1 ? 0x60 : 0x38;
    void *pA=NULL;
    HRESULT hr=((PFN)VS(pSrc,off))(pSrc,i,&pA);
    if(SUCCEEDED(hr)) hr=WrapAdapter_Create((IUnknown*)pA,&IID_IDXGIAdapter1,pp);
    if(pA) { ((IUnknown*)pA)->lpVtbl->Release((IUnknown*)pA); pA=NULL; }
    LOG_LEAVE("IDXGIFactory1::EnumAdapters1",hr); return hr;
}
static BOOL STDMETHODCALLTYPE WF_IsCurrent(WrapFactory*w)
{
    typedef BOOL(STDMETHODCALLTYPE*PFN)(void*);
    BOOL b = w->pRealF1 ? ((PFN)VS(w->pRealF1,0x68))(w->pRealF1) : TRUE;
    LOG("IDXGIFactory1::IsCurrent → %d",b); return b;
}

#define F2_GUARD(ret) do{ if(!w->pRealF2){ LOG("IDXGIFactory2 not available (Win7)"); return ret; } }while(0)

static BOOL STDMETHODCALLTYPE WF_IsWindowedStereoEnabled(WrapFactory*w)
{
    typedef BOOL(STDMETHODCALLTYPE*PFN)(void*);
    LOG("IDXGIFactory2::IsWindowedStereoEnabled");
    F2_GUARD(FALSE);
    return ((PFN)VS(w->pRealF2,0x70))(w->pRealF2);
}

/* =========================================================
 * ======== INTEROP D3D12 -> D3D11 ========
 * ========================================================= */

static IUnknown* D3D12_GetDeviceFromQueue(IUnknown *pCQ)
{
    typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDevice)(void*, REFIID, void**);
    void *pDev=NULL;
    if(!pCQ) return NULL;
    HRESULT hr=((PFN_GetDevice)VS(pCQ,D3D12CQ_GetDevice_OFF))(pCQ,&IID_ID3D12Device,&pDev);
    LOG("D3D12_GetDeviceFromQueue: GetDevice(IID_ID3D12Device) = 0x%08X pDev=%p",(unsigned)hr,pDev);
    if(FAILED(hr)) return NULL;
    return (IUnknown*)pDev;
}

static HRESULT D3D12_CreateSharedRT(
    void *pD3D11Dev, IUnknown *pD3D12Dev,
    UINT W, UINT H, DXGI_FORMAT fmt,
    void **ppD3D11Tex, void **ppD3D12Res)
{
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture2D)(
        void*, const D3D11_TEXTURE2D_DESC_MIN*, const void*, void**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_GetSharedHandle)(void*, HANDLE*);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_OpenSharedHandle)(void*, HANDLE, REFIID, void**);

    HRESULT hr;
    void   *pTex  = NULL;
    HANDLE  hWin32 = NULL;
    void   *pD3D12Res = NULL;

    LOG("D3D12_CreateSharedRT: %ux%u fmt=%u", W, H, (unsigned)fmt);

    {
        D3D11_TEXTURE2D_DESC_MIN desc; ZeroMemory(&desc,sizeof(desc));
        desc.Width          = W;
        desc.Height         = H;
        desc.MipLevels      = 1;
        desc.ArraySize      = 1;
        desc.Format         = fmt;
        desc.SampleDesc.Count   = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags      = D3D11_RESOURCE_MISC_SHARED;

        hr = ((PFN_CreateTexture2D)VS(pD3D11Dev, 0x28))(pD3D11Dev, &desc, NULL, &pTex);
        LOG("D3D12_CreateSharedRT: CreateTexture2D = 0x%08X pTex=%p", (unsigned)hr, pTex);
        if (FAILED(hr) || !pTex) return hr;
    }

    {
        void *pDXGIRes = NULL;
        hr = ((IUnknown*)pTex)->lpVtbl->QueryInterface((IUnknown*)pTex,
             &IID_IDXGIResource, &pDXGIRes);
        LOG("D3D12_CreateSharedRT: QI(IDXGIResource) = 0x%08X pDXGIRes=%p",
            (unsigned)hr, pDXGIRes);
        if (FAILED(hr) || !pDXGIRes) {
            ((IUnknown*)pTex)->lpVtbl->Release((IUnknown*)pTex);
            return hr;
        }

        hr = ((PFN_GetSharedHandle)VS(pDXGIRes, 0x40))(pDXGIRes, &hWin32);
        ((IUnknown*)pDXGIRes)->lpVtbl->Release((IUnknown*)pDXGIRes);
        LOG("D3D12_CreateSharedRT: GetSharedHandle = 0x%08X hWin32=%p",
            (unsigned)hr, (void*)hWin32);
        if (FAILED(hr) || !hWin32) {
            ((IUnknown*)pTex)->lpVtbl->Release((IUnknown*)pTex);
            return (FAILED(hr)) ? hr : E_FAIL;
        }
    }

    hr = ((PFN_OpenSharedHandle)VS(pD3D12Dev, D3D12DEV_OpenSharedHandle_OFF))(
        pD3D12Dev, hWin32, &IID_ID3D12Resource, &pD3D12Res);
    LOG("D3D12_CreateSharedRT: OpenSharedHandle(D3D12) = 0x%08X pD3D12Res=%p",
        (unsigned)hr, pD3D12Res);

    if (FAILED(hr) || !pD3D12Res) {
        LOG("D3D12_CreateSharedRT: retry OpenSharedHandle with IID_IUnknown_...");
        hr = ((PFN_OpenSharedHandle)VS(pD3D12Dev, D3D12DEV_OpenSharedHandle_OFF))(
            pD3D12Dev, hWin32, &IID_IUnknown_, &pD3D12Res);
        LOG("D3D12_CreateSharedRT: retry OpenSharedHandle = 0x%08X pD3D12Res=%p",
            (unsigned)hr, pD3D12Res);
    }

    if (FAILED(hr) || !pD3D12Res) {
        ((IUnknown*)pTex)->lpVtbl->Release((IUnknown*)pTex);
        D3D12_DumpInfoQueue(pD3D12Dev);
        return FAILED(hr) ? hr : E_FAIL;
    }

    LOG("D3D12_CreateSharedRT: OK pD3D11Tex=%p pD3D12Res=%p", pTex, pD3D12Res);

    *ppD3D11Tex  = pTex;
    *ppD3D12Res  = pD3D12Res;
    return S_OK;
}

/* =========================================================
 * FPS OVERLAY — GDI double-buffered implementation
 *
 * Ov_Init  : reads g_ovEnabled (set from dxgw.ini), initialises
 *             QPC counters and marks overlay ready.
 *
 * Ov_Render: called once per Present.  Measures elapsed time via
 *             QueryPerformanceCounter, maintains an 8-sample rolling
 *             FPS average (responsive even at low FPS), then paints
 *             the counter via double-buffered GDI:
 *               1. Draw into an off-screen MemDC (no flicker)
 *               2. BitBlt the result onto the window DC in one shot
 *
 * Config    : dxgw.ini (auto-created next to dxgi.dll on first run):
 *               [dxgi_win7]
 *               FPS_Overlay=0    ; set to 1 to enable
 * ========================================================= */

#define OV_SAMPLES 8   /* ring buffer size -- keep in sync with ovFpsSamples[8] */
#define OV_X 8
#define OV_Y 8
#define OV_W 112
#define OV_H 20

static void Ov_Destroy(D3D12Interop *it);   /* forward decl for Ov_Init error path */

static void Ov_Init(D3D12Interop *it)
{
    HDC hdc;

    if (!it) return;

    if (!g_ovEnabled) {
        it->ovReady = FALSE;
        LOG("Ov_Init: FPS overlay disabled via config");
        return;
    }

    /* QPC */
    if (!QueryPerformanceFrequency(&it->ovQpcFreq) || it->ovQpcFreq.QuadPart == 0) {
        LOG("Ov_Init: QueryPerformanceFrequency failed -- overlay disabled");
        it->ovReady = FALSE;
        return;
    }
    QueryPerformanceCounter(&it->ovQpcLast);
    memset(it->ovFpsSamples, 0, sizeof(it->ovFpsSamples));
    it->ovFpsSampleIdx = 0;
    it->ovFpsCurrent   = 0;

    /* Pre-create GDI resources so Ov_Render has zero allocations per frame */
    hdc = GetDC(it->hwnd);
    if (!hdc) { it->ovReady = FALSE; return; }

    it->ovMemDC = CreateCompatibleDC(hdc);
    if (!it->ovMemDC) { ReleaseDC(it->hwnd, hdc); it->ovReady = FALSE; return; }

    it->ovBmp = CreateCompatibleBitmap(hdc, OV_W, OV_H);
    ReleaseDC(it->hwnd, hdc);
    if (!it->ovBmp) { it->ovReady = FALSE; return; }

    it->ovOldBmp = (HBITMAP)SelectObject(it->ovMemDC, it->ovBmp);

    it->ovFont = CreateFontA(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY, FIXED_PITCH | FF_DONTCARE, "Courier New");
    if (it->ovFont)
        it->ovOldFont = (HFONT)SelectObject(it->ovMemDC, it->ovFont);

    SetBkMode(it->ovMemDC, OPAQUE);
    SetBkColor(it->ovMemDC, RGB(0, 0, 0));
    SetTextColor(it->ovMemDC, RGB(0, 255, 0));

    it->ovReady = TRUE;
    LOG("Ov_Init: FPS overlay ready (GDI cached, %u-sample average)", OV_SAMPLES);
}

/* Called from D3D12Interop_Destroy to free the pre-created GDI objects */
static void Ov_Destroy(D3D12Interop *it)
{
    if (!it) return;
    if (it->ovMemDC) {
        if (it->ovOldFont) SelectObject(it->ovMemDC, it->ovOldFont);
        if (it->ovOldBmp)  SelectObject(it->ovMemDC, it->ovOldBmp);
        DeleteDC(it->ovMemDC);
        it->ovMemDC = NULL;
    }
    if (it->ovFont) { DeleteObject(it->ovFont); it->ovFont = NULL; }
    if (it->ovBmp)  { DeleteObject(it->ovBmp);  it->ovBmp  = NULL; }
}

static void Ov_Render(D3D12Interop *it, void *pRenderRes)
{
    LARGE_INTEGER now;
    LONGLONG      elapsed;
    UINT          instantFps, i, sum, filled;
    char          buf[32];
    HDC           hdc;
    RECT          rc;

    (void)pRenderRes; /* reserved for future D3D12 native path */

    if (!it || !it->ovReady || !it->hwnd || !it->ovMemDC) return;

    /* ---- FPS measurement ---- */
    QueryPerformanceCounter(&now);
    elapsed = now.QuadPart - it->ovQpcLast.QuadPart;
    it->ovQpcLast = now;

    if (elapsed > 0 && it->ovQpcFreq.QuadPart > 0) {
        LONGLONG fps = it->ovQpcFreq.QuadPart / elapsed;
        instantFps = (fps < 1) ? 1 : (fps > 9999) ? 9999 : (UINT)fps;
    } else {
        instantFps = 0;
    }

    it->ovFpsSamples[it->ovFpsSampleIdx % OV_SAMPLES] = instantFps;
    it->ovFpsSampleIdx++;
    filled = (it->ovFpsSampleIdx < OV_SAMPLES) ? it->ovFpsSampleIdx : OV_SAMPLES;
    sum = 0;
    for (i = 0; i < filled; i++) sum += it->ovFpsSamples[i];
    it->ovFpsCurrent = (filled > 0) ? (sum / filled) : 0;

    _snprintf(buf, sizeof(buf), "FPS: %u", it->ovFpsCurrent);
    buf[sizeof(buf) - 1] = '\0';

    /* ---- Draw into cached MemDC (no allocs), blit atomically ---- */
    rc.left = 0; rc.top = 0; rc.right = OV_W; rc.bottom = OV_H;
    DrawTextA(it->ovMemDC, buf, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    hdc = GetDC(it->hwnd);
    if (hdc) {
        BitBlt(hdc, OV_X, OV_Y, OV_W, OV_H, it->ovMemDC, 0, 0, SRCCOPY);
        ReleaseDC(it->hwnd, hdc);
    }
}

static D3D12Interop* D3D12Interop_Create(IUnknown *pD3D12CQ, HWND hwnd,
    const void *pDescVoid, IUnknown **ppRealSC)
{
    const DXGI_SWAP_CHAIN_DESC1 *pDesc = (const DXGI_SWAP_CHAIN_DESC1*)pDescVoid;
    D3D12Interop *it = NULL;
    IUnknown *pD3D12Dev = NULL;
    void *pDownlevel = NULL;
    /* volatile: required for reliability after longjmp (SEH) */
    volatile void *pCmdAlloc = NULL;
    volatile void *pCmdList = NULL;
    volatile HRESULT hr;
    UINT i;

    if (!pDesc) { LOG("D3D12Interop_Create: pDesc==NULL"); return NULL; }
    if (pDesc->BufferCount == 0 || pDesc->BufferCount > D3D12_INTEROP_MAX_BUFFERS) {
        return NULL;
    }

    pD3D12Dev = D3D12_GetDeviceFromQueue(pD3D12CQ);
    if (!pD3D12Dev) { LOG("D3D12Interop_Create: failed to get ID3D12Device"); return NULL; }
    IsLikelyValidObjectPtr(pD3D12Dev, "pD3D12Dev (this)");
    IsLikelyValidCodePtr(*(void**)pD3D12Dev, "pD3D12Dev vtable ptr (first entry)");

    /* Try to grab ID3D12InfoQueue so we can surface D3D12 validation errors.
     * This only works if the app created the device with the debug layer
     * (D3D12GetDebugInterface + EnableDebugLayer before D3D12CreateDevice).
     * If unavailable we log a hint and carry on. */
    {
        /* ID3D12InfoQueue vtable offsets (inherits IUnknown × 3)
         *   0x18  SetMessageCountLimit
         *   0x20  ClearStoredMessages
         *   0x28  GetMessage
         *   0x30  GetNumMessagesAllowedByStorageFilter
         *   0x38  GetNumMessagesDeniedByStorageFilter
         *   0x40  GetNumStoredMessages          <- D3D12IQ_GetNumStoredMessages_OFF
         *   0x48  GetNumStoredMessagesAllowedByRetrievalFilter
         *   0x50  ClearRetrievalFilter
         *   0x58  PushEmptyRetrievalFilter
         *   0x60  PushCopyOfRetrievalFilter
         *   0x68  PushRetrievalFilter
         *   0x70  PopRetrievalFilter
         *   0x78  SetRetrievalFilterStackSize
         *   0x80  GetStorageFilter
         *   0x88  ClearStorageFilter
         *   0x90  PushEmptyStorageFilter
         *   0x98  PushCopyOfStorageFilter
         *   0xa0  PushStorageFilter
         *   0xa8  PopStorageFilter
         *   0xb0  GetStorageFilterStackSize
         *   0xb8  AddRetrievalFilterEntries      (unused here)
         *   0xc0  GetRetrievalFilter
         *   0xc8  ClearRetrievalFilter2
         *   0xd0  PushEmptyRetrievalFilter2
         *   0xd8  PushCopyOfRetrievalFilter2
         *   0xe0  PushRetrievalFilter2
         *   0xe8  PopRetrievalFilter2
         *   0xf0  GetRetrievalFilterStackSize
         *   0xf8  AddStorageFilterEntries
         *   0x100 GetStorageFilter2
         *   0x108 ClearStorageFilter2
         *   0x110 PushEmptyStorageFilter2
         *   0x118 PushCopyOfStorageFilter2
         *   0x120 PushStorageFilter2
         *   0x128 PopStorageFilter2
         *   0x130 GetStorageFilterStackSize2
         *   0x138 AddApplicationMessage
         *   0x140 SetBreakOnCategory
         *   0x148 SetBreakOnSeverity            <- we use this
         *   0x150 SetBreakOnID
         *   0x158 GetBreakOnCategory
         *   0x160 GetBreakOnSeverity
         *   0x168 GetBreakOnID
         *   0x170 SetMuteDebugOutput
         *   0x178 GetMuteDebugOutput
         */
#define D3D12IQ_SetBreakOnSeverity_OFF 0x148
#define D3D12IQ_SetMessageCountLimit_OFF 0x18
        /* D3D12_MESSAGE_SEVERITY values */
#define D3D12_MESSAGE_SEVERITY_CORRUPTION_ 0
#define D3D12_MESSAGE_SEVERITY_ERROR_      1
#define D3D12_MESSAGE_SEVERITY_WARNING_    2
#define D3D12_MESSAGE_SEVERITY_INFO_       3

        typedef HRESULT(STDMETHODCALLTYPE*PFN_SetBreakOnSev)(void*, UINT, BOOL);
        typedef HRESULT(STDMETHODCALLTYPE*PFN_SetMsgCountLimit)(void*, UINT64);

        void *pIQ = NULL;
        HRESULT hrIQ = pD3D12Dev->lpVtbl->QueryInterface(
            pD3D12Dev, &IID_ID3D12InfoQueue, &pIQ);
        if (SUCCEEDED(hrIQ) && pIQ) {
            LOG("D3D12Interop_Create: ID3D12InfoQueue available -> configuring break-on-error");
            /* Raise the message store limit so we capture everything */
            PFN_SetMsgCountLimit pfnLimit =
                (PFN_SetMsgCountLimit)VS(pIQ, D3D12IQ_SetMessageCountLimit_OFF);
            if (pfnLimit) pfnLimit(pIQ, (UINT64)-1);
            /* Break on CORRUPTION and ERROR so any failing HRESULT surfaces in
             * the debug output even without an attached debugger. */
            PFN_SetBreakOnSev pfnBreak =
                (PFN_SetBreakOnSev)VS(pIQ, D3D12IQ_SetBreakOnSeverity_OFF);
            if (pfnBreak) {
                pfnBreak(pIQ, D3D12_MESSAGE_SEVERITY_CORRUPTION_, TRUE);
                pfnBreak(pIQ, D3D12_MESSAGE_SEVERITY_ERROR_,      TRUE);
                LOG("D3D12Interop_Create: ID3D12InfoQueue break-on-severity set for CORRUPTION+ERROR");
            }
            ((IUnknown*)pIQ)->lpVtbl->Release((IUnknown*)pIQ);
        } else {
            LOG("D3D12Interop_Create: QueryInterface(ID3D12InfoQueue) failed hr=0x%08X "
                "-- the app must create its D3D12 device with the debug layer enabled: "
                "call D3D12GetDebugInterface + EnableDebugLayer BEFORE D3D12CreateDevice, "
                "or set the registry key "
                "HKLM\\SOFTWARE\\Microsoft\\Direct3D\\EnableDebugLayer=1",
                (unsigned)hrIQ);
        }
    }

    /* QI the app's CommandQueue for ID3D12CommandQueueDownlevel */
    hr = pD3D12CQ->lpVtbl->QueryInterface(pD3D12CQ, &IID_ID3D12CommandQueueDownlevel, &pDownlevel);
    if (FAILED(hr) || !pDownlevel) {
        goto fail;
    }

    /* Create our own CommandAllocator + CommandList DIRECT */
    {
        typedef HRESULT(STDMETHODCALLTYPE*PFN_CreateCmdAlloc)(void*, D3D12_COMMAND_LIST_TYPE_, REFIID, void**);
        void *pFn = VS(pD3D12Dev, D3D12DEV_CreateCommandAllocator_OFF);
        DWORD exCode = 0;
        void *pTmpAlloc = NULL;
        IsLikelyValidCodePtr(pFn, "D3D12DEV_CreateCommandAllocator_OFF");
        SEH_TRY {
            hr = ((PFN_CreateCmdAlloc)pFn)(
                pD3D12Dev, D3D12_COMMAND_LIST_TYPE_DIRECT_, &IID_ID3D12CommandAllocator, &pTmpAlloc);
            pCmdAlloc = pTmpAlloc;
        } SEH_EXCEPT(exCode) {
            LOG("D3D12Interop_Create: SEH exception in CreateCommandAllocator exCode=0x%08X",
                (unsigned)exCode);
            hr = E_FAIL;
            pCmdAlloc = NULL;
        } SEH_END
        if (FAILED(hr) || !pCmdAlloc) goto fail;
    }
    {
        typedef HRESULT(STDMETHODCALLTYPE*PFN_CreateCmdList)(void*, UINT, D3D12_COMMAND_LIST_TYPE_, void*, void*, REFIID, void**);
        void *pFn = VS(pD3D12Dev, D3D12DEV_CreateCommandList_OFF);
        DWORD exCode = 0;
        void *pTmpList = NULL;
        void *pCmdAllocSnapshot = (void*)pCmdAlloc;
        IsLikelyValidCodePtr(pFn, "D3D12DEV_CreateCommandList_OFF");
        SEH_TRY {
            hr = ((PFN_CreateCmdList)pFn)(
                pD3D12Dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT_, pCmdAllocSnapshot, NULL, &IID_ID3D12GraphicsCommandList, &pTmpList);
            pCmdList = pTmpList;
        } SEH_EXCEPT(exCode) {
            LOG("D3D12Interop_Create: SEH exception in CreateCommandList exCode=0x%08X "
                "(STATUS_ACCESS_DENIED=0xC0000022 typical of CFG blocking) — "
                "pFn=%p pD3D12Dev=%p pCmdAlloc=%p",
                (unsigned)exCode, pFn, pD3D12Dev, pCmdAllocSnapshot);
            hr = E_FAIL;
            pCmdList = NULL;
        } SEH_END
        if (FAILED(hr) || !pCmdList) goto fail;
        /* CreateCommandList returns list in "recording" (open) state.
         * Close it immediately so that D3D12Interop_Present can always
         * do a uniform Reset+Reopen before calling Downlevel::Present. */
#define D3D12_GRAPHICS_COMMAND_LIST_CLOSE_OFF 0x48
        {
            typedef HRESULT(STDMETHODCALLTYPE*PFN_CLClose)(void*);
            PFN_CLClose pfnClose = (PFN_CLClose)VS((void*)pTmpList, D3D12_GRAPHICS_COMMAND_LIST_CLOSE_OFF);
            HRESULT hrClose = pfnClose(pTmpList);
            if (FAILED(hrClose)) { pCmdList = pTmpList; goto fail; }
        }
    }

    it = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*it));
    if (!it) goto fail;
    it->pD3D12Dev   = pD3D12Dev;  pD3D12Dev = NULL;
    it->pD3D12CQ    = pD3D12CQ;   pD3D12CQ->lpVtbl->AddRef(pD3D12CQ);
    it->pDownlevel  = pDownlevel; pDownlevel = NULL;
    it->pCmdAlloc   = (void*)pCmdAlloc;  pCmdAlloc = NULL;
    it->pCmdList    = (void*)pCmdList;   pCmdList = NULL;
    it->BufferCount = pDesc->BufferCount;
    it->Width = pDesc->Width; it->Height = pDesc->Height; it->Format = pDesc->Format;
    it->hwnd = hwnd;

    /* Create D3D12 "back buffer" resources (no sharing) */
        /* Create D3D12 "back buffer" resources (no sharing) */
    for (i = 0; i < it->BufferCount; i++) {
        typedef HRESULT(STDMETHODCALLTYPE*PFN_CCR)(void*, const D3D12_HEAP_PROPERTIES_MIN*,
            D3D12_HEAP_FLAGS_, const D3D12_RESOURCE_DESC_MIN*, D3D12_RESOURCE_STATES_, const void*, REFIID, void**);
        D3D12_HEAP_PROPERTIES_MIN hp; ZeroMemory(&hp, sizeof(hp));
        hp.Type = D3D12_HEAP_TYPE_DEFAULT_;
        D3D12_RESOURCE_DESC_MIN rd; ZeroMemory(&rd, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D_;
        rd.Width = it->Width; rd.Height = it->Height;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = it->Format; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN_;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET_; // Remove ALLOW_SIMULTANEOUS_ACCESS
        D3D12_CLEAR_VALUE_MIN cv; ZeroMemory(&cv, sizeof(cv));
        cv.Format = it->Format;
        void *pRes = NULL;
        void *pFn = VS(it->pD3D12Dev, D3D12DEV_CreateCommittedResource_OFF);
        IsLikelyValidCodePtr(pFn, "D3D12DEV_CreateCommittedResource_OFF");
        hr = ((PFN_CCR)pFn)(
            it->pD3D12Dev, &hp, D3D12_HEAP_FLAG_NONE_, &rd,
            D3D12_RESOURCE_STATE_COMMON_, &cv, &IID_ID3D12Resource, &pRes); // COMMON state
        D3D12_DumpInfoQueue(it->pInfoQueue);  /* NULL until end of Create -- ok */
        if (FAILED(hr) || !pRes) goto fail;
        it->pD3D12RenderRes[i] = pRes;
    }

    *ppRealSC = NULL;

    /* ---- Cache Present hot-path vtable pointers (resolved once, used every frame) ---- */
    it->pfnAllocReset = (PFN_AllocReset_t)VS(it->pCmdAlloc,  D3D12_COMMAND_ALLOCATOR_RESET_OFF);
    it->pfnListReset  = (PFN_ListReset_t) VS(it->pCmdList,   D3D12_GRAPHICS_COMMAND_LIST_RESET_OFF);
    it->pfnDLPresent  = (PFN_DLPresent_t) VS(it->pDownlevel, D3D12_COMMAND_QUEUE_DOWNLEVEL_PRESENT_OFF);
    if (!it->pfnAllocReset || !it->pfnListReset || !it->pfnDLPresent) {
        LOG("D3D12Interop_Create: failed to cache one or more vtable pointers");
        goto fail;
    }

    /* ---- Precompute buffer index mask (avoids per-frame division) ----
     * If BufferCount is a power of two (2 or 4 -- by far the most common),
     * presentIndex & bufIdxMask is equivalent to presentIndex % BufferCount
     * and is a single AND instruction.  For non-pow2 counts bufIdxMask=0
     * signals the fallback to the modulo path.                           */
    it->bufIdxMask = ((it->BufferCount & (it->BufferCount - 1)) == 0)
                     ? (it->BufferCount - 1) : 0;
    LOG("D3D12Interop_Create: bufIdxMask=0x%X (BufferCount=%u, %s)",
        it->bufIdxMask, it->BufferCount,
        it->bufIdxMask ? "pow2 AND path" : "modulo fallback");

    /* ---- Cache ID3D12InfoQueue (QI once, reuse on error paths) ----
     * D3D12_DumpInfoQueue previously did a QI on every call.  We cache
     * the pointer here so error-path dumps cost only a vtable call,
     * not a COM QI.  pInfoQueue stays NULL when the debug layer is off
     * (the normal production case) -- zero overhead on the happy path. */
    {
        void *pIQ = NULL;
        if (SUCCEEDED(((IUnknown*)it->pD3D12Dev)->lpVtbl->QueryInterface(
                (IUnknown*)it->pD3D12Dev, &IID_ID3D12InfoQueue, &pIQ)) && pIQ)
            it->pInfoQueue = pIQ;   /* AddRef already done by QI */
        else
            it->pInfoQueue = NULL;
        LOG("D3D12Interop_Create: pInfoQueue=%p (%s)",
            it->pInfoQueue, it->pInfoQueue ? "debug layer active" : "debug layer off");
    }

    /* ---- Frame-latency fence (MaxFrameLatency=1 emulation) ----
     * Signal the fence after every Downlevel::Present, then wait at the
     * top of the NEXT Present -- keeps at most 1 frame in flight, identical
     * to DXGI on Win10 with SetMaximumFrameLatency(1).
     * Non-fatal: if creation fails we simply skip the wait.           */
    {
        typedef HRESULT(STDMETHODCALLTYPE*PFN_CreateFence)(void*,UINT64,UINT,REFIID,void**);
        PFN_CreateFence pfnCF = (PFN_CreateFence)VS(it->pD3D12Dev, D3D12DEV_CreateFence_OFF);
        void *pFence = NULL;
        hr = S_FALSE;
        if (pfnCF) hr = pfnCF(it->pD3D12Dev, 0, 0 /*D3D12_FENCE_FLAG_NONE*/, &IID_ID3D12Fence, &pFence);
        if (SUCCEEDED(hr) && pFence) {
            HANDLE hEv = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (hEv) {
                it->pFence         = pFence;
                it->hFenceEvent    = hEv;
                it->fenceValue     = 0;
                it->maxFrameLatency = 1;
                it->pfnFenceGetVal = (PFN_FenceGetVal_t)VS(pFence, D3D12FENCE_GetCompletedValue_OFF);
                it->pfnFenceWait   = (PFN_FenceWait_t) VS(pFence, D3D12FENCE_SetEventOnCompletion_OFF);
                it->pfnCQSignal    = (PFN_CQSignal_t)  VS(it->pD3D12CQ, D3D12CQ_Signal_OFF);
                LOG("D3D12Interop_Create: frame-latency fence created (MaxFrameLatency=%u)",
                    it->maxFrameLatency);
            } else {
                ((IUnknown*)pFence)->lpVtbl->Release((IUnknown*)pFence);
                LOG("D3D12Interop_Create: CreateEvent failed -- frame-latency fence disabled");
            }
        } else {
            LOG("D3D12Interop_Create: CreateFence hr=0x%08X -- frame-latency fence disabled",
                (unsigned)hr);
        }
    }

    /* Initialise FPS overlay (non-fatal if it fails) */
    Ov_Init(it);

    return it;

fail:
    if (pCmdList)   ((IUnknown*)pCmdList)->lpVtbl->Release((IUnknown*)pCmdList);
    if (pCmdAlloc)  ((IUnknown*)pCmdAlloc)->lpVtbl->Release((IUnknown*)pCmdAlloc);
    if (pDownlevel) ((IUnknown*)pDownlevel)->lpVtbl->Release((IUnknown*)pDownlevel);
    if (pD3D12Dev)  pD3D12Dev->lpVtbl->Release(pD3D12Dev);
    if (it) D3D12Interop_Destroy(it);
    return NULL;
}

static void D3D12Interop_Destroy(D3D12Interop *it)
{
    UINT i;
    if (!it) return;
    for (i = 0; i < it->BufferCount; i++) {
        if (it->pD3D12RenderRes[i]) ((IUnknown*)it->pD3D12RenderRes[i])->lpVtbl->Release((IUnknown*)it->pD3D12RenderRes[i]);
    }
    /* Cached InfoQueue (debug layer) */
    if (it->pInfoQueue) { ((IUnknown*)it->pInfoQueue)->lpVtbl->Release((IUnknown*)it->pInfoQueue); it->pInfoQueue = NULL; }
    /* Fence + event */
    if (it->pFence)      ((IUnknown*)it->pFence)->lpVtbl->Release((IUnknown*)it->pFence);
    if (it->hFenceEvent && it->hFenceEvent != INVALID_HANDLE_VALUE)
        CloseHandle(it->hFenceEvent);
    /* GDI overlay resources */
    Ov_Destroy(it);
    if (it->ovVBuf)    ((IUnknown*)it->ovVBuf)->lpVtbl->Release((IUnknown*)it->ovVBuf);
    if (it->ovTex)     ((IUnknown*)it->ovTex)->lpVtbl->Release((IUnknown*)it->ovTex);
    if (it->ovDescHeap)((IUnknown*)it->ovDescHeap)->lpVtbl->Release((IUnknown*)it->ovDescHeap);
    if (it->ovPSO)     ((IUnknown*)it->ovPSO)->lpVtbl->Release((IUnknown*)it->ovPSO);
    if (it->ovRootSig) ((IUnknown*)it->ovRootSig)->lpVtbl->Release((IUnknown*)it->ovRootSig);
    /* D3D12 objects */
    if (it->pCmdList)   ((IUnknown*)it->pCmdList)->lpVtbl->Release((IUnknown*)it->pCmdList);
    if (it->pCmdAlloc)  ((IUnknown*)it->pCmdAlloc)->lpVtbl->Release((IUnknown*)it->pCmdAlloc);
    if (it->pDownlevel) ((IUnknown*)it->pDownlevel)->lpVtbl->Release((IUnknown*)it->pDownlevel);
    if (it->pD3D12CQ)   ((IUnknown*)it->pD3D12CQ)->lpVtbl->Release((IUnknown*)it->pD3D12CQ);
    if (it->pD3D12Dev)  ((IUnknown*)it->pD3D12Dev)->lpVtbl->Release((IUnknown*)it->pD3D12Dev);
    HeapFree(GetProcessHeap(), 0, it);
}

static HRESULT D3D12Interop_GetBuffer(WrapSC *w, UINT Buf, REFIID riid, void **pp)
{
    D3D12Interop *it = w->interop;
    (void)riid; /* always return ID3D12Resource */
    if (!it) return E_NOTIMPL;
    if (Buf >= it->BufferCount) { LOG("D3D12Interop_GetBuffer: Buf=%u out of range", Buf); return DXGI_ERROR_INVALID_CALL; }
    if (!it->pD3D12RenderRes[Buf]) return E_FAIL;
    ((IUnknown*)it->pD3D12RenderRes[Buf])->lpVtbl->AddRef((IUnknown*)it->pD3D12RenderRes[Buf]);
    *pp = it->pD3D12RenderRes[Buf];

    return S_OK;
}

static HRESULT D3D12Interop_Present(WrapSC *w, UINT SyncInterval, UINT Flags, HRESULT *outHr)
{
    D3D12Interop *it = w->interop;
    UINT idx;
    HRESULT hr = S_OK;

    if (!it) { *outHr = E_NOTIMPL; return E_NOTIMPL; }

    /* Use precomputed mask when BufferCount is pow2 (avoids integer division) */
    idx = it->bufIdxMask
          ? (it->presentIndex & it->bufIdxMask)
          : (it->presentIndex % it->BufferCount);

    if (it->presentIndex == 0)
        D3D12_DumpInfoQueue(it->pInfoQueue);  /* cached, no QI */

    if (!it->pDownlevel || !it->pCmdList || !it->pD3D12RenderRes[idx]) {
        *outHr = E_FAIL; return E_FAIL;
    }

    /* ---- Wait for the previous frame to finish (MaxFrameLatency=1) ----
     * We wait at the TOP of Present so the CPU is already done waiting
     * by the time the game renders the next frame, minimising stall time.
     * Skip on frame 0 (fence not yet signaled).                          */
    if (it->pFence && it->pfnFenceGetVal && it->pfnFenceWait &&
        it->presentIndex > 0)
    {
        UINT64 completed = it->pfnFenceGetVal(it->pFence);
        if (completed < it->fenceValue) {
            it->pfnFenceWait(it->pFence, it->fenceValue, it->hFenceEvent);
            WaitForSingleObject(it->hFenceEvent, INFINITE);
        }
    }

    /* ---- Reset allocator + list (using cached pointers, no vtable walk) ---- */
    hr = it->pfnAllocReset(it->pCmdAlloc);
    if (FAILED(hr)) {
        LOG_ERR("D3D12Interop_Present: AllocReset 0x%08X", (unsigned)hr);
        *outHr = hr; return hr;
    }
    hr = it->pfnListReset(it->pCmdList, it->pCmdAlloc, NULL);
    if (FAILED(hr)) {
        LOG_ERR("D3D12Interop_Present: ListReset 0x%08X", (unsigned)hr);
        *outHr = hr; return hr;
    }

    /* FPS overlay (GDI, non-fatal) */
    Ov_Render(it, it->pD3D12RenderRes[idx]);

    /* ---- Downlevel::Present (cached pointer) ---- */
    hr = it->pfnDLPresent(it->pDownlevel,
                          it->pCmdList,
                          it->pD3D12RenderRes[idx],
                          it->hwnd,
                          D3D12_DOWNLEVEL_PRESENT_FLAG_NONE_);
    if (FAILED(hr)) {
        D3D12_DumpInfoQueue(it->pInfoQueue);  /* cached, no QI */
        *outHr = hr; return hr;
    }

    /* ---- Signal fence so the NEXT Present can wait on this frame ---- */
    if (it->pFence && it->pfnCQSignal) {
        it->fenceValue++;
        it->pfnCQSignal(it->pD3D12CQ, it->pFence, it->fenceValue);
    }

    it->presentIndex++;
    *outHr = S_OK;
    return S_OK;
}

/* =========================================================
 * ======== FACTORY WRAPPER - CreateSwapChainForHwnd ========
 * ========================================================= */
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChainForHwnd(WrapFactory*w,IUnknown*pDev,HWND hwnd,const void*pD,const void*pFD,void*pRO,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,IUnknown*,HWND,const void*,const void*,void*,void**);
    LOG("IDXGIFactory2::CreateSwapChainForHwnd(hwnd=%p)",(void*)hwnd);
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    if(!pp) return E_INVALIDARG; *pp=NULL;

    /* D3D12 detection */
    {
        void *pCQ=NULL;
        HRESULT hrcq = pDev ? pDev->lpVtbl->QueryInterface(pDev,&IID_ID3D12CommandQueue,&pCQ) : E_POINTER;
        if(SUCCEEDED(hrcq) && pCQ) {
            IUnknown *pRealSC=NULL;
            D3D12Interop *it;
            LOG("  CreateSwapChainForHwnd: ID3D12CommandQueue detected -> D3D12on7 (Downlevel)");
            it = D3D12Interop_Create((IUnknown*)pCQ, hwnd, pD, &pRealSC);
            ((IUnknown*)pCQ)->lpVtbl->Release((IUnknown*)pCQ);
            if(!it) {
                LOG("  CreateSwapChainForHwnd: D3D12Interop_Create failed");
                LOG_LEAVE("IDXGIFactory2::CreateSwapChainForHwnd",E_FAIL);
                return E_FAIL;
            }
            {
                WrapSC *wsc=NULL;
                HRESULT hr2=WrapSC_Create2(NULL,&IID_IDXGISwapChain1,pp,&wsc,it);
                if(FAILED(hr2)||!wsc) {
                    LOG("  CreateSwapChainForHwnd: WrapSC_Create2 = 0x%08X",(unsigned)hr2);
                    D3D12Interop_Destroy(it);
                    LOG_LEAVE("IDXGIFactory2::CreateSwapChainForHwnd",hr2);
                    return hr2;
                }
            }
            LOG_LEAVE("IDXGIFactory2::CreateSwapChainForHwnd",S_OK);
            return S_OK;
        }
    }

    /* Normal D3D11/D3D9 path */
    void *pSC=NULL;
    HRESULT hr=((PFN)VS(w->pRealF2,0x78))(w->pRealF2,pDev,hwnd,pD,pFD,pRO,&pSC);
    LOG("  IDXGIFactory2::CreateSwapChainForHwnd (real call) = 0x%08X pSC=%p",(unsigned)hr,pSC);
    if(SUCCEEDED(hr)) hr=WrapSC_Create((IUnknown*)pSC,&IID_IDXGISwapChain1,pp);
    if(pSC) { ((IUnknown*)pSC)->lpVtbl->Release((IUnknown*)pSC); pSC=NULL; }
    LOG_LEAVE("IDXGIFactory2::CreateSwapChainForHwnd",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE WF_CreateSwapChainForCoreWindow(WrapFactory*w,IUnknown*pDev,IUnknown*pWin,const void*pD,void*pRO,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,IUnknown*,IUnknown*,const void*,void*,void**);
    LOG("IDXGIFactory2::CreateSwapChainForCoreWindow");
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    if(!pp) return E_INVALIDARG; *pp=NULL;
    void *pSC=NULL;
    HRESULT hr=((PFN)VS(w->pRealF2,0x80))(w->pRealF2,pDev,pWin,pD,pRO,&pSC);
    if(SUCCEEDED(hr)) hr=WrapSC_Create((IUnknown*)pSC,&IID_IDXGISwapChain1,pp);
    if(pSC) { ((IUnknown*)pSC)->lpVtbl->Release((IUnknown*)pSC); pSC=NULL; }
    LOG_LEAVE("IDXGIFactory2::CreateSwapChainForCoreWindow",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WF_GetSharedResourceAdapterLuid(WrapFactory*w,HANDLE h,LUID*pL)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HANDLE,LUID*);
    LOG("IDXGIFactory2::GetSharedResourceAdapterLuid");
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    return ((PFN)VS(w->pRealF2,0x88))(w->pRealF2,h,pL);
}
static HRESULT STDMETHODCALLTYPE WF_RegisterStereoStatusWindow(WrapFactory*w,HWND h,UINT m,DWORD*pc)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HWND,UINT,DWORD*);
    LOG("IDXGIFactory2::RegisterStereoStatusWindow");
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    return ((PFN)VS(w->pRealF2,0x90))(w->pRealF2,h,m,pc);
}
static HRESULT STDMETHODCALLTYPE WF_RegisterStereoStatusEvent(WrapFactory*w,HANDLE h,DWORD*pc)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HANDLE,DWORD*);
    LOG("IDXGIFactory2::RegisterStereoStatusEvent");
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    return ((PFN)VS(w->pRealF2,0x98))(w->pRealF2,h,pc);
}
static void STDMETHODCALLTYPE WF_UnregisterStereoStatus(WrapFactory*w,DWORD c)
{
    typedef void(STDMETHODCALLTYPE*PFN)(void*,DWORD);
    LOG("IDXGIFactory2::UnregisterStereoStatus(0x%X)",c);
    if(!w->pRealF2) return;
    ((PFN)VS(w->pRealF2,0xa0))(w->pRealF2,c);
}
static HRESULT STDMETHODCALLTYPE WF_RegisterOcclusionStatusWindow(WrapFactory*w,HWND h,UINT m,DWORD*pc)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HWND,UINT,DWORD*);
    LOG("IDXGIFactory2::RegisterOcclusionStatusWindow");
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    return ((PFN)VS(w->pRealF2,0xa8))(w->pRealF2,h,m,pc);
}
static HRESULT STDMETHODCALLTYPE WF_RegisterOcclusionStatusEvent(WrapFactory*w,HANDLE h,DWORD*pc)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HANDLE,DWORD*);
    LOG("IDXGIFactory2::RegisterOcclusionStatusEvent");
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    return ((PFN)VS(w->pRealF2,0xb0))(w->pRealF2,h,pc);
}
static void STDMETHODCALLTYPE WF_UnregisterOcclusionStatus(WrapFactory*w,DWORD c)
{
    typedef void(STDMETHODCALLTYPE*PFN)(void*,DWORD);
    LOG("IDXGIFactory2::UnregisterOcclusionStatus(0x%X)",c);
    if(!w->pRealF2) return;
    ((PFN)VS(w->pRealF2,0xb8))(w->pRealF2,c);
}
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChainForComposition(WrapFactory*w,IUnknown*pDev,const void*pD,void*pRO,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,IUnknown*,const void*,void*,void**);
    LOG("IDXGIFactory2::CreateSwapChainForComposition");
    F2_GUARD(DXGI_ERROR_UNSUPPORTED);
    if(!pp) return E_INVALIDARG; *pp=NULL;
    void *pSC=NULL;
    HRESULT hr=((PFN)VS(w->pRealF2,0xc0))(w->pRealF2,pDev,pD,pRO,&pSC);
    if(SUCCEEDED(hr)) hr=WrapSC_Create((IUnknown*)pSC,&IID_IDXGISwapChain1,pp);
    if(pSC) { ((IUnknown*)pSC)->lpVtbl->Release((IUnknown*)pSC); pSC=NULL; }
    LOG_LEAVE("IDXGIFactory2::CreateSwapChainForComposition",hr); return hr;
}

static UINT STDMETHODCALLTYPE WF_GetCreationFlags(WrapFactory*w)
{
    LOG("IDXGIFactory3::GetCreationFlags → 0x%X",w->uFlags);
    return w->uFlags;
}
static HRESULT STDMETHODCALLTYPE WF_EnumAdapterByLuid(WrapFactory*w,LUID luid,REFIID riid,void**pp)
{
    LOG("IDXGIFactory4::EnumAdapterByLuid({%08X,%08X},%s)",luid.LowPart,luid.HighPart,GuidName(riid));
    if(!pp) return E_INVALIDARG; *pp=NULL;
    for(UINT i=0;;i++) {
        void *pA=NULL;
        HRESULT hr=WF_EnumAdapters1(w,i,&pA);
        if(hr==DXGI_ERROR_NOT_FOUND||FAILED(hr)) break;
        BYTE desc[sizeof(DXGI_ADAPTER_DESC)*2];
        void *pA1=NULL;
        ((IUnknown*)pA)->lpVtbl->QueryInterface((IUnknown*)pA,&IID_IDXGIAdapter1,&pA1);
        if(pA1) {
            typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
            HRESULT hd=((PFN)VS(pA1,0x50))(pA1,desc);
            if(SUCCEEDED(hd)) {
                LUID *pl=(LUID*)(desc+296);
                if(pl->LowPart==luid.LowPart && pl->HighPart==luid.HighPart) {
                    HRESULT hr2=((IUnknown*)pA)->lpVtbl->QueryInterface((IUnknown*)pA,riid,pp);
                    ((IUnknown*)pA1)->lpVtbl->Release((IUnknown*)pA1);
                    ((IUnknown*)pA)->lpVtbl->Release((IUnknown*)pA);
                    LOG_LEAVE("IDXGIFactory4::EnumAdapterByLuid",hr2); return hr2;
                }
            }
            ((IUnknown*)pA1)->lpVtbl->Release((IUnknown*)pA1);
        }
        ((IUnknown*)pA)->lpVtbl->Release((IUnknown*)pA);
    }
    LOG("IDXGIFactory4::EnumAdapterByLuid → NOT_FOUND");
    return DXGI_ERROR_NOT_FOUND;
}
static HRESULT STDMETHODCALLTYPE WF_EnumWarpAdapter(WrapFactory*w,REFIID riid,void**pp)
{
    LOG("IDXGIFactory4::EnumWarpAdapter");
    if(!pp) return E_INVALIDARG; *pp=NULL;
    HMODULE hWarp=LoadLibraryExW(L"d3d10warp.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!hWarp) return DXGI_ERROR_NOT_FOUND;
    void *pA=NULL;
    HRESULT hr=WF_CreateSoftwareAdapter(w,hWarp,&pA);
    FreeLibrary(hWarp);
    if(SUCCEEDED(hr)&&pA) {
        hr=((IUnknown*)pA)->lpVtbl->QueryInterface((IUnknown*)pA,riid,pp);
        ((IUnknown*)pA)->lpVtbl->Release((IUnknown*)pA);
    }
    LOG_LEAVE("IDXGIFactory4::EnumWarpAdapter",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WF_CheckFeatureSupport(WrapFactory*w,UINT feat,void*pData,UINT sz)
{
    LOG("IDXGIFactory5::CheckFeatureSupport(feat=%u)",feat);
    (void)w;
    if(feat==0 && pData && sz>=sizeof(BOOL)) { *(BOOL*)pData=FALSE; return S_OK; }
    return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE WF_EnumAdapterByGpuPreference(WrapFactory*w,UINT i,UINT pref,REFIID riid,void**pp)
{
    LOG("IDXGIFactory6::EnumAdapterByGpuPreference(idx=%u,pref=%u,%s)",i,pref,GuidName(riid));
    (void)pref; (void)riid;
    return WF_EnumAdapters1(w,i,pp);
}
static HRESULT STDMETHODCALLTYPE WF_RegisterAdaptersChangedEvent(WrapFactory*w,HANDLE h,DWORD*pc)
{
    LOG("IDXGIFactory7::RegisterAdaptersChangedEvent (WNF stub)");
    (void)h;
    if(pc) { w->dwAdapChgCookie=0xDEAD7700; *pc=w->dwAdapChgCookie; }
    if(h && h!=INVALID_HANDLE_VALUE) SetEvent(h);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE WF_UnregisterAdaptersChangedEvent(WrapFactory*w,DWORD c)
{
    LOG("IDXGIFactory7::UnregisterAdaptersChangedEvent(0x%X)",c);
    (void)w; (void)c; return S_OK;
}

/* =========================================================
 * PIX interception
 * ========================================================= */
static FARPROC PIX_Intercept(FARPROC pfnDefault, LPCSTR pGetPP, LPCSTR pGen)
{
    HMODULE hCap=NULL; BOOL bAlready=FALSE;
    FARPROC pfnLazy,pfnGetPP,pfnGen;

    bAlready = GetModuleHandleExW(0,L"DXCaptureReplay.dll",&hCap);
    if(!bAlready||!hCap) {
        HANDLE hSem=OpenSemaphoreW(SYNCHRONIZE,FALSE,L"DXEnableCapture");
        if(!hSem) return pfnDefault;
        CloseHandle(hSem);

        BOOL bAny=TRUE;
        HKEY hk=NULL;
        if(RegOpenKeyExW((HKEY)0x80000002L,L"Software\\Microsoft\\DXTools",0,KEY_READ,&hk)==0){
            DWORD v=0,t=0,cb=4;
            if(RegGetValueW(hk,NULL,L"LoadFromAnywhere",8,&t,&v,&cb)==0&&t==4&&v)bAny=FALSE;
            RegCloseKey(hk);
        }
        hCap=LoadLibraryExW(L"DXCaptureReplay.dll",NULL,bAny?0:LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!hCap) return pfnDefault;
    }

    pfnLazy  =GetProcAddress(hCap,"LazyAttachToMonitor");
    pfnGetPP =GetProcAddress(hCap,pGetPP);
    pfnGen   =GetProcAddress(hCap,pGen);
    if(!pfnLazy||!pfnGetPP||!pfnGen) { if(!bAlready)FreeLibrary(hCap); return pfnDefault; }
    if(((int(WINAPI*)(void))pfnLazy)()<0)  { if(!bAlready)FreeLibrary(hCap); return pfnDefault; }

    if(bAlready) FreeLibrary(hCap);
    void **ppSlot=(void**)((FARPROC(WINAPI*)(void))pfnGetPP)();
    *ppSlot=(void*)pfnDefault;
    return pfnGen;
}

/* =========================================================
 * =================== PUBLIC EXPORTS ====================
 * ========================================================= */

__declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory)
{
    HRESULT hr;
    /* Reentrancy guard: PIX/RenderDoc hooks or UE4's DXCapture integration
     * can cause CreateDXGIFactory to be called recursively back into our
     * wrapper (infinite loop).  If we are already inside this function on
     * this thread, bypass wrapping and call the system function directly. */
    if (InterlockedCompareExchange(&g_inCreateFactory, 1, 0) != 0) {
        LOG("CreateDXGIFactory: reentrant call detected, bypassing wrapper");
        if(!g_pfnFactory) return DXGI_ERROR_UNSUPPORTED;
        /* Call system with IID_IDXGIFactory — Win7 system dxgi.dll only
         * knows IDXGIFactory/IDXGIFactory1, not IDXGIFactory4+. */
        IUnknown *pRaw2=NULL;
        HRESULT hr2=g_pfnFactory(&IID_IDXGIFactory,(void**)&pRaw2);
        if(FAILED(hr2)||!pRaw2) {
            if(g_pfnFactory1) hr2=g_pfnFactory1(&IID_IDXGIFactory1,(void**)&pRaw2);
        }
        if(SUCCEEDED(hr2)&&pRaw2) {
            hr2=WrapFactory_Create(pRaw2,0,riid,ppFactory);
            pRaw2->lpVtbl->Release(pRaw2);
        }
        return hr2;
    }

    LOG_ENTER("CreateDXGIFactory");
    if(!ppFactory) { InterlockedExchange(&g_inCreateFactory,0); LOG_LEAVE("CreateDXGIFactory",E_INVALIDARG); return E_INVALIDARG; }
    *ppFactory=NULL;
    if(!g_pfnFactory) { InterlockedExchange(&g_inCreateFactory,0); LOG_LEAVE("CreateDXGIFactory",DXGI_ERROR_UNSUPPORTED); return DXGI_ERROR_UNSUPPORTED; }

    typedef HRESULT(WINAPI*PFN)(REFIID,void**);
    PFN pfn=(PFN)PIX_Intercept((FARPROC)g_pfnFactory,
        "GetRealPtrPtrCreateDXGIFactory","CreateDXGIFactoryGenerated");

    /* Always call the system CreateDXGIFactory with IID_IDXGIFactory —
     * the system dxgi.dll on Win7 only knows IDXGIFactory/IDXGIFactory1.
     * We then wrap the result and QI to whatever riid the caller wants. */
    LOG_GUID("  CreateDXGIFactory: requested riid", riid);
    IUnknown *pRaw=NULL;
    hr=pfn(&IID_IDXGIFactory,(void**)&pRaw);
    if(FAILED(hr)||!pRaw) {
        /* Try IID_IDXGIFactory1 as fallback */
        if(g_pfnFactory1) hr=g_pfnFactory1(&IID_IDXGIFactory1,(void**)&pRaw);
    }
    if(SUCCEEDED(hr)&&pRaw) {
        hr=WrapFactory_Create(pRaw,0,riid,ppFactory);
        pRaw->lpVtbl->Release(pRaw);
    }
    InterlockedExchange(&g_inCreateFactory, 0);
    LOG_LEAVE("CreateDXGIFactory",hr); return hr;
}

__declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory)
{
    HRESULT hr;
    if (InterlockedCompareExchange(&g_inCreateFactory1, 1, 0) != 0) {
        LOG("CreateDXGIFactory1: reentrant call detected, bypassing wrapper");
        IUnknown *pRaw2=NULL;
        HRESULT hr2=S_OK;
        PFN_CreateDXGIFactory1 pfnD=g_pfnFactory1?(g_pfnFactory1):(PFN_CreateDXGIFactory1)g_pfnFactory;
        if(!pfnD) return DXGI_ERROR_UNSUPPORTED;
        hr2=pfnD(&IID_IDXGIFactory1,(void**)&pRaw2);
        if(SUCCEEDED(hr2)&&pRaw2) {
            hr2=WrapFactory_Create(pRaw2,0,riid,ppFactory);
            pRaw2->lpVtbl->Release(pRaw2);
        }
        return hr2;
    }


    LOG_ENTER("CreateDXGIFactory1");
    LOG_GUID("  CreateDXGIFactory1: request",riid);
    LOG("  CreateDXGIFactory1: ppFactory=%p",(void*)ppFactory);
    if(!ppFactory) { InterlockedExchange(&g_inCreateFactory1,0); LOG_LEAVE("CreateDXGIFactory1",E_INVALIDARG); return E_INVALIDARG; }
    *ppFactory=NULL;

    PFN_CreateDXGIFactory1 pfnReal=g_pfnFactory1?(g_pfnFactory1):((PFN_CreateDXGIFactory1)g_pfnFactory);
    LOG("  CreateDXGIFactory1: pfnReal=%p (g_pfnFactory1=%p g_pfnFactory=%p)",
        (void*)pfnReal,(void*)g_pfnFactory1,(void*)g_pfnFactory);
    if(!pfnReal) { LOG_LEAVE("CreateDXGIFactory1",DXGI_ERROR_UNSUPPORTED); return DXGI_ERROR_UNSUPPORTED; }

    typedef HRESULT(WINAPI*PFN)(REFIID,void**);
    PFN pfn=(PFN)PIX_Intercept((FARPROC)pfnReal,
        "GetRealPtrPtrCreateDXGIFactory1","CreateDXGIFactory1Generated");
    LOG("  CreateDXGIFactory1: pfn (after PIX_Intercept)=%p",(void*)pfn);

    IUnknown *pRaw=NULL;
    LOG("  CreateDXGIFactory1: --> call pfn(IID_IDXGIFactory1,&pRaw)");
    hr=pfn(&IID_IDXGIFactory1,(void**)&pRaw);
    LOG("  CreateDXGIFactory1: <-- pfn = 0x%08X pRaw=%p",(unsigned)hr,(void*)pRaw);
    if(SUCCEEDED(hr)&&pRaw) {
        LOG("  CreateDXGIFactory1: --> WrapFactory_Create");
        hr=WrapFactory_Create(pRaw,0,riid,ppFactory);
        LOG("  CreateDXGIFactory1: <-- WrapFactory_Create = 0x%08X ppFactory=%p",(unsigned)hr,*ppFactory);
        pRaw->lpVtbl->Release(pRaw);
    }
    InterlockedExchange(&g_inCreateFactory1, 0);
    LOG_LEAVE("CreateDXGIFactory1",hr); return hr;
}

__declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
{
    if (InterlockedCompareExchange(&g_inCreateFactory2, 1, 0) != 0) {
        LOG("CreateDXGIFactory2: reentrant call detected, bypassing wrapper");
        IUnknown *pRaw2=NULL;
        HRESULT hr2=S_OK;
        if(g_pfnFactory1) hr2=g_pfnFactory1(&IID_IDXGIFactory1,(void**)&pRaw2);
        else if(g_pfnFactory) hr2=g_pfnFactory(&IID_IDXGIFactory,(void**)&pRaw2);
        else return DXGI_ERROR_UNSUPPORTED;
        if(SUCCEEDED(hr2)&&pRaw2) {
            hr2=WrapFactory_Create(pRaw2,Flags&DXGI_CREATE_FACTORY_DEBUG,riid,ppFactory);
            pRaw2->lpVtbl->Release(pRaw2);
        }
        return hr2;
    }


    LOG_ENTER("CreateDXGIFactory2");
    LOG("  CreateDXGIFactory2: raw params: Flags=0x%X, riid=%p, ppFactory=%p",
        Flags, (void*)riid, (void*)ppFactory);

    const GUID *actual_riid = riid;
    void **actual_ppFactory = ppFactory;
    UINT actual_Flags = Flags;

    // Détection d'ordre : si ppFactory n'est pas writable, essayer d'utiliser riid comme ppFactory
    if (!IsWriteablePtr(ppFactory, sizeof(void*))) {
        LOG("  ppFactory not writable, trying riid as ppFactory");
        if (IsWriteablePtr((void**)riid, sizeof(void*))) {
            actual_ppFactory = (void**)riid;
            actual_riid = NULL; // sera défini plus tard
        }
    }

    // Si actual_riid n'est pas un GUID valide ou est nul, utiliser IID_IDXGIFactory1
    if (!actual_riid || !IsReadablePtr(actual_riid, sizeof(GUID))) {
        LOG("  riid is not a valid GUID, using IID_IDXGIFactory1");
        actual_riid = &IID_IDXGIFactory1;
    } else {
        const GUID *g = actual_riid;
        if (g->Data1 == 0 && g->Data2 == 0 && g->Data3 == 0 &&
            ((const UINT64*)g->Data4)[0] == 0 && ((const UINT64*)g->Data4)[1] == 0) {
            LOG("  riid is null GUID, using IID_IDXGIFactory1");
            actual_riid = &IID_IDXGIFactory1;
        }
    }

    if (!actual_ppFactory || !IsWriteablePtr(actual_ppFactory, sizeof(void*))) {
        LOG("  no writable ppFactory found");
        InterlockedExchange(&g_inCreateFactory2, 0);
        LOG_LEAVE("CreateDXGIFactory2", E_INVALIDARG);
        return E_INVALIDARG;
    }

    // Nettoyer les flags (seul DEBUG est supporté sur Win7)
    actual_Flags &= DXGI_CREATE_FACTORY_DEBUG;
    if (actual_Flags != Flags) {
        LOG("  CreateDXGIFactory2: flags cleaned to 0x%X", actual_Flags);
    }

    // Debug layer handling
    if (actual_Flags & DXGI_CREATE_FACTORY_DEBUG) {
        LOG("  CreateDXGIFactory2: DXGI_CREATE_FACTORY_DEBUG requested");
        HMODULE hDbg = LoadLibraryExW(L"DXGIDebug.dll", NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (!hDbg) {
            OutputDebugStringA("CreateDXGIFactory2: DXGI_CREATE_FACTORY_DEBUG but DXGIDebug.dll missing\n");
            InterlockedExchange(&g_inCreateFactory2, 0);
            LOG_LEAVE("CreateDXGIFactory2", DXGI_ERROR_SDK_COMPONENT_MISSING);
            return DXGI_ERROR_SDK_COMPONENT_MISSING;
        }
        FreeLibrary(hDbg);
    }

    // --- Appel direct à la vraie factory (toujours avec IID_IDXGIFactory1) ---
    IUnknown *pRaw = NULL;
    HRESULT hr;
    if (g_pfnFactory1) {
        LOG("  CreateDXGIFactory2: calling g_pfnFactory1 with IID_IDXGIFactory1 (always)");
        hr = g_pfnFactory1(&IID_IDXGIFactory1, (void**)&pRaw);
        LOG("  CreateDXGIFactory2: g_pfnFactory1 returned 0x%08X, pRaw=%p", (unsigned)hr, (void*)pRaw);
    } else if (g_pfnFactory) {
        LOG("  CreateDXGIFactory2: calling g_pfnFactory with IID_IDXGIFactory (always)");
        hr = g_pfnFactory(&IID_IDXGIFactory, (void**)&pRaw);
        LOG("  CreateDXGIFactory2: g_pfnFactory returned 0x%08X, pRaw=%p", (unsigned)hr, (void*)pRaw);
    } else {
        LOG("  CreateDXGIFactory2: no DXGI factory function available");
        InterlockedExchange(&g_inCreateFactory2, 0);
        LOG_LEAVE("CreateDXGIFactory2", DXGI_ERROR_UNSUPPORTED);
        return DXGI_ERROR_UNSUPPORTED;
    }

    if (SUCCEEDED(hr) && pRaw) {
        LOG("  CreateDXGIFactory2: wrapping factory with requested riid=%s, actual_Flags=0x%X",
            GuidName(actual_riid), actual_Flags);
        hr = WrapFactory_Create(pRaw, actual_Flags, actual_riid, actual_ppFactory);
        pRaw->lpVtbl->Release(pRaw);
        LOG("  CreateDXGIFactory2: WrapFactory_Create returned 0x%08X, *ppFactory=%p",
            (unsigned)hr, *actual_ppFactory);
    } else {
        *actual_ppFactory = NULL;
        LOG("  CreateDXGIFactory2: factory creation failed (hr=0x%08X)", (unsigned)hr);
    }

    InterlockedExchange(&g_inCreateFactory2, 0);
    LOG_LEAVE("CreateDXGIFactory2", hr);
    return hr;
}

__declspec(dllexport)
HRESULT WINAPI DXGID3D10CreateDevice(HMODULE hm,void*pF,void*pA,UINT fl,const void*pU,void**pp)
{
    LOG("DXGID3D10CreateDevice → E_NOTIMPL");
    (void)hm;(void)pF;(void)pA;(void)fl;(void)pU;
    if(pp)*pp=NULL; return E_NOTIMPL;
}
__declspec(dllexport)
HRESULT WINAPI DXGID3D10CreateLayeredDevice(const void*p1,DWORD f,const void*p2,REFIID r,void**pp)
{
    LOG("DXGID3D10CreateLayeredDevice → E_NOTIMPL");
    (void)p1;(void)f;(void)p2;(void)r;
    if(pp)*pp=NULL; return E_NOTIMPL;
}
__declspec(dllexport)
SIZE_T WINAPI DXGID3D10GetLayeredDeviceSize(const void*p,UINT c)
{ (void)p;(void)c; LOG("DXGID3D10GetLayeredDeviceSize → 0"); return 0; }
__declspec(dllexport)
HRESULT WINAPI DXGID3D10RegisterLayers(const void*p,UINT c)
{ (void)p;(void)c; LOG("DXGID3D10RegisterLayers → E_NOTIMPL"); return E_NOTIMPL; }

__declspec(dllexport)
HRESULT WINAPI PIXBeginCapture(DWORD f,const void*p)
{ (void)f;(void)p; LOG("PIXBeginCapture → S_OK"); return S_OK; }
__declspec(dllexport)
HRESULT WINAPI PIXEndCapture(BOOL d)
{ (void)d; LOG("PIXEndCapture → S_OK"); return S_OK; }
__declspec(dllexport)
DWORD WINAPI PIXGetCaptureState(void)
{ LOG("PIXGetCaptureState → 0"); return 0; }

static volatile PVOID  s_CompatStrBuf = NULL;
static volatile SIZE_T s_CompatStrLen = 0;
static volatile SIZE_T s_CompatStrCap = 0;
static volatile BOOL   s_CompatDirty  = 0;
static volatile PVOID  s_CompatBase   = NULL;
static volatile PVOID  s_CompatSecond = NULL;

__declspec(dllexport)
void WINAPI SetAppCompatStringPointer(void *p1, void *p2)
{
    LOG("SetAppCompatStringPointer(p1=%p, p2=%p)", p1, p2);
    s_CompatStrLen = 0;
    s_CompatDirty  = 0;
    s_CompatBase   = p1;
    s_CompatSecond = p2;
    void *pBuf = (void*)s_CompatStrBuf;
    if(s_CompatStrCap > 0xf && pBuf) *(char*)pBuf = '\0';
    else g_szCompatBuf[0] = '\0';
    g_cbCompatBuf  = 0;
    g_bCompatBuilt = FALSE;
    LOG_VOID("SetAppCompatStringPointer");
}

static void CompatBuildCache(void)
{
    if(g_bCompatBuilt) return;
    EnterCriticalSection(&g_csCompat);
    if(!g_bCompatBuilt) {
        SIZE_T off=0;
        if(s_CompatBase) {
            SIZE_T n=strlen((const char*)s_CompatBase);
            if(n<COMPAT_BUF_SZ-2) { memcpy(g_szCompatBuf,s_CompatBase,n); off=n; g_szCompatBuf[off++]=';'; }
        }
        HKEY hk=NULL;
        if(RegOpenKeyExA((HKEY)0x80000001L,"Software\\Microsoft\\Direct3D",0,KEY_READ,&hk)==0){
            DWORD cb=(DWORD)(COMPAT_BUF_SZ-off-1),t=0;
            if(RegQueryValueExA(hk,"D3DBehaviors",NULL,&t,(LPBYTE)(g_szCompatBuf+off),&cb)==0&&t==1&&cb>0)
                off+=cb-1;
            RegCloseKey(hk);
        }
        g_szCompatBuf[off]='\0'; g_cbCompatBuf=off;
        g_bCompatBuilt=TRUE;
    }
    LeaveCriticalSection(&g_csCompat);
}

static BOOL CompatLookup(const char *pName,char *pOut,ulonglong *pcbOut)
{
    CompatBuildCache();
    SIZE_T nName=strlen(pName); const char *p=g_szCompatBuf; SIZE_T rem=g_cbCompatBuf;
    while(rem) {
        if(rem>nName && _strnicmp(p,pName,nName)==0 && p[nName]=='=') {
            const char *val=p+nName+1, *end=val;
            while(end<p+rem && *end!=';') end++;
            SIZE_T n=(SIZE_T)(end-val);
            if(pOut && *pcbOut>n) { memcpy(pOut,val,n); pOut[n]='\0'; }
            *pcbOut=(ulonglong)(n+1); return TRUE;
        }
        while(rem && *p!=';'){p++;rem--;} if(rem){p++;rem--;}
    }
    return FALSE;
}

__declspec(dllexport)
HRESULT WINAPI CompatString(const char *pName, ulonglong *pSz, char *pOut, char def)
{
    LOG("CompatString(\"%s\")",pName?pName:"NULL");
    if(!pName||!pSz) return 0;
    if(CompatLookup(pName,pOut,pSz)) { LOG("CompatString(\"%s\") found",pName); return 1; }
    if(def) { if(pOut){pOut[0]=def;pOut[1]='\0';} *pSz=2; return 1; }
    return 0;
}

__declspec(dllexport)
HRESULT WINAPI CompatValue(void *pName, void *ppOut, void *pUnused)
{
    char buf[100]; ulonglong cb=sizeof(buf);
    (void)pUnused;
    LOG("CompatValue(\"%s\")",(const char*)pName);
    if(!CompatLookup((const char*)pName,buf,&cb)) return 0;
    if(ppOut&&cb<0x65) *(long long*)ppOut=(long long)atoi(buf);
    return 1;
}

__declspec(dllexport)
void WINAPI DXGIDumpJournal(void *pCallback)
{
    typedef void(WINAPI*CB)(const char*);
    CB cb=(CB)pCallback;
    LOG("DXGIDumpJournal(cb=%p)",pCallback);
    if(!cb) return;
    int head=g_JournalHead;
    for(int i=0;i<JOURNAL_N;i++) {
        int idx=(head+i)&(JOURNAL_N-1);
        JOURNAL_ENTRY *e=&g_Journal[idx];
        char msg[LOG_BUF];
        _snprintf(msg,LOG_BUF-1,"DXGI Error %08x: (%u@%u) at %u - %s\r\n",
            e->type,e->arg1,e->arg2,e->arg3,e->msg);
        cb(msg);
    }
    LOG_VOID("DXGIDumpJournal");
}

__declspec(dllexport)
HRESULT WINAPI UpdateHMDEmulationStatus(char param_1)
{
    LOG("UpdateHMDEmulationStatus(%d)",(int)param_1);
    EnterCriticalSection(&g_csHMD);
    LeaveCriticalSection(&g_csHMD);
    LOG_VOID("UpdateHMDEmulationStatus");
    return S_OK;
}

static BOOL CALLBACK InitOnceCompatRes(PINIT_ONCE p,PVOID ctx,PVOID*pp2)
{
    (void)p;(void)ctx;(void)pp2;
    DEVMODEW dm; memset(&dm,0,sizeof(dm)); dm.dmSize=sizeof(dm);
    if(!EnumDisplaySettingsW(NULL,(DWORD)-1,&dm)) return TRUE;
    g_dwNativeW=dm.dmPelsWidth; g_dwNativeH=dm.dmPelsHeight;
    HKEY hk=NULL;
    if(RegOpenKeyExA((HKEY)0x80000002L,"Software\\Microsoft\\Shell\\LogicalResolution",
                     0,KEY_READ,&hk)==0) {
        DWORD v=0,cb=4;
        if(RegQueryValueExA(hk,"Height",NULL,NULL,(LPBYTE)&v,&cb)==0) g_dwLogicH=v; cb=4;
        if(RegQueryValueExA(hk,"Width", NULL,NULL,(LPBYTE)&v,&cb)==0) g_dwLogicW=v;
        RegCloseKey(hk);
        g_bCompatResQuirkSet=(g_dwLogicW&&g_dwLogicH&&
            (g_dwLogicW!=g_dwNativeW||g_dwLogicH!=g_dwNativeH));
    }
    return TRUE;
}

__declspec(dllexport)
HRESULT WINAPI ApplyCompatResolutionQuirking(void *pW, void *pH)
{
    DWORD *pw=(DWORD*)pW, *ph=(DWORD*)pH;
    LOG("ApplyCompatResolutionQuirking(w=%u,h=%u)",pw?*pw:0,ph?*ph:0);
    if(!pw||!ph) return S_OK;
    InitOnceExecuteOnce(&g_InitOnceCompat,InitOnceCompatRes,NULL,NULL);
    if(g_bCompatResQuirkSet && *pw==g_dwNativeW && *ph==g_dwNativeH)
        { *pw=g_dwLogicW; *ph=g_dwLogicH; LOG("  quirk: %ux%u→%ux%u",g_dwNativeW,g_dwNativeH,*pw,*ph); }
    else if(*pw==1080&&*ph==1920) { *pw=720;  *ph=1280; LOG("  quirk portrait 1080×1920→720×1280"); }
    else if(*pw==540 &&*ph==960 ) { *pw=480;  *ph=800;  LOG("  quirk 540×960→480×800"); }
    else if(*pw==1440&&*ph==2560) { *pw=720;  *ph=1280; LOG("  quirk 1440×2560→720×1280"); }
    LOG_VOID("ApplyCompatResolutionQuirking");
    return S_OK;
}

__declspec(dllexport)
HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void)
{
    LOG_ENTER("DXGIDeclareAdapterRemovalSupport");
    if(!g_D3DKMTSetProcessDeviceRemovalSupport) {
        LOG("  D3DKMTSetProcessDeviceRemovalSupport not available -> returning S_OK (stub)");
        LOG_LEAVE("DXGIDeclareAdapterRemovalSupport", S_OK);
        return S_OK;
    }
    D3DKMT_SETPROCESSDEVICEREMOVALSUPPORT args={1};
    NTSTATUS st=g_D3DKMTSetProcessDeviceRemovalSupport(&args);
    HRESULT hr=NtStatusToDxgiHR(st);
    LOG_LEAVE("DXGIDeclareAdapterRemovalSupport",hr);
    return hr;
}

__declspec(dllexport)
HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug)
{
    typedef HRESULT(WINAPI*PFN)(REFIID,void**);
    LOG("DXGIGetDebugInterface1(flags=0x%X,riid=%s)",Flags,GuidName(riid));
    (void)Flags;
    if(!ppDebug) { LOG_LEAVE("DXGIGetDebugInterface1",E_INVALIDARG); return E_INVALIDARG; }
    *ppDebug=NULL;

    HMODULE hDbg=NULL;
    if(g_pExeDir) {
        WCHAR path[MAX_PATH]; lstrcpyW(path,g_pExeDir); lstrcatW(path,L"DXGIDebug.dll");
        hDbg=LoadLibraryW(path);
    }
    if(!hDbg) hDbg=LoadLibraryExW(L"DXGIDebug.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!hDbg) {
        LOG("  DXGIDebug.dll not found");
        LOG_LEAVE("DXGIGetDebugInterface1",DXGI_ERROR_SDK_COMPONENT_MISSING);
        return DXGI_ERROR_SDK_COMPONENT_MISSING;
    }

    PFN pfn=(PFN)GetProcAddress(hDbg,"DXGIGetDebugInterface1");
    if(!pfn) pfn=(PFN)GetProcAddress(hDbg,"DXGIGetDebugInterface");
    if(!pfn) { FreeLibrary(hDbg); LOG_LEAVE("DXGIGetDebugInterface1",DXGI_ERROR_SDK_COMPONENT_MISSING); return DXGI_ERROR_SDK_COMPONENT_MISSING; }

    HRESULT hr=pfn(riid,ppDebug);
    if(FAILED(hr)) FreeLibrary(hDbg);
    LOG_LEAVE("DXGIGetDebugInterface1",hr); return hr;
}

__declspec(dllexport)
HRESULT WINAPI DXGIReportAdapterConfiguration(DWORD dwFlags)
{
    LOG("DXGIReportAdapterConfiguration(flags=0x%X)",dwFlags);
    if(!dwFlags) { LOG_LEAVE("DXGIReportAdapterConfiguration",E_INVALIDARG); return E_INVALIDARG; }
    LOG_VOID("DXGIReportAdapterConfiguration");
    return S_OK;
}

/* =========================================================
 * Initialization / cleanup
 * ========================================================= */
static BOOL Init_SystemDxgi(void)
{
    g_hDxgiSystem = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_hDxgiSystem) {
        WCHAR path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\dxgi.dll");
        g_hDxgiSystem = LoadLibraryW(path);
    }
    if (!g_hDxgiSystem) {
        OutputDebugStringA("[dxgi_win7] ERROR: system dxgi.dll not found!\n");
        return FALSE;
    }
    *(FARPROC*)&g_pfnFactory  = GetProcAddress(g_hDxgiSystem, "CreateDXGIFactory");
    *(FARPROC*)&g_pfnFactory1 = GetProcAddress(g_hDxgiSystem, "CreateDXGIFactory1");
    *(FARPROC*)&g_pfnFactory2 = GetProcAddress(g_hDxgiSystem, "CreateDXGIFactory2");
    LOG("system dxgi.dll loaded (F=%p F1=%p F2=%p)", (void*)g_pfnFactory, (void*)g_pfnFactory1, (void*)g_pfnFactory2);
    return g_pfnFactory != NULL;
}

static void Init_D3DKMT(void)
{
    HMODULE hG=GetModuleHandleA("gdi32.dll");
    if(!hG) hG=LoadLibraryW(L"gdi32.dll");
    if(!hG) return;
    *(FARPROC*)&g_D3DKMTSetProcessDeviceRemovalSupport=GetProcAddress(hG,"D3DKMTSetProcessDeviceRemovalSupport");
    *(FARPROC*)&g_D3DKMTCheckVidPnExclusiveOwnership  =GetProcAddress(hG,"D3DKMTCheckVidPnExclusiveOwnership");
    *(FARPROC*)&g_D3DKMTWaitForVerticalBlankEvent     =GetProcAddress(hG,"D3DKMTWaitForVerticalBlankEvent");
    *(FARPROC*)&g_D3DKMTWaitForVerticalBlankEvent2    =GetProcAddress(hG,"D3DKMTWaitForVerticalBlankEvent2");
    LOG("D3DKMT: SetProcDevRem=%p VBlank=%p VBlank2=%p",
        (void*)g_D3DKMTSetProcessDeviceRemovalSupport,
        (void*)g_D3DKMTWaitForVerticalBlankEvent,
        (void*)g_D3DKMTWaitForVerticalBlankEvent2);
}

static void Init_Ntdll(void)
{
    HMODULE hN=GetModuleHandleA("ntdll.dll");
    if(!hN) return;
    *(FARPROC*)&g_pfnRtlSubscribeWnf      =GetProcAddress(hN,"RtlSubscribeWnfStateChangeNotification");
    *(FARPROC*)&g_pfnRtlUnsubscribeWnf    =GetProcAddress(hN,"RtlUnsubscribeWnfStateChangeNotification");
    *(FARPROC*)&g_pfnRtlQueryWnfStateData  =GetProcAddress(hN,"RtlQueryWnfStateData");
    *(FARPROC*)&g_pfnRtlUnsubscribeWnfWait =GetProcAddress(hN,"RtlUnsubscribeWnfNotificationWaitForCompletion");
    LOG("WNF: Subscribe=%p Unsubscribe=%p Query=%p",
        (void*)g_pfnRtlSubscribeWnf,(void*)g_pfnRtlUnsubscribeWnf,(void*)g_pfnRtlQueryWnfStateData);
}

static void Init_ExePath(void)
{
    DWORD sz=0x104; LPWSTR p=NULL,pn;
    for(;;) {
        pn=(LPWSTR)HeapReAlloc(GetProcessHeap(),0,p,(SIZE_T)sz*sizeof(WCHAR));
        if(!pn){if(p)HeapFree(GetProcessHeap(),0,p);return;}
        p=pn;
        DWORD n=GetModuleFileNameW(NULL,p,sz-2);
        if(!n){HeapFree(GetProcessHeap(),0,p);return;}
        if(n<sz-2)break;
        sz+=0x104;
    }
    g_pExePath=p;
    DWORD i=(DWORD)lstrlenW(p);
    while(i>0&&p[i]!=L'\\')i--;
    if(i>0){
        g_pExeDir=(LPWSTR)HeapAlloc(GetProcessHeap(),0,(i+2)*sizeof(WCHAR));
        if(g_pExeDir){memcpy(g_pExeDir,p,i*sizeof(WCHAR));g_pExeDir[i]=L'\\';g_pExeDir[i+1]=0;}
    }
    LOG("ExePath: %ls", g_pExePath);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch(reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        DXGILog_Init();
        LOG("DllMain DLL_PROCESS_ATTACH");
        LOG("Build: " __DATE__ " " __TIME__ " | fence_off GetCompletedValue=0x%X SetEventOnCompletion=0x%X Signal(CQ)=0x%X CreateFence(Dev)=0x%X",
            D3D12FENCE_GetCompletedValue_OFF, D3D12FENCE_SetEventOnCompletion_OFF,
            D3D12CQ_Signal_OFF, D3D12DEV_CreateFence_OFF);
        InitializeCriticalSection(&g_csCompat);
        InitializeCriticalSection(&g_csHMD);
        Init_Ntdll();
        Init_D3DKMT();
        Init_ExePath();
        if(!Init_SystemDxgi())
            OutputDebugStringA("[dxgi_win7] Warning: system dxgi.dll not loaded (will fallback to built-in stubs)\n");
        LOG("DllMain OK");
        break;

    case DLL_PROCESS_DETACH:
        LOG("DllMain DLL_PROCESS_DETACH");
        if(g_hDxgiSystem){FreeLibrary(g_hDxgiSystem);g_hDxgiSystem=NULL;}
        if(g_pExePath){HeapFree(GetProcessHeap(),0,g_pExePath);g_pExePath=NULL;}
        if(g_pExeDir) {HeapFree(GetProcessHeap(),0,g_pExeDir); g_pExeDir=NULL;}
        DeleteCriticalSection(&g_csCompat);
        DeleteCriticalSection(&g_csHMD);
        if(g_hLogFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_hLogFile);
            g_hLogFile = INVALID_HANDLE_VALUE;
        }
        DeleteCriticalSection(&g_logCS);
        break;
    }
    return TRUE;
}