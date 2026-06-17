/*
 * dxgi_win7.c — Port de dxgi.dll (Windows 10) pour Windows 7 x64
 *
 * Compilé avec MinGW64 : x86_64-w64-mingw32-gcc -m64 -shared ...
 * Source unique + dxgi_win7.def
 */

/* =========================================================
 * Includes / types de base
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

#include <winternl.h>   /* fournit NTSTATUS, PVOID, ULONG, PULONG */
#include <dxgi1_2.h>     /* fournit DXGI_ADAPTER_DESC, DXGI_ADAPTER_DESC1, et déjà les DXGI_ERROR_* */

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
 * Définitions D3D11 manquantes (MinGW64 avec _WIN32_WINNT=0x0601)
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

static void DXGILog(const char *fmt, ...)
{
    char buf[LOG_BUF];
    va_list va;
    va_start(va, fmt);
    _vsnprintf(buf, LOG_BUF - 1, fmt, va);
    buf[LOG_BUF - 1] = '\0';
    va_end(va);
    OutputDebugStringA(buf);
}

/* Macros pratiques */
#define LOG(...)         DXGILog("[dxgi_win7] " __VA_ARGS__)
#define LOG_ENTER(fn)    DXGILog("[dxgi_win7] --> " fn)
#define LOG_LEAVE(fn,hr) DXGILog("[dxgi_win7] <-- " fn " = 0x%08X", (unsigned)(hr))
#define LOG_VOID(fn)     DXGILog("[dxgi_win7] <-- " fn " (void)")

/* =========================================================
 * HRESULT / erreurs DXGI
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
 * NtStatus → HRESULT (FUN_180003614 décompilé de la dxgi Win10)
 * Reproduit à l'identique la table de conversion.
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
 * GUIDs DXGI (publics SDK Microsoft, définis ici pour éviter
 * de dépendre de dxgi.h / initguid.h)
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
 * GUIDs D3D11 / D3D12
 * ========================================================= */
MAKE_GUID(IID_ID3D11Device,        0xDB6F6DDB,0xAC77,0x4E88,0x82,0x53,0x81,0x9D,0xF9,0xBB,0xF1,0x40);
MAKE_GUID(IID_ID3D11Device1,       0xA04BFB29,0x08EF,0x43D6,0xA4,0x9C,0xA9,0xBD,0xBD,0xCB,0xE6,0x86);
MAKE_GUID(IID_ID3D11Texture2D,     0x6F15AAF2,0xD208,0x4E89,0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C);
MAKE_GUID(IID_ID3D11Resource,      0xDC8E63F3,0xD12B,0x4952,0xB4,0x7B,0x5E,0x45,0x02,0x6A,0x86,0x2D);
MAKE_GUID(IID_ID3D12Device,        0x189819F1,0x1DB6,0x4B57,0xBE,0x54,0x18,0x21,0x33,0x9B,0x85,0xF7);
MAKE_GUID(IID_ID3D12CommandQueue,  0x0EC870A6,0x5D7E,0x4C22,0x8C,0xFC,0x5B,0xAA,0xE0,0x76,0x16,0xED);
MAKE_GUID(IID_ID3D12InfoQueue,     0x0742A90B,0xC387,0x483F,0xB9,0x46,0x30,0xA7,0xE4,0xE6,0x14,0x58);
MAKE_GUID(IID_ID3D12Resource,      0x696442BE,0xA72E,0x4059,0xBC,0x79,0x5B,0x5C,0x98,0x04,0x0F,0xAD);

typedef struct _D3D12_MESSAGE_MIN {
    UINT Category;
    UINT Severity;
    UINT ID;
    LPCSTR pDescription;
    SIZE_T DescriptionByteLength;
} D3D12_MESSAGE_MIN;

#define D3D12IQ_GetMessage_OFF          0x28
#define D3D12IQ_GetNumStoredMessages_OFF 0x40

static void D3D12_DumpInfoQueue(IUnknown *pD3D12Dev);

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
 * Structures D3D12 minimales
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

#define D3D12DEV_OpenSharedHandle_OFF        0x100
#define D3D12CQ_GetDevice_OFF   0x38


static BOOL GuidEq(REFIID a, const GUID *b)
{
    return (a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3
        && ((const UINT64*)a->Data4)[0] == ((const UINT64*)b->Data4)[0]);
}

/* GUID → string pour le logging */
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
 * Types de pointeurs de fonctions — dxgi_real.dll
 * ========================================================= */
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory )(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT,   REFIID, void**);

/* =========================================================
 * Types de pointeurs D3DKMT (gdi32.dll)
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

/* WNF */
typedef ULONG64 WNF_STATE_NAME;
typedef NTSTATUS (NTAPI *WNF_USER_CALLBACK)(WNF_STATE_NAME,ULONG,PVOID,PVOID,PVOID,ULONG);
typedef PVOID WNF_SUBSCRIPTION_HANDLE;
typedef NTSTATUS (NTAPI *PFN_RtlSubscribeWnf)(WNF_SUBSCRIPTION_HANDLE*,WNF_STATE_NAME,ULONG,PVOID,WNF_USER_CALLBACK,PVOID,ULONG,ULONG);
typedef NTSTATUS (NTAPI *PFN_RtlUnsubscribeWnf)(WNF_SUBSCRIPTION_HANDLE);
typedef NTSTATUS (NTAPI *PFN_RtlQueryWnfStateData)(PULONG,WNF_STATE_NAME,WNF_USER_CALLBACK,PVOID,PVOID);
typedef NTSTATUS (NTAPI *PFN_RtlUnsubscribeWnfWait)(WNF_SUBSCRIPTION_HANDLE);

/* =========================================================
 * État global
 * ========================================================= */

static HMODULE               g_hDxgiReal     = NULL;
static PFN_CreateDXGIFactory  g_pfnFactory    = NULL;
static PFN_CreateDXGIFactory1 g_pfnFactory1   = NULL;
static PFN_CreateDXGIFactory2 g_pfnFactory2   = NULL;

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

/* =========================================================
 * Private Data Store (pour les wrappers COM)
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
 * Macro vtable dispatch
 * ========================================================= */
#define VTBL(obj)           (*(void***)(obj))
#define VS(obj,off)         (VTBL(obj)[(off)/8])

static void D3D12_DumpInfoQueue(IUnknown *pD3D12Dev)
{
    typedef UINT64(STDMETHODCALLTYPE*PFN_GetNum)(void*);
    typedef HRESULT(STDMETHODCALLTYPE*PFN_GetMsg)(void*,UINT64,D3D12_MESSAGE_MIN*,SIZE_T*);
    typedef void(STDMETHODCALLTYPE*PFN_Clear)(void*);
    void *pIQ=NULL;
    if(!pD3D12Dev) return;
    if(FAILED(pD3D12Dev->lpVtbl->QueryInterface(pD3D12Dev,&IID_ID3D12InfoQueue,&pIQ)) || !pIQ) {
        LOG("  D3D12_DumpInfoQueue: ID3D12InfoQueue non disponible");
        return;
    }
    {
        UINT64 n=((PFN_GetNum)VS(pIQ,D3D12IQ_GetNumStoredMessages_OFF))(pIQ);
        UINT64 i;
        LOG("  D3D12_DumpInfoQueue: %llu message(s) stocké(s)",(unsigned long long)n);
        for(i=0;i<n;i++) {
            SIZE_T len=0;
            ((PFN_GetMsg)VS(pIQ,D3D12IQ_GetMessage_OFF))(pIQ,i,NULL,&len);
            if(len==0) continue;
            {
                D3D12_MESSAGE_MIN *m=(D3D12_MESSAGE_MIN*)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,len);
                if(!m) continue;
                if(SUCCEEDED(((PFN_GetMsg)VS(pIQ,D3D12IQ_GetMessage_OFF))(pIQ,i,m,&len))) {
                    LOG("  D3D12 MSG[%llu] cat=%u sev=%u id=%u: %s",
                        (unsigned long long)i,m->Category,m->Severity,m->ID,
                        m->pDescription?m->pDescription:"(null)");
                }
                HeapFree(GetProcessHeap(),0,m);
            }
        }
    }
    ((PFN_Clear)VS(pIQ,4*8))(pIQ);
    ((IUnknown*)pIQ)->lpVtbl->Release((IUnknown*)pIQ);
}


/* =========================================================
 * ==================== WRAPPERS COM =======================
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

typedef struct _D3D12Interop {
    void *pD3D11Dev;
    void *pD3D11Dev1;
    void *pD3D11Ctx;
    void *pD3D12Dev;
    UINT  BufferCount;
    UINT  Width, Height;
    DXGI_FORMAT Format;
    UINT  presentIndex;
    void *pD3D11Tex   [D3D12_INTEROP_MAX_BUFFERS];
    void *pD3D12Res   [D3D12_INTEROP_MAX_BUFFERS];
    void *pD3D11Shared[D3D12_INTEROP_MAX_BUFFERS];
    void *pD3D11Staging[D3D12_INTEROP_MAX_BUFFERS];
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

static HRESULT WrapSC_Create2(IUnknown *pReal, REFIID riid, void **ppOut, WrapSC **ppWrapOut)
{
    WrapSC *w; HRESULT hr;
    LOG("WrapSC_Create riid=%s", GuidName(riid));
    if (!ppOut) return E_INVALIDARG;
    *ppOut=NULL;
    if (!pReal) return E_INVALIDARG;
    w = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*w));
    if (!w) return E_OUTOFMEMORY;
    pReal->lpVtbl->AddRef(pReal);
    Base_Init(&w->base,(void*)g_SC_Vtbl,pReal);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain1,&w->pReal1);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain2,&w->pReal2);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain3,&w->pReal3);
    pReal->lpVtbl->QueryInterface(pReal,&IID_IDXGISwapChain4,&w->pReal4);
    hr = SC_QI(w,riid,ppOut);
    if(ppWrapOut) *ppWrapOut = SUCCEEDED(hr) ? w : NULL;
    SC_Release(w);
    return hr;
}
static HRESULT WrapSC_Create(IUnknown *pReal, REFIID riid, void **ppOut)
{
    return WrapSC_Create2(pReal,riid,ppOut,NULL);
}

static HRESULT STDMETHODCALLTYPE SC_QI(WrapSC *w, REFIID riid, void **ppv)
{
    LOG("IDXGISwapChain::QI(%s)", GuidName(riid));
    if (!ppv) return E_INVALIDARG; *ppv=NULL;
    
    if (GuidEq(riid,&IID_IUnknown_)      || GuidEq(riid,&IID_IDXGIObject)    ||
        GuidEq(riid,&IID_IDXGIDeviceSubObj)||GuidEq(riid,&IID_IDXGISwapChain))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    if (GuidEq(riid,&IID_IDXGISwapChain1) && (w->pReal1 || w->interop))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    if (GuidEq(riid,&IID_IDXGISwapChain2) && (w->pReal2 || w->interop))
        { *ppv=w; SC_AddRef(w); return S_OK; }
    /* IDXGISwapChain3 : toujours retourner le wrapper, même si pReal3 est NULL */
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
    HRESULT hr = ((PFN)VS(w->base.pReal,0x30))(w->base.pReal,riid,pp);
    LOG_LEAVE("IDXGISwapChain::GetParent",hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE SC_Present(WrapSC*w,UINT SyncInterval,UINT Flags)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT);
    HRESULT hr;
    LOG("IDXGISwapChain::Present(sync=%u flags=0x%X)",SyncInterval,Flags);
    if(w->interop) { D3D12Interop_Present(w,SyncInterval,Flags,&hr); LOG_LEAVE("IDXGISwapChain::Present[interop]",hr); return hr; }
    hr=((PFN)VS(w->base.pReal,0x40))(w->base.pReal,SyncInterval,Flags);
    LOG_LEAVE("IDXGISwapChain::Present",hr); return hr;
}

static HRESULT STDMETHODCALLTYPE SC_GetBuffer(WrapSC*w,UINT Buf,REFIID riid,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,REFIID,void**);
    HRESULT hr;
    LOG("IDXGISwapChain::GetBuffer(%u,%s)",Buf,GuidName(riid));
    if(!pp) return E_INVALIDARG; *pp=NULL;
    
    if(w->interop) { 
        hr=D3D12Interop_GetBuffer(w,Buf,riid,pp); 
        LOG_LEAVE("IDXGISwapChain::GetBuffer[interop]",hr); 
        return hr; 
    }
    
    /* Si le GUID est un GUID D3D11/DXGI standard, on le passe au pReal */
    if(GuidEq(riid, &IID_ID3D11Texture2D) || GuidEq(riid, &IID_IDXGISurface) ||
       GuidEq(riid, &IID_IDXGIResource) || GuidEq(riid, &IID_ID3D11Resource) ||
       GuidEq(riid, &IID_IDXGISurface1) || GuidEq(riid, &IID_IDXGISurface2)) {
        hr = ((PFN)VS(w->base.pReal,0x48))(w->base.pReal,Buf,riid,pp);
        LOG_LEAVE("IDXGISwapChain::GetBuffer",hr); 
        return hr;
    }
    
    /*
     * Pour les GUIDs inconnus (comme IDXGISwapChain3), on récupère d'abord
     * la texture en tant que IUnknown, puis on la retourne directement.
     * Le jeu pourra faire le QI lui-même.
     */
    void *pTex = NULL;
    hr = ((PFN)VS(w->base.pReal,0x48))(w->base.pReal, Buf, &IID_IUnknown_, &pTex);
    if(SUCCEEDED(hr) && pTex) {
        LOG("IDXGISwapChain::GetBuffer: retourne IUnknown=%p pour GUID inconnu %s", pTex, GuidName(riid));
        *pp = pTex;
        return S_OK;
    }
    
    /* Fallback : déléguer directement */
    hr = ((PFN)VS(w->base.pReal,0x48))(w->base.pReal,Buf,riid,pp);
    LOG_LEAVE("IDXGISwapChain::GetBuffer",hr); 
    return hr;
}

static HRESULT STDMETHODCALLTYPE SC_SetFullscreenState(WrapSC*w,BOOL fs,void*pTarget)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,BOOL,void*);
    LOG("IDXGISwapChain::SetFullscreenState(%d)",fs);
    HRESULT hr=((PFN)VS(w->base.pReal,0x50))(w->base.pReal,fs,pTarget);
    LOG_LEAVE("IDXGISwapChain::SetFullscreenState",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE SC_GetFullscreenState(WrapSC*w,BOOL*pFs,void**ppTarget)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,BOOL*,void**);
    LOG("IDXGISwapChain::GetFullscreenState");
    HRESULT hr=((PFN)VS(w->base.pReal,0x58))(w->base.pReal,pFs,ppTarget);
    LOG_LEAVE("IDXGISwapChain::GetFullscreenState",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE SC_GetDesc(WrapSC*w,void*pDesc)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    LOG("IDXGISwapChain::GetDesc");
    HRESULT hr=((PFN)VS(w->base.pReal,0x60))(w->base.pReal,pDesc);
    LOG_LEAVE("IDXGISwapChain::GetDesc",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE SC_ResizeBuffers(WrapSC*w,UINT C,UINT W,UINT H,UINT F,UINT Fl)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,UINT,UINT,UINT);
    LOG("IDXGISwapChain::ResizeBuffers(%u,%u,%u,fmt=%u,flags=%u)",C,W,H,F,Fl);
    HRESULT hr=((PFN)VS(w->base.pReal,0x68))(w->base.pReal,C,W,H,F,Fl);
    LOG_LEAVE("IDXGISwapChain::ResizeBuffers",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE SC_ResizeTarget(WrapSC*w,const void*pDesc)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,const void*);
    LOG("IDXGISwapChain::ResizeTarget");
    HRESULT hr=((PFN)VS(w->base.pReal,0x70))(w->base.pReal,pDesc);
    LOG_LEAVE("IDXGISwapChain::ResizeTarget",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE SC_GetContainingOutput(WrapSC*w,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void**);
    LOG("IDXGISwapChain::GetContainingOutput");
    if(!pp) return E_INVALIDARG; *pp=NULL;
    HRESULT hr=((PFN)VS(w->base.pReal,0x78))(w->base.pReal,pp);
    LOG_LEAVE("IDXGISwapChain::GetContainingOutput",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE SC_GetFrameStatistics(WrapSC*w,void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    HRESULT hr=((PFN)VS(w->base.pReal,0x80))(w->base.pReal,p);
    return hr;
}
static HRESULT STDMETHODCALLTYPE SC_GetLastPresentCount(WrapSC*w,UINT*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT*);
    return ((PFN)VS(w->base.pReal,0x88))(w->base.pReal,p);
}

#define SC1_CALL_1(off,a) do { \
    if (!w->pReal1) return E_NOTIMPL; \
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*, void*); \
    return ((PFN)VS(w->pReal1,(off)))(w->pReal1, (void*)(a)); } while(0)

static HRESULT STDMETHODCALLTYPE SC_GetDesc1(WrapSC*w,void*p)           { SC1_CALL_1(0x90,p); }
static HRESULT STDMETHODCALLTYPE SC_GetFullscreenDesc(WrapSC*w,void*p)  { SC1_CALL_1(0x98,p); }
static HRESULT STDMETHODCALLTYPE SC_GetHwnd(WrapSC*w,HWND*p)
{
    if(!w->pReal1) return E_NOTIMPL;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HWND*);
    LOG("IDXGISwapChain1::GetHwnd");
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
    LOG("IDXGISwapChain1::Present1(sync=%u,flags=0x%X)",si,fl);
    if(w->interop) { D3D12Interop_Present(w,si,fl,&hr); LOG_LEAVE("IDXGISwapChain1::Present1[interop]",hr); return hr; }
    if(!w->pReal1) return SC_Present(w,si,fl);
    hr=((PFN)VS(w->pReal1,0xb0))(w->pReal1,si,fl,p);
    LOG_LEAVE("IDXGISwapChain1::Present1",hr); return hr;
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
    LOG("IDXGISwapChain3::GetCurrentBackBufferIndex");
    if(w->interop) {
        UINT idx = w->interop->presentIndex % w->interop->BufferCount;
        LOG("  [interop] GetCurrentBackBufferIndex -> %u",idx);
        return idx;
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
    LOG("IDXGIAdapter::GetDesc");
    HRESULT hr=((PFN)VS(w->base.pReal,0x40))(w->base.pReal,p);
    LOG_LEAVE("IDXGIAdapter::GetDesc",hr); return hr;
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
    LOG("IDXGIAdapter1::GetDesc1");
    if(!w->pReal1) return E_NOTIMPL;
    HRESULT hr=((PFN)VS(w->pReal1,0x50))(w->pReal1,p);
    LOG_LEAVE("IDXGIAdapter1::GetDesc1",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_GetDesc2(WrapAdapter*w,void*p)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,void*);
    LOG("IDXGIAdapter2::GetDesc2");
    if(!w->pReal2) return DXGI_ERROR_UNSUPPORTED;
    HRESULT hr=((PFN)VS(w->pReal2,0x58))(w->pReal2,p);
    LOG_LEAVE("IDXGIAdapter2::GetDesc2",hr); return hr;
}
static HRESULT STDMETHODCALLTYPE WA_RegisterVideoMemoryBudgetChangeNotification(WrapAdapter*w,HANDLE h,DWORD*p)
{
    LOG("IDXGIAdapter3::RegisterVideoMemoryBudgetChangeNotification (stub)");
    if(!w->pReal3) return DXGI_ERROR_UNSUPPORTED;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,HANDLE,DWORD*);
    return ((PFN)VS(w->pReal3,0x60))(w->pReal3,h,p);
}
static void STDMETHODCALLTYPE WA_UnregisterVideoMemoryBudgetChangeNotification(WrapAdapter*w,DWORD c)
{
    LOG("IDXGIAdapter3::UnregisterVideoMemoryBudgetChangeNotification (stub)");
    if(!w->pReal3) return;
    typedef void(STDMETHODCALLTYPE*PFN)(void*,DWORD);
    ((PFN)VS(w->pReal3,0x68))(w->pReal3,c);
}
static HRESULT STDMETHODCALLTYPE WA_QueryVideoMemoryInfo(WrapAdapter*w,UINT node,UINT sg,void*p)
{
    LOG("IDXGIAdapter3::QueryVideoMemoryInfo(node=%u,seg=%u)",node,sg);
    if(!w->pReal3) {
        typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,void*);
        HRESULT (*pfn)(void*,UINT,UINT,void*) = NULL;
        HMODULE hGdi = GetModuleHandleA("gdi32.dll");
        if(hGdi) *(FARPROC*)&pfn = GetProcAddress(hGdi,"D3DKMTQueryVideoMemoryInfo");
        if(pfn) return pfn(w->base.pReal,node,sg,p);
        return DXGI_ERROR_UNSUPPORTED;
    }
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,void*);
    return ((PFN)VS(w->pReal3,0x70))(w->pReal3,node,sg,p);
}
static HRESULT STDMETHODCALLTYPE WA_SetVideoMemoryReservation(WrapAdapter*w,UINT node,UINT sg,UINT64 r)
{
    LOG("IDXGIAdapter3::SetVideoMemoryReservation(node=%u,seg=%u,res=%llu)",node,sg,(unsigned long long)r);
    if(!w->pReal3) return DXGI_ERROR_UNSUPPORTED;
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,UINT,UINT,UINT64);
    return ((PFN)VS(w->pReal3,0x78))(w->pReal3,node,sg,r);
}
static HRESULT STDMETHODCALLTYPE WA_GetDesc3(WrapAdapter*w,void*p)
{
    LOG("IDXGIAdapter4::GetDesc3");
    if(!w->pReal4) return DXGI_ERROR_UNSUPPORTED;
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
    if(GuidEq(riid,&IID_IDXGIAdapter1)&&w->pReal1){ *ppv=w; WA_AddRef(w); return S_OK; }
    if(GuidEq(riid,&IID_IDXGIAdapter2)&&w->pReal2){ *ppv=w; WA_AddRef(w); return S_OK; }
    if(GuidEq(riid,&IID_IDXGIAdapter3)&&w->pReal3){ *ppv=w; WA_AddRef(w); return S_OK; }
    if(GuidEq(riid,&IID_IDXGIAdapter4)&&w->pReal4){ *ppv=w; WA_AddRef(w); return S_OK; }
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
    LOG_GUID("IDXGIFactory::QI unknown -> passthrough vers pReal",riid);
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
static HRESULT STDMETHODCALLTYPE WF_CreateSwapChain(WrapFactory*w,IUnknown*pDev,void*pDesc,void**pp)
{
    typedef HRESULT(STDMETHODCALLTYPE*PFN)(void*,IUnknown*,void*,void**);
    LOG("IDXGIFactory::CreateSwapChain");
    if(!pp) return E_INVALIDARG; *pp=NULL;
    void *pSC=NULL;
    HRESULT hr=((PFN)VS(w->pRealF,0x50))(w->pRealF,pDev,pDesc,&pSC);
    if(SUCCEEDED(hr)) hr=WrapSC_Create((IUnknown*)pSC,&IID_IDXGISwapChain,pp);
    if(pSC) { ((IUnknown*)pSC)->lpVtbl->Release((IUnknown*)pSC); pSC=NULL; }
    LOG_LEAVE("IDXGIFactory::CreateSwapChain",hr); return hr;
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
        LOG("D3D12_CreateSharedRT: retry OpenSharedHandle avec IID_IUnknown_...");
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

static D3D12Interop* D3D12Interop_Create(IUnknown *pD3D12CQ, HWND hwnd,
    const void *pDescVoid, IUnknown **ppRealSC)
{
    const DXGI_SWAP_CHAIN_DESC1 *pDesc = (const DXGI_SWAP_CHAIN_DESC1*)pDescVoid;
    D3D12Interop *it = NULL;
    HMODULE hD3D11 = NULL;
    PFN_D3D11CreateDevice pfnCreate = NULL;
    void *pD3D11Dev = NULL, *pD3D11Ctx = NULL;
    IUnknown *pD3D12Dev = NULL;
    HRESULT hr;
    UINT i;

    LOG("D3D12Interop_Create: entrée");
    if(!pDesc) { 
        LOG("D3D12Interop_Create: pDesc==NULL, abandon");
        return NULL; 
    }

    LOG("D3D12Interop_Create: %ux%u fmt=%u BufferCount=%u Flags=0x%X",
        pDesc->Width, pDesc->Height, (unsigned)pDesc->Format, 
        pDesc->BufferCount, pDesc->Flags);

    if(pDesc->BufferCount == 0 || pDesc->BufferCount > D3D12_INTEROP_MAX_BUFFERS) {
        LOG("D3D12Interop_Create: BufferCount=%u hors limites (max %u)", 
            pDesc->BufferCount, D3D12_INTEROP_MAX_BUFFERS);
        return NULL;
    }

    LOG("D3D12Interop_Create: appel D3D12_GetDeviceFromQueue");
    pD3D12Dev = D3D12_GetDeviceFromQueue(pD3D12CQ);
    if(!pD3D12Dev) { 
        LOG("D3D12Interop_Create: impossible de remonter au ID3D12Device");
        return NULL; 
    }
    LOG("D3D12Interop_Create: pD3D12Dev=%p", (void*)pD3D12Dev);

    /* Charger d3d11.dll depuis System32 */
    LOG("D3D12Interop_Create: chargement de d3d11.dll");
    {
        WCHAR sysdir[MAX_PATH]; 
        WCHAR path[MAX_PATH+32];
        UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
        if(n > 0 && n < MAX_PATH) {
            _snwprintf(path, MAX_PATH+31, L"%s\\d3d11.dll", sysdir);
            path[MAX_PATH+31] = 0;
            hD3D11 = LoadLibraryW(path);
            LOG("D3D12Interop_Create: LoadLibraryW(%S) = %p", path, (void*)hD3D11);
        }
    }
    if(!hD3D11) {
        hD3D11 = GetModuleHandleW(L"d3d11.dll");
        LOG("D3D12Interop_Create: GetModuleHandleW(d3d11.dll) = %p", (void*)hD3D11);
    }
    if(!hD3D11) { 
        LOG("D3D12Interop_Create: LoadLibrary d3d11.dll échoué"); 
        goto fail; 
    }
    
    pfnCreate = (PFN_D3D11CreateDevice)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if(!pfnCreate) { 
        LOG("D3D12Interop_Create: GetProcAddress D3D11CreateDevice échoué"); 
        goto fail; 
    }
    LOG("D3D12Interop_Create: pfnCreate=%p", (void*)pfnCreate);

    /* Créer le device D3D11 sur le même adapter que D3D12 */
    LOG("D3D12Interop_Create: création du device D3D11");
    {
        D3D_FEATURE_LEVEL_ levels[2] = {D3D_FEATURE_LEVEL_11_1_, D3D_FEATURE_LEVEL_11_0_};
        D3D_FEATURE_LEVEL_ outLevel = 0;
        void *pDXGIAdapterForD3D11 = NULL;

        /* Obtenir le LUID de l'adapter D3D12 */
        {
            typedef void (STDMETHODCALLTYPE *PFN_GetAdapterLuid)(void*, LUID*);
            LUID d3d12Luid = {0,0};
            ((PFN_GetAdapterLuid)VS(pD3D12Dev, 0x158))(pD3D12Dev, &d3d12Luid);
            LOG("D3D12Interop_Create: D3D12 adapter LUID={%08X,%08X}",
                (unsigned)d3d12Luid.LowPart, (unsigned)d3d12Luid.HighPart);

            /* Chercher l'adapter DXGI avec ce LUID */
            if ((d3d12Luid.LowPart || d3d12Luid.HighPart) && g_pfnFactory1) {
                void *pTmpFactory = NULL;
                LOG("D3D12Interop_Create: recherche de l'adapter DXGI correspondant");
                if (SUCCEEDED(g_pfnFactory1(&IID_IDXGIFactory1, &pTmpFactory)) && pTmpFactory) {
                    typedef HRESULT(STDMETHODCALLTYPE*PFN_EnumAdapters1)(void*,UINT,void**);
                    for(UINT ai = 0; ; ai++) {
                        void *pAdap = NULL;
                        HRESULT har = ((PFN_EnumAdapters1)VS(pTmpFactory, 0x60))(pTmpFactory, ai, &pAdap);
                        if(FAILED(har) || !pAdap) {
                            LOG("D3D12Interop_Create: fin de l'énumération des adapters (ai=%u)", ai);
                            break;
                        }
                        /* GetDesc1 = offset 0x50 dans IDXGIAdapter1 vtable */
                        typedef HRESULT(STDMETHODCALLTYPE*PFN_GetDesc1)(void*,void*);
                        BYTE descBuf[sizeof(DXGI_ADAPTER_DESC) * 2];
                        if(SUCCEEDED(((PFN_GetDesc1)VS(pAdap, 0x50))(pAdap, descBuf))) {
                            /* LUID à l'offset 296 (0x128) dans DXGI_ADAPTER_DESC1 */
                            LUID *pL = (LUID*)(descBuf + 296);
                            LOG("D3D12Interop_Create: adapter[%u] LUID={%08X,%08X}",
                                ai, (unsigned)pL->LowPart, (unsigned)pL->HighPart);
                            if(pL->LowPart == d3d12Luid.LowPart && pL->HighPart == d3d12Luid.HighPart) {
                                pDXGIAdapterForD3D11 = pAdap;
                                LOG("D3D12Interop_Create: adapter D3D11 sélectionné = adapter[%u]", ai);
                                break;
                            }
                        }
                        ((IUnknown*)pAdap)->lpVtbl->Release((IUnknown*)pAdap);
                    }
                    ((IUnknown*)pTmpFactory)->lpVtbl->Release((IUnknown*)pTmpFactory);
                }
            }
            if(!pDXGIAdapterForD3D11) {
                LOG("D3D12Interop_Create: adapter D3D12 non trouvé, utilisation de l'adapter par défaut");
            }
        }

        /* Créer le device D3D11 */
        {
            UINT flags = 0x20; /* D3D11_CREATE_DEVICE_BGRA_SUPPORT */
            D3D_DRIVER_TYPE_ dtype = pDXGIAdapterForD3D11
                ? D3D_DRIVER_TYPE_UNKNOWN_  /* obligatoire si pAdapter != NULL */
                : D3D_DRIVER_TYPE_HARDWARE_;

            LOG("D3D12Interop_Create: appel D3D11CreateDevice (flags=0x%X, dtype=%d)", flags, (int)dtype);
            hr = pfnCreate((IUnknown*)pDXGIAdapterForD3D11, dtype, NULL, flags,
                levels, 2, D3D11_SDK_VERSION_, &pD3D11Dev, &outLevel, &pD3D11Ctx);
            LOG("D3D12Interop_Create: D3D11CreateDevice (1er essai) = 0x%08X", (unsigned)hr);

            if(FAILED(hr)) {
                LOG("D3D12Interop_Create: retry D3D11CreateDevice sans D3D_FEATURE_LEVEL list");
                hr = pfnCreate((IUnknown*)pDXGIAdapterForD3D11, dtype, NULL, flags,
                    NULL, 0, D3D11_SDK_VERSION_, &pD3D11Dev, &outLevel, &pD3D11Ctx);
                LOG("D3D12Interop_Create: D3D11CreateDevice (2ème essai) = 0x%08X", (unsigned)hr);
            }
            if(FAILED(hr) && pDXGIAdapterForD3D11) {
                LOG("D3D12Interop_Create: retry D3D11CreateDevice sans adapter explicite");
                hr = pfnCreate(NULL, D3D_DRIVER_TYPE_HARDWARE_, NULL, flags,
                    NULL, 0, D3D11_SDK_VERSION_, &pD3D11Dev, &outLevel, &pD3D11Ctx);
                LOG("D3D12Interop_Create: D3D11CreateDevice (3ème essai) = 0x%08X", (unsigned)hr);
            }
            if(pDXGIAdapterForD3D11) {
                ((IUnknown*)pDXGIAdapterForD3D11)->lpVtbl->Release((IUnknown*)pDXGIAdapterForD3D11);
            }
        }

        LOG("D3D12Interop_Create: D3D11CreateDevice final = 0x%08X pDev=%p level=0x%X",
            (unsigned)hr, pD3D11Dev, (unsigned)outLevel);
        if(FAILED(hr) || !pD3D11Dev) {
            LOG("D3D12Interop_Create: échec de D3D11CreateDevice");
            goto fail;
        }
    }

    /* Créer la swapchain D3D11 via le device caché */
    LOG("D3D12Interop_Create: création de la swapchain D3D11");
    {
        void *pDXGIDev = NULL, *pAdapter = NULL, *pFactory2 = NULL, *pRealSC = NULL;
        typedef HRESULT(STDMETHODCALLTYPE*PFN_GetParent)(void*, REFIID, void**);
        typedef HRESULT(STDMETHODCALLTYPE*PFN_GetAdapter)(void*, void**);
        typedef HRESULT(STDMETHODCALLTYPE*PFN_CreateSwapChainForHwnd)(
            void*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const void*, void*, void**);

        LOG("D3D12Interop_Create: QI(IDXGIDevice)");
        hr = ((IUnknown*)pD3D11Dev)->lpVtbl->QueryInterface((IUnknown*)pD3D11Dev, &IID_IDXGIDevice, &pDXGIDev);
        LOG("D3D12Interop_Create: QI(IDXGIDevice) = 0x%08X pDXGIDev=%p", (unsigned)hr, (void*)pDXGIDev);
        if(FAILED(hr)) {
            LOG("D3D12Interop_Create: QI(IDXGIDevice) échoué");
            goto fail;
        }

        LOG("D3D12Interop_Create: IDXGIDevice::GetAdapter");
        hr = ((PFN_GetAdapter)VS(pDXGIDev, 0x38))(pDXGIDev, &pAdapter);
        ((IUnknown*)pDXGIDev)->lpVtbl->Release((IUnknown*)pDXGIDev);
        LOG("D3D12Interop_Create: IDXGIDevice::GetAdapter = 0x%08X pAdapter=%p", (unsigned)hr, (void*)pAdapter);
        if(FAILED(hr) || !pAdapter) {
            LOG("D3D12Interop_Create: GetAdapter échoué");
            goto fail;
        }

        LOG("D3D12Interop_Create: IDXGIObject::GetParent vers IDXGIFactory2");
        hr = ((PFN_GetParent)VS(pAdapter, 0x30))(pAdapter, &IID_IDXGIFactory2, &pFactory2);
        ((IUnknown*)pAdapter)->lpVtbl->Release((IUnknown*)pAdapter);
        LOG("D3D12Interop_Create: GetParent(IDXGIFactory2) = 0x%08X pFactory2=%p", (unsigned)hr, (void*)pFactory2);
        if(FAILED(hr) || !pFactory2) {
            LOG("D3D12Interop_Create: GetParent échoué");
            goto fail;
        }

        /* Créer la swapchain avec des paramètres compatibles Win7 */
        {
            DXGI_SWAP_CHAIN_DESC1 d = *pDesc;
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            d.Scaling    = DXGI_SCALING_STRETCH;
            d.AlphaMode  = DXGI_ALPHA_MODE_UNSPECIFIED;
            d.Flags      = 0;
            d.BufferCount = 1; /* DISCARD impose BufferCount<=1 sur certains drivers */
            LOG("D3D12Interop_Create: appel CreateSwapChainForHwnd avec SwapEffect=DISCARD, BufferCount=1");
            hr = ((PFN_CreateSwapChainForHwnd)VS(pFactory2, 0x78))(
                pFactory2, (IUnknown*)pD3D11Dev, hwnd, &d, NULL, NULL, &pRealSC);
        }
        ((IUnknown*)pFactory2)->lpVtbl->Release((IUnknown*)pFactory2);
        LOG("D3D12Interop_Create: CreateSwapChainForHwnd(D3D11) = 0x%08X pRealSC=%p", (unsigned)hr, (void*)pRealSC);
        if(FAILED(hr) || !pRealSC) {
            LOG("D3D12Interop_Create: CreateSwapChainForHwnd échoué");
            goto fail;
        }

        *ppRealSC = (IUnknown*)pRealSC;
        LOG("D3D12Interop_Create: swapchain D3D11 créée avec succès");
    }

    /* Allouer le contexte interop */
    LOG("D3D12Interop_Create: allocation du contexte interop");
    it = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*it));
    if(!it) {
        LOG("D3D12Interop_Create: HeapAlloc échoué");
        goto fail;
    }
    it->pD3D11Dev = pD3D11Dev;
    it->pD3D11Ctx = pD3D11Ctx;
    it->pD3D12Dev = pD3D12Dev;
    pD3D12Dev = NULL; /* la ref est transférée à it */
    it->BufferCount = pDesc->BufferCount;
    it->Width = pDesc->Width;
    it->Height = pDesc->Height;
    it->Format = pDesc->Format;
    it->presentIndex = 0;

    LOG("D3D12Interop_Create: QueryInterface ID3D11Device1");
    ((IUnknown*)pD3D11Dev)->lpVtbl->QueryInterface((IUnknown*)pD3D11Dev, &IID_ID3D11Device1, &it->pD3D11Dev1);
    LOG("D3D12Interop_Create: pD3D11Dev1=%p", (void*)it->pD3D11Dev1);

    /* Créer les ressources partagées */
    LOG("D3D12Interop_Create: création des %u buffers partagés", it->BufferCount);
    for(i = 0; i < it->BufferCount; i++) {
        void *pD3D11Tex = NULL, *pD3D12Res = NULL;
        LOG("D3D12Interop_Create: création du buffer[%u]", i);
        hr = D3D12_CreateSharedRT(it->pD3D11Dev, it->pD3D12Dev,
                                  it->Width, it->Height, it->Format,
                                  &pD3D11Tex, &pD3D12Res);
        LOG("D3D12Interop_Create: D3D12_CreateSharedRT[%u] = 0x%08X pTex=%p pRes=%p",
            i, (unsigned)hr, (void*)pD3D11Tex, (void*)pD3D12Res);
        if(FAILED(hr)) {
            LOG("D3D12Interop_Create: D3D12_CreateSharedRT[%u] échoué", i);
            goto fail;
        }
        it->pD3D11Tex[i] = pD3D11Tex;
        it->pD3D11Shared[i] = pD3D11Tex;
        it->pD3D12Res[i] = pD3D12Res;
        LOG("D3D12Interop_Create: buffer[%u] pD3D11Tex=%p pD3D12Res=%p", 
            i, (void*)pD3D11Tex, (void*)pD3D12Res);

        /* Créer la texture staging avec USAGE_STAGING pour permettre Map/Unmap */
        LOG("D3D12Interop_Create: création de la staging[%u] (USAGE_STAGING + CPU_ACCESS_READ)", i);
        {
            typedef HRESULT(STDMETHODCALLTYPE*PFN_CreateTexture2D)(void*, const D3D11_TEXTURE2D_DESC_MIN*, const void*, void**);
            D3D11_TEXTURE2D_DESC_MIN sd; 
            ZeroMemory(&sd, sizeof(sd));
            sd.Width       = it->Width;
            sd.Height      = it->Height;
            sd.MipLevels   = 1;
            sd.ArraySize   = 1;
            sd.Format      = it->Format;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            /* CHANGEMENT CRITIQUE : USAGE_STAGING + CPU_ACCESS_READ pour permettre Map */
            sd.Usage       = D3D11_USAGE_STAGING;
            sd.BindFlags   = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags   = 0;
            
            void *pStaging = NULL;
            HRESULT hrs = ((PFN_CreateTexture2D)VS(it->pD3D11Dev, 0x28))(it->pD3D11Dev, &sd, NULL, &pStaging);
            LOG("D3D12Interop_Create: CreateTexture2D staging[%u] = 0x%08X pStaging=%p", 
                i, (unsigned)hrs, (void*)pStaging);
            if(FAILED(hrs) || !pStaging) {
                LOG("D3D12Interop_Create: CreateTexture2D staging[%u] échoué", i);
                goto fail;
            }
            it->pD3D11Staging[i] = pStaging;
            LOG("D3D12Interop_Create: staging[%u] créée avec succès (USAGE_STAGING)", i);
        }
    }

    LOG("D3D12Interop_Create: OK, %u buffers %ux%u", it->BufferCount, it->Width, it->Height);
    return it;

fail:
    LOG("D3D12Interop_Create: échec, nettoyage");
    if(pD3D12Dev) pD3D12Dev->lpVtbl->Release(pD3D12Dev);
    if(it) D3D12Interop_Destroy(it);
    else {
        if(pD3D11Ctx) ((IUnknown*)pD3D11Ctx)->lpVtbl->Release((IUnknown*)pD3D11Ctx);
        if(pD3D11Dev) ((IUnknown*)pD3D11Dev)->lpVtbl->Release((IUnknown*)pD3D11Dev);
    }
    LOG("D3D12Interop_Create: retour NULL");
    return NULL;
}

static void D3D12Interop_Destroy(D3D12Interop *it)
{
    UINT i;
    if(!it) return;
    for(i=0;i<it->BufferCount;i++) {
        if(it->pD3D12Res[i])    ((IUnknown*)it->pD3D12Res[i])->lpVtbl->Release((IUnknown*)it->pD3D12Res[i]);
        if(it->pD3D11Tex[i])    ((IUnknown*)it->pD3D11Tex[i])->lpVtbl->Release((IUnknown*)it->pD3D11Tex[i]);
        if(it->pD3D11Staging[i])((IUnknown*)it->pD3D11Staging[i])->lpVtbl->Release((IUnknown*)it->pD3D11Staging[i]);
    }
    if(it->pD3D11Dev1) ((IUnknown*)it->pD3D11Dev1)->lpVtbl->Release((IUnknown*)it->pD3D11Dev1);
    if(it->pD3D11Ctx)  ((IUnknown*)it->pD3D11Ctx)->lpVtbl->Release((IUnknown*)it->pD3D11Ctx);
    if(it->pD3D11Dev)  ((IUnknown*)it->pD3D11Dev)->lpVtbl->Release((IUnknown*)it->pD3D11Dev);
    if(it->pD3D12Dev)  ((IUnknown*)it->pD3D12Dev)->lpVtbl->Release((IUnknown*)it->pD3D12Dev);
    HeapFree(GetProcessHeap(),0,it);
}

static HRESULT D3D12Interop_GetBuffer(WrapSC *w, UINT Buf, REFIID riid, void **pp)
{
    D3D12Interop *it = w->interop;
    
    if(!it) return E_NOTIMPL;
    if(Buf >= it->BufferCount) return DXGI_ERROR_INVALID_CALL;
    
    LOG("D3D12Interop_GetBuffer(%u, %s)", Buf, GuidName(riid));
    
    /* Si on demande ID3D12Resource, on retourne directement */
    if(GuidEq(riid, &IID_ID3D12Resource) || GuidEq(riid, &IID_IUnknown_)) {
        if(!it->pD3D12Res[Buf]) return E_FAIL;
        ((IUnknown*)it->pD3D12Res[Buf])->lpVtbl->AddRef((IUnknown*)it->pD3D12Res[Buf]);
        *pp = it->pD3D12Res[Buf];
        LOG("D3D12Interop_GetBuffer(%u) -> pD3D12Res=%p", Buf, *pp);
        return S_OK;
    }
    
    /* Si on demande IDXGISurface ou ID3D11Texture2D, on retourne la texture D3D11 */
    if(GuidEq(riid, &IID_IDXGISurface) || GuidEq(riid, &IID_ID3D11Texture2D) ||
       GuidEq(riid, &IID_IDXGIResource) || GuidEq(riid, &IID_ID3D11Resource) ||
       GuidEq(riid, &IID_IDXGISurface1) || GuidEq(riid, &IID_IDXGISurface2)) {
        if(!it->pD3D11Shared[Buf]) return E_FAIL;
        ((IUnknown*)it->pD3D11Shared[Buf])->lpVtbl->AddRef((IUnknown*)it->pD3D11Shared[Buf]);
        *pp = it->pD3D11Shared[Buf];
        LOG("D3D12Interop_GetBuffer(%u) -> pD3D11Shared=%p", Buf, *pp);
        return S_OK;
    }
    
    LOG("D3D12Interop_GetBuffer: interface %s non supportée", GuidName(riid));
    return E_NOINTERFACE;
}

static HRESULT D3D12Interop_Present(WrapSC *w, UINT SyncInterval, UINT Flags, HRESULT *outHr)
{
    D3D12Interop *it = w->interop;
    typedef HRESULT(STDMETHODCALLTYPE*PFN_Present)(void*,UINT,UINT);
    typedef HRESULT(STDMETHODCALLTYPE*PFN_GetBuffer)(void*,UINT,REFIID,void**);
    typedef void(STDMETHODCALLTYPE*PFN_CopySubresourceRegion)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const void*);
    typedef void(STDMETHODCALLTYPE*PFN_Flush)(void*);

    /* D3D11_BOX manuelle */
    typedef struct { UINT left, top, front, right, bottom, back; } D3D11_BOX_MANUAL;
    D3D11_BOX_MANUAL box = {0, 0, 0, it->Width, it->Height, 1};

    UINT idx;
    HRESULT hr;
    void *pRealBackBuf = NULL;
    void *pCtx = NULL;

    if (!it) return E_NOTIMPL;
    idx = it->presentIndex % it->BufferCount;

    LOG("D3D12Interop_Present: idx=%u", idx);
    if (!it->pD3D11Ctx) { *outHr = E_FAIL; return E_FAIL; }
    if (!it->pD3D11Shared[idx]) { *outHr = E_FAIL; return E_FAIL; }
    if (!it->pD3D11Staging[idx]) { *outHr = E_FAIL; return E_FAIL; }

    hr = ((PFN_GetBuffer)VS(w->base.pReal, 0x48))(w->base.pReal, 0, &IID_ID3D11Texture2D, &pRealBackBuf);
    LOG("D3D12Interop_Present: GetBuffer(0) = 0x%08X pRealBackBuf=%p", (unsigned)hr, (void*)pRealBackBuf);
    if (FAILED(hr) || !pRealBackBuf) { *outHr = FAILED(hr) ? hr : E_FAIL; return *outHr; }

    {
        typedef void(STDMETHODCALLTYPE*PFN_GetImmCtx)(void*, void**);
        ((PFN_GetImmCtx)VS(it->pD3D11Dev, 0x140))(it->pD3D11Dev, &pCtx);
    }
    if (!pCtx) pCtx = it->pD3D11Ctx;

    /*
     * Étape 1 : Copier de la texture MISC_SHARED vers la staging
     * en utilisant CopySubresourceRegion avec box=NULL.
     */
    LOG("D3D12Interop_Present: --> CopySubresourceRegion shared→staging (box=NULL)");
    ((PFN_CopySubresourceRegion)VS(pCtx, 0x130))(pCtx,
        it->pD3D11Staging[idx], 0, 0, 0, 0,
        it->pD3D11Shared[idx], 0, NULL);
    LOG("D3D12Interop_Present: CopySubresourceRegion shared→staging OK");

    /*
     * Étape 2 : Copier de la staging vers le back buffer via CopySubresourceRegion
     * avec une box définie. C'est plus robuste que CopyResource sur Win7.
     */
    LOG("D3D12Interop_Present: --> CopySubresourceRegion staging→backbuf (box définie)");
    ((PFN_CopySubresourceRegion)VS(pCtx, 0x130))(pCtx,
        pRealBackBuf, 0, 0, 0, 0,
        it->pD3D11Staging[idx], 0, (const void*)&box);
    LOG("D3D12Interop_Present: CopySubresourceRegion staging→backbuf OK");

    /* Flush */
    ((PFN_Flush)VS(pCtx, 0x138))(pCtx);

    ((IUnknown*)pRealBackBuf)->lpVtbl->Release((IUnknown*)pRealBackBuf);
    if (pCtx != it->pD3D11Ctx) ((IUnknown*)pCtx)->lpVtbl->Release((IUnknown*)pCtx);

    hr = ((PFN_Present)VS(w->base.pReal, 0x40))(w->base.pReal, SyncInterval, Flags);
    LOG("D3D12Interop_Present: Present réel = 0x%08X", (unsigned)hr);
    it->presentIndex++;
    *outHr = hr;
    return hr;
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

    /* Détection D3D12 */
    {
        void *pCQ=NULL;
        HRESULT hrcq = pDev ? pDev->lpVtbl->QueryInterface(pDev,&IID_ID3D12CommandQueue,&pCQ) : E_POINTER;
        if(SUCCEEDED(hrcq) && pCQ) {
            IUnknown *pRealSC=NULL;
            D3D12Interop *it;
            LOG("  CreateSwapChainForHwnd: ID3D12CommandQueue détectée -> interop D3D11");
            it = D3D12Interop_Create((IUnknown*)pCQ, hwnd, pD, &pRealSC);
            ((IUnknown*)pCQ)->lpVtbl->Release((IUnknown*)pCQ);
            if(!it || !pRealSC) {
                LOG("  CreateSwapChainForHwnd: D3D12Interop_Create a échoué");
                if(pRealSC) pRealSC->lpVtbl->Release(pRealSC);
                LOG_LEAVE("IDXGIFactory2::CreateSwapChainForHwnd",E_FAIL);
                return E_FAIL;
            }
            {
                WrapSC *wsc=NULL;
                HRESULT hr2=WrapSC_Create2(pRealSC,&IID_IDXGISwapChain1,pp,&wsc);
                pRealSC->lpVtbl->Release(pRealSC);
                if(FAILED(hr2)||!wsc) {
                    LOG("  CreateSwapChainForHwnd: WrapSC_Create2 = 0x%08X",(unsigned)hr2);
                    D3D12Interop_Destroy(it);
                    LOG_LEAVE("IDXGIFactory2::CreateSwapChainForHwnd",hr2);
                    return hr2;
                }
                wsc->interop=it;
            }
            LOG_LEAVE("IDXGIFactory2::CreateSwapChainForHwnd",S_OK);
            return S_OK;
        }
    }

    /* Chemin normal D3D11/D3D9 */
    void *pSC=NULL;
    HRESULT hr=((PFN)VS(w->pRealF2,0x78))(w->pRealF2,pDev,hwnd,pD,pFD,pRO,&pSC);
    LOG("  IDXGIFactory2::CreateSwapChainForHwnd (appel réel) = 0x%08X pSC=%p",(unsigned)hr,pSC);
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
 * =================== EXPORTS PUBLICS ====================
 * ========================================================= */

__declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory)
{
    HRESULT hr;
    LOG_ENTER("CreateDXGIFactory");
    if(!ppFactory) { LOG_LEAVE("CreateDXGIFactory",E_INVALIDARG); return E_INVALIDARG; }
    *ppFactory=NULL;
    if(!g_pfnFactory) { LOG_LEAVE("CreateDXGIFactory",DXGI_ERROR_UNSUPPORTED); return DXGI_ERROR_UNSUPPORTED; }

    typedef HRESULT(WINAPI*PFN)(REFIID,void**);
    PFN pfn=(PFN)PIX_Intercept((FARPROC)g_pfnFactory,
        "GetRealPtrPtrCreateDXGIFactory","CreateDXGIFactoryGenerated");

    IUnknown *pRaw=NULL;
    hr=pfn(riid,(void**)&pRaw);
    if(SUCCEEDED(hr)&&pRaw) {
        hr=WrapFactory_Create(pRaw,0,riid,ppFactory);
        pRaw->lpVtbl->Release(pRaw);
    }
    LOG_LEAVE("CreateDXGIFactory",hr); return hr;
}

__declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory)
{
    HRESULT hr;
    LOG_ENTER("CreateDXGIFactory1");
    LOG_GUID("  CreateDXGIFactory1: demande",riid);
    LOG("  CreateDXGIFactory1: ppFactory=%p",(void*)ppFactory);
    if(!ppFactory) { LOG_LEAVE("CreateDXGIFactory1",E_INVALIDARG); return E_INVALIDARG; }
    *ppFactory=NULL;

    PFN_CreateDXGIFactory1 pfnReal=g_pfnFactory1?(g_pfnFactory1):((PFN_CreateDXGIFactory1)g_pfnFactory);
    LOG("  CreateDXGIFactory1: pfnReal=%p (g_pfnFactory1=%p g_pfnFactory=%p)",
        (void*)pfnReal,(void*)g_pfnFactory1,(void*)g_pfnFactory);
    if(!pfnReal) { LOG_LEAVE("CreateDXGIFactory1",DXGI_ERROR_UNSUPPORTED); return DXGI_ERROR_UNSUPPORTED; }

    typedef HRESULT(WINAPI*PFN)(REFIID,void**);
    PFN pfn=(PFN)PIX_Intercept((FARPROC)pfnReal,
        "GetRealPtrPtrCreateDXGIFactory1","CreateDXGIFactory1Generated");
    LOG("  CreateDXGIFactory1: pfn (apres PIX_Intercept)=%p",(void*)pfn);

    IUnknown *pRaw=NULL;
    LOG("  CreateDXGIFactory1: --> appel pfn(IID_IDXGIFactory1,&pRaw)");
    hr=pfn(&IID_IDXGIFactory1,(void**)&pRaw);
    LOG("  CreateDXGIFactory1: <-- pfn = 0x%08X pRaw=%p",(unsigned)hr,(void*)pRaw);
    if(SUCCEEDED(hr)&&pRaw) {
        LOG("  CreateDXGIFactory1: --> WrapFactory_Create");
        hr=WrapFactory_Create(pRaw,0,riid,ppFactory);
        LOG("  CreateDXGIFactory1: <-- WrapFactory_Create = 0x%08X ppFactory=%p",(unsigned)hr,*ppFactory);
        pRaw->lpVtbl->Release(pRaw);
    }
    LOG_LEAVE("CreateDXGIFactory1",hr); return hr;
}

__declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
{
    HRESULT hr;
    LOG_ENTER("CreateDXGIFactory2");
    LOG("  CreateDXGIFactory2: Flags=0x%X ppFactory=%p",Flags,(void*)ppFactory);
    LOG_GUID("  CreateDXGIFactory2: demande",riid);
    if(!ppFactory) { LOG_LEAVE("CreateDXGIFactory2",E_INVALIDARG); return E_INVALIDARG; }
    *ppFactory=NULL;

    if(Flags & DXGI_CREATE_FACTORY_DEBUG) {
        LOG("  CreateDXGIFactory2: DXGI_CREATE_FACTORY_DEBUG demandé");
        HMODULE hDbg=LoadLibraryExW(L"DXGIDebug.dll",NULL,LOAD_LIBRARY_AS_DATAFILE);
        LOG("  CreateDXGIFactory2: LoadLibraryExW(DXGIDebug.dll) = %p",(void*)hDbg);
        if(!hDbg) {
            OutputDebugStringA("CreateDXGIFactory2: DXGI_CREATE_FACTORY_DEBUG but DXGIDebug.dll missing\n");
            LOG_LEAVE("CreateDXGIFactory2",DXGI_ERROR_SDK_COMPONENT_MISSING);
            return DXGI_ERROR_SDK_COMPONENT_MISSING;
        }
        FreeLibrary(hDbg);
    }

    IUnknown *pRaw=NULL;

    if(g_pfnFactory2) {
        typedef HRESULT(WINAPI*PFN)(UINT,REFIID,void**);
        PFN pfn=(PFN)PIX_Intercept((FARPROC)g_pfnFactory2,
            "GetRealPtrPtrCreateDXGIFactory2","CreateDXGIFactory2Generated");
        LOG("  CreateDXGIFactory2: --> appel pfn(Flags=0x%X,riid,&pRaw) [Factory2]",
            Flags & ~DXGI_CREATE_FACTORY_DEBUG);
        hr=pfn(Flags & ~DXGI_CREATE_FACTORY_DEBUG, riid,(void**)&pRaw);
        LOG("  CreateDXGIFactory2: <-- pfn = 0x%08X pRaw=%p [Factory2]",(unsigned)hr,(void*)pRaw);
    } else {
        if(Flags & ~DXGI_CREATE_FACTORY_DEBUG)
            LOG("  CreateDXGIFactory2: flags 0x%X ignorés sur Win7 (pas de Factory2)",
                Flags & ~DXGI_CREATE_FACTORY_DEBUG);
        PFN_CreateDXGIFactory1 pfnReal=g_pfnFactory1?g_pfnFactory1:(PFN_CreateDXGIFactory1)g_pfnFactory;
        LOG("  CreateDXGIFactory2: pfnReal (fallback Factory1)=%p",(void*)pfnReal);
        if(!pfnReal) { LOG_LEAVE("CreateDXGIFactory2",DXGI_ERROR_UNSUPPORTED); return DXGI_ERROR_UNSUPPORTED; }
        LOG("  CreateDXGIFactory2: --> appel pfnReal(IID_IDXGIFactory1,&pRaw) [fallback Factory1]");
        hr=pfnReal(&IID_IDXGIFactory1,(void**)&pRaw);
        LOG("  CreateDXGIFactory2: <-- pfnReal = 0x%08X pRaw=%p [fallback Factory1]",(unsigned)hr,(void*)pRaw);
    }

    if(SUCCEEDED(hr)&&pRaw) {
        LOG("  CreateDXGIFactory2: --> WrapFactory_Create(pRaw=%p,Flags=0x%X)",(void*)pRaw,Flags);
        hr=WrapFactory_Create(pRaw,Flags,riid,ppFactory);
        LOG("  CreateDXGIFactory2: <-- WrapFactory_Create = 0x%08X ppFactory=%p",(unsigned)hr,*ppFactory);
        pRaw->lpVtbl->Release(pRaw);
    }
    LOG_LEAVE("CreateDXGIFactory2",hr); return hr;
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
        LOG("  D3DKMTSetProcessDeviceRemovalSupport non disponible");
        LOG_LEAVE("DXGIDeclareAdapterRemovalSupport",DXGI_ERROR_UNSUPPORTED);
        return DXGI_ERROR_UNSUPPORTED;
    }
    D3DKMT_SETPROCESSDEVICEREMOVALSUPPORT args={1};
    NTSTATUS st=g_D3DKMTSetProcessDeviceRemovalSupport(&args);
    HRESULT hr=NtStatusToDxgiHR(st);
    LOG_LEAVE("DXGIDeclareAdapterRemovalSupport",hr); return hr;
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
        LOG("  DXGIDebug.dll introuvable");
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
 * Initialisation / finalisation
 * ========================================================= */
static BOOL Init_RealDxgi(void)
{
    g_hDxgiReal=LoadLibraryExW(L"dxgi_real.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!g_hDxgiReal) {
        WCHAR path[MAX_PATH];
        GetSystemDirectoryW(path,MAX_PATH);
        lstrcatW(path,L"\\dxgi_real.dll");
        g_hDxgiReal=LoadLibraryW(path);
    }
    if(!g_hDxgiReal) {
        OutputDebugStringA("[dxgi_win7] ERREUR: dxgi_real.dll introuvable !\n");
        return FALSE;
    }
    *(FARPROC*)&g_pfnFactory  =GetProcAddress(g_hDxgiReal,"CreateDXGIFactory");
    *(FARPROC*)&g_pfnFactory1 =GetProcAddress(g_hDxgiReal,"CreateDXGIFactory1");
    *(FARPROC*)&g_pfnFactory2 =GetProcAddress(g_hDxgiReal,"CreateDXGIFactory2");
    LOG("dxgi_real.dll chargé (F=%p F1=%p F2=%p)",(void*)g_pfnFactory,(void*)g_pfnFactory1,(void*)g_pfnFactory2);
    return g_pfnFactory!=NULL;
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
        LOG("DllMain DLL_PROCESS_ATTACH");
        InitializeCriticalSection(&g_csCompat);
        InitializeCriticalSection(&g_csHMD);
        Init_Ntdll();
        Init_D3DKMT();
        Init_ExePath();
        if(!Init_RealDxgi())
            OutputDebugStringA("[dxgi_win7] Warning: mode dégradé sans dxgi_real.dll\n");
        LOG("DllMain OK");
        break;

    case DLL_PROCESS_DETACH:
        LOG("DllMain DLL_PROCESS_DETACH");
        if(g_hDxgiReal){FreeLibrary(g_hDxgiReal);g_hDxgiReal=NULL;}
        if(g_pExePath){HeapFree(GetProcessHeap(),0,g_pExePath);g_pExePath=NULL;}
        if(g_pExeDir) {HeapFree(GetProcessHeap(),0,g_pExeDir); g_pExeDir=NULL;}
        DeleteCriticalSection(&g_csCompat);
        DeleteCriticalSection(&g_csHMD);
        break;
    }
    return TRUE;
}