/*
 * ============================================================================
 *  d3d12proxy.c
 * ============================================================================
 *
 *  A minimal D3D12 "proxy" DLL for Windows 7, designed to sit between a
 *  Direct3D 12 application and Microsoft's D3D12On7 runtime (the redistributed
 *  d3d12.dll that translates D3D12 calls down to a D3D11 driver on Windows 7).
 *
 *  HOW IT WORKS
 *  ------------
 *  1. The application loads "d3d12.dll" as usual (LoadLibrary / import table).
 *  2. Because of normal Windows DLL search order, it actually loads THIS
 *     proxy, which has been built and renamed to "d3d12.dll" and placed next
 *     to the application's executable (or wherever the real one used to be).
 *  3. This proxy reads "d3d12proxy.ini" (next to itself), then uses
 *     LoadLibrary() to load the REAL D3D12On7 DLL from the path given by the
 *     "DLLPath" setting (that real DLL must have been renamed to something
 *     else, e.g. "d3d12on7_orig.dll", so it does not collide with this proxy).
 *  4. Every standard D3D12 export is forwarded to the real DLL. Along the
 *     way, the proxy can:
 *       - Override the feature level requested by the app in D3D12CreateDevice
 *         (EmulatedFeatureLevel).
 *       - Log every call to a text file (OutputLogFile).
 *       - Patch the answers returned by ID3D12Device::CheckFeatureSupport for
 *         ray tracing tier / shader model queries (BypassRayTracingChecks,
 *         BypassDLSSChecks). See the big warning below about what this can
 *         and cannot do.
 *
 *  IMPORTANT, HONEST DISCLAIMER ABOUT THE "BYPASS" OPTIONS
 *  ---------------------------------------------------------
 *  - BypassRayTracingChecks only fakes the result of CheckFeatureSupport for
 *    D3D12_FEATURE_D3D12_OPTIONS5 (RaytracingTier). It does NOT add real DXR
 *    support to D3D12On7. D3D12On7 translates D3D12 calls into D3D11 driver
 *    calls, and the underlying D3D11 driver model has no notion of DXR
 *    raytracing pipelines/acceleration structures. If an application is
 *    fooled into believing raytracing is available and actually tries to use
 *    it (CreateStateObject, BuildRaytracingAccelerationStructure, etc.), those
 *    calls will most likely fail or the app may crash. This option is only
 *    useful to get past an application's launch-time capability check, not
 *    to obtain working hardware ray tracing on Windows 7.
 *  - BypassDLSSChecks only fakes the reported "highest shader model" returned
 *    by CheckFeatureSupport for D3D12_FEATURE_SHADER_MODEL. Real NVIDIA DLSS
 *    validation happens through the separate NGX/NVAPI stack, completely
 *    outside the D3D12 API surface, so this option cannot make actual DLSS
 *    work. It is a best-effort hook for titles/launchers that gate an
 *    upscaler behind a D3D12 shader-model check.
 *
 *  DEPLOYMENT
 *  ----------
 *  1. Rename the real D3D12On7 redistributable DLL, e.g.:
 *         d3d12.dll  ->  d3d12on7_orig.dll
 *  2. Build this project (see the .def file and the compile command at the
 *     bottom of this comment block) to produce "d3d12.dll".
 *  3. Place the freshly built "d3d12.dll" where the application expects to
 *     find the real one.
 *  4. Place "d3d12proxy.ini" next to it (see d3d12proxy.ini example file),
 *     with DLLPath pointing at "d3d12on7_orig.dll" (relative or absolute
 *     path).
 *  5. Run the application as usual.
 *
 *  BUILD (MinGW-w64, 64-bit target)
 *  ---------------------------------
 *      x86_64-w64-mingw32-gcc -O2 -s -shared -static-libgcc ^
 *          d3d12proxy.c d3d12proxy.def -o d3d12.dll -luser32
 *
 *  NOTE: This proxy is written for 64-bit (x64) targets only, which matches
 *  MinGW-w64's x86_64-w64-mingw32-gcc and how D3D12On7 is normally shipped.
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ----------------------------------------------------------------------- *
 *  Minimal local type definitions
 *
 *  We deliberately avoid including the official d3d12.h / unknwn.h headers.
 *  Doing so keeps this single file fully self-contained and avoids any
 *  dependency on a specific Windows SDK / MinGW header version. COM pointers
 *  that we never dereference ourselves (adapters, blobs, debug interfaces...)
 *  are simply passed through as opaque "void *", which is perfectly valid
 *  since their size (a pointer) does not depend on their pointed-to type.
 * ----------------------------------------------------------------------- */

/* GUID may already be defined by some transitively-included header.
 * Use the exact same guard Microsoft's own headers use, so this is safe
 * whether or not GUID is already available. */
#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
    unsigned long  Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char  Data4[8];
} GUID;
#endif

/* Custom name (instead of REFIID) on purpose: avoids any clash with a
 * REFIID typedef that might come from a transitively-included COM header. */
typedef const GUID *DPROXY_REFIID;

#ifndef D3D_FEATURE_LEVEL_DEFINED
#define D3D_FEATURE_LEVEL_DEFINED
typedef enum D3D_FEATURE_LEVEL {
    D3D_FEATURE_LEVEL_9_1  = 0x9100,
    D3D_FEATURE_LEVEL_9_2  = 0x9200,
    D3D_FEATURE_LEVEL_9_3  = 0x9300,
    D3D_FEATURE_LEVEL_10_0 = 0xA000,
    D3D_FEATURE_LEVEL_10_1 = 0xA100,
    D3D_FEATURE_LEVEL_11_0 = 0xB000,
    D3D_FEATURE_LEVEL_11_1 = 0xB100,
    D3D_FEATURE_LEVEL_12_0 = 0xC000,
    D3D_FEATURE_LEVEL_12_1 = 0xC100
} D3D_FEATURE_LEVEL;
#endif

/* Numeric values for the D3D12_FEATURE enum entries we actually care about.
 * These values come from the public Windows SDK's d3d12.h and have been
 * stable across every SDK release that defines them. If a future SDK ever
 * changes them (very unlikely), update the two DPROXY_FEATURE_* defines. */
#define DPROXY_FEATURE_D3D12_OPTIONS     0
#define DPROXY_FEATURE_D3D12_OPTIONS5   27
#define DPROXY_FEATURE_SHADER_MODEL      7
/* D3D12On7 on Windows 7 predates the Enhanced Barriers feature entirely
 * (introduced with Windows 11 / Agility SDK 1.6) and does not recognize
 * this D3D12_FEATURE enum value -- its CheckFeatureSupport implementation
 * returns a failing HRESULT for it instead of a zeroed/FALSE struct. Any
 * sample that does ThrowIfFailed(CheckFeatureSupport(D3D12_OPTIONS12, ...))
 * to *probe* for enhanced-barrier support (a completely valid, spec-
 * sanctioned pattern -- the feature is optional and meant to be probed)
 * will crash outright instead of falling back to legacy ResourceBarrier,
 * even though it already has that fallback path written and ready to go.
 * See DPROXY_FEATURE_DATA_OPTIONS12 / the OPTIONS12 case in
 * Hook_CheckFeatureSupport below for the synthesized-success workaround. */
#define DPROXY_FEATURE_D3D12_OPTIONS12  41

/* D3D12_RAYTRACING_TIER values. */
#define DPROXY_RAYTRACING_TIER_NOT_SUPPORTED 0
#define DPROXY_RAYTRACING_TIER_1_0           10
#define DPROXY_RAYTRACING_TIER_1_1           11

/* D3D12_SHADER_MODEL values (hex-encoded major/minor, as in the real enum). */
#define DPROXY_SHADER_MODEL_6_0 0x60
#define DPROXY_SHADER_MODEL_6_4 0x64
#define DPROXY_SHADER_MODEL_6_6 0x66

/* D3D12_TILED_RESOURCES_TIER values. */
#define DPROXY_TILED_RESOURCES_TIER_NOT_SUPPORTED 0

/* ABI-compatible shadow of D3D12_FEATURE_DATA_D3D12_OPTIONS.
 * We only need to read/write the leading fields up to TiledResourcesTier;
 * every field in the real struct is a 32-bit BOOL/enum/UINT, so offsets
 * line up exactly as long as we declare them in the same order:
 *   0  DoublePrecisionFloatShaderOps
 *   4  OutputMergerLogicOp
 *   8  MinPrecisionSupport
 *   12 TiledResourcesTier   <- the one we care about
 *   16 ResourceBindingTier
 *   ... (rest of the real struct continues after this; we never touch it,
 *        we only read FeatureSupportDataSize to confirm it's large enough
 *        to contain TiledResourcesTier before touching it). */
typedef struct {
    int DoublePrecisionFloatShaderOps;
    int OutputMergerLogicOp;
    int MinPrecisionSupport;
    int TiledResourcesTier;
    int ResourceBindingTier;
} DPROXY_FEATURE_DATA_OPTIONS;

/* ABI-compatible shadow of D3D12_FEATURE_DATA_D3D12_OPTIONS5.
 * Field order/sizes (3 x 32-bit fields) match the official struct exactly;
 * the field *names* don't matter for memory layout purposes. */
typedef struct {
    int SRVOnlyTiledResourceTier3; /* BOOL */
    int RenderPassesTier;          /* enum */
    int RaytracingTier;            /* enum */
} DPROXY_FEATURE_DATA_OPTIONS5;

/* ABI-compatible shadow of D3D12_FEATURE_DATA_SHADER_MODEL. */
typedef struct {
    int HighestShaderModel; /* enum, in/out */
} DPROXY_FEATURE_DATA_SHADER_MODEL;

/* ABI-compatible shadow of D3D12_FEATURE_DATA_D3D12_OPTIONS12:
 *   0  MSPrimitivesPipelineStatisticIncludesCulledPrimitives (enum, D3D12_TRI_STATE)
 *   4  EnhancedBarriersSupported                             (BOOL) <- the one we care about
 *   8  RelaxedFormatCastingSupported                          (BOOL)
 * Total size 12 bytes, matching the real struct (3 x 32-bit fields). */
typedef struct {
    int MSPrimitivesPipelineStatisticIncludesCulledPrimitives;
    int EnhancedBarriersSupported;
    int RelaxedFormatCastingSupported;
} DPROXY_FEATURE_DATA_OPTIONS12;

/* ABI-compatible shadow of D3D12_SHADER_BYTECODE (public, stable API):
 *   0  const void *pShaderBytecode
 *   8  SIZE_T      BytecodeLength
 * Total 16 bytes on x64. */
typedef struct {
    const void *pShaderBytecode;
    SIZE_T      BytecodeLength;
} DPROXY_SHADER_BYTECODE;

/* Byte offsets of each D3D12_SHADER_BYTECODE field within
 * D3D12_GRAPHICS_PIPELINE_STATE_DESC on x64 (documented, stable struct --
 * not reverse-engineered). pRootSignature is an 8-byte pointer at offset 0,
 * followed immediately by VS/PS/DS/HS/GS, each 16 bytes. */
#define DPROXY_GFXPSO_OFFSET_ROOTSIG 0
#define DPROXY_GFXPSO_OFFSET_VS      8
#define DPROXY_GFXPSO_OFFSET_PS      24
#define DPROXY_GFXPSO_OFFSET_DS      40
#define DPROXY_GFXPSO_OFFSET_HS      56
#define DPROXY_GFXPSO_OFFSET_GS      72

/* Same reasoning for D3D12_COMPUTE_PIPELINE_STATE_DESC: pRootSignature at
 * offset 0 (8 bytes), CS (D3D12_SHADER_BYTECODE) immediately after. */
#define DPROXY_CPSO_OFFSET_ROOTSIG 0
#define DPROXY_CPSO_OFFSET_CS      8

/* ABI-compatible shadow of D3D12_CPU_DESCRIPTOR_HANDLE: a single SIZE_T,
 * passed by value. */
typedef struct {
    SIZE_T ptr;
} DPROXY_CPU_DESCRIPTOR_HANDLE;

/* ABI-compatible shadow of D3D12_GPU_VIRTUAL_ADDRESS: a plain UINT64. */
typedef UINT64 DPROXY_GPU_VIRTUAL_ADDRESS;

/* ABI-compatible shadow of D3D12_RESOURCE_BARRIER (public, stable API --
 * layout straight from the documented struct, not reverse-engineered).
 * We never interpret Type/Transition/Aliasing/UAV ourselves beyond copying
 * them byte-for-byte into our own pending buffer and handing the same
 * bytes back to the real ResourceBarrier() later -- see
 * Hook_ResourceBarrier / Coalesce_FlushBarriers. pResource fields are kept
 * as void* since we never dereference them, only compare/forward pointers. */
typedef struct {
    void *pResource;
    UINT  Subresource;
    UINT  StateBefore;
    UINT  StateAfter;
} DPROXY_RESOURCE_TRANSITION_BARRIER;

typedef struct {
    void *pResourceBefore;
    void *pResourceAfter;
} DPROXY_RESOURCE_ALIASING_BARRIER;

typedef struct {
    void *pResource;
} DPROXY_RESOURCE_UAV_BARRIER;

typedef struct {
    UINT Type;   /* D3D12_RESOURCE_BARRIER_TYPE: 0=Transition, 1=Aliasing, 2=UAV */
    UINT Flags;  /* D3D12_RESOURCE_BARRIER_FLAGS */
    union {
        DPROXY_RESOURCE_TRANSITION_BARRIER Transition;
        DPROXY_RESOURCE_ALIASING_BARRIER   Aliasing;
        DPROXY_RESOURCE_UAV_BARRIER        UAV;
    };
} DPROXY_RESOURCE_BARRIER;

/* D3D12_SRV_DIMENSION value for a plain (non-array) 2D texture. */
#define DPROXY_SRV_DIMENSION_TEXTURE2D 4

/* Byte offsets of the fields we need inside D3D12_SHADER_RESOURCE_VIEW_DESC
 * when ViewDimension == TEXTURE2D, i.e. the D3D12_TEX2D_SRV union member:
 *   0  DXGI_FORMAT Format                  (4 bytes)
 *   4  D3D12_SRV_DIMENSION ViewDimension   (4 bytes)
 *   8  UINT Shader4ComponentMapping        (4 bytes)
 *   12 <compiler padding>                  (4 bytes -- see below)
 *   16 union { ... D3D12_TEX2D_SRV { MostDetailedMip; MipLevels; ... } ... }
 * The 4-byte pad at offset 12 exists because D3D12_BUFFER_SRV (another
 * member of the same union) starts with a UINT64, which forces the whole
 * union -- and therefore its start offset within the enclosing struct -- to
 * be 8-byte aligned. This is a stable, publicly documented ABI (not
 * SDK-version-dependent), but Hook_CreateShaderResourceView still
 * sanity-checks the values read from these offsets before writing back,
 * specifically so a wrong assumption here can never corrupt the app's own
 * descriptor struct. */
#define DPROXY_SRV_TEX2D_MOSTDETAILEDMIP_OFFSET 16
#define DPROXY_SRV_TEX2D_MIPLEVELS_OFFSET       20


typedef HRESULT (WINAPI *PFN_CreateFence)(
    void *This,
    UINT64 InitialValue,
    UINT Flags,                /* D3D12_FENCE_FLAG_* */
    DPROXY_REFIID riid,
    void **ppFence
);


static PFN_CreateFence g_pfnOriginalCreateFence = NULL;


/* ----------------------------------------------------------------------- *
 *  Diagnostic GUID / HRESULT naming
 *
 *  Added specifically to answer "does the app even call D3D12CreateDevice,
 *  with what riid/feature level, and what does the real DLL return" --
 *  the one blind spot the dxgi_win7 wrapper cannot see, since apps load
 *  d3d12.dll directly (see the big comment in dxgi_win734.c about
 *  ID3D12Device::CheckFeatureSupport patching). Every entry here is a
 *  well-known, publicly documented interface ID -- nothing app-specific.
 * ----------------------------------------------------------------------- */
static const GUID DPROXY_IID_ID3D12Device  = {0x189819F1,0x1DB6,0x4B57,{0xBE,0x54,0x18,0x21,0x33,0x9B,0x85,0xF7}};
static const GUID DPROXY_IID_ID3D12Device1 = {0x77ACCE80,0x638E,0x4E65,{0x88,0x95,0xC1,0xF2,0x33,0x86,0x86,0x3E}};
/* IID_ID3D12Resource   = {696442be-a72e-4059-bc79-5b5c98040fad} */
static const GUID DPROXY_IID_ID3D12Resource  = {0x696442BE,0xA72E,0x4059,{0xBC,0x79,0x5B,0x5C,0x98,0x04,0x0F,0xAD}};
/* IID_ID3D12Resource1  = {9d5e227a-4430-4161-88b3-3eca6bb16e19} */
static const GUID DPROXY_IID_ID3D12Resource1 = {0x9D5E227A,0x4430,0x4161,{0x88,0xB3,0x3E,0xCA,0x6B,0xB1,0x6E,0x19}};
static const GUID DPROXY_IID_ID3D12Device2 = {0x30BAA41E,0xB15B,0x475C,{0xA0,0xBB,0x1A,0xF5,0xC5,0xB6,0x43,0x28}};
static const GUID DPROXY_IID_ID3D12Device3 = {0x81DADC15,0x2BAD,0x4392,{0x93,0xC5,0x10,0x13,0x45,0xC4,0xAC,0x73}};
static const GUID DPROXY_IID_ID3D12Device4 = {0xE865DF17,0xA9EE,0x46F9,{0xA4,0x63,0x30,0x98,0x31,0x5A,0xA2,0xE5}};
static const GUID DPROXY_IID_ID3D12Device5 = {0x8B4F173B,0x2FEA,0x4B80,{0x8F,0x58,0x43,0x07,0x19,0x1A,0xB9,0x5D}};

/* NOUVEAU: D3D12On7's Windows-7-specific present mechanism. On Win7 there
 * is no IDXGISwapChain for D3D12 at all -- apps must QueryInterface their
 * ID3D12CommandQueue for this tear-off interface and call its Present()
 * instead. This is a completely separate code path from ExecuteCommandLists/
 * Signal/Wait, and one this proxy never touched before -- see
 * Hook_DownlevelPresent below. */
static const GUID DPROXY_IID_ID3D12CommandQueueDownlevel =
    {0x38a8c5ef,0x7ccb,0x4e81,{0x91,0x4f,0xa6,0xe9,0xd0,0x72,0xc4,0x94}};
#define DPROXY_DOWNLEVEL_PRESENT_FLAG_WAIT_FOR_VBLANK 1

static int GuidEquals(DPROXY_REFIID a, const GUID *b)
{
    return a && memcmp(a, b, sizeof(GUID)) == 0;
}

/* Never fails: unknown GUIDs fall back to their raw hex form so we always
 * get *something* useful in the log, even for an interface not in the
 * (necessarily incomplete) table above. */
static const char *FormatGuid(DPROXY_REFIID riid, char *buf, size_t bufSize)
{
    if (!riid) { snprintf(buf, bufSize, "(null)"); return buf; }

    if (GuidEquals(riid, &DPROXY_IID_ID3D12Device))  return "ID3D12Device";
    if (GuidEquals(riid, &DPROXY_IID_ID3D12Device1)) return "ID3D12Device1";
    if (GuidEquals(riid, &DPROXY_IID_ID3D12Device2)) return "ID3D12Device2";
    if (GuidEquals(riid, &DPROXY_IID_ID3D12Device3)) return "ID3D12Device3";
    if (GuidEquals(riid, &DPROXY_IID_ID3D12Device4)) return "ID3D12Device4";
    if (GuidEquals(riid, &DPROXY_IID_ID3D12Device5)) return "ID3D12Device5";
    if (GuidEquals(riid, &DPROXY_IID_ID3D12Resource))  return "ID3D12Resource";
    if (GuidEquals(riid, &DPROXY_IID_ID3D12Resource1)) return "ID3D12Resource1";

    snprintf(buf, bufSize, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)riid->Data1, riid->Data2, riid->Data3,
             riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3],
             riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7]);
    return buf;
}

/* Decodes the handful of HRESULTs that actually show up at this call site
 * in practice, so the console/log line is readable without looking each
 * one up by hand. Falls back to plain hex for anything else. */
static const char *DecodeHR(HRESULT hr, char *buf, size_t bufSize)
{
    switch ((unsigned long)hr) {
        case 0x00000000UL: return "S_OK";
        case 0x00000001UL: return "S_FALSE";
        case 0x80004001UL: return "E_NOTIMPL";
        case 0x80004002UL: return "E_NOINTERFACE (adapter didn't answer QI the way D3D12On7 expected)";
        case 0x80004005UL: return "E_FAIL";
        case 0x8007000EUL: return "E_OUTOFMEMORY";
        case 0x80070057UL: return "E_INVALIDARG (bad adapter/feature level/riid combination)";
        case 0x887A0001UL: return "DXGI_ERROR_INVALID_CALL";
        case 0x887A0002UL: return "DXGI_ERROR_NOT_FOUND";
        case 0x887A0004UL: return "DXGI_ERROR_UNSUPPORTED (device/feature level rejected)";
        case 0x887E0001UL: return "D3D12_ERROR_ADAPTER_NOT_FOUND";
        case 0x887E0002UL: return "D3D12_ERROR_DRIVER_VERSION_MISMATCH";
        default:
            snprintf(buf, bufSize, "0x%08lX", (unsigned long)hr);
            return buf;
    }
}

/* Human-readable D3D_FEATURE_LEVEL name for log lines. */
static const char *FormatFeatureLevel(D3D_FEATURE_LEVEL fl, char *buf, size_t bufSize)
{
    switch (fl) {
        case D3D_FEATURE_LEVEL_9_1:  return "9.1";
        case D3D_FEATURE_LEVEL_9_2:  return "9.2";
        case D3D_FEATURE_LEVEL_9_3:  return "9.3";
        case D3D_FEATURE_LEVEL_10_0: return "10.0";
        case D3D_FEATURE_LEVEL_10_1: return "10.1";
        case D3D_FEATURE_LEVEL_11_0: return "11.0";
        case D3D_FEATURE_LEVEL_11_1: return "11.1";
        case D3D_FEATURE_LEVEL_12_0: return "12.0";
        case D3D_FEATURE_LEVEL_12_1: return "12.1";
        default:
            snprintf(buf, bufSize, "unknown(0x%04X)", (unsigned)fl);
            return buf;
    }
}

/* Human-readable D3D12_FEATURE enum name (values from the public d3d12.h,
 * stable across SDK versions -- see the comment on DPROXY_FEATURE_* above).
 * Only the values engines commonly probe at startup/capability-check time
 * are named; anything else falls back to its raw number. */
static const char *FormatFeatureName(int feature, char *buf, size_t bufSize)
{
    switch (feature) {
        case 0:  return "D3D12_OPTIONS";
        case 1:  return "ARCHITECTURE";
        case 2:  return "FEATURE_LEVELS";
        case 3:  return "FORMAT_SUPPORT";
        case 4:  return "MULTISAMPLE_QUALITY_LEVELS";
        case 5:  return "FORMAT_INFO";
        case 6:  return "GPU_VIRTUAL_ADDRESS_SUPPORT";
        case 7:  return "SHADER_MODEL";
        case 8:  return "D3D12_OPTIONS1";
        case 10: return "PROTECTED_RESOURCE_SESSION_SUPPORT";
        case 12: return "ROOT_SIGNATURE";
        case 16: return "ARCHITECTURE1";
        case 18: return "D3D12_OPTIONS2";
        case 19: return "SHADER_CACHE";
        case 20: return "COMMAND_QUEUE_PRIORITY";
        case 21: return "D3D12_OPTIONS3";
        case 22: return "EXISTING_HEAPS";
        case 23: return "D3D12_OPTIONS4";
        case 24: return "SERIALIZATION";
        case 25: return "CROSS_NODE";
        case 27: return "D3D12_OPTIONS5";
        case 28: return "DISPLAYABLE";
        case 30: return "D3D12_OPTIONS6";
        case 31: return "QUERY_META_COMMAND";
        case 32: return "D3D12_OPTIONS7";
        case 41: return "D3D12_OPTIONS12";
        default:
            snprintf(buf, bufSize, "unknown(%d)", feature);
            return buf;
    }
}


/* ----------------------------------------------------------------------- *
 *  Function pointer types matching the real d3d12.dll exports we forward.
 * ----------------------------------------------------------------------- */
typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(
    void *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
    DPROXY_REFIID riid, void **ppDevice);

typedef HRESULT (WINAPI *PFN_D3D12GetDebugInterface)(
    DPROXY_REFIID riid, void **ppvDebug);

typedef HRESULT (WINAPI *PFN_D3D12CreateRootSignatureDeserializer)(
    const void *pSrcData, SIZE_T SrcDataSizeInBytes,
    DPROXY_REFIID pRootSignatureDeserializerInterface,
    void **ppRootSignatureDeserializer);

typedef HRESULT (WINAPI *PFN_D3D12CreateVersionedRootSignatureDeserializer)(
    const void *pSrcData, SIZE_T SrcDataSizeInBytes,
    DPROXY_REFIID pRootSignatureDeserializerInterface,
    void **ppRootSignatureDeserializer);

typedef HRESULT (WINAPI *PFN_D3D12SerializeRootSignature)(
    const void *pRootSignature, UINT Version,
    void **ppBlob, void **ppErrorBlob);

typedef HRESULT (WINAPI *PFN_D3D12SerializeVersionedRootSignature)(
    const void *pRootSignature, void **ppBlob, void **ppErrorBlob);

typedef HRESULT (WINAPI *PFN_D3D12EnableExperimentalFeatures)(
    UINT NumFeatures, const GUID *pIIDs,
    void *pConfigurationStructs, UINT *pConfigurationStructSizes);

/* ABI-compatible shadow of D3D12_HEAP_PROPERTIES (all 32-bit enum/UINT
 * fields, layout matches exactly):
 *   0x00 Type                    (D3D12_HEAP_TYPE)
 *   0x04 CPUPageProperty         (D3D12_CPU_PAGE_PROPERTY)
 *   0x08 MemoryPoolPreference    (D3D12_MEMORY_POOL)
 *   0x0C CreationNodeMask        (UINT)
 *   0x10 VisibleNodeMask         (UINT) */
typedef struct {
    UINT Type;
    UINT CPUPageProperty;
    UINT MemoryPoolPreference;
    UINT CreationNodeMask;
    UINT VisibleNodeMask;
} DPROXY_HEAP_PROPERTIES_SHADOW;

/* ABI-compatible shadow of D3D12_RESOURCE_DESC, read-only here (we only
 * log fields, never touch them). Layout matches the real struct exactly:
 *   0x00 Dimension     (D3D12_RESOURCE_DIMENSION, enum/UINT)
 *   0x08 Alignment     (UINT64)
 *   0x10 Width         (UINT64)
 *   0x18 Height        (UINT)
 *   0x1C DepthOrArraySize (UINT16)
 *   0x1E MipLevels     (UINT16)
 *   0x20 Format        (DXGI_FORMAT, UINT)
 *   0x24 SampleDesc.Count   (UINT)
 *   0x28 SampleDesc.Quality (UINT)
 *   0x2C Layout        (D3D12_TEXTURE_LAYOUT, enum/UINT)
 *   0x30 Flags         (D3D12_RESOURCE_FLAGS, UINT) */
typedef struct {
    UINT   Dimension;
    UINT   _pad0;
    UINT64 Alignment;
    UINT64 Width;
    UINT   Height;
    UINT16 DepthOrArraySize;
    UINT16 MipLevels;
    UINT   Format;
    UINT   SampleCount;
    UINT   SampleQuality;
    UINT   Layout;
    UINT   Flags;
} DPROXY_RESOURCE_DESC_SHADOW;

/* Matches ID3D12Device::CreateCommittedResource. HeapFlags is the raw
 * D3D12_HEAP_FLAGS bitmask passed as the 2nd argument (not embedded in a
 * struct), so this hook can rewrite it directly without needing to know
 * the layout of D3D12_HEAP_PROPERTIES/D3D12_RESOURCE_DESC at all. */
typedef HRESULT (WINAPI *PFN_CreateCommittedResource)(
    void *This, const void *pHeapProperties, UINT HeapFlags,
    const void *pDesc, UINT InitialResourceState,
    const void *pOptimizedClearValue, DPROXY_REFIID riidResource,
    void **ppvResource);

/* Matches ID3D12Device::CheckFeatureSupport, the method we hook on the
 * returned device's vtable. "This" is the device's own interface pointer. */
typedef HRESULT (WINAPI *PFN_CheckFeatureSupport)(
    void *This, int Feature, void *pFeatureSupportData,
    UINT FeatureSupportDataSize);

/* Matches ID3D12Device::CreateCommandQueue. We hook this purely to get a
 * fresh ID3D12CommandQueue instance so we can in turn hook
 * UpdateTileMappings on ITS vtable (see InstallQueueHooksIfNeeded). We never
 * need to interpret D3D12_COMMAND_QUEUE_DESC ourselves, so it stays a raw
 * const void*. */
typedef HRESULT (WINAPI *PFN_CreateCommandQueue)(
    void *This, const void *pDesc, DPROXY_REFIID riid, void **ppCommandQueue);

/* Matches ID3D12Device::CreateCommandList. Hooked purely as a vehicle to
 * reach the freshly created list's own vtable (see
 * InstallCommandListHooksIfNeeded). pCommandAllocator/pInitialState stay
 * raw void* -- we never need to interpret them ourselves, only forward
 * them unchanged to the real function. */
typedef HRESULT (WINAPI *PFN_CreateCommandList)(
    void *This, UINT nodeMask, int type, void *pCommandAllocator,
    void *pInitialState, DPROXY_REFIID riid, void **ppCommandList);

/* NOUVEAU : investigation "les PSO/root signatures sont-ils recrees a
 * chaque frame au lieu d'etre mis en cache ?". D3D12CreateRootSignatureDeserializer
 * (export DLL, pas une methode COM) est appele ~7480 fois sur une session
 * de 230s dans le log fourni -- soit ~1x par frame, un rythme qui colle
 * exactement au taux de Present. Ce n'est normalement PAS un chemin par-
 * frame (c'est un outil de reflexion/introspection). CreateRootSignature/
 * CreateGraphicsPipelineState/CreateComputePipelineState sont les vraies
 * methodes de creation ; si elles aussi tournent a ~1x/frame, l'appli
 * recompile son etat de pipeline a chaque frame -- un cout CPU enorme via
 * la couche de traduction D3D12On7 (qui doit repasser par une compilation/
 * creation d'etat D3D11 a chaque fois), et une explication bien plus
 * probable de "CPU sature, GPU a 15-17%" que le scheduler WDDM a lui seul. */
typedef HRESULT (WINAPI *PFN_CreateRootSignature)(
    void *This, UINT NodeMask, const void *pBlobWithRootSignature,
    SIZE_T BlobLengthInBytes, DPROXY_REFIID riid, void **ppvRootSignature);
typedef HRESULT (WINAPI *PFN_CreateGraphicsPipelineState)(
    void *This, const void *pDesc, DPROXY_REFIID riid, void **ppPipelineState);
typedef HRESULT (WINAPI *PFN_CreateComputePipelineState)(
    void *This, const void *pDesc, DPROXY_REFIID riid, void **ppPipelineState);

/* Matches ID3D12CommandQueue::UpdateTileMappings. Returns void (it's a
 * GPU-timeline queue operation, not something validated synchronously), so
 * there is no HRESULT to check -- we just forward and record. The
 * coordinate/size/range-flag arrays are passed through opaquely since we
 * never read them ourselves. */
typedef void (WINAPI *PFN_UpdateTileMappings)(
    void *This, void *pResource, UINT NumResourceRegions,
    const void *pResourceRegionStartCoordinates, const void *pResourceRegionSizes,
    void *pHeap, UINT NumRanges, const void *pRangeFlags,
    const UINT *pHeapRangeStartOffsets, const UINT *pRangeTileCounts,
    UINT Flags);

/* Matches ID3D12Device::CreateShaderResourceView. Returns void. pDesc may
 * legitimately be NULL (meaning "create a default view over the whole
 * resource"), which the hook below has to handle. */
typedef void (WINAPI *PFN_CreateShaderResourceView)(
    void *This, void *pResource, void *pDesc, DPROXY_CPU_DESCRIPTOR_HANDLE DestDescriptor);

/* Matches ID3D12CommandQueue::ExecuteCommandLists / Signal / Wait -- hooked
 * only when CoalesceCommandLists is enabled, to batch back-to-back
 * ExecuteCommandLists calls into fewer D3DKMT_RENDER kernel escapes. See
 * the big comment block above Hook_ExecuteCommandLists for the full
 * rationale and the correctness argument. */
typedef void (WINAPI *PFN_ExecuteCommandLists)(
    void *This, UINT NumCommandLists, void *const *ppCommandLists);
typedef HRESULT (WINAPI *PFN_Signal)(void *This, void *pFence, UINT64 Value);
typedef HRESULT (WINAPI *PFN_Wait)(void *This, void *pFence, UINT64 Value);

/* NOUVEAU: ID3D12CommandQueue::QueryInterface (always vtable slot 0, like
 * every COM interface) -- hooked purely to catch the app asking for
 * ID3D12CommandQueueDownlevel, so we can in turn hook ITS Present method.
 * Matches ID3D12CommandQueueDownlevel::Present exactly: takes the open
 * command list doing the blt, the source Texture2D, the target HWND, and
 * the D3D12_DOWNLEVEL_PRESENT_FLAGS bitmask (bit 0 = WAIT_FOR_VBLANK). */
typedef HRESULT (WINAPI *PFN_QueueQueryInterface)(
    void *This, DPROXY_REFIID riid, void **ppvObject);
typedef HRESULT (WINAPI *PFN_DownlevelPresent)(
    void *This, void *pOpenCommandList, void *pSourceTex2D, HWND hWindow, UINT Flags);

/* ID3D12GraphicsCommandList methods -- hooked only when
 * CoalesceResourceBarriers is enabled. See Hook_ResourceBarrier for the
 * full rationale/correctness argument and its scope limitation (this is
 * ONLY safe for a command list surface we've fully enumerated ourselves --
 * see the big comment there before considering this on a real game). */
typedef HRESULT (WINAPI *PFN_CmdListClose)(void *This);
typedef HRESULT (WINAPI *PFN_CmdListReset)(void *This, void *pAllocator, void *pInitialState);
typedef void    (WINAPI *PFN_CmdListDispatch)(void *This, UINT x, UINT y, UINT z);
typedef void    (WINAPI *PFN_CmdListSetPipelineState)(void *This, void *pPipelineState);
typedef void    (WINAPI *PFN_CmdListResourceBarrier)(void *This, UINT NumBarriers, const DPROXY_RESOURCE_BARRIER *pBarriers);
typedef void    (WINAPI *PFN_CmdListSetComputeRootSignature)(void *This, void *pRootSignature);
typedef void    (WINAPI *PFN_CmdListSetComputeRootUAV)(void *This, UINT RootParameterIndex, DPROXY_GPU_VIRTUAL_ADDRESS BufferLocation);

/* ----------------------------------------------------------------------- *
 *  Global state
 * ----------------------------------------------------------------------- */
typedef struct {
    int               HasFeatureLevelOverride;
    D3D_FEATURE_LEVEL EmulatedFeatureLevel;
    int               OutputLogFile;
    int               BypassRayTracingChecks;
    int               BypassDLSSChecks;
    int               StripUnsupportedRootSigFlags;
    int               StripUnsupportedHeapFlags;
    int               FixReadbackInitialState;
    int               ShowConsole;
    int               DisableTiledResources;
    int               FixReservedResourceRefresh;
    int               LogDeviceCreation;
    int               LogFeatureSupport;
    int               DisableSharedFences;
    int               CoalesceCommandLists;   /* NOUVEAU */
    int               SynthesizeFailedFeatureQueries;  /* NOUVEAU */
    int               LogPresentTiming;       /* NOUVEAU */
    int               StripPresentVBlankWait; /* NOUVEAU */
    int               LogPipelineStateCreation; /* NOUVEAU */
    int               LogQueueActivity;       /* NOUVEAU */
    int               CoalesceResourceBarriers; /* NOUVEAU */
    int               DumpShaderModel66Blobs;   /* NOUVEAU: diagnostic SM6.6 */
    int               LogShaderModelDetection;  /* NOUVEAU: diagnostic SM6.6 */
    char              DLLPath[MAX_PATH];
} ProxyConfig;

static HINSTANCE   g_hModule  = NULL;
static HMODULE      g_hRealDll = NULL;
static ProxyConfig g_Config;
static char         g_ModuleDir[MAX_PATH];

static CRITICAL_SECTION g_LogLock;
static FILE             *g_LogFile = NULL;

static PFN_D3D12CreateDevice                            g_pfn_D3D12CreateDevice                            = NULL;
static PFN_D3D12GetDebugInterface                       g_pfn_D3D12GetDebugInterface                       = NULL;
static PFN_D3D12CreateRootSignatureDeserializer         g_pfn_D3D12CreateRootSignatureDeserializer         = NULL;
static PFN_D3D12CreateVersionedRootSignatureDeserializer g_pfn_D3D12CreateVersionedRootSignatureDeserializer = NULL;
static PFN_D3D12SerializeRootSignature                  g_pfn_D3D12SerializeRootSignature                  = NULL;
static PFN_D3D12SerializeVersionedRootSignature         g_pfn_D3D12SerializeVersionedRootSignature         = NULL;
static PFN_D3D12EnableExperimentalFeatures              g_pfn_D3D12EnableExperimentalFeatures              = NULL;

static int                    g_DeviceHooksInstalled         = 0;
static PFN_CheckFeatureSupport g_pfnOriginalCheckFeatureSupport = NULL;
static PFN_CreateCommittedResource g_pfnOriginalCreateCommittedResource = NULL;

/* ---- Reserved-resource SRV-refresh fix (FixReservedResourceRefresh) ---- */
static int                          g_TileFixHooksInstalled       = 0;
static PFN_CreateCommandQueue        g_pfnOriginalCreateCommandQueue = NULL;
static PFN_CreateCommandList         g_pfnOriginalCreateCommandList  = NULL;
static PFN_CreateShaderResourceView  g_pfnOriginalCreateSRV          = NULL;
static PFN_UpdateTileMappings        g_pfnOriginalUpdateTileMappings = NULL;

/* Small table tracking which reserved resources have had UpdateTileMappings
 * called on them since their last CreateShaderResourceView -- see the big
 * comment above Hook_CreateShaderResourceView for why. Guarded by
 * g_TileTrackLock; only ever populated/read when FixReservedResourceRefresh
 * is enabled. */
#define DPROXY_MAX_TRACKED_RESOURCES 128
typedef struct {
    void *pResource;
    int   dirty;
} DPROXY_TrackedResource;

static CRITICAL_SECTION       g_TileTrackLock;
static int                    g_TileTrackLockReady    = 0;
static DPROXY_TrackedResource g_TrackedResources[DPROXY_MAX_TRACKED_RESOURCES];
static int                    g_TrackedResourceCount  = 0;

static int                    g_FenceHookInstalled           = 0;   /* NOUVEAU */
static int                    g_CmdListDeviceHookInstalled   = 0;   /* NOUVEAU */

/* ---- PSO/root-signature-recreated-every-frame investigation ----------- */
static PFN_CreateRootSignature         g_pfnOriginalCreateRootSignature         = NULL;
static PFN_CreateGraphicsPipelineState g_pfnOriginalCreateGraphicsPipelineState = NULL;
static PFN_CreateComputePipelineState  g_pfnOriginalCreateComputePipelineState  = NULL;
static int                             g_PsoHooksInstalled                      = 0;
static volatile LONG64                 g_CreateRootSignatureCount               = 0;
static volatile LONG64                 g_CreateGraphicsPSOCount                 = 0;
static volatile LONG64                 g_CreateComputePSOCount                  = 0;
/* also used by the D3D12CreateRootSignatureDeserializer export below */
static volatile LONG64                 g_RootSigDeserializerCount               = 0;

/* ---- ExecuteCommandLists coalescing (CoalesceCommandLists) ------------- *
 * Goal: on WDDM 1.x, every ExecuteCommandLists ends up as one or more
 * blocking D3DKMT_RENDER kernel escapes, with a roughly fixed cost per
 * escape regardless of how much GPU work is inside it. Some engines call
 * ExecuteCommandLists once per pass instead of batching (fine on WDDM 2.x/
 * hardware queues, expensive here). This buffers back-to-back calls on the
 * SAME queue and only actually submits (in one real ExecuteCommandLists
 * call) when a Signal, Wait, or tile-mapping change on that queue is about
 * to happen -- i.e. whenever ordering could become observable. This never
 * changes app-visible ordering; see Hook_ExecuteCommandLists for the full
 * correctness argument. Off by default -- enable per game via the ini. */
static PFN_ExecuteCommandLists g_pfnOriginalExecuteCommandLists = NULL;
static PFN_Signal              g_pfnOriginalSignal              = NULL;
static PFN_Wait                g_pfnOriginalWait                = NULL;

#define COALESCE_MAX_QUEUES   8
#define COALESCE_MAX_LISTS  64

typedef struct {
    void             *queue;
    void             *pending[COALESCE_MAX_LISTS];
    UINT              pendingCount;
    CRITICAL_SECTION  lock;    /* one lock PER SLOT (not one global lock) so
                                * a graphics queue and an async compute queue
                                * submitting from different threads don't
                                * serialize against each other needlessly. */
    int               lockInit;
} CoalesceSlot;

static CoalesceSlot     g_CoalesceSlots[COALESCE_MAX_QUEUES];
static CRITICAL_SECTION g_CoalesceTableLock;   /* guards only slot lookup/alloc */
static int              g_CoalesceTableLockInit = 0;

/* ---- ResourceBarrier coalescing (CoalesceResourceBarriers) ------------- *
 * Different problem from the ExecuteCommandLists coalescing above, and a
 * much narrower fix -- see the big comment above Hook_ResourceBarrier for
 * why. Buffers consecutive ResourceBarrier calls on a SPECIFIC command
 * list INSTANCE (not a queue -- barriers are recorded per-list, not
 * per-queue) and flushes (does the real, combined ResourceBarrier call)
 * the instant ANY other hooked recording method fires on that same list --
 * Dispatch/SetPipelineState/SetComputeRootSignature/
 * SetComputeRootUnorderedAccessView/Close/Reset. Never reorders barriers
 * relative to each other or to surrounding GPU work; only ever changes how
 * many ResourceBarrier() calls it takes to submit the exact same sequence. */
#define BARRIER_COALESCE_MAX_LISTS    8
#define BARRIER_COALESCE_MAX_PENDING 64

typedef struct {
    void                    *cmdList;   /* NULL = free slot */
    DPROXY_RESOURCE_BARRIER  pending[BARRIER_COALESCE_MAX_PENDING];
    UINT                     pendingCount;
    CRITICAL_SECTION         lock;      /* one per slot, same reasoning as CoalesceSlot above */
    int                      lockInit;
} BarrierCoalesceSlot;

static BarrierCoalesceSlot g_BarrierSlots[BARRIER_COALESCE_MAX_LISTS];
static CRITICAL_SECTION    g_BarrierTableLock;
static int                 g_BarrierTableLockInit = 0;

static PFN_CmdListClose                    g_pfnOriginalCmdListClose                = NULL;
static PFN_CmdListReset                    g_pfnOriginalCmdListReset                = NULL;
static PFN_CmdListDispatch                 g_pfnOriginalCmdListDispatch             = NULL;
static PFN_CmdListSetPipelineState         g_pfnOriginalCmdListSetPipelineState     = NULL;
static PFN_CmdListResourceBarrier          g_pfnOriginalCmdListResourceBarrier      = NULL;
static PFN_CmdListSetComputeRootSignature  g_pfnOriginalCmdListSetComputeRootSig    = NULL;
static PFN_CmdListSetComputeRootUAV        g_pfnOriginalCmdListSetComputeRootUAV    = NULL;

/* Same reasoning as MAX_TRACKED_QUEUE_VTABLES for queues: DIRECT/BUNDLE/
 * COMPUTE/COPY command lists are very likely backed by distinct C++
 * classes (hence distinct vtables) in the real runtime too, same as queues
 * were. Track every distinct vtable we've patched instead of a single
 * "done once" boolean. */
#define MAX_TRACKED_CMDLIST_VTABLES 8
static void *g_PatchedCmdListVtables[MAX_TRACKED_CMDLIST_VTABLES];
static int   g_PatchedCmdListVtableCount = 0;

/* ---- ID3D12CommandQueueDownlevel::Present investigation ---------------- *
 * See the big comment above Hook_DownlevelPresent: this is D3D12On7's
 * ONLY present path on Windows 7 (no IDXGISwapChain), it's a windowed-blt-
 * only operation, and it can carry a WAIT_FOR_VBLANK flag that blocks the
 * CPU synchronously inside the call. Never investigated before this. */
static PFN_QueueQueryInterface g_pfnOriginalQueueQueryInterface = NULL;
static PFN_DownlevelPresent    g_pfnOriginalDownlevelPresent    = NULL;
static int                    g_DownlevelPresentHookInstalled  = 0;
static volatile LONG64        g_ExecuteCommandListsCallCount   = 0;
static volatile LONG64        g_SignalCallCount                = 0;

/* ----------------------------------------------------------------------- *
 *  Small helpers
 * ----------------------------------------------------------------------- */

/* Case-insensitive ASCII string comparison, written by hand to avoid
 * relying on the (sometimes inconsistent) availability of _stricmp across
 * different MinGW runtime versions. */
static int EqualsIgnoreCaseA(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Parses strings like "9.1" .. "12.1", plus "Auto" / empty meaning
 * "no override, pass through whatever the application requested". */
static D3D_FEATURE_LEVEL ParseFeatureLevelString(const char *s, int *outHasOverride)
{
    *outHasOverride = 1;

    if (!s || s[0] == '\0' || EqualsIgnoreCaseA(s, "Auto")) {
        *outHasOverride = 0;
        return (D3D_FEATURE_LEVEL)0;
    }

    if (strcmp(s, "9.1")  == 0) return D3D_FEATURE_LEVEL_9_1;
    if (strcmp(s, "9.2")  == 0) return D3D_FEATURE_LEVEL_9_2;
    if (strcmp(s, "9.3")  == 0) return D3D_FEATURE_LEVEL_9_3;
    if (strcmp(s, "10.0") == 0) return D3D_FEATURE_LEVEL_10_0;
    if (strcmp(s, "10.1") == 0) return D3D_FEATURE_LEVEL_10_1;
    if (strcmp(s, "11.0") == 0) return D3D_FEATURE_LEVEL_11_0;
    if (strcmp(s, "11.1") == 0) return D3D_FEATURE_LEVEL_11_1;
    if (strcmp(s, "12.0") == 0) return D3D_FEATURE_LEVEL_12_0;
    if (strcmp(s, "12.1") == 0) return D3D_FEATURE_LEVEL_12_1;

    /* Unrecognized value: behave like "Auto" rather than failing hard. */
    *outHasOverride = 0;
    return (D3D_FEATURE_LEVEL)0;
}

/* Forward declaration: LogMsg itself is defined further down (Logging
 * section), but TileTrack_MarkDirty below needs to report a full tracking
 * table before we get there. */
static void LogMsg(const char *fmt, ...);

/* ----------------------------------------------------------------------- *
 *  Reserved-resource tile-mapping tracking (FixReservedResourceRefresh)
 * ----------------------------------------------------------------------- */
static void TileTrack_Init(void)
{
    InitializeCriticalSection(&g_TileTrackLock);
    g_TileTrackLockReady = 1;
}

static void TileTrack_Shutdown(void)
{
    if (g_TileTrackLockReady) {
        DeleteCriticalSection(&g_TileTrackLock);
        g_TileTrackLockReady = 0;
    }
}

/* Records that UpdateTileMappings was just called on pResource, meaning the
 * next CreateShaderResourceView over it should get the mip-toggle refresh
 * trick. Adds a new tracking slot if this resource hasn't been seen before. */
static void TileTrack_MarkDirty(void *pResource)
{
    int i;
    if (!g_TileTrackLockReady || !pResource) return;

    EnterCriticalSection(&g_TileTrackLock);
    for (i = 0; i < g_TrackedResourceCount; i++) {
        if (g_TrackedResources[i].pResource == pResource) {
            g_TrackedResources[i].dirty = 1;
            LeaveCriticalSection(&g_TileTrackLock);
            return;
        }
    }
    if (g_TrackedResourceCount < DPROXY_MAX_TRACKED_RESOURCES) {
        g_TrackedResources[g_TrackedResourceCount].pResource = pResource;
        g_TrackedResources[g_TrackedResourceCount].dirty     = 1;
        g_TrackedResourceCount++;
    } else {
        LogMsg("WARNING: tile-tracking table full (%d entries) -- not tracking "
               "resource %p for the SRV-refresh fix. Increase "
               "DPROXY_MAX_TRACKED_RESOURCES if this shows up in practice.",
               DPROXY_MAX_TRACKED_RESOURCES, pResource);
    }
    LeaveCriticalSection(&g_TileTrackLock);
}

/* Returns non-zero exactly once per UpdateTileMappings call on pResource:
 * checks the dirty flag and clears it atomically, so a steady-state app
 * that keeps recreating the same SRV every frame only pays the extra
 * "toggle" cost on the frame right after a real tile-mapping change. */
static int TileTrack_IsDirtyAndClear(void *pResource)
{
    int i, wasDirty = 0;
    if (!g_TileTrackLockReady || !pResource) return 0;

    EnterCriticalSection(&g_TileTrackLock);
    for (i = 0; i < g_TrackedResourceCount; i++) {
        if (g_TrackedResources[i].pResource == pResource) {
            wasDirty = g_TrackedResources[i].dirty;
            g_TrackedResources[i].dirty = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_TileTrackLock);
    return wasDirty;
}

/* ----------------------------------------------------------------------- *
 *  Logging
 * ----------------------------------------------------------------------- */
static void LogInit(const char *path)
{
    InitializeCriticalSection(&g_LogLock);
    if (g_Config.OutputLogFile) {
        g_LogFile = fopen(path, "a");
    }
}

static void LogMsg(const char *fmt, ...)
{
    if (!g_LogFile) return;

    EnterCriticalSection(&g_LogLock);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_LogFile, "[%02u:%02u:%02u.%03u] ",
            (unsigned)st.wHour, (unsigned)st.wMinute,
            (unsigned)st.wSecond, (unsigned)st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_LogFile, fmt, args);
    va_end(args);

    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);

    LeaveCriticalSection(&g_LogLock);
}

static void LogShutdown(void)
{
    if (g_LogFile) {
        fclose(g_LogFile);
        g_LogFile = NULL;
    }
    DeleteCriticalSection(&g_LogLock);
}

/* ----------------------------------------------------------------------- *
 *  Diagnostic console
 *
 *  Separate from the file logger on purpose: the file log is opt-in and
 *  meant for full call traces (verbose, only useful when actively
 *  debugging). The console is meant for the rarer, more actionable case
 *  the user asked for: "if we detect a bad/unsupported value coming back
 *  from d3d12on7 and decide to patch around it ourselves, say so loudly,
 *  right away, even if file logging is off." It is allocated lazily, the
 *  first time something actually needs to be printed to it, so a normal
 *  run with no workaround triggered never pops a console window at all
 *  (unless ShowConsole=1 forces it on unconditionally for general use).
 * ----------------------------------------------------------------------- */
static int g_ConsoleReady = 0;

static void EnsureConsole(void)
{
    if (g_ConsoleReady) return;
    if (AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        SetConsoleTitleA("d3d12proxy diagnostics");
    }
    g_ConsoleReady = 1;
}

/* Prints to the diagnostic console (lazily allocating it on first use)
 * AND mirrors the message into the file log if that's enabled. Use this
 * for anything the user genuinely needs to notice -- a detected bad value
 * from d3d12on7, a workaround kicking in, etc. -- as opposed to LogMsg's
 * routine call tracing. */
static void ConsoleMsg(const char *fmt, ...)
{
    EnsureConsole();

    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);

    if (g_LogFile) {
        EnterCriticalSection(&g_LogLock);
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_LogFile, "[%02u:%02u:%02u.%03u] [WORKAROUND] ",
                (unsigned)st.wHour, (unsigned)st.wMinute,
                (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
        va_start(args, fmt);
        vfprintf(g_LogFile, fmt, args);
        va_end(args);
        fprintf(g_LogFile, "\n");
        fflush(g_LogFile);
        LeaveCriticalSection(&g_LogLock);
    }
}

/* ----------------------------------------------------------------------- *
 *  Configuration loading
 * ----------------------------------------------------------------------- */

/* Fills outDir with the directory (including trailing backslash) that
 * contains the module identified by hModule. */
static void GetModuleDirectory(HMODULE hModule, char *outDir, size_t outSize)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH);

    if (len == 0 || len >= MAX_PATH) {
        outDir[0] = '\0';
        return;
    }

    char *lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    } else {
        path[0] = '\0';
    }

    strncpy(outDir, path, outSize - 1);
    outDir[outSize - 1] = '\0';
}

/* NOUVEAU : si d3d12proxy.ini n'existe pas encore a cote de la DLL, on en
 * genere un avec toutes les valeurs par defaut (et des commentaires), pour
 * que le fichier soit immediatement editable au lieu d'avoir a le deviner
 * ou a le recopier depuis ce fichier source. N'ecrase jamais un ini
 * existant : GENERIC_WRITE + CREATE_NEW echoue proprement (et sans race
 * TOCTOU vs un simple GetFileAttributes+fopen) si le fichier est deja la. */
static void CreateDefaultIniIfMissing(const char *iniPath)
{
    static const char *kDefaultIni =
        "; d3d12proxy.ini -- genere automatiquement avec les valeurs par\r\n"
        "; defaut la premiere fois que la DLL est chargee. Modifie ce que tu\r\n"
        "; veux ; les cles absentes retombent de toute facon sur ces memes\r\n"
        "; valeurs par defaut.\r\n"
        "\r\n"
        "[Settings]\r\n"
        "\r\n"
        "; Chemin complet vers le vrai d3d12.dll (D3D12On7) a wrapper.\r\n"
        "; OBLIGATOIRE -- rien ne fonctionne tant que ce n'est pas renseigne.\r\n"
        "DLLPath=\r\n"
        "\r\n"
        "; Feature level a rapporter a l'appli. \"Auto\" = ne change rien.\r\n"
        "EmulatedFeatureLevel=Auto\r\n"
        "\r\n"
        "; Ecrit le log dans d3d12proxy.log en plus de OutputDebugString.\r\n"
        "OutputLogFile=0\r\n"
        "\r\n"
        "; Fait passer les checks de ray tracing / DLSS comme supportes.\r\n"
        "BypassRayTracingChecks=0\r\n"
        "BypassDLSSChecks=0\r\n"
        "StripUnsupportedRootSigFlags=0\r\n"
        "StripUnsupportedHeapFlags=0\r\n"
        "FixReadbackInitialState=0\r\n"
        "\r\n"
        "; Ouvre une console pour voir les logs [d3d12proxy] en direct.\r\n"
        "ShowConsole=0\r\n"
        "\r\n"
        "; Fait repondre \"non supportees\" pour les tiled/reserved resources.\r\n"
        "DisableTiledResources=0\r\n"
        "\r\n"
        "; Tentative de fix pour le bug \"carre noir tant qu'un mip n'est pas\r\n"
        "; force a se rafraichir\" sur les reserved resources. Heuristique,\r\n"
        "; pas validee sur tous les builds D3D12On7 -- au cas par cas.\r\n"
        "FixReservedResourceRefresh=0\r\n"
        "\r\n"
        "; Logue la creation du device et le resultat de CheckFeatureSupport\r\n"
        "; (utile pour comprendre pourquoi un jeu conclut \"DX12 non supporte\").\r\n"
        "LogDeviceCreation=1\r\n"
        "LogFeatureSupport=1\r\n"
        "\r\n"
        "; Retire D3D12_FENCE_FLAG_SHARED des CreateFence (D3D12On7 ne\r\n"
        "; supporte pas les fences partagees inter-process).\r\n"
        "DisableSharedFences=1\r\n"
        "\r\n"
        "; Regroupe les ExecuteCommandLists consecutifs sur une meme queue\r\n"
        "; en un seul appel reel, pour reduire le nombre d'escapes noyau\r\n"
        "; D3DKMT_RENDER par frame sur WDDM 1.x. A activer au cas par cas.\r\n"
        "CoalesceCommandLists=0\r\n"
        "\r\n"
        "; Si une query CheckFeatureSupport echoue (feature plus recente que\r\n"
        "; D3D12On7), renvoie S_OK avec une structure a zero plutot que de\r\n"
        "; laisser l'echec se propager (l'appli le traite comme \"non\r\n"
        "; supportee\" au lieu de planter ou boucler dessus).\r\n"
        "SynthesizeFailedFeatureQueries=1\r\n"
        "\r\n"
        "; Mesure et logue chaque appel a ID3D12CommandQueueDownlevel::Present\r\n"
        "; (seul chemin de presentation sur D3D12On7/Win7), avec les flags\r\n"
        "; D3D12_DOWNLEVEL_PRESENT_FLAGS demandes par l'appli.\r\n"
        "LogPresentTiming=1\r\n"
        "\r\n"
        "; Retire le flag WAIT_FOR_VBLANK avant de forwarder le Present --\r\n"
        "; a tester si l'appli semble bloquer sur un double-vsync (CPU/GPU\r\n"
        "; bas mais FPS plafonne bien en dessous du refresh rate).\r\n"
        "StripPresentVBlankWait=0\r\n"
        "\r\n"
        "; Mesure et logue chaque CreateRootSignature / CreateGraphicsPipelineState\r\n"
        "; / CreateComputePipelineState (compte + temps passe dans le vrai\r\n"
        "; appel). Purement observationnel. Sert a verifier si l'appli\r\n"
        "; recompile son etat de pipeline a chaque frame au lieu de le\r\n"
        "; mettre en cache -- un cout CPU enorme via D3D12On7 si oui.\r\n"
        "LogPipelineStateCreation=1\r\n"
        "\r\n"
        "; Logue chaque ExecuteCommandLists/Signal reel (compte, taille,\r\n"
        "; valeur de fence), independamment de CoalesceCommandLists. Sert a\r\n"
        "; comparer le vrai rythme de rendu au rythme de Present : si Present\r\n"
        "; tourne a ~60Hz mais que ce compteur ne monte qu'a 15-20Hz, l'appli\r\n"
        "; re-presente de vieilles frames pendant que le rendu reel traine.\r\n"
        "LogQueueActivity=1\r\n"
        "\r\n"
        "; Fusionne les ResourceBarrier consecutifs (sans Dispatch/Draw/\r\n"
        "; SetPipelineState/etc. entre eux) enregistres sur une meme command\r\n"
        "; list en un seul appel groupe. Ne retire ni ne reordonne JAMAIS une\r\n"
        "; barriere -- change seulement le nombre d'appels API necessaires\r\n"
        "; pour transmettre la meme sequence au driver. ATTENTION : hooke une\r\n"
        "; large surface de ID3D12GraphicsCommandList (Dispatch, SetPipelineState,\r\n"
        "; SetComputeRootSignature, SetComputeRootUnorderedAccessView, Close,\r\n"
        "; Reset, en plus de ResourceBarrier). Concu et valide d'abord contre\r\n"
        "; notre propre banc d'essai (voir RunCorrectnessCheck) -- NE PAS activer\r\n"
        "; sur un vrai jeu sans etendre la liste des methodes hookees a tout ce\r\n"
        "; que CE jeu utilise reellement (DrawInstanced, CopyResource, etc.),\r\n"
        "; sous peine de fusionner une barriere a tort et corrompre le rendu\r\n"
        "; en silence.\r\n"
        "CoalesceResourceBarriers=0\r\n"
        "\r\n"
        "; Pour les jeux qui exigent Shader Model 6.6 (voir BypassDLSSChecks)\r\n"
        "; mais crashent des la premiere PSO/root signature une fois lances :\r\n"
        "; inspecte le conteneur DXBC de chaque shader passe a\r\n"
        "; CreateGraphicsPipelineState/CreateComputePipelineState/\r\n"
        "; CreateRootSignature et logue le Shader Model reellement declare\r\n"
        "; dans ses metadonnees DXIL. Purement observationnel, actif par\r\n"
        "; defaut (volume de log faible : quelques lignes par PSO cree).\r\n"
        "LogShaderModelDetection=1\r\n"
        "\r\n"
        "; Ecrit sur disque (sous-dossier shader_dumps) chaque blob shader\r\n"
        "; dont le DXIL declare SM6.6 ou plus, pour inspection hors-ligne\r\n"
        "; (dxil-dis, etc.). Desactive par defaut : peut ecrire beaucoup de\r\n"
        "; fichiers sur un jeu qui compile ses shaders SM6.6 en masse.\r\n"
        "DumpShaderModel66Blobs=0\r\n";

    HANDLE hFile = CreateFileA(iniPath, GENERIC_WRITE, 0, NULL,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        /* Most likely cause: the file already exists -- that's the normal,
         * expected case on every run after the first, so nothing to log. */
        return;
    }

    DWORD written = 0;
    WriteFile(hFile, kDefaultIni, (DWORD)strlen(kDefaultIni), &written, NULL);
    CloseHandle(hFile);
}

static void LoadConfig(void)
{
    char iniPath[MAX_PATH];
    char buf[MAX_PATH];

    snprintf(iniPath, sizeof(iniPath), "%sd3d12proxy.ini", g_ModuleDir);

    CreateDefaultIniIfMissing(iniPath);

    GetPrivateProfileStringA("Settings", "DLLPath", "",
                              g_Config.DLLPath, MAX_PATH, iniPath);

    GetPrivateProfileStringA("Settings", "EmulatedFeatureLevel", "Auto",
                              buf, MAX_PATH, iniPath);
    g_Config.EmulatedFeatureLevel =
        ParseFeatureLevelString(buf, &g_Config.HasFeatureLevelOverride);

    g_Config.OutputLogFile =
        GetPrivateProfileIntA("Settings", "OutputLogFile", 0, iniPath);
    g_Config.BypassRayTracingChecks =
        GetPrivateProfileIntA("Settings", "BypassRayTracingChecks", 0, iniPath);
    g_Config.BypassDLSSChecks =
        GetPrivateProfileIntA("Settings", "BypassDLSSChecks", 0, iniPath);
    g_Config.StripUnsupportedRootSigFlags =
        GetPrivateProfileIntA("Settings", "StripUnsupportedRootSigFlags", 0, iniPath);
    g_Config.StripUnsupportedHeapFlags =
        GetPrivateProfileIntA("Settings", "StripUnsupportedHeapFlags", 0, iniPath);
    g_Config.FixReadbackInitialState =
        GetPrivateProfileIntA("Settings", "FixReadbackInitialState", 0, iniPath);
    g_Config.ShowConsole =
        GetPrivateProfileIntA("Settings", "ShowConsole", 0, iniPath);
    g_Config.DisableTiledResources =
        GetPrivateProfileIntA("Settings", "DisableTiledResources", 0, iniPath);
    /* Separate, independent workaround from DisableTiledResources above:
     * that one makes well-behaved engines avoid tiled resources entirely.
     * This one is an actual attempted fix for the black-square-until-mip-
     * toggle bug, for apps (or samples) that use reserved resources
     * regardless -- see the big comment above Hook_CreateShaderResourceView.
     * Off by default: it's a heuristic based on one observed repro and
     * hasn't been validated across the full range of D3D12On7 builds. */
    g_Config.FixReservedResourceRefresh =
        GetPrivateProfileIntA("Settings", "FixReservedResourceRefresh", 0, iniPath);
    /* Default ON: this is the one call site that decides whether the app
     * concludes "DX12 supported" or not, so we want it visible even for
     * someone who hasn't turned on full call tracing (OutputLogFile). */
    g_Config.LogDeviceCreation =
        GetPrivateProfileIntA("Settings", "LogDeviceCreation", 1, iniPath);
    /* Default ON, same reasoning as LogDeviceCreation: this is the other
     * half of "why does the app think DX12/a feature is unsupported",
     * and it's otherwise invisible unless one of the Bypass options
     * happens to already be on (which most people won't have enabled
     * just to look). Purely observational -- does NOT patch any values
     * by itself; that's still gated by the Bypass / DisableTiledResources
     * settings below. */
    g_Config.LogFeatureSupport =
        GetPrivateProfileIntA("Settings", "LogFeatureSupport", 1, iniPath);

    /* NOUVEAU : supprime D3D12_FENCE_FLAG_SHARED des appels à CreateFence
     * Activé par défaut (1) car D3D12On7 ne supporte pas les fences partagées,
     * et ce drapeau n'est utile que pour le partage inter-processus.
     * Les applications qui utilisent des fences partagées au sein d'un même
     * processus (comme D3D12nBodyGravity) fonctionneront parfaitement sans. */
    g_Config.DisableSharedFences =
        GetPrivateProfileIntA("Settings", "DisableSharedFences", 1, iniPath);

    /* NOUVEAU : regroupe les ExecuteCommandLists consécutifs sur une même
     * queue en un seul appel réel, pour réduire le nombre d'escapes noyau
     * D3DKMT_RENDER par frame sur WDDM 1.x. Désactivé par défaut : à activer
     * au cas par cas (Control, Grid 2019...) via l'ini, pas globalement --
     * voir le commentaire au-dessus de Hook_ExecuteCommandLists. */
    g_Config.CoalesceCommandLists =
        GetPrivateProfileIntA("Settings", "CoalesceCommandLists", 0, iniPath);

    /* NOUVEAU : generalise le workaround OPTIONS12 a TOUTE feature qui
     * echoue. Active par defaut (1) car c'est la meme logique deja
     * jugee sure pour OPTIONS12 -- une query de feature optionnelle qui
     * echoue est censee etre traitee par l'appli comme "non supportee",
     * pas comme une exception. Certains jeux (Grid 2019 observe) bouclent
     * sur CheckFeatureSupport tant qu'ils n'obtiennent pas un SUCCEEDED(),
     * meme pour des features plus recentes que D3D12On7. */
    g_Config.SynthesizeFailedFeatureQueries =
        GetPrivateProfileIntA("Settings", "SynthesizeFailedFeatureQueries", 1, iniPath);

    /* NOUVEAU : investigation du seul chemin de present sur D3D12On7
     * (ID3D12CommandQueueDownlevel::Present -- pas d'IDXGISwapChain sur
     * Win7). LogPresentTiming se contente de mesurer/logguer, sans danger,
     * actif par defaut. StripPresentVBlankWait modifie le comportement
     * (retire le flag WAIT_FOR_VBLANK avant de forwarder) -- desactive par
     * defaut, a activer pour tester l'hypothese du double-vsync. */
    g_Config.LogPresentTiming =
        GetPrivateProfileIntA("Settings", "LogPresentTiming", 1, iniPath);
    g_Config.StripPresentVBlankWait =
        GetPrivateProfileIntA("Settings", "StripPresentVBlankWait", 0, iniPath);

    /* NOUVEAU : investigation "PSO/root signature recree a chaque frame ?".
     * Purement observationnel (aucun changement de comportement), actif
     * par defaut. Voir le commentaire au-dessus de Hook_CreateRootSignature. */
    g_Config.LogPipelineStateCreation =
        GetPrivateProfileIntA("Settings", "LogPipelineStateCreation", 1, iniPath);

    /* NOUVEAU : logue chaque ExecuteCommandLists/Signal reel, independamment
     * de CoalesceCommandLists. Purement observationnel : sert a comparer
     * le VRAI rythme de soumission/fin de frame (ce compteur) au rythme de
     * Present -- si Present tourne a ~60Hz mais que ce compteur-ci ne
     * monte qu'a ~15-20Hz, l'appli re-presente de vieilles frames pendant
     * que le rendu reel traine derriere. Actif par defaut. */
    g_Config.LogQueueActivity =
        GetPrivateProfileIntA("Settings", "LogQueueActivity", 1, iniPath);

    /* NOUVEAU : fusionne les ResourceBarrier consecutifs (sans Dispatch/Draw/
     * SetPipelineState/etc. entre eux) enregistres sur une MEME command list
     * en un seul appel groupe. Ne retire ni ne reordonne jamais une barriere
     * -- elle change seulement le nombre d'appels ResourceBarrier() reels
     * necessaires pour transmettre exactement la meme sequence au driver.
     * Voir Hook_ResourceBarrier / Coalesce_FlushBarriers. Defaut off : cette
     * fonctionnalite hooke la totalite des methodes de ID3D12GraphicsCommandList
     * qui enregistrent du travail GPU (Dispatch, Draw, SetPipelineState,
     * SetComputeRootSignature/SetComputeRootUnorderedAccessView/Close/Reset)
     * -- surface de hook bien plus large que tout ce qu'on a fait jusqu'ici,
     * a activer en connaissance de cause. */
    g_Config.CoalesceResourceBarriers =
        GetPrivateProfileIntA("Settings", "CoalesceResourceBarriers", 0, iniPath);

    /* NOUVEAU : diagnostic pour les jeux qui exigent Shader Model 6.6 (via
     * BypassDLSSChecks / EmulatedFeatureLevel) alors que D3D12On7 ne connait
     * reellement que jusqu'a SM6.4/6.5. LogShaderModelDetection inspecte le
     * conteneur DXBC de chaque shader passe a CreateGraphicsPipelineState/
     * CreateComputePipelineState/CreateRootSignature, lit le "program
     * version" et le "validator version" du chunk DXIL, et logue si le
     * bytecode declare reellement SM6.6 dans ses metadonnees (independamment
     * de ce qu'on a repondu a CheckFeatureSupport). DumpShaderModel66Blobs
     * ecrit sur disque (sous-dossier "shader_dumps") chaque blob VS/PS/CS/
     * DS/HS/GS ainsi que chaque root signature blob dont le DXIL declare
     * SM6.6, pour inspection hors-ligne (dxil-dis, etc.). Les deux sont
     * purement observationnels : rien n'est modifie avant l'appel reel. */
    g_Config.LogShaderModelDetection =
        GetPrivateProfileIntA("Settings", "LogShaderModelDetection", 1, iniPath);
    g_Config.DumpShaderModel66Blobs =
        GetPrivateProfileIntA("Settings", "DumpShaderModel66Blobs", 0, iniPath);
}

/* ----------------------------------------------------------------------- *
 *  ID3D12Device::CheckFeatureSupport hook
 *
 *  We replace the function pointer stored in slot 13 of the device's
 *  vtable (0-based index, counting from IUnknown::QueryInterface). This
 *  index matches the well-documented, stable layout of ID3D12Device:
 *    0 QueryInterface, 1 AddRef, 2 Release,
 *    3 GetPrivateData, 4 SetPrivateData, 5 SetPrivateDataInterface,
 *    6 SetName,
 *    7 GetNodeCount, 8 CreateCommandQueue, 9 CreateCommandAllocator,
 *    10 CreateGraphicsPipelineState, 11 CreateComputePipelineState,
 *    12 CreateCommandList, 13 CheckFeatureSupport, ...
 *
 *  If a future / different D3D12On7 build ever uses a different ABI for
 *  ID3D12Device, this index would need to be adjusted accordingly.
 * ----------------------------------------------------------------------- */
#define DEVICE_VTBL_INDEX_CHECKFEATURESUPPORT 13

/* Same reasoning/stability guarantee as DEVICE_VTBL_INDEX_CHECKFEATURESUPPORT
 * above, just further down the well-documented ID3D12Device layout:
 *   ... 7 GetNodeCount, 8 CreateCommandQueue, 9 CreateCommandAllocator,
 *   10 CreateGraphicsPipelineState, 11 CreateComputePipelineState,
 *   12 CreateCommandList, 13 CheckFeatureSupport, 14 CreateDescriptorHeap,
 *   15 GetDescriptorHandleIncrementSize, 16 CreateRootSignature,
 *   17 CreateConstantBufferView, 18 CreateShaderResourceView, ... */
#define DEVICE_VTBL_INDEX_CREATECOMMANDQUEUE        8
#define DEVICE_VTBL_INDEX_CREATECOMMANDLIST         12
#define DEVICE_VTBL_INDEX_CREATESHADERRESOURCEVIEW 18
/* NOUVEAU : pour l'investigation "PSO recree a chaque frame ?". Meme
 * layout public ID3D12Device deja utilise pour deduire les index ci-dessus :
 * 7 GetNodeCount, 8 CreateCommandQueue, 9 CreateCommandAllocator,
 * 10 CreateGraphicsPipelineState, 11 CreateComputePipelineState,
 * 12 CreateCommandList, 13 CheckFeatureSupport, 14 CreateDescriptorHeap,
 * 15 GetDescriptorHandleIncrementSize, 16 CreateRootSignature,
 * 17 CreateConstantBufferView, 18 CreateShaderResourceView (confirme par
 * DEVICE_VTBL_INDEX_CREATESHADERRESOURCEVIEW ci-dessus, deja valide en
 * prod). */
#define DEVICE_VTBL_INDEX_CREATEGRAPHICSPIPELINESTATE 10
#define DEVICE_VTBL_INDEX_CREATECOMPUTEPIPELINESTATE  11
#define DEVICE_VTBL_INDEX_CREATEROOTSIGNATURE         16
#define DEVICE_VTBL_INDEX_CREATECOMMITTEDRESOURCE     27

/* ---- ID3D12Device::CreateCommittedResource hook -----------------------
 * D3D12On7 predates D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT (0x800) and
 * D3D12_HEAP_FLAG_CREATE_NOT_ZEROED (0x1000), both introduced in Windows
 * 10 version 1903 -- years after D3D12On7 shipped. When HeapFlags carries
 * either bit, the real CreateCommittedResource rejects the call outright
 * (observed: failure, with the app's own error text reporting the failed
 * HRESULT). Apps that assume this call can only fail for out-of-memory/
 * device-removed reasons sometimes don't check the HRESULT before using
 * the (never-populated) output resource pointer, crashing with a null
 * dereference instead of hitting a clean error path.
 * This hook simply strips the two unsupported bits from HeapFlags before
 * forwarding to the real device. It cannot make "not zeroed"/"not
 * resident" allocation actually happen on D3D12On7 -- these are pure
 * performance/optimization hints, so forcing normal (zeroed, resident)
 * allocation behavior is always safe correctness-wise, just potentially
 * slower to allocate than the app intended. */
/* Matches ID3D12Device::GetResourceAllocationInfo. Returns
 * D3D12_RESOURCE_ALLOCATION_INFO {UINT64 SizeInBytes; UINT64 Alignment;}
 * by value, which in the Win64 ABI means the caller passes a pointer to
 * storage for the return value as the (hidden) first real argument, and
 * gets that same pointer back. visibleMask/numResourceDescs/pResourceDescs
 * match the real signature (single-resource form: visibleMask=1 node,
 * numResourceDescs=1). Purely used here as a read-only diagnostic probe;
 * we never hook this method, just call through the app's existing device
 * vtable slot with our own D3D12_RESOURCE_DESC to see what D3D12On7
 * itself thinks about that resource's size/alignment before we call
 * CreateCommittedResource with it. */
typedef void *(WINAPI *PFN_GetResourceAllocationInfo)(
    void *This, void *pRetVal, UINT visibleMask,
    UINT numResourceDescs, const void *pResourceDescs);

#define DEVICE_VTBL_INDEX_GETRESOURCEALLOCATIONINFO 25

static HRESULT WINAPI Hook_CreateCommittedResource(
    void *This, const void *pHeapProperties, UINT HeapFlags,
    const void *pDesc, UINT InitialResourceState,
    const void *pOptimizedClearValue, DPROXY_REFIID riidResource,
    void **ppvResource)
{
    UINT fixedFlags = HeapFlags;
    UINT fixedInitialState = InitialResourceState;

    if (g_Config.StripUnsupportedHeapFlags) {
        UINT badBits = HeapFlags & 0x1800u; /* NOT_RESIDENT | NOT_ZEROED */
        if (badBits) {
            fixedFlags = HeapFlags & ~0x1800u;
            LogMsg("CreateCommittedResource: stripping unsupported HeapFlags "
                   "0x%X from 0x%X (D3D12On7 does not support "
                   "CREATE_NOT_RESIDENT/CREATE_NOT_ZEROED).",
                   badBits, HeapFlags);
        }
    }

    /* ---- Fix up InitialResourceState for READBACK heaps -------------
     * Observed: Ghost of Tsushima creates one specific 8MB READBACK
     * buffer with InitialResourceState=0x0 (COMMON), which D3D12On7
     * rejects with E_INVALIDARG (0x80070057) -- deterministically,
     * every run, at the same call. Per the documented D3D12 contract,
     * READBACK resources must use D3D12_RESOURCE_STATE_COPY_DEST
     * (=0x400).
     * IMPORTANT: an earlier version of this fix forced *any*
     * non-COPY_DEST state on a READBACK heap to COPY_DEST. That broke a
     * DIFFERENT, previously-working READBACK buffer that the app
     * creates with InitialResourceState=0xAC3 (D3D12_RESOURCE_STATE_
     * GENERIC_READ) -- D3D12On7 apparently tolerates GENERIC_READ on a
     * READBACK heap just fine (undocumented but empirically true here),
     * so forcing it to COPY_DEST turned a working call into a failing
     * one. We now only touch the exact case actually seen failing
     * (COMMON, i.e. 0x0) and leave every other state alone, since we
     * only have evidence this one specific value is rejected. */
    if (g_Config.FixReadbackInitialState && pHeapProperties) {
        const DPROXY_HEAP_PROPERTIES_SHADOW *hp =
            (const DPROXY_HEAP_PROPERTIES_SHADOW *)pHeapProperties;
        if (hp->Type == 2u /* D3D12_HEAP_TYPE_READBACK */ &&
            InitialResourceState == 0x0u /* D3D12_RESOURCE_STATE_COMMON */) {
            LogMsg("CreateCommittedResource: fixing InitialResourceState "
                   "0x0 (COMMON) -> 0x400 (COPY_DEST) for READBACK heap "
                   "resource (D3D12 spec requires COPY_DEST; app passed "
                   "COMMON, which D3D12On7 rejects).");
            fixedInitialState = 0x400u;
        }
    }

    if (g_Config.LogPipelineStateCreation && pDesc && This) {
        /* Diagnostic probe: ask the real device what it thinks this
         * resource's allocation requirements are, via its own vtable
         * (index 25). If SizeInBytes/Alignment come back as 0 or
         * UINT64_MAX ("invalid"), that points at something wrong with
         * pDesc itself rather than the heap/state we've been focused on. */
        void **deviceVtable = *(void ***)This;
        PFN_GetResourceAllocationInfo pfnGetAllocInfo =
            (PFN_GetResourceAllocationInfo)deviceVtable[DEVICE_VTBL_INDEX_GETRESOURCEALLOCATIONINFO];
        struct { UINT64 SizeInBytes; UINT64 Alignment; } allocInfo = {0};
        pfnGetAllocInfo(This, &allocInfo, 0 /* visibleMask, node 0 */, 1, pDesc);
        LogMsg("CreateCommittedResource: GetResourceAllocationInfo probe -- "
               "SizeInBytes=%llu Alignment=%llu",
               (unsigned long long)allocInfo.SizeInBytes,
               (unsigned long long)allocInfo.Alignment);
    }

    if (g_Config.LogPipelineStateCreation) {
        char guidBuf[64];
        LogMsg("CreateCommittedResource: riidResource=%s",
               FormatGuid(riidResource, guidBuf, sizeof(guidBuf)));
    }

    if (g_Config.LogPipelineStateCreation) {
        LogMsg("CreateCommittedResource: pOptimizedClearValue=%p%s",
               pOptimizedClearValue,
               pOptimizedClearValue ? " (non-NULL -- INVALID for a BUFFER resource per D3D12 spec, must be nullptr)" : " (NULL, as required for a BUFFER resource)");
    }

    if (g_Config.LogPipelineStateCreation && pDesc) {
        const DPROXY_RESOURCE_DESC_SHADOW *d = (const DPROXY_RESOURCE_DESC_SHADOW *)pDesc;
        const DPROXY_HEAP_PROPERTIES_SHADOW *hp =
            (const DPROXY_HEAP_PROPERTIES_SHADOW *)pHeapProperties;
        LogMsg("CreateCommittedResource: about to call real fn -- "
               "Dimension=%u Width=%llu Height=%u DepthOrArraySize=%u "
               "MipLevels=%u Format=%u SampleCount=%u Layout=%u "
               "ResFlags=0x%X HeapFlags(fixed)=0x%X InitialState(fixed)=0x%X | "
               "HeapType=%u CPUPageProperty=%u MemoryPoolPreference=%u "
               "CreationNodeMask=0x%X VisibleNodeMask=0x%X",
               d->Dimension, (unsigned long long)d->Width, d->Height,
               d->DepthOrArraySize, d->MipLevels, d->Format, d->SampleCount,
               d->Layout, d->Flags, fixedFlags, fixedInitialState,
               hp ? hp->Type : 0xFFFFFFFFu,
               hp ? hp->CPUPageProperty : 0xFFFFFFFFu,
               hp ? hp->MemoryPoolPreference : 0xFFFFFFFFu,
               hp ? hp->CreationNodeMask : 0xFFFFFFFFu,
               hp ? hp->VisibleNodeMask : 0xFFFFFFFFu);
    }

    HRESULT hr = g_pfnOriginalCreateCommittedResource(
        This, pHeapProperties, fixedFlags, pDesc, fixedInitialState,
        pOptimizedClearValue, riidResource, ppvResource);

    LogMsg("CreateCommittedResource: real call returned 0x%08lX", (unsigned long)hr);

    return hr;
}

static HRESULT WINAPI Hook_CheckFeatureSupport(
    void *This, int Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize)
{
    HRESULT hr = g_pfnOriginalCheckFeatureSupport(
        This, Feature, pFeatureSupportData, FeatureSupportDataSize);

    /* ---- D3D12_FEATURE_D3D12_OPTIONS12 workaround (always on, not gated
     * by any Bypass-style config flag -- this is a correctness fix, not an
     * opt-in feature spoof) -------------------------------------------
     * Enhanced Barriers were introduced with Windows 11 / Agility SDK 1.6.
     * D3D12On7 on Windows 7 predates this entirely and does not recognize
     * the D3D12_FEATURE_D3D12_OPTIONS12 enum value: its CheckFeatureSupport
     * returns a failing HRESULT (e.g. E_INVALIDARG) instead of a
     * zeroed/FALSE struct, because as far as it's concerned this isn't a
     * valid D3D12_FEATURE value at all.
     * The spec-sanctioned way to probe for this *optional* feature is
     * exactly:
     *   D3D12_FEATURE_DATA_D3D12_OPTIONS12 o = {};
     *   if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,
     *                                          &o, sizeof(o))))
     *       enhancedBarriers = o.EnhancedBarriersSupported;
     * -- i.e. the app is SUPPOSED to treat a failing HRESULT here as "not
     * supported" and fall back to legacy ResourceBarrier. Well-behaved
     * samples (D3D12nBodyGravity included) do exactly that... but wrap the
     * call in ThrowIfFailed first, so the failing HRESULT throws a C++
     * exception before the app ever gets to look at it. We can't change
     * the app's code, so instead we make sure it never sees that failure:
     * whenever this query fails, we synthesize a clean S_OK response with
     * everything reported as unsupported (FALSE/0), which is the truthful
     * answer for a driver stack that predates the feature entirely. This
     * always lets the app fall through to its own legacy-barrier path. */
    /* ---- Generic "unrecognized/failed feature query" workaround --------
     * (always covers D3D12_FEATURE_D3D12_OPTIONS12 as a special case below
     * for MSPrimitivesPipelineStatisticIncludesCulledPrimitives history/
     * documentation; also covers everything else via
     * SynthesizeFailedFeatureQueries) -----------------------------------
     * D3D12On7 sits on a Windows 7-era D3D11 driver stack and its
     * CheckFeatureSupport implementation predates a lot of D3D12 feature
     * enum values added in later Windows 10 SDKs (Enhanced Barriers,
     * Sampler Feedback, Mesh Shaders, VRS Tier 2, etc.). For a value it
     * doesn't recognize at all, it returns a failing HRESULT (e.g.
     * E_INVALIDARG) instead of a zeroed/FALSE struct -- even though the
     * spec-sanctioned way apps are SUPPOSED to probe any *optional*
     * feature is exactly:
     *   D3D12_FEATURE_DATA_WHATEVER d = {};
     *   if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_WHATEVER,
     *                                          &d, sizeof(d))))
     *       ... use d ...
     * i.e. a failing HRESULT here is supposed to mean "treat as
     * unsupported / all fields false", not "something is wrong". Some
     * engines wrap this in ThrowIfFailed (crashes outright -- see the
     * OPTIONS12/Enhanced-Barriers case below, first found this way), and
     * others instead sit in a retry loop waiting for SUCCEEDED() before
     * continuing initialization (observed with Grid 2019 spamming
     * CheckFeatureSupport at high frequency and never proceeding to
     * render). Either way, the fix is the same: whenever the real call
     * fails, hand back a clean S_OK with the caller's own buffer zeroed
     * out (0/FALSE/Tier-0 for every field, which is the truthful answer
     * for a driver stack that predates the feature). This can never
     * report a capability as MORE supported than it truly is -- only
     * ever "not supported", which is always a safe answer for anything
     * genuinely optional. */
    if (FAILED(hr) && pFeatureSupportData && FeatureSupportDataSize > 0 &&
        FeatureSupportDataSize <= 4096 &&
        (Feature == DPROXY_FEATURE_D3D12_OPTIONS12 ||
         g_Config.SynthesizeFailedFeatureQueries)) {

        char featBuf[24];
        LogMsg("CheckFeatureSupport(%s): real call failed 0x%08lX -- "
               "synthesizing S_OK with a zeroed/all-unsupported struct so "
               "the app treats this as 'feature not supported' instead of "
               "an error/exception/retry condition.",
               FormatFeatureName(Feature, featBuf, sizeof(featBuf)), (unsigned long)hr);
        ConsoleMsg("[d3d12proxy] CheckFeatureSupport(%s) failed on the real "
                   "device (0x%08lX) -- reporting 'not supported' (zeroed "
                   "struct, S_OK) instead of letting the failure propagate.",
                   FormatFeatureName(Feature, featBuf, sizeof(featBuf)), (unsigned long)hr);

        ZeroMemory(pFeatureSupportData, FeatureSupportDataSize);
        hr = S_OK;
    }

    /* ---- Pure observation, no patching -----------------------------
     * Logs every CheckFeatureSupport call the app makes and what
     * D3D12On7 actually answered, regardless of whether any Bypass*
     * workaround is enabled. This is the other half of the
     * D3D12CreateDevice diagnostic: the device creation succeeding
     * doesn't mean the app is happy with what it finds afterwards --
     * an engine's DX12-support check often continues past device
     * creation into a series of CheckFeatureSupport calls (feature
     * level, shader model, options tiers...) and can still decide
     * "unsupported" based on any one of those.                        */
    if (g_Config.LogFeatureSupport) {
        char featBuf[24], hrBuf[16];
        const char *featName = FormatFeatureName(Feature, featBuf, sizeof(featBuf));

        if (!SUCCEEDED(hr) || !pFeatureSupportData) {
            ConsoleMsg("[d3d12proxy] CheckFeatureSupport(%s, size=%u) -> %s%s",
                       featName, FeatureSupportDataSize,
                       DecodeHR(hr, hrBuf, sizeof(hrBuf)),
                       FAILED(hr) ? " <-- feature query itself failed" : "");
        } else {
            /* Decode the fields most likely to gate a DX12-capable check.
             * Bounds-checked against FeatureSupportDataSize before each
             * struct cast, same pattern the Bypass* blocks below use. */
            switch (Feature) {
            case DPROXY_FEATURE_D3D12_OPTIONS:
                if (FeatureSupportDataSize >= sizeof(DPROXY_FEATURE_DATA_OPTIONS)) {
                    DPROXY_FEATURE_DATA_OPTIONS *o = (DPROXY_FEATURE_DATA_OPTIONS *)pFeatureSupportData;
                    ConsoleMsg("[d3d12proxy] CheckFeatureSupport(D3D12_OPTIONS) -> S_OK | "
                               "ResourceBindingTier=%d TiledResourcesTier=%d "
                               "MinPrecisionSupport=%d",
                               o->ResourceBindingTier, o->TiledResourcesTier, o->MinPrecisionSupport);
                }
                break;
            case DPROXY_FEATURE_D3D12_OPTIONS5:
                if (FeatureSupportDataSize >= sizeof(DPROXY_FEATURE_DATA_OPTIONS5)) {
                    DPROXY_FEATURE_DATA_OPTIONS5 *o5 = (DPROXY_FEATURE_DATA_OPTIONS5 *)pFeatureSupportData;
                    ConsoleMsg("[d3d12proxy] CheckFeatureSupport(D3D12_OPTIONS5) -> S_OK | "
                               "RaytracingTier=%d RenderPassesTier=%d",
                               o5->RaytracingTier, o5->RenderPassesTier);
                }
                break;
            case DPROXY_FEATURE_D3D12_OPTIONS12:
                if (FeatureSupportDataSize >= sizeof(DPROXY_FEATURE_DATA_OPTIONS12)) {
                    DPROXY_FEATURE_DATA_OPTIONS12 *o12 = (DPROXY_FEATURE_DATA_OPTIONS12 *)pFeatureSupportData;
                    ConsoleMsg("[d3d12proxy] CheckFeatureSupport(D3D12_OPTIONS12) -> S_OK | "
                               "EnhancedBarriersSupported=%d RelaxedFormatCastingSupported=%d",
                               o12->EnhancedBarriersSupported, o12->RelaxedFormatCastingSupported);
                }
                break;
            case DPROXY_FEATURE_SHADER_MODEL:
                if (FeatureSupportDataSize >= sizeof(DPROXY_FEATURE_DATA_SHADER_MODEL)) {
                    DPROXY_FEATURE_DATA_SHADER_MODEL *sm = (DPROXY_FEATURE_DATA_SHADER_MODEL *)pFeatureSupportData;
                    ConsoleMsg("[d3d12proxy] CheckFeatureSupport(SHADER_MODEL) -> S_OK | "
                               "HighestShaderModel=0x%X",
                               (unsigned)sm->HighestShaderModel);
                }
                break;
            case 2: /* FEATURE_LEVELS: D3D12_FEATURE_DATA_FEATURE_LEVELS.
                     * Layout on x64:
                     *   0  UINT NumFeatureLevels            (+4 padding)
                     *   8  const D3D_FEATURE_LEVEL *pFeatureLevelsRequested
                     *      (8-byte pointer, NOT 4 -- this was the bug: an
                     *      earlier version of this code read the field at
                     *      offset 8 expecting MaxSupportedFeatureLevel and
                     *      got half of this pointer instead, producing
                     *      garbage like 0xD5D8F0)
                     *  16  D3D_FEATURE_LEVEL MaxSupportedFeatureLevel  <-- this one
                     */
                if (FeatureSupportDataSize >= 20) {
                    D3D_FEATURE_LEVEL maxLevel =
                        *(D3D_FEATURE_LEVEL *)((char *)pFeatureSupportData + 16);
                    char flBuf[24];
                    ConsoleMsg("[d3d12proxy] CheckFeatureSupport(FEATURE_LEVELS) -> S_OK | "
                               "MaxSupportedFeatureLevel=%s",
                               FormatFeatureLevel(maxLevel, flBuf, sizeof(flBuf)));
                }
                break;
            default:
                ConsoleMsg("[d3d12proxy] CheckFeatureSupport(%s, size=%u) -> S_OK",
                           featName, FeatureSupportDataSize);
                break;
            }
        }
    }

    if (SUCCEEDED(hr) && pFeatureSupportData) {

        /* ---- Tiled / Reserved Resources workaround -------------------
         * D3D12On7 sits on top of a D3D11 driver. D3D11's "tiled
         * resources" support does not map cleanly onto everything D3D12
         * reserved resources / tile mapping calls expect (partial
         * residency semantics, certain tile-mapping flag combinations,
         * etc.), even when the underlying hardware/driver happily reports
         * Tier 2/3 support for D3D11. An app that trusts that reported
         * tier and then drives reserved resources the "full D3D12 way"
         * (e.g. heavy per-frame UpdateTileMappings during streaming) can
         * end up exercising a D3D12On7 code path that was never properly
         * validated against real D3D12 reserved-resource semantics --
         * which is exactly the kind of thing that can produce a
         * deterministic GPU hang at a specific heavy-streaming moment
         * (a cutscene/zone transition), independent of anything in our
         * own DXGI wrapper.
         *
         * We can't fix D3D12On7's translation itself from here, but we
         * CAN stop the app from ever trying to use tiled resources in the
         * first place: if it queries D3D12_FEATURE_D3D12_OPTIONS before
         * deciding whether to use reserved resources (which is standard
         * practice -- you're supposed to check TiledResourcesTier before
         * relying on it), we report NOT_SUPPORTED. A well-behaved engine
         * will then fall back to ordinary fully-resident (committed)
         * resources for texture streaming, trading some VRAM/streaming
         * efficiency for not hitting the broken path at all.            */
        if (g_Config.DisableTiledResources &&
            Feature == DPROXY_FEATURE_D3D12_OPTIONS &&
            FeatureSupportDataSize >= sizeof(DPROXY_FEATURE_DATA_OPTIONS)) {

            DPROXY_FEATURE_DATA_OPTIONS *opts =
                (DPROXY_FEATURE_DATA_OPTIONS *)pFeatureSupportData;

            if (opts->TiledResourcesTier != DPROXY_TILED_RESOURCES_TIER_NOT_SUPPORTED) {
                ConsoleMsg("[d3d12proxy] D3D12On7 reported TiledResourcesTier=%d "
                           "(claims tiled/reserved resource support) -- "
                           "DisableTiledResources workaround is forcing this "
                           "down to NOT_SUPPORTED so the app uses fully-resident "
                           "resources instead. This works around a known class "
                           "of D3D12On7 issues with reserved-resource tile "
                           "mapping, at the cost of higher VRAM/streaming "
                           "overhead in the app.",
                           opts->TiledResourcesTier);
                opts->TiledResourcesTier = DPROXY_TILED_RESOURCES_TIER_NOT_SUPPORTED;
            }
        }

        if (g_Config.BypassRayTracingChecks &&
            Feature == DPROXY_FEATURE_D3D12_OPTIONS5 &&
            FeatureSupportDataSize >= sizeof(DPROXY_FEATURE_DATA_OPTIONS5)) {

            DPROXY_FEATURE_DATA_OPTIONS5 *opts5 =
                (DPROXY_FEATURE_DATA_OPTIONS5 *)pFeatureSupportData;

            if (opts5->RaytracingTier < DPROXY_RAYTRACING_TIER_1_1) {
                LogMsg("CheckFeatureSupport: bypass forcing RaytracingTier %d -> %d",
                       opts5->RaytracingTier, DPROXY_RAYTRACING_TIER_1_1);
                opts5->RaytracingTier = DPROXY_RAYTRACING_TIER_1_1;
            }
        }

        if (g_Config.BypassDLSSChecks &&
            Feature == DPROXY_FEATURE_SHADER_MODEL &&
            FeatureSupportDataSize >= sizeof(DPROXY_FEATURE_DATA_SHADER_MODEL)) {

            DPROXY_FEATURE_DATA_SHADER_MODEL *sm =
                (DPROXY_FEATURE_DATA_SHADER_MODEL *)pFeatureSupportData;

            if (sm->HighestShaderModel < DPROXY_SHADER_MODEL_6_6) {
                LogMsg("CheckFeatureSupport: bypass forcing HighestShaderModel 0x%X -> 0x%X",
                       (unsigned)sm->HighestShaderModel, (unsigned)DPROXY_SHADER_MODEL_6_6);
                sm->HighestShaderModel = DPROXY_SHADER_MODEL_6_6;
            }
        }
    }

    return hr;
}

// Fence hook, for samples like D3D12nBodyGravity

/* ----------------------------------------------------------------------- *
 *  ID3D12Device::CreateFence hook
 *
 *  D3D12On7 on Windows 7 does NOT support D3D12_FENCE_FLAG_SHARED.
 *  Applications that use this flag (even for in-process synchronization,
 *  like D3D12nBodyGravity) will fail with E_INVALIDARG or E_NOTIMPL.
 *
 *  We strip the SHARED flag transparently so the app works as expected.
 *  This is safe because the SHARED flag is only needed for cross-process
 *  fence sharing, which is not supported on D3D12On7 anyway.
 * ----------------------------------------------------------------------- */
#define DEVICE_VTBL_INDEX_CREATEFENCE  (0x120 / 8)   /* 36 */
#define D3D12_FENCE_FLAG_SHARED        0x1

static HRESULT WINAPI Hook_CreateFence(
    void *This,
    UINT64 InitialValue,
    UINT Flags,
    DPROXY_REFIID riid,
    void **ppFence)
{
    UINT newFlags = Flags;

    /* Si le drapeau SHARED est présent, le supprimer silencieusement */
    if (g_Config.DisableSharedFences && (Flags & D3D12_FENCE_FLAG_SHARED)) {
        newFlags = Flags & ~D3D12_FENCE_FLAG_SHARED;
        LogMsg("CreateFence: stripping D3D12_FENCE_FLAG_SHARED (was 0x%X, now 0x%X)", Flags, newFlags);
        ConsoleMsg("[d3d12proxy] CreateFence: SHARED flag removed to work around D3D12On7 limitation.");
    }

    return g_pfnOriginalCreateFence(This, InitialValue, newFlags, riid, ppFence);
}


/* ----------------------------------------------------------------------- *
 *  Reserved-resource SRV-refresh fix
 *
 *  THE BUG (as reported): a reserved/tiled resource (e.g. D3D12ReservedResources'
 *  demo texture) renders as a solid black square the first time it's sampled
 *  after its tiles get mapped via ID3D12CommandQueue::UpdateTileMappings. The
 *  square only starts rendering correctly once the user manually decreases the
 *  displayed mip level and then increases it back -- i.e. once the app's UI
 *  causes a *second* CreateShaderResourceView with a different MostDetailedMip
 *  to happen. This does not occur with vkd3d.
 *
 *  WORKING THEORY: D3D12On7 translates D3D12 tile-mapping calls down into the
 *  D3D11 driver's own (differently-shaped) tiled-resource API. Something in
 *  that translation defers actually committing/binding the newly-mapped tile
 *  data until the driver revalidates the resource's bound state -- and the
 *  very first SRV created over the resource after mapping doesn't trigger
 *  that revalidation, while a second, *different* SRV creation does (this is
 *  a common shape of bug in D3D11-style dirty-state tracking). We can't fix
 *  D3D12On7's internals from here, but we CAN automatically replicate the
 *  exact manual workaround the tester found: the moment we see a
 *  CreateShaderResourceView call for a resource that just had
 *  UpdateTileMappings called on it, we transparently create the view TWICE --
 *  once with a deliberately different MostDetailedMip (immediately
 *  overwritten, never actually exposed to the app), then again with the
 *  app's real requested descriptor -- before returning. This costs one extra
 *  (cheap, CPU-side) view creation, exactly once per tile-mapping change, and
 *  only for D3D12_SRV_DIMENSION_TEXTURE2D views (the common case for a tiled
 *  streaming texture; other view dimensions are passed through unmodified).
 *
 *  This is a heuristic based on a single observed repro, not something we've
 *  been able to validate against every D3D12On7 build -- hence its own
 *  opt-in ini flag, independent of DisableTiledResources above (which is a
 *  blunter "avoid tiled resources altogether" workaround for engines that
 *  check TiledResourcesTier before using them; this flag is for the cases
 *  -- like the sample itself -- that use them regardless).
 * ----------------------------------------------------------------------- */
static void WINAPI Hook_CreateShaderResourceView(
    void *This, void *pResource, void *pDesc, DPROXY_CPU_DESCRIPTOR_HANDLE Dest)
{
    if (g_Config.FixReservedResourceRefresh && pResource && pDesc &&
        TileTrack_IsDirtyAndClear(pResource)) {

        UINT viewDim = *(UINT *)((BYTE *)pDesc + 4);

        if (viewDim == DPROXY_SRV_DIMENSION_TEXTURE2D) {
            UINT *pMostDetailedMip =
                (UINT *)((BYTE *)pDesc + DPROXY_SRV_TEX2D_MOSTDETAILEDMIP_OFFSET);
            UINT *pMipLevels =
                (UINT *)((BYTE *)pDesc + DPROXY_SRV_TEX2D_MIPLEVELS_OFFSET);
            UINT origMip  = *pMostDetailedMip;
            UINT mipCount = *pMipLevels;

            /* Sanity-check before touching the app's own struct at all: if
             * these offsets were ever wrong (different SDK/compiler ABI),
             * refuse to write rather than risk corrupting memory we don't
             * own. A real MipLevels is never 0 and rarely above ~16. */
            if (mipCount >= 1 && mipCount <= 32 && origMip < mipCount) {
                UINT dummyMip = (origMip > 0) ? (origMip - 1)
                                               : ((mipCount > 1) ? (origMip + 1) : origMip);

                if (dummyMip != origMip) {
                    ConsoleMsg("[d3d12proxy] CreateShaderResourceView: resource %p just had "
                               "UpdateTileMappings called on it -- replaying the manual "
                               "\"toggle the mip slider\" workaround automatically "
                               "(MostDetailedMip %u -> %u -> %u) before creating the real view.",
                               pResource, origMip, dummyMip, origMip);

                    *pMostDetailedMip = dummyMip;
                    g_pfnOriginalCreateSRV(This, pResource, pDesc, Dest);
                    *pMostDetailedMip = origMip; /* restore the app's own descriptor */
                }
            } else {
                LogMsg("Hook_CreateShaderResourceView: mip sanity check failed "
                       "(MostDetailedMip=%u MipLevels=%u) for resource %p -- "
                       "skipping the refresh trick for this call.",
                       origMip, mipCount, pResource);
            }
        }
    }

    g_pfnOriginalCreateSRV(This, pResource, pDesc, Dest);
}

/* Matches ID3D12CommandQueue::UpdateTileMappings. Just forwards, then marks
 * the resource dirty so the next CreateShaderResourceView over it gets the
 * refresh trick above. There's no HRESULT to check (the real method returns
 * void), so we always mark dirty; a spurious extra "toggle" on a call the
 * driver silently ignored is harmless. */
/* Forward declaration: defined further below, alongside the rest of the
 * ExecuteCommandLists-coalescing machinery, but needed here since
 * Hook_UpdateTileMappings (right below) calls it. */
static void Coalesce_FlushBeforeTileMappingChange(void *queueThis);

static void WINAPI Hook_UpdateTileMappings(
    void *This, void *pResource, UINT NumResourceRegions,
    const void *pResourceRegionStartCoordinates, const void *pResourceRegionSizes,
    void *pHeap, UINT NumRanges, const void *pRangeFlags,
    const UINT *pHeapRangeStartOffsets, const UINT *pRangeTileCounts, UINT Flags)
{
    /* NOUVEAU: if CoalesceCommandLists buffered an ExecuteCommandLists on
     * THIS queue that references pResource, that command list must observe
     * the tile mapping as it was when it was originally submitted, not
     * whatever it becomes after this call. Flush first. Cheap relative to
     * a kernel escape, so always done regardless of pResource's identity. */
    Coalesce_FlushBeforeTileMappingChange(This);

    g_pfnOriginalUpdateTileMappings(This, pResource, NumResourceRegions,
                                     pResourceRegionStartCoordinates, pResourceRegionSizes,
                                     pHeap, NumRanges, pRangeFlags,
                                     pHeapRangeStartOffsets, pRangeTileCounts, Flags);

    if (pResource) {
        TileTrack_MarkDirty(pResource);
    }
}

#define CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS 7
#define CMDQUEUE_VTBL_INDEX_EXECUTECOMMANDLISTS (CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS + 2)
#define CMDQUEUE_VTBL_INDEX_SIGNAL              (CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS + 6)
#define CMDQUEUE_VTBL_INDEX_WAIT                (CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS + 7)

/* ID3D12GraphicsCommandList vtable indices -- verifies contre l'ordre reel
 * de l'interface (confirme via un reimplementation complete de cette
 * vtable qu'on avait sous la main, pas juste memorise) :
 *   0 QueryInterface, 1 AddRef, 2 Release, 3 GetPrivateData,
 *   4 SetPrivateData, 5 SetPrivateDataInterface, 6 SetName, 7 GetDevice,
 *   8 GetType, 9 Close, 10 Reset, 11 ClearState, 12 DrawInstanced,
 *   13 DrawIndexedInstanced, 14 Dispatch, ..., 25 SetPipelineState,
 *   26 ResourceBarrier, ..., 29 SetComputeRootSignature, ...,
 *   41 SetComputeRootUnorderedAccessView, ...
 * On ne hooke QUE les methodes que notre PROPRE banc d'essai utilise --
 * volontairement pas plus, voir le commentaire au-dessus de
 * Hook_ResourceBarrier pour pourquoi ce n'est PAS extensible tel quel a un
 * vrai jeu. */
#define CMDLIST_VTBL_INDEX_CLOSE                          9
#define CMDLIST_VTBL_INDEX_RESET                          10
#define CMDLIST_VTBL_INDEX_DISPATCH                       14
#define CMDLIST_VTBL_INDEX_SETPIPELINESTATE               25
#define CMDLIST_VTBL_INDEX_RESOURCEBARRIER                26
#define CMDLIST_VTBL_INDEX_SETCOMPUTEROOTSIGNATURE        29
#define CMDLIST_VTBL_INDEX_SETCOMPUTEROOTUNORDEREDACCESSVIEW 41

/* Call once from ProxyInitialize(), before any queue can be created. */
static void Coalesce_GlobalInit(void)
{
    if (!g_CoalesceTableLockInit) {
        InitializeCriticalSection(&g_CoalesceTableLock);
        g_CoalesceTableLockInit = 1;
        ZeroMemory(g_CoalesceSlots, sizeof(g_CoalesceSlots));
    }
}

/* Call once from ProxyShutdown(), after the last queue is guaranteed done. */
static void Coalesce_GlobalShutdown(void)
{
    if (!g_CoalesceTableLockInit) return;
    for (int i = 0; i < COALESCE_MAX_QUEUES; i++) {
        if (g_CoalesceSlots[i].lockInit) DeleteCriticalSection(&g_CoalesceSlots[i].lock);
    }
    DeleteCriticalSection(&g_CoalesceTableLock);
    g_CoalesceTableLockInit = 0;
}

/* Finds (or allocates) this queue's slot and returns it LOCKED. Caller must
 * LeaveCriticalSection(&slot->lock) when done. NULL (nothing locked) if
 * every slot is already taken by some other still-alive queue -- caller
 * should just bypass coalescing for that call in that case. */
static CoalesceSlot *CoalesceSlot_FindAndLock(void *queue)
{
    CoalesceSlot *slot = NULL;

    EnterCriticalSection(&g_CoalesceTableLock);
    for (int i = 0; i < COALESCE_MAX_QUEUES; i++) {
        if (g_CoalesceSlots[i].queue == queue) { slot = &g_CoalesceSlots[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < COALESCE_MAX_QUEUES; i++) {
            if (g_CoalesceSlots[i].queue == NULL) {
                g_CoalesceSlots[i].queue = queue;
                InitializeCriticalSection(&g_CoalesceSlots[i].lock);
                g_CoalesceSlots[i].lockInit = 1;
                slot = &g_CoalesceSlots[i];
                break;
            }
        }
    }
    LeaveCriticalSection(&g_CoalesceTableLock);

    if (slot) EnterCriticalSection(&slot->lock);
    return slot;
}

/* Forwards whatever is pending to the real queue in ONE ExecuteCommandLists
 * call. Must be called with slot->lock held. */
static void CoalesceSlot_FlushLocked(CoalesceSlot *slot)
{
    if (!slot || slot->pendingCount == 0) return;
    g_pfnOriginalExecuteCommandLists(slot->queue, slot->pendingCount, slot->pending);
    LogMsg("Coalesce: flushed %u command list(s) in a single ExecuteCommandLists.",
           slot->pendingCount);
    slot->pendingCount = 0;
}

/* Flushes every live slot. Call from ProxyShutdown() so nothing pending is
 * silently dropped on exit / device removal. (This proxy has no Present
 * hook point today -- D3D12On7's d3d12.dll never sees IDXGISwapChain::
 * Present, the app calls that directly -- so in practice the per-Signal/
 * Wait flush above is what keeps each frame's work flushed before the app's
 * own Present call, since essentially every engine signals a frame fence
 * at some point at or after the end of the frame.) */
static void CoalesceSlot_FlushAll(void)
{
    if (!g_CoalesceTableLockInit) return;
    for (int i = 0; i < COALESCE_MAX_QUEUES; i++) {
        if (!g_CoalesceSlots[i].lockInit) continue;
        EnterCriticalSection(&g_CoalesceSlots[i].lock);
        CoalesceSlot_FlushLocked(&g_CoalesceSlots[i]);
        LeaveCriticalSection(&g_CoalesceSlots[i].lock);
    }
}

static void Coalesce_FlushBeforeTileMappingChange(void *queueThis)
{
    if (!g_Config.CoalesceCommandLists) return;
    CoalesceSlot *slot = CoalesceSlot_FindAndLock(queueThis);
    if (slot) { CoalesceSlot_FlushLocked(slot); LeaveCriticalSection(&slot->lock); }
}

/* ---- ResourceBarrier coalescing helpers --------------------------------
 * Mirrors the ExecuteCommandLists coalescing helpers above almost exactly,
 * but keyed per COMMAND LIST INSTANCE (This pointer of the actual
 * ID3D12GraphicsCommandList) instead of per queue -- barriers are recorded
 * into a specific list, they don't belong to a queue until ExecuteCommandLists
 * happens. See the big comment above Hook_ResourceBarrier for the full
 * correctness argument. */

static void Coalesce_BarrierGlobalInit(void)
{
    if (!g_BarrierTableLockInit) {
        InitializeCriticalSection(&g_BarrierTableLock);
        g_BarrierTableLockInit = 1;
        ZeroMemory(g_BarrierSlots, sizeof(g_BarrierSlots));
    }
}

static void Coalesce_BarrierGlobalShutdown(void)
{
    if (!g_BarrierTableLockInit) return;
    for (int i = 0; i < BARRIER_COALESCE_MAX_LISTS; i++) {
        if (g_BarrierSlots[i].lockInit) DeleteCriticalSection(&g_BarrierSlots[i].lock);
    }
    DeleteCriticalSection(&g_BarrierTableLock);
    g_BarrierTableLockInit = 0;
}

/* Finds (or allocates) this command list's slot and returns it LOCKED.
 * Caller must LeaveCriticalSection(&slot->lock) when done. NULL if every
 * slot is taken by some other still-recording list -- caller should just
 * bypass coalescing (pass the call straight through) in that case. */
static BarrierCoalesceSlot *BarrierSlot_FindAndLock(void *cmdList)
{
    BarrierCoalesceSlot *slot = NULL;

    EnterCriticalSection(&g_BarrierTableLock);
    for (int i = 0; i < BARRIER_COALESCE_MAX_LISTS; i++) {
        if (g_BarrierSlots[i].cmdList == cmdList) { slot = &g_BarrierSlots[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < BARRIER_COALESCE_MAX_LISTS; i++) {
            if (g_BarrierSlots[i].cmdList == NULL) {
                g_BarrierSlots[i].cmdList = cmdList;
                InitializeCriticalSection(&g_BarrierSlots[i].lock);
                g_BarrierSlots[i].lockInit = 1;
                slot = &g_BarrierSlots[i];
                break;
            }
        }
    }
    LeaveCriticalSection(&g_BarrierTableLock);

    if (slot) EnterCriticalSection(&slot->lock);
    return slot;
}

/* Forwards whatever is pending to the real command list in ONE
 * ResourceBarrier call, in the EXACT order they were recorded (the pending
 * buffer is filled strictly in append order, never reordered). Must be
 * called with slot->lock held. */
static void BarrierSlot_FlushLocked(BarrierCoalesceSlot *slot)
{
    if (!slot || slot->pendingCount == 0) return;
    g_pfnOriginalCmdListResourceBarrier(slot->cmdList, slot->pendingCount, slot->pending);
    if (slot->pendingCount > 1) {
        LogMsg("Coalesce: flushed %u ResourceBarrier(s) into a single call (cmdList=%p).",
               slot->pendingCount, slot->cmdList);
    }
    slot->pendingCount = 0;
}

static void BarrierSlot_FlushAll(void)
{
    if (!g_BarrierTableLockInit) return;
    for (int i = 0; i < BARRIER_COALESCE_MAX_LISTS; i++) {
        if (!g_BarrierSlots[i].lockInit) continue;
        EnterCriticalSection(&g_BarrierSlots[i].lock);
        BarrierSlot_FlushLocked(&g_BarrierSlots[i]);
        LeaveCriticalSection(&g_BarrierSlots[i].lock);
    }
}

/* Called by every OTHER hooked command-list method (Dispatch/
 * SetPipelineState/SetComputeRootSignature/SetComputeRootUnorderedAccessView/
 * Close/Reset) before doing its own real work -- see the big comment above
 * Hook_ResourceBarrier. Frees the slot too on Close/Reset (isRecordingEnd)
 * since a list about to be reset has no more use for its old pending state,
 * and freeing the slot lets another list reuse it. */
static void Coalesce_FlushBarriersFor(void *cmdList, int isRecordingEnd)
{
    if (!g_Config.CoalesceResourceBarriers) return;
    BarrierCoalesceSlot *slot = BarrierSlot_FindAndLock(cmdList);
    if (!slot) return;
    BarrierSlot_FlushLocked(slot);
    if (isRecordingEnd) {
        slot->cmdList = NULL; /* free the slot for reuse by another list */
    }
    LeaveCriticalSection(&slot->lock);
}

/* ============================================================================
 * ID3D12GraphicsCommandList hooks (CoalesceResourceBarriers)
 *
 * WHY: the D3D12on7 submission-pattern benchmark showed that splitting the
 * SAME total GPU work into more Dispatch()+barrier passes costs real,
 * measurable time even with the total ExecuteCommandLists/Present count
 * unchanged -- each barrier is a genuine GPU pipeline synchronization
 * point. We can't safely eliminate a barrier that protects a REAL
 * dependency (that would be a silent GPU race condition -- see the long
 * discussion that led here). What we CAN safely do: some engines emit
 * several *independent* resources' barriers as separate, consecutive
 * ResourceBarrier(1, &x) calls (one resource at a time) when nothing
 * actually depends on the order between them. The API already supports
 * passing all of them in ONE ResourceBarrier(N, array) call -- ours simply
 * does that batching for the engine when it doesn't do it itself.
 *
 * HOW THIS STAYS SAFE: unlike a real game (where hooking every single
 * GPU-work-recording method would be an enormous, too-risky undertaking),
 * this feature is scoped to be used with our OWN benchmark first, where we
 * know and have hooked EVERY method that records GPU work on the list:
 * Dispatch, SetPipelineState, SetComputeRootSignature,
 * SetComputeRootUnorderedAccessView, Close, Reset. Every one of them
 * flushes (does the real, immediate, combined ResourceBarrier call for
 * whatever's pending) BEFORE doing its own real work. That means: from the
 * driver's point of view, the exact same sequence of barriers arrives in
 * the exact same relative order relative to the exact same Dispatch/Close/
 * Reset calls as without coalescing -- only the NUMBER of ResourceBarrier()
 * API calls needed to deliver that sequence changes, never the sequence
 * itself. If you extend this to hook a real game's command lists, you MUST
 * hook literally every GPU-work-recording method it might call (DrawInstanced,
 * DrawIndexedInstanced, CopyResource, CopyBufferRegion, CopyTextureRegion,
 * ClearRenderTargetView, ClearDepthStencilView, ClearUnorderedAccessView*,
 * ResolveSubresource, ExecuteBundle, ExecuteIndirect, ...) or this
 * guarantee silently breaks.
 * ============================================================================ */

static void WINAPI Hook_ResourceBarrier(
    void *This, UINT NumBarriers, const DPROXY_RESOURCE_BARRIER *pBarriers)
{
    BarrierCoalesceSlot *slot;
    UINT i;

    if (!g_Config.CoalesceResourceBarriers || NumBarriers == 0 || !pBarriers) {
        g_pfnOriginalCmdListResourceBarrier(This, NumBarriers, pBarriers);
        return;
    }

    slot = BarrierSlot_FindAndLock(This);
    if (!slot) {
        /* No free tracking slot -- fall back to an immediate, unbatched
         * call rather than dropping or corrupting anything. */
        g_pfnOriginalCmdListResourceBarrier(This, NumBarriers, pBarriers);
        return;
    }

    if (slot->pendingCount + NumBarriers > BARRIER_COALESCE_MAX_PENDING) {
        BarrierSlot_FlushLocked(slot); /* make room */
    }

    if (NumBarriers > BARRIER_COALESCE_MAX_PENDING) {
        /* Single call bigger than our whole buffer -- pass it straight
         * through, nothing to gain from batching an already-large call. */
        g_pfnOriginalCmdListResourceBarrier(This, NumBarriers, pBarriers);
        LeaveCriticalSection(&slot->lock);
        return;
    }

    for (i = 0; i < NumBarriers; i++) {
        slot->pending[slot->pendingCount++] = pBarriers[i];
    }
    LeaveCriticalSection(&slot->lock);
}

static void WINAPI Hook_CmdListDispatch(void *This, UINT x, UINT y, UINT z)
{
    Coalesce_FlushBarriersFor(This, 0);
    g_pfnOriginalCmdListDispatch(This, x, y, z);
}

static void WINAPI Hook_CmdListSetPipelineState(void *This, void *pPipelineState)
{
    Coalesce_FlushBarriersFor(This, 0);
    g_pfnOriginalCmdListSetPipelineState(This, pPipelineState);
}

static void WINAPI Hook_CmdListSetComputeRootSignature(void *This, void *pRootSignature)
{
    Coalesce_FlushBarriersFor(This, 0);
    g_pfnOriginalCmdListSetComputeRootSig(This, pRootSignature);
}

static void WINAPI Hook_CmdListSetComputeRootUAV(
    void *This, UINT RootParameterIndex, DPROXY_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    Coalesce_FlushBarriersFor(This, 0);
    g_pfnOriginalCmdListSetComputeRootUAV(This, RootParameterIndex, BufferLocation);
}

static HRESULT WINAPI Hook_CmdListClose(void *This)
{
    Coalesce_FlushBarriersFor(This, 1); /* 1 = frees the tracking slot too */
    return g_pfnOriginalCmdListClose(This);
}

static HRESULT WINAPI Hook_CmdListReset(void *This, void *pAllocator, void *pInitialState)
{
    /* Whatever was pending belongs to the recording session that's about to
     * end -- flush it (harmless if empty) and free the slot so the fresh
     * recording session starts with a clean, empty pending buffer. */
    Coalesce_FlushBarriersFor(This, 1);
    return g_pfnOriginalCmdListReset(This, pAllocator, pInitialState);
}

/* IMPORTANT CORRECTNESS NOTE: two separate ExecuteCommandLists calls
 * guarantee "A fully finishes before B starts" AND let the app race a
 * fence Signal/Wait against that boundary (see MSDN). A single merged
 * call gives the driver a bigger, coarser scheduling unit and loses that
 * fine-grained boundary. This hook stays correct by flushing (forcing a
 * real, separate ExecuteCommandLists call) on every Signal/Wait/tile-
 * mapping-change for this queue -- see Hook_Signal/Hook_Wait/
 * Coalesce_FlushBeforeTileMappingChange -- so app-visible ordering never
 * changes; only truly back-to-back, fence-free submissions get merged. */
static void WINAPI Hook_ExecuteCommandLists(
    void *This, UINT NumCommandLists, void *const *ppCommandLists)
{
    /* NOUVEAU: purely observational, independent of CoalesceCommandLists --
     * see the big comment above Hook_DownlevelPresent's "stale frame"
     * detection. If the real render/submit rate (this call) is much lower
     * than the Present call rate, the app is re-presenting old frames
     * while GPU work lags behind -- exactly what would show low GPU%
     * alongside a perfectly steady-looking Present cadence in the log. */
    if (g_Config.LogQueueActivity) {
        LONG64 n = InterlockedIncrement64(&g_ExecuteCommandListsCallCount);
        LogMsg("ExecuteCommandLists #%lld: %u command list(s), queue=%p",
               (long long)n, NumCommandLists, This);
    }

    if (!g_Config.CoalesceCommandLists || NumCommandLists == 0) {
        g_pfnOriginalExecuteCommandLists(This, NumCommandLists, ppCommandLists);
        return;
    }

    CoalesceSlot *slot = CoalesceSlot_FindAndLock(This);
    if (!slot) {
        g_pfnOriginalExecuteCommandLists(This, NumCommandLists, ppCommandLists);
        return;
    }

    if (NumCommandLists > COALESCE_MAX_LISTS) {
        CoalesceSlot_FlushLocked(slot);
        g_pfnOriginalExecuteCommandLists(This, NumCommandLists, ppCommandLists);
    } else {
        if (slot->pendingCount + NumCommandLists > COALESCE_MAX_LISTS) {
            CoalesceSlot_FlushLocked(slot);
        }
        memcpy(&slot->pending[slot->pendingCount], ppCommandLists,
               NumCommandLists * sizeof(void *));
        slot->pendingCount += NumCommandLists;
    }
    LeaveCriticalSection(&slot->lock);
}

static HRESULT WINAPI Hook_Signal(void *This, void *pFence, UINT64 Value)
{
    /* NOUVEAU: purely observational. The rate at which the app signals its
     * own frame-completion fence (Value going up by 1 each frame is the
     * usual pattern) is the real "how many frames actually finished per
     * second" number -- compare its cadence to Present's in the log. */
    if (g_Config.LogQueueActivity) {
        LONG64 n = InterlockedIncrement64(&g_SignalCallCount);
        LogMsg("Signal #%lld: queue=%p, fence=%p, Value=%llu",
               (long long)n, This, pFence, (unsigned long long)Value);
    }

    if (g_Config.CoalesceCommandLists) {
        CoalesceSlot *slot = CoalesceSlot_FindAndLock(This);
        if (slot) { CoalesceSlot_FlushLocked(slot); LeaveCriticalSection(&slot->lock); }
    }
    return g_pfnOriginalSignal(This, pFence, Value);
}

static HRESULT WINAPI Hook_Wait(void *This, void *pFence, UINT64 Value)
{
    if (g_Config.CoalesceCommandLists) {
        CoalesceSlot *slot = CoalesceSlot_FindAndLock(This);
        if (slot) { CoalesceSlot_FlushLocked(slot); LeaveCriticalSection(&slot->lock); }
    }
    return g_pfnOriginalWait(This, pFence, Value);
}

/* Installs the UpdateTileMappings hook on a freshly created command queue's
 * vtable. Same "once for the whole process" reasoning as InstallDeviceHooks:
 * every ID3D12CommandQueue instance normally shares one vtable. */
/* ----------------------------------------------------------------------- *
 *  ID3D12CommandQueueDownlevel::Present investigation (NOUVEAU)
 *
 *  On Windows 7 there is no IDXGISwapChain for D3D12 -- this proxy never
 *  had ANY visibility into how/how often the app actually gets pixels on
 *  screen. This hook fixes that: it times every real Present call (so we
 *  can see directly, in milliseconds, how much of each ~50-65ms frame
 *  budget at 15-20 FPS is spent inside this single blt-present operation)
 *  and logs the D3D12_DOWNLEVEL_PRESENT_FLAGS the app passed in -- notably
 *  whether WAIT_FOR_VBLANK is set, which blocks the CPU synchronously on
 *  the next vblank INSIDE this call. If the app already syncs itself
 *  elsewhere (a fence wait, or its own frame pacing) and ALSO requests
 *  WAIT_FOR_VBLANK here, that's two independent vsync waits stacking per
 *  frame -- a classic way to end up capped well below the refresh rate
 *  despite very low GPU utilization. StripPresentVBlankWait lets you test
 *  that hypothesis directly by clearing the flag before forwarding.
 * ----------------------------------------------------------------------- */
static HRESULT WINAPI Hook_DownlevelPresent(
    void *This, void *pOpenCommandList, void *pSourceTex2D, HWND hWindow, UINT Flags)
{
    static volatile LONGLONG s_callCount = 0;
    /* NOUVEAU: tracks the previous frame's source texture pointer (and how
     * many consecutive Presents reused it) to check whether the app is
     * calling Present at a steady ~60Hz cadence while re-presenting the
     * SAME already-rendered texture over and over -- i.e. the on-screen
     * update rate is much lower than the Present call rate, which would
     * explain "Present looks smooth in the log, but the game visibly
     * feels like 15-20 FPS" and low GPU usage at the same time: most
     * Presents would just be a cheap blt of stale data, not new GPU work. */
    static void   *s_lastSourceTex = NULL;
    static LONGLONG s_repeatStreak = 0;

    LONGLONG callIndex = InterlockedIncrement64(&s_callCount);

    int isRepeat = (pSourceTex2D == s_lastSourceTex);
    if (isRepeat) {
        s_repeatStreak++;
    } else {
        s_repeatStreak = 0;
        s_lastSourceTex = pSourceTex2D;
    }

    UINT effectiveFlags = Flags;
    if (g_Config.StripPresentVBlankWait &&
        (Flags & DPROXY_DOWNLEVEL_PRESENT_FLAG_WAIT_FOR_VBLANK)) {
        effectiveFlags = Flags & ~(UINT)DPROXY_DOWNLEVEL_PRESENT_FLAG_WAIT_FOR_VBLANK;
    }

    LARGE_INTEGER freq = {0}, t0 = {0}, t1 = {0};
    int haveTimer = g_Config.LogPresentTiming &&
                    QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&t0);

    HRESULT hr = g_pfnOriginalDownlevelPresent(
        This, pOpenCommandList, pSourceTex2D, hWindow, effectiveFlags);

    if (haveTimer && QueryPerformanceCounter(&t1)) {
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        LogMsg("Downlevel Present #%lld: %.2f ms inside real Present, "
               "requested Flags=0x%X%s%s -> hr=0x%08lX, srcTex=%p%s",
               (long long)callIndex, ms, Flags,
               (Flags & DPROXY_DOWNLEVEL_PRESENT_FLAG_WAIT_FOR_VBLANK)
                   ? " (WAIT_FOR_VBLANK requested by app)" : "",
               (effectiveFlags != Flags) ? " [VBlank wait stripped by proxy]" : "",
               (unsigned long)hr, pSourceTex2D,
               isRepeat ? " [SAME texture as previous Present -- stale/re-presented frame!]" : "");
        if (g_Config.ShowConsole) {
            ConsoleMsg("[d3d12proxy] Present #%lld: %.2f ms%s%s%s",
                       (long long)callIndex, ms,
                       (Flags & DPROXY_DOWNLEVEL_PRESENT_FLAG_WAIT_FOR_VBLANK)
                           ? " (app requested VBLANK wait)" : "",
                       (effectiveFlags != Flags) ? " [stripped]" : "",
                       isRepeat ? " [STALE FRAME]" : "");
        }
    }

    return hr;
}

/* Hooked on the command queue's own QueryInterface (vtable slot 0, same
 * for every COM object) purely to catch the moment the app asks for
 * ID3D12CommandQueueDownlevel, so we can hook Present on THAT returned
 * interface -- which is very likely a distinct tear-off object with its
 * own small vtable (QueryInterface/AddRef/Release/Present), not simply an
 * extension of the queue's own vtable, since the public header declares
 * "ID3D12CommandQueueDownlevel : public IUnknown" directly. */
static HRESULT WINAPI Hook_QueueQueryInterface(
    void *This, DPROXY_REFIID riid, void **ppvObject)
{
    HRESULT hr = g_pfnOriginalQueueQueryInterface(This, riid, ppvObject);

    if (SUCCEEDED(hr) && ppvObject && *ppvObject && !g_DownlevelPresentHookInstalled &&
        GuidEquals(riid, &DPROXY_IID_ID3D12CommandQueueDownlevel)) {

        void ***ppVtbl = (void ***)(*ppvObject);
        void **vtable  = *ppVtbl;
        DWORD  oldProtect;

        /* IUnknown: 0 QueryInterface, 1 AddRef, 2 Release, then Present at 3
         * -- ID3D12CommandQueueDownlevel derives directly from IUnknown and
         * adds exactly one method. */
        g_pfnOriginalDownlevelPresent = (PFN_DownlevelPresent)vtable[3];

        if (VirtualProtect(&vtable[3], sizeof(void *),
                            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[3] = (void *)Hook_DownlevelPresent;
            VirtualProtect(&vtable[3], sizeof(void *), oldProtect, &oldProtect);
            g_DownlevelPresentHookInstalled = 1;
            LogMsg("ID3D12CommandQueueDownlevel::Present hook installed (slot 3).");
            ConsoleMsg("[d3d12proxy] Found and hooked the app's D3D12On7 present "
                       "path (ID3D12CommandQueueDownlevel::Present).");
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "Downlevel Present hook NOT installed.", (unsigned long)GetLastError());
        }
    }

    return hr;
}

/* ----------------------------------------------------------------------- *
 *  Shader Model 6.6 detection / dump (NOUVEAU)
 *
 *  Context: some titles (observed: Ghost of Tsushima) refuse to start
 *  unless CheckFeatureSupport(SHADER_MODEL) reports >= 6.6 (see
 *  BypassDLSSChecks / DPROXY_SHADER_MODEL_6_6 above). Once the app is
 *  fooled into launching, D3D12On7 can still choke on the ACTUAL bytecode
 *  handed to CreateGraphicsPipelineState/CreateComputePipelineState/
 *  CreateRootSignature, because D3D12On7's DXIL validator was built against
 *  an older validator version and doesn't understand SM6.6 metadata/opcodes
 *  (Dynamic Resources, 64-bit atomics, IsHelperLane, etc). That mismatch
 *  surfaces as an opaque C++ exception (CPP_EH_EXCEPTION) deep inside
 *  d3d12on7.dll, not a clean HRESULT -- which is exactly what a debugger
 *  trace showed for Ghost of Tsushima's first PSO/root-signature creation
 *  after launch.
 *
 *  The functions below are diagnostic ONLY: they parse the DXBC container
 *  format (magic "DXBC" + chunk table) to find the "DXIL" chunk, read its
 *  embedded program version and validator version, and log/dump the blob
 *  so we can tell, per shader:
 *    (a) whether the compiler actually tagged it as SM6.6 in metadata, and
 *    (b) whether it's plausible that's JUST a metadata/validator-version
 *        mismatch (safe to consider re-tagging down to 6.4 in a future,
 *        non-diagnostic patch) vs. a shader that plausibly USES SM6.6-only
 *        features (Dynamic Resources etc), which would need actual DXIL
 *        rewriting, not just a version-field patch.
 *  Nothing here modifies the bytecode or changes what gets forwarded to
 *  the real DLL -- see DumpShaderModel66Blobs / LogShaderModelDetection in
 *  d3d12proxy.ini. */

#pragma pack(push, 1)
typedef struct {
    UINT32 Magic;          /* 'DXBC' = 0x43425844 little-endian */
    UINT32 Checksum[4];
    UINT32 Reserved;       /* version field, always 1 */
    UINT32 TotalSizeBytes;
    UINT32 ChunkCount;
    /* followed by ChunkCount x UINT32 chunk offsets */
} DPROXY_DXBC_HEADER;

typedef struct {
    UINT32 Tag;            /* e.g. 'DXIL' = 0x4C495844 little-endian */
    UINT32 SizeBytes;
    /* followed by chunk-specific payload */
} DPROXY_DXBC_CHUNK_HEADER;

/* Layout of the payload of a "DXIL" chunk:
 *   UINT16 CompilerMajor, CompilerMinor  (unused here)
 *   UINT16 ShaderKind
 *   UINT32 ProgramVersionAndSize / ...
 * In practice the field we actually want -- the DXIL "validator version"
 * embedded by dxcompiler -- lives in the bitcode module's
 * "!dx.valver" metadata, not in a fixed-offset header field, so a fully
 * correct read needs a real DXIL bitcode parser. As a cheap first-pass
 * heuristic (good enough for diagnosis, NOT for a correctness-critical
 * patch), we instead read the well-defined "program version" byte that
 * DXC always writes at the start of the DXIL chunk payload: high nibble =
 * shader model major, low nibble = shader model minor (e.g. 0x66 = SM6.6,
 * 0x64 = SM6.4). This is the same encoding as DPROXY_SHADER_MODEL_6_x
 * above. */
typedef struct {
    UINT16 ProgramVersion;  /* high byte unused/reserved, low byte = (major<<4)|minor -- see above */
    UINT16 ShaderKind;
    UINT32 SizeInUint32;
} DPROXY_DXIL_CHUNK_PAYLOAD_HEADER;
#pragma pack(pop)

#define DPROXY_DXBC_MAGIC 0x43425844u /* "DXBC" */
#define DPROXY_DXIL_TAG   0x4C495844u /* "DXIL" */

/* Scans a DXBC container for a DXIL chunk and extracts its program-version
 * byte. Returns 1 and fills *outShaderModel (e.g. 0x66) on success, 0 if
 * this isn't a well-formed DXBC blob or no DXIL chunk was found (e.g. an
 * old-style DXBC/SM5.x shader compiled by fxc has no DXIL chunk at all --
 * that's a normal, non-error result, just report "not applicable"). */
static int DXIL_ExtractShaderModel(const void *pBlob, SIZE_T blobSize, int *outShaderModel)
{
    const BYTE *base = (const BYTE *)pBlob;
    const DPROXY_DXBC_HEADER *hdr;
    const UINT32 *chunkOffsets;
    UINT32 i;

    if (!pBlob || blobSize < sizeof(DPROXY_DXBC_HEADER) || !outShaderModel)
        return 0;

    hdr = (const DPROXY_DXBC_HEADER *)base;
    if (hdr->Magic != DPROXY_DXBC_MAGIC)
        return 0;
    if (hdr->TotalSizeBytes > blobSize)
        return 0; /* truncated/corrupt -- bail out, never read past the blob */

    chunkOffsets = (const UINT32 *)(base + sizeof(DPROXY_DXBC_HEADER));

    for (i = 0; i < hdr->ChunkCount; i++) {
        UINT32 off;
        const DPROXY_DXBC_CHUNK_HEADER *chunkHdr;
        const DPROXY_DXIL_CHUNK_PAYLOAD_HEADER *dxilHdr;

        /* Bounds-check the offset table entry itself before dereferencing. */
        if ((BYTE *)(chunkOffsets + i + 1) > base + blobSize)
            break;
        off = chunkOffsets[i];
        if ((UINT64)off + sizeof(DPROXY_DXBC_CHUNK_HEADER) > blobSize)
            continue;

        chunkHdr = (const DPROXY_DXBC_CHUNK_HEADER *)(base + off);
        if (chunkHdr->Tag != DPROXY_DXIL_TAG)
            continue;

        if ((UINT64)off + sizeof(DPROXY_DXBC_CHUNK_HEADER) + sizeof(DPROXY_DXIL_CHUNK_PAYLOAD_HEADER) > blobSize)
            continue;

        dxilHdr = (const DPROXY_DXIL_CHUNK_PAYLOAD_HEADER *)(base + off + sizeof(DPROXY_DXBC_CHUNK_HEADER));
        *outShaderModel = (int)(dxilHdr->ProgramVersion & 0xFF);
        return 1;
    }
    return 0;
}

/* Writes a raw blob to shader_dumps\<label>_<n>.dxbc next to the proxy DLL,
 * creating the directory on first use. Best-effort: failures are logged
 * but never propagated (this must never be able to break a real launch). */
static void DumpShaderBlobToDisk(const void *pBlob, SIZE_T blobSize, const char *label, LONG64 n)
{
    char dirPath[MAX_PATH];
    char filePath[MAX_PATH];
    HANDLE hFile;
    DWORD written;

    if (!pBlob || blobSize == 0) return;

    _snprintf(dirPath, sizeof(dirPath) - 1, "%s\\shader_dumps", g_ModuleDir);
    dirPath[sizeof(dirPath) - 1] = '\0';
    CreateDirectoryA(dirPath, NULL); /* ignore ERROR_ALREADY_EXISTS */

    _snprintf(filePath, sizeof(filePath) - 1, "%s\\%s_%lld.dxbc", dirPath, label, (long long)n);
    filePath[sizeof(filePath) - 1] = '\0';

    hFile = CreateFileA(filePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogMsg("DumpShaderBlobToDisk: CreateFile failed for %s (err=%lu)", filePath, GetLastError());
        return;
    }
    if (!WriteFile(hFile, pBlob, (DWORD)blobSize, &written, NULL) || written != (DWORD)blobSize) {
        LogMsg("DumpShaderBlobToDisk: WriteFile failed/short for %s (err=%lu)", filePath, GetLastError());
    } else {
        LogMsg("DumpShaderBlobToDisk: wrote %s (%zu bytes)", filePath, (size_t)blobSize);
    }
    CloseHandle(hFile);
}

/* Inspects one D3D12_SHADER_BYTECODE-shaped field (stageName is just for
 * logging, e.g. "VS"/"PS"/"CS") and, if configured, logs its detected
 * shader model and/or dumps it to disk. Safe to call with a null/empty
 * bytecode (unused stage in a PSO desc) -- becomes a no-op. */
/* Cheap heuristic for "does this blob actually USE SM6.6-only features",
 * as opposed to just being TAGGED SM6.6/6.5 in its DXIL header. DXIL is
 * LLVM bitcode, but intrinsic/external function names (like
 * "dx.op.createHandleFromHeap", the Dynamic Resources opcode wrapper) are
 * still stored as literal ASCII strings inside the bitcode's symbol/string
 * table -- LLVM bitcode compresses instruction encoding, not identifier
 * text. A raw substring scan is NOT a real bitcode parser and can miss
 * obfuscated/stripped cases, but for diagnosis (deciding whether Phase 2A
 * "just retag the version" is plausible, or whether we're in Phase 2B
 * "real feature, needs DXIL rewriting" territory) it's a fast, good-enough
 * first signal. A hit means "definitely uses it"; a miss means "probably
 * doesn't, but not proven" -- treat accordingly, don't over-trust a miss. */
static int ScanForSubstring(const BYTE *haystack, SIZE_T haystackLen, const char *needle)
{
    SIZE_T needleLen = strlen(needle);
    SIZE_T i;
    if (needleLen == 0 || haystackLen < needleLen) return 0;
    for (i = 0; i + needleLen <= haystackLen; i++) {
        if (memcmp(haystack + i, needle, needleLen) == 0)
            return 1;
    }
    return 0;
}

static void LogLikelySM66FeatureUsage(const void *pBlob, SIZE_T blobSize, const char *stageName)
{
    const BYTE *b = (const BYTE *)pBlob;
    int usesDynamicResources = ScanForSubstring(b, blobSize, "dx.op.createHandleFromHeap");
    int usesAtomics64        = ScanForSubstring(b, blobSize, "dx.op.atomicBinOp64") ||
                                ScanForSubstring(b, blobSize, "dx.op.atomicCompareExchange64");
    int usesHelperLane       = ScanForSubstring(b, blobSize, "dx.op.isHelperLane");

    if (usesDynamicResources || usesAtomics64 || usesHelperLane) {
        LogMsg("ShaderModelDetect[%s]: USES SM6.6+ feature(s) -- DynamicResources=%d Atomics64=%d "
               "IsHelperLane=%d -- a version-tag patch alone will NOT be enough here, this needs "
               "real DXIL rewriting (Phase 2B).",
               stageName, usesDynamicResources, usesAtomics64, usesHelperLane);
    } else {
        LogMsg("ShaderModelDetect[%s]: no SM6.6-only intrinsic name found (substring scan) -- "
               "plausible this shader only NEEDS a lower shader model but got compiled/tagged "
               "higher than necessary (candidate for a Phase 2A version-tag patch). Not proof: "
               "a real DXIL bitcode parse would be needed to be certain.",
               stageName);
    }
}

static void InspectShaderBytecode(const DPROXY_SHADER_BYTECODE *pBytecode, const char *stageName, LONG64 psoCallIndex)
{
    int sm = 0;

    if (!g_Config.LogShaderModelDetection && !g_Config.DumpShaderModel66Blobs)
        return;
    if (!pBytecode || !pBytecode->pShaderBytecode || pBytecode->BytecodeLength == 0)
        return;

    if (DXIL_ExtractShaderModel(pBytecode->pShaderBytecode, pBytecode->BytecodeLength, &sm)) {
        if (g_Config.LogShaderModelDetection) {
            LogMsg("ShaderModelDetect[%s]: DXIL program version = SM%d.%d (blob %zu bytes)",
                   stageName, (sm >> 4) & 0xF, sm & 0xF, (size_t)pBytecode->BytecodeLength);
            if (sm >= DPROXY_SHADER_MODEL_6_4) {
                LogLikelySM66FeatureUsage(pBytecode->pShaderBytecode, pBytecode->BytecodeLength, stageName);
            }
        }
        if (g_Config.DumpShaderModel66Blobs && sm >= DPROXY_SHADER_MODEL_6_4) {
            /* NOTE: threshold is >= SM6.4, not just >= SM6.6. First real
             * failure observed against Ghost of Tsushima was a compute
             * shader tagged SM6.5 (CreateComputePipelineState -> hr=
             * 0x80070057), so D3D12On7's real validator ceiling is at or
             * below SM6.4 -- anything from 6.4 up is worth capturing, not
             * just the 6.6 threshold the app's own launch-time check uses. */
            char label[64];
            _snprintf(label, sizeof(label) - 1, "%s_sm%d_%d", stageName, (sm >> 4) & 0xF, sm & 0xF);
            label[sizeof(label) - 1] = '\0';
            DumpShaderBlobToDisk(pBytecode->pShaderBytecode, pBytecode->BytecodeLength, label, psoCallIndex);
        }
    } else if (g_Config.LogShaderModelDetection) {
        /* Not a DXBC/DXIL container we could parse -- could be legacy
         * SM5.x DXBC (no DXIL chunk at all, which is normal/expected), or
         * a genuinely malformed blob. Either way, log at low volume so it
         * doesn't spam if a game legitimately mixes SM5.x and SM6.x
         * shaders across different PSOs. */
        LogMsg("ShaderModelDetect[%s]: no DXIL chunk found (likely legacy SM5.x DXBC, blob %zu bytes)",
               stageName, (size_t)pBytecode->BytecodeLength);
    }
}

/* ----------------------------------------------------------------------- *
 *  PSO / root-signature-recreated-every-frame investigation (NOUVEAU)
 *
 *  D3D12CreateRootSignatureDeserializer (a DLL export, not a COM method --
 *  see below) was observed at ~7480 calls over a ~230s capture, almost
 *  exactly 1 call per Present. That is NOT a normal per-frame code path --
 *  it's a reflection/introspection helper, not something the render loop
 *  is supposed to touch every frame. These three hooks answer the natural
 *  follow-up question: is the app ALSO recreating actual root signatures
 *  and pipeline state objects every frame instead of caching them? If so,
 *  that's plausibly THE bottleneck: on D3D12On7, creating a PSO or root
 *  signature isn't just a CPU-side struct fill-in, it has to go through
 *  the D3D11-translation layer's equivalent state-object creation, which
 *  is exactly the kind of thing that stays CPU-bound (driver/runtime work,
 *  invisible as "GPU busy") while the GPU itself sits mostly idle waiting
 *  for something to actually execute -- matching "GPU 15-17%, 15-20 FPS"
 *  far better than kernel-escape submission overhead alone would. */
static HRESULT WINAPI Hook_CreateRootSignature(
    void *This, UINT NodeMask, const void *pBlobWithRootSignature,
    SIZE_T BlobLengthInBytes, DPROXY_REFIID riid, void **ppvRootSignature)
{
    LONG64 n = InterlockedIncrement64(&g_CreateRootSignatureCount);
    LARGE_INTEGER freq = {0}, t0 = {0}, t1 = {0};
    int haveTimer = QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&t0);

    /* Diagnostic only, and deliberately BEFORE the real call: root
     * signature blobs aren't DXBC/DXIL themselves (they're a separate
     * serialized-root-signature format), so DXIL_ExtractShaderModel will
     * normally report "no DXIL chunk" here -- that's expected and fine.
     * This is kept mainly for completeness/symmetry with the PSO hooks
     * and to catch the (rare) case of an embedded DXIL blob. Placed
     * before the real call so we still capture something if the real
     * call is what throws. */
    if (g_Config.LogShaderModelDetection || g_Config.DumpShaderModel66Blobs) {
        DPROXY_SHADER_BYTECODE rsBytecode;
        rsBytecode.pShaderBytecode = pBlobWithRootSignature;
        rsBytecode.BytecodeLength  = BlobLengthInBytes;
        InspectShaderBytecode(&rsBytecode, "RootSig", n);
    }

    HRESULT hr = g_pfnOriginalCreateRootSignature(
        This, NodeMask, pBlobWithRootSignature, BlobLengthInBytes, riid, ppvRootSignature);

    if (haveTimer && QueryPerformanceCounter(&t1)) {
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        LogMsg("CreateRootSignature #%lld: %.2f ms, blob=%zu bytes -> hr=0x%08lX",
               (long long)n, ms, (size_t)BlobLengthInBytes, (unsigned long)hr);
    }
    return hr;
}

static HRESULT WINAPI Hook_CreateGraphicsPipelineState(
    void *This, const void *pDesc, DPROXY_REFIID riid, void **ppPipelineState)
{
    LONG64 n = InterlockedIncrement64(&g_CreateGraphicsPSOCount);
    LARGE_INTEGER freq = {0}, t0 = {0}, t1 = {0};
    int haveTimer = QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&t0);

    /* Diagnostic only. Deliberately done BEFORE the real call: if
     * D3D12On7 throws a C++ exception inside CreateGraphicsPipelineState
     * itself (observed with Ghost of Tsushima, right after a spoofed
     * SM6.6 CheckFeatureSupport answer), we still want the shader-model
     * detection/dump to have happened for the blob that caused it. */
    if ((g_Config.LogShaderModelDetection || g_Config.DumpShaderModel66Blobs) && pDesc) {
        const BYTE *descBytes = (const BYTE *)pDesc;
        InspectShaderBytecode((const DPROXY_SHADER_BYTECODE *)(descBytes + DPROXY_GFXPSO_OFFSET_VS), "VS", n);
        InspectShaderBytecode((const DPROXY_SHADER_BYTECODE *)(descBytes + DPROXY_GFXPSO_OFFSET_PS), "PS", n);
        InspectShaderBytecode((const DPROXY_SHADER_BYTECODE *)(descBytes + DPROXY_GFXPSO_OFFSET_DS), "DS", n);
        InspectShaderBytecode((const DPROXY_SHADER_BYTECODE *)(descBytes + DPROXY_GFXPSO_OFFSET_HS), "HS", n);
        InspectShaderBytecode((const DPROXY_SHADER_BYTECODE *)(descBytes + DPROXY_GFXPSO_OFFSET_GS), "GS", n);
    }

    HRESULT hr = g_pfnOriginalCreateGraphicsPipelineState(This, pDesc, riid, ppPipelineState);

    if (haveTimer && QueryPerformanceCounter(&t1)) {
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        LogMsg("CreateGraphicsPipelineState #%lld: %.2f ms -> hr=0x%08lX",
               (long long)n, ms, (unsigned long)hr);
    }
    return hr;
}

static HRESULT WINAPI Hook_CreateComputePipelineState(
    void *This, const void *pDesc, DPROXY_REFIID riid, void **ppPipelineState)
{
    LONG64 n = InterlockedIncrement64(&g_CreateComputePSOCount);
    LARGE_INTEGER freq = {0}, t0 = {0}, t1 = {0};
    int haveTimer = QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&t0);

    /* Diagnostic only, before the real call -- see comment in
     * Hook_CreateGraphicsPipelineState above. */
    if ((g_Config.LogShaderModelDetection || g_Config.DumpShaderModel66Blobs) && pDesc) {
        const BYTE *descBytes = (const BYTE *)pDesc;
        InspectShaderBytecode((const DPROXY_SHADER_BYTECODE *)(descBytes + DPROXY_CPSO_OFFSET_CS), "CS", n);
    }

    HRESULT hr = g_pfnOriginalCreateComputePipelineState(This, pDesc, riid, ppPipelineState);

    if (haveTimer && QueryPerformanceCounter(&t1)) {
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        LogMsg("CreateComputePipelineState #%lld: %.2f ms -> hr=0x%08lX",
               (long long)n, ms, (unsigned long)hr);
    }
    return hr;
}

/* Different D3D12_COMMAND_LIST_TYPEs (Direct/Compute/Copy) are backed by
 * genuinely different C++ classes inside the real D3D12On7 runtime -- its
 * own PDB shows separate template instantiations (CCommandQueue<0>, <15>,
 * <16>, <20>, <30>, ...) for different queue kinds, each with ITS OWN
 * vtable. A single "hooked once, ever" boolean here silently hooks only
 * whichever queue TYPE the app happens to create first and misses every
 * other type for the rest of the process -- confirmed by a real capture
 * against Grid 2019 where the queue used for Present was clearly active
 * (3000+ real frames logged) yet ExecuteCommandLists/Signal never fired
 * once, because that queue's vtable was never actually patched (a
 * different-typed queue, created first, "used up" the single boolean).
 * Track every DISTINCT vtable pointer we've already patched instead. */
#define MAX_TRACKED_QUEUE_VTABLES 8
static void            *g_PatchedQueueVtables[MAX_TRACKED_QUEUE_VTABLES];
static int               g_PatchedQueueVtableCount = 0;

static void InstallQueueHooksIfNeeded(void *queue)
{
    DWORD oldProtect;
    int   i;
    void ***ppVtbl;
    void **vtable;

    if (!queue) return;

    ppVtbl = (void ***)queue;
    vtable = *ppVtbl;

    for (i = 0; i < g_PatchedQueueVtableCount; i++) {
        if (g_PatchedQueueVtables[i] == (void *)vtable) return; /* this exact vtable already patched */
    }
    if (g_PatchedQueueVtableCount >= MAX_TRACKED_QUEUE_VTABLES) {
        LogMsg("WARNING: more than %d distinct command queue vtables seen "
               "(queue=%p) -- not patching this one. If this legitimately "
               "happens, raise MAX_TRACKED_QUEUE_VTABLES.",
               MAX_TRACKED_QUEUE_VTABLES, queue);
        return;
    }
    g_PatchedQueueVtables[g_PatchedQueueVtableCount++] = (void *)vtable;

    g_pfnOriginalUpdateTileMappings =
        (PFN_UpdateTileMappings)vtable[CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS];

    if (VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS],
                        sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {

        vtable[CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS] = (void *)Hook_UpdateTileMappings;

        VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS],
                        sizeof(void *), oldProtect, &oldProtect);

        LogMsg("Command queue vtable hook installed on UpdateTileMappings (slot %d) "
               "[vtable=%p, distinct vtable #%d].",
               CMDQUEUE_VTBL_INDEX_UPDATETILEMAPPINGS, (void *)vtable, g_PatchedQueueVtableCount);
    } else {
        LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
               "UpdateTileMappings hook NOT installed.", (unsigned long)GetLastError());
    }

    /* NOUVEAU: ExecuteCommandLists/Signal/Wait hooks for CoalesceCommandLists
     * AND/OR LogQueueActivity (purely observational). No longer gated by its
     * own separate "once ever" boolean either -- same per-vtable tracking
     * above already covers it, since this whole function now only runs
     * once per distinct vtable regardless of which feature asked for it. */
    if (g_Config.CoalesceCommandLists || g_Config.LogQueueActivity) {
        Coalesce_GlobalInit();

        g_pfnOriginalExecuteCommandLists =
            (PFN_ExecuteCommandLists)vtable[CMDQUEUE_VTBL_INDEX_EXECUTECOMMANDLISTS];
        g_pfnOriginalSignal =
            (PFN_Signal)vtable[CMDQUEUE_VTBL_INDEX_SIGNAL];
        g_pfnOriginalWait =
            (PFN_Wait)vtable[CMDQUEUE_VTBL_INDEX_WAIT];

        if (VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_EXECUTECOMMANDLISTS],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[CMDQUEUE_VTBL_INDEX_EXECUTECOMMANDLISTS] = (void *)Hook_ExecuteCommandLists;
            VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_EXECUTECOMMANDLISTS],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "ExecuteCommandLists hook NOT installed -- "
                   "CoalesceCommandLists will not work.", (unsigned long)GetLastError());
        }

        if (VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_SIGNAL],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[CMDQUEUE_VTBL_INDEX_SIGNAL] = (void *)Hook_Signal;
            VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_SIGNAL],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "Signal hook NOT installed -- CoalesceCommandLists will "
                   "not flush correctly before fence signals.", (unsigned long)GetLastError());
        }

        if (VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_WAIT],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[CMDQUEUE_VTBL_INDEX_WAIT] = (void *)Hook_Wait;
            VirtualProtect(&vtable[CMDQUEUE_VTBL_INDEX_WAIT],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "Wait hook NOT installed -- CoalesceCommandLists will "
                   "not flush correctly before fence waits.", (unsigned long)GetLastError());
        }

        LogMsg("Command queue vtable hooks installed for CoalesceCommandLists "
               "(ExecuteCommandLists slot %d, Signal slot %d, Wait slot %d) "
               "[vtable=%p, distinct vtable #%d].",
               CMDQUEUE_VTBL_INDEX_EXECUTECOMMANDLISTS,
               CMDQUEUE_VTBL_INDEX_SIGNAL, CMDQUEUE_VTBL_INDEX_WAIT,
               (void *)vtable, g_PatchedQueueVtableCount);
    }

    /* NOUVEAU: hook QueryInterface (slot 0, same for every COM object) so
     * we catch the app asking the queue for ID3D12CommandQueueDownlevel --
     * D3D12On7's only present path on Windows 7 -- and can in turn hook
     * ITS Present method. Gated on either investigation flag so you can
     * enable just the timing/flag logging (LogPresentTiming) without
     * necessarily also stripping WAIT_FOR_VBLANK (StripPresentVBlankWait),
     * or vice versa. No longer gated by its own "once ever" boolean --
     * same reasoning as above. */
    if (g_Config.LogPresentTiming || g_Config.StripPresentVBlankWait) {
        g_pfnOriginalQueueQueryInterface = (PFN_QueueQueryInterface)vtable[0];

        if (VirtualProtect(&vtable[0], sizeof(void *),
                            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[0] = (void *)Hook_QueueQueryInterface;
            VirtualProtect(&vtable[0], sizeof(void *), oldProtect, &oldProtect);
            LogMsg("Command queue vtable hook installed on QueryInterface (slot 0) "
                   "to catch ID3D12CommandQueueDownlevel requests "
                   "[vtable=%p, distinct vtable #%d].",
                   (void *)vtable, g_PatchedQueueVtableCount);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "QueryInterface hook NOT installed -- Present investigation "
                   "will not work.", (unsigned long)GetLastError());
        }
    }
}

/* Matches ID3D12Device::CreateCommandQueue. Hooked purely as a vehicle to
 * reach the queue's own vtable -- see InstallQueueHooksIfNeeded. */
static HRESULT WINAPI Hook_CreateCommandQueue(
    void *This, const void *pDesc, DPROXY_REFIID riid, void **ppCommandQueue)
{
    HRESULT hr = g_pfnOriginalCreateCommandQueue(This, pDesc, riid, ppCommandQueue);

    if (SUCCEEDED(hr) && ppCommandQueue && *ppCommandQueue) {
        InstallQueueHooksIfNeeded(*ppCommandQueue);
    }

    return hr;
}

/* Same per-vtable-pointer tracking as InstallQueueHooksIfNeeded, and for the
 * exact same reason: DIRECT/BUNDLE/COMPUTE/COPY command lists are very
 * likely different C++ classes internally (distinct vtables) even though
 * they all implement ID3D12GraphicsCommandList. Only installs the 7
 * CoalesceResourceBarriers hooks -- gated on that single flag, since unlike
 * the queue-level hooks there's no separate "just log it" mode here. */
static void InstallCommandListHooksIfNeeded(void *cmdList)
{
    DWORD oldProtect;
    int   i;
    void ***ppVtbl;
    void **vtable;

    if (!g_Config.CoalesceResourceBarriers || !cmdList) return;

    ppVtbl = (void ***)cmdList;
    vtable = *ppVtbl;

    for (i = 0; i < g_PatchedCmdListVtableCount; i++) {
        if (g_PatchedCmdListVtables[i] == (void *)vtable) return; /* already patched */
    }
    if (g_PatchedCmdListVtableCount >= MAX_TRACKED_CMDLIST_VTABLES) {
        LogMsg("WARNING: more than %d distinct command list vtables seen "
               "(cmdList=%p) -- not patching this one. If this legitimately "
               "happens, raise MAX_TRACKED_CMDLIST_VTABLES.",
               MAX_TRACKED_CMDLIST_VTABLES, cmdList);
        return;
    }
    g_PatchedCmdListVtables[g_PatchedCmdListVtableCount++] = (void *)vtable;

    Coalesce_BarrierGlobalInit();

#define PATCH_CMDLIST_SLOT(INDEX, HOOKFN, ORIGVAR, NAME)                                        \
    ORIGVAR = (void *)vtable[INDEX];                                                            \
    if (VirtualProtect(&vtable[INDEX], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {  \
        vtable[INDEX] = (void *)(HOOKFN);                                                       \
        VirtualProtect(&vtable[INDEX], sizeof(void *), oldProtect, &oldProtect);                \
    } else {                                                                                    \
        LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "                            \
               NAME " hook NOT installed -- CoalesceResourceBarriers will "                     \
               "not work correctly.", (unsigned long)GetLastError());                           \
    }

    PATCH_CMDLIST_SLOT(CMDLIST_VTBL_INDEX_RESOURCEBARRIER, Hook_ResourceBarrier,
                        g_pfnOriginalCmdListResourceBarrier, "ResourceBarrier");
    PATCH_CMDLIST_SLOT(CMDLIST_VTBL_INDEX_DISPATCH, Hook_CmdListDispatch,
                        g_pfnOriginalCmdListDispatch, "Dispatch");
    PATCH_CMDLIST_SLOT(CMDLIST_VTBL_INDEX_SETPIPELINESTATE, Hook_CmdListSetPipelineState,
                        g_pfnOriginalCmdListSetPipelineState, "SetPipelineState");
    PATCH_CMDLIST_SLOT(CMDLIST_VTBL_INDEX_SETCOMPUTEROOTSIGNATURE, Hook_CmdListSetComputeRootSignature,
                        g_pfnOriginalCmdListSetComputeRootSig, "SetComputeRootSignature");
    PATCH_CMDLIST_SLOT(CMDLIST_VTBL_INDEX_SETCOMPUTEROOTUNORDEREDACCESSVIEW, Hook_CmdListSetComputeRootUAV,
                        g_pfnOriginalCmdListSetComputeRootUAV, "SetComputeRootUnorderedAccessView");
    PATCH_CMDLIST_SLOT(CMDLIST_VTBL_INDEX_CLOSE, Hook_CmdListClose,
                        g_pfnOriginalCmdListClose, "Close");
    PATCH_CMDLIST_SLOT(CMDLIST_VTBL_INDEX_RESET, Hook_CmdListReset,
                        g_pfnOriginalCmdListReset, "Reset");

#undef PATCH_CMDLIST_SLOT

    LogMsg("Command list vtable hooks installed for CoalesceResourceBarriers "
           "(ResourceBarrier=%d, Dispatch=%d, SetPipelineState=%d, "
           "SetComputeRootSignature=%d, SetComputeRootUnorderedAccessView=%d, "
           "Close=%d, Reset=%d) [vtable=%p, distinct vtable #%d].",
           CMDLIST_VTBL_INDEX_RESOURCEBARRIER, CMDLIST_VTBL_INDEX_DISPATCH,
           CMDLIST_VTBL_INDEX_SETPIPELINESTATE, CMDLIST_VTBL_INDEX_SETCOMPUTEROOTSIGNATURE,
           CMDLIST_VTBL_INDEX_SETCOMPUTEROOTUNORDEREDACCESSVIEW,
           CMDLIST_VTBL_INDEX_CLOSE, CMDLIST_VTBL_INDEX_RESET,
           (void *)vtable, g_PatchedCmdListVtableCount);
}

/* Matches ID3D12Device::CreateCommandList. Hooked purely as a vehicle to
 * reach the command list's own vtable -- see InstallCommandListHooksIfNeeded. */
static HRESULT WINAPI Hook_CreateCommandList(
    void *This, UINT nodeMask, int type, void *pCommandAllocator,
    void *pInitialState, DPROXY_REFIID riid, void **ppCommandList)
{
    HRESULT hr = g_pfnOriginalCreateCommandList(
        This, nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);

    if (SUCCEEDED(hr) && ppCommandList && *ppCommandList) {
        InstallCommandListHooksIfNeeded(*ppCommandList);
    }

    return hr;
}

/* Installs the CheckFeatureSupport hook, and (independently, gated on
 * FixReservedResourceRefresh) the CreateCommandQueue/CreateShaderResourceView
 * hooks needed for the reserved-resource SRV-refresh fix, on the vtable of a
 * freshly created device. Since all instances of a given COM class normally
 * share a single vtable, each hook only needs to be installed once for the
 * whole process -- the two groups are gated by separate flags so either can
 * be enabled/disabled independently via the ini. */
static void InstallDeviceHooks(void *device)
{
    void ***ppVtbl = (void ***)device;
    void **vtable  = *ppVtbl;
    DWORD  oldProtect;

    /* Hook CheckFeatureSupport */
    if (!g_DeviceHooksInstalled) {
        g_pfnOriginalCheckFeatureSupport =
            (PFN_CheckFeatureSupport)vtable[DEVICE_VTBL_INDEX_CHECKFEATURESUPPORT];

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CHECKFEATURESUPPORT],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {

            vtable[DEVICE_VTBL_INDEX_CHECKFEATURESUPPORT] = (void *)Hook_CheckFeatureSupport;

            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CHECKFEATURESUPPORT],
                            sizeof(void *), oldProtect, &oldProtect);

            g_DeviceHooksInstalled = 1;
            LogMsg("Device vtable hook installed on CheckFeatureSupport (slot %d).",
                   DEVICE_VTBL_INDEX_CHECKFEATURESUPPORT);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "CheckFeatureSupport hook NOT installed.",
                   (unsigned long)GetLastError());
        }

        if (g_Config.StripUnsupportedHeapFlags || g_Config.FixReadbackInitialState) {
            g_pfnOriginalCreateCommittedResource =
                (PFN_CreateCommittedResource)vtable[DEVICE_VTBL_INDEX_CREATECOMMITTEDRESOURCE];

            if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMMITTEDRESOURCE],
                                sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {

                vtable[DEVICE_VTBL_INDEX_CREATECOMMITTEDRESOURCE] = (void *)Hook_CreateCommittedResource;

                VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMMITTEDRESOURCE],
                                sizeof(void *), oldProtect, &oldProtect);

                LogMsg("Device vtable hook installed on CreateCommittedResource (slot %d).",
                       DEVICE_VTBL_INDEX_CREATECOMMITTEDRESOURCE);
            } else {
                LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                       "CreateCommittedResource hook NOT installed -- "
                       "StripUnsupportedHeapFlags/FixReadbackInitialState will not work.",
                       (unsigned long)GetLastError());
            }
        }
    }

    /* NOUVEAU: also enter this block for CoalesceCommandLists, or the
     * Downlevel-Present investigation flags, alone (not just
     * FixReservedResourceRefresh) -- InstallQueueHooksIfNeeded, reached
     * only via Hook_CreateCommandQueue below, is where ALL of the
     * queue-vtable-level features (tile mapping fix, command list
     * coalescing, and now the Present/VBlank investigation) actually get
     * installed. Hook_CreateShaderResourceView is harmless to install even
     * when FixReservedResourceRefresh is off: it internally no-ops unless
     * that flag is set (see the top of Hook_CreateShaderResourceView). */
    if ((g_Config.FixReservedResourceRefresh || g_Config.CoalesceCommandLists ||
         g_Config.LogPresentTiming || g_Config.StripPresentVBlankWait ||
         g_Config.LogQueueActivity) &&
        !g_TileFixHooksInstalled) {
        g_pfnOriginalCreateCommandQueue =
            (PFN_CreateCommandQueue)vtable[DEVICE_VTBL_INDEX_CREATECOMMANDQUEUE];
        g_pfnOriginalCreateSRV =
            (PFN_CreateShaderResourceView)vtable[DEVICE_VTBL_INDEX_CREATESHADERRESOURCEVIEW];

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMMANDQUEUE],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[DEVICE_VTBL_INDEX_CREATECOMMANDQUEUE] = (void *)Hook_CreateCommandQueue;
            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMMANDQUEUE],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "CreateCommandQueue hook NOT installed -- "
                   "FixReservedResourceRefresh will not work.",
                   (unsigned long)GetLastError());
        }

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATESHADERRESOURCEVIEW],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[DEVICE_VTBL_INDEX_CREATESHADERRESOURCEVIEW] = (void *)Hook_CreateShaderResourceView;
            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATESHADERRESOURCEVIEW],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "CreateShaderResourceView hook NOT installed -- "
                   "FixReservedResourceRefresh will not work.",
                   (unsigned long)GetLastError());
        }

        g_TileFixHooksInstalled = 1;
        LogMsg("Device vtable hooks installed for reserved-resource SRV-refresh fix "
               "(CreateCommandQueue slot %d, CreateShaderResourceView slot %d).",
               DEVICE_VTBL_INDEX_CREATECOMMANDQUEUE, DEVICE_VTBL_INDEX_CREATESHADERRESOURCEVIEW);
    }
    if (g_Config.DisableSharedFences && !g_FenceHookInstalled) {
        g_pfnOriginalCreateFence = (PFN_CreateFence)vtable[DEVICE_VTBL_INDEX_CREATEFENCE];

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATEFENCE],
                           sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[DEVICE_VTBL_INDEX_CREATEFENCE] = (void *)Hook_CreateFence;
            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATEFENCE],
                           sizeof(void *), oldProtect, &oldProtect);
            g_FenceHookInstalled = 1;
            LogMsg("Device vtable hook installed on CreateFence (slot %d).",
                   DEVICE_VTBL_INDEX_CREATEFENCE);
        } else {
            LogMsg("WARNING: VirtualProtect failed for CreateFence hook.");
        }
    }

    /* NOUVEAU: CreateCommandList hook, vehicule pour atteindre la vtable des
     * command lists elles-memes (voir InstallCommandListHooksIfNeeded).
     * Feature independante de tout ce qui precede -- gardee sur son propre
     * flag plutot que d'etre ajoutee au bloc g_TileFixHooksInstalled
     * ci-dessus, puisqu'elle patch un slot de vtable device totalement
     * different (CreateCommandList, pas CreateCommandQueue). */
    if (g_Config.CoalesceResourceBarriers && !g_CmdListDeviceHookInstalled) {
        g_pfnOriginalCreateCommandList =
            (PFN_CreateCommandList)vtable[DEVICE_VTBL_INDEX_CREATECOMMANDLIST];

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMMANDLIST],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[DEVICE_VTBL_INDEX_CREATECOMMANDLIST] = (void *)Hook_CreateCommandList;
            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMMANDLIST],
                            sizeof(void *), oldProtect, &oldProtect);
            g_CmdListDeviceHookInstalled = 1;
            LogMsg("Device vtable hook installed on CreateCommandList (slot %d).",
                   DEVICE_VTBL_INDEX_CREATECOMMANDLIST);
        } else {
            LogMsg("WARNING: VirtualProtect failed (GetLastError=%lu), "
                   "CreateCommandList hook NOT installed -- "
                   "CoalesceResourceBarriers will not work.",
                   (unsigned long)GetLastError());
        }
    }

    /* NOUVEAU: purely observational (no behavior change) -- times and
     * counts CreateRootSignature/CreateGraphicsPipelineState/
     * CreateComputePipelineState to check whether the app is recreating
     * pipeline state every frame instead of caching it. See the big
     * comment above Hook_CreateRootSignature. */
    if (g_Config.LogPipelineStateCreation && !g_PsoHooksInstalled) {
        g_pfnOriginalCreateRootSignature =
            (PFN_CreateRootSignature)vtable[DEVICE_VTBL_INDEX_CREATEROOTSIGNATURE];
        g_pfnOriginalCreateGraphicsPipelineState =
            (PFN_CreateGraphicsPipelineState)vtable[DEVICE_VTBL_INDEX_CREATEGRAPHICSPIPELINESTATE];
        g_pfnOriginalCreateComputePipelineState =
            (PFN_CreateComputePipelineState)vtable[DEVICE_VTBL_INDEX_CREATECOMPUTEPIPELINESTATE];

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATEROOTSIGNATURE],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[DEVICE_VTBL_INDEX_CREATEROOTSIGNATURE] = (void *)Hook_CreateRootSignature;
            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATEROOTSIGNATURE],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed, CreateRootSignature hook NOT installed.");
        }

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATEGRAPHICSPIPELINESTATE],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[DEVICE_VTBL_INDEX_CREATEGRAPHICSPIPELINESTATE] =
                (void *)Hook_CreateGraphicsPipelineState;
            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATEGRAPHICSPIPELINESTATE],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed, CreateGraphicsPipelineState hook NOT installed.");
        }

        if (VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMPUTEPIPELINESTATE],
                            sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[DEVICE_VTBL_INDEX_CREATECOMPUTEPIPELINESTATE] =
                (void *)Hook_CreateComputePipelineState;
            VirtualProtect(&vtable[DEVICE_VTBL_INDEX_CREATECOMPUTEPIPELINESTATE],
                            sizeof(void *), oldProtect, &oldProtect);
        } else {
            LogMsg("WARNING: VirtualProtect failed, CreateComputePipelineState hook NOT installed.");
        }

        g_PsoHooksInstalled = 1;
        LogMsg("Device vtable hooks installed for PSO/root-signature creation "
               "investigation (CreateRootSignature slot %d, "
               "CreateGraphicsPipelineState slot %d, CreateComputePipelineState slot %d).",
               DEVICE_VTBL_INDEX_CREATEROOTSIGNATURE,
               DEVICE_VTBL_INDEX_CREATEGRAPHICSPIPELINESTATE,
               DEVICE_VTBL_INDEX_CREATECOMPUTEPIPELINESTATE);
    }
}


/* ----------------------------------------------------------------------- *
 *  Proxy init / shutdown
 * ----------------------------------------------------------------------- */
static BOOL ProxyInitialize(void)
{
    GetModuleDirectory(g_hModule, g_ModuleDir, sizeof(g_ModuleDir));
    LoadConfig();

    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%sd3d12proxy.log", g_ModuleDir);
    LogInit(logPath);

    LogMsg("d3d12proxy starting up.");
    LogMsg("  DLLPath                 = %s", g_Config.DLLPath);
    LogMsg("  EmulatedFeatureLevel    = %s",
           g_Config.HasFeatureLevelOverride ? "override active" : "Auto (no override)");
    LogMsg("  OutputLogFile           = %d", g_Config.OutputLogFile);
    LogMsg("  BypassRayTracingChecks  = %d", g_Config.BypassRayTracingChecks);
    LogMsg("  BypassDLSSChecks        = %d", g_Config.BypassDLSSChecks);
    LogMsg("  StripUnsupportedRootSigFlags = %d", g_Config.StripUnsupportedRootSigFlags);
    LogMsg("  StripUnsupportedHeapFlags = %d", g_Config.StripUnsupportedHeapFlags);
    LogMsg("  FixReadbackInitialState = %d", g_Config.FixReadbackInitialState);
    LogMsg("  ShowConsole             = %d", g_Config.ShowConsole);
    LogMsg("  DisableTiledResources   = %d", g_Config.DisableTiledResources);
    LogMsg("  FixReservedResourceRefresh = %d", g_Config.FixReservedResourceRefresh);
    LogMsg("  LogDeviceCreation       = %d", g_Config.LogDeviceCreation);
    LogMsg("  LogFeatureSupport       = %d", g_Config.LogFeatureSupport);
    LogMsg("  DisableSharedFences     = %d", g_Config.DisableSharedFences);
    LogMsg("  CoalesceCommandLists    = %d", g_Config.CoalesceCommandLists);
    LogMsg("  SynthesizeFailedFeatureQueries = %d", g_Config.SynthesizeFailedFeatureQueries);
    LogMsg("  LogPresentTiming        = %d", g_Config.LogPresentTiming);
    LogMsg("  StripPresentVBlankWait  = %d", g_Config.StripPresentVBlankWait);
    LogMsg("  LogPipelineStateCreation = %d", g_Config.LogPipelineStateCreation);
    LogMsg("  LogQueueActivity        = %d", g_Config.LogQueueActivity);

    if (g_Config.ShowConsole) {
        EnsureConsole();
    }

    if (g_Config.FixReservedResourceRefresh) {
        TileTrack_Init();
    }

    if (g_Config.DLLPath[0] == '\0') {
        LogMsg("FATAL: DLLPath is empty in d3d12proxy.ini.");
        MessageBoxA(NULL,
            "d3d12proxy: \"DLLPath\" is not set in d3d12proxy.ini.\n"
            "Please point it to the real D3D12On7 DLL (renamed so it is not "
            "called d3d12.dll itself).",
            "d3d12proxy error", MB_ICONERROR | MB_OK);
        return FALSE;
    }

    g_hRealDll = LoadLibraryA(g_Config.DLLPath);
    if (!g_hRealDll) {
        LogMsg("FATAL: could not load real DLL '%s' (GetLastError=%lu)",
               g_Config.DLLPath, (unsigned long)GetLastError());
        MessageBoxA(NULL,
            "d3d12proxy: failed to load the real D3D12On7 DLL.\n"
            "Check the \"DLLPath\" setting in d3d12proxy.ini.",
            "d3d12proxy error", MB_ICONERROR | MB_OK);
        return FALSE;
    }

    g_pfn_D3D12CreateDevice =
        (PFN_D3D12CreateDevice)GetProcAddress(g_hRealDll, "D3D12CreateDevice");
    g_pfn_D3D12GetDebugInterface =
        (PFN_D3D12GetDebugInterface)GetProcAddress(g_hRealDll, "D3D12GetDebugInterface");
    g_pfn_D3D12CreateRootSignatureDeserializer =
        (PFN_D3D12CreateRootSignatureDeserializer)GetProcAddress(
            g_hRealDll, "D3D12CreateRootSignatureDeserializer");
    g_pfn_D3D12CreateVersionedRootSignatureDeserializer =
        (PFN_D3D12CreateVersionedRootSignatureDeserializer)GetProcAddress(
            g_hRealDll, "D3D12CreateVersionedRootSignatureDeserializer");
    g_pfn_D3D12SerializeRootSignature =
        (PFN_D3D12SerializeRootSignature)GetProcAddress(
            g_hRealDll, "D3D12SerializeRootSignature");
    g_pfn_D3D12SerializeVersionedRootSignature =
        (PFN_D3D12SerializeVersionedRootSignature)GetProcAddress(
            g_hRealDll, "D3D12SerializeVersionedRootSignature");
    g_pfn_D3D12EnableExperimentalFeatures =
        (PFN_D3D12EnableExperimentalFeatures)GetProcAddress(
            g_hRealDll, "D3D12EnableExperimentalFeatures");

	LogMsg("Resolved exports: CreateDevice=%s GetDebugInterface=%s "
           "CreateRootSigDeserializer=%s CreateVersionedRootSigDeserializer=%s "
           "SerializeRootSig=%s SerializeVersionedRootSig=%s "
           "EnableExperimentalFeatures=%s",
           g_pfn_D3D12CreateDevice ? "yes" : "NO",
           g_pfn_D3D12GetDebugInterface ? "yes" : "NO",
           g_pfn_D3D12CreateRootSignatureDeserializer ? "yes" : "NO",
           g_pfn_D3D12CreateVersionedRootSignatureDeserializer ? "yes" : "NO",
           g_pfn_D3D12SerializeRootSignature ? "yes" : "NO",
           g_pfn_D3D12SerializeVersionedRootSignature ? "yes" : "NO",
           g_pfn_D3D12EnableExperimentalFeatures ? "yes" : "NO");

    LogMsg("d3d12proxy initialized successfully.");
    return TRUE;
}

static void ProxyShutdown(void)
{
    LogMsg("d3d12proxy shutting down.");
    CoalesceSlot_FlushAll();       /* NOUVEAU: don't drop pending work on exit */
    Coalesce_GlobalShutdown();     /* NOUVEAU */
    BarrierSlot_FlushAll();        /* NOUVEAU: don't drop pending barriers on exit */
    Coalesce_BarrierGlobalShutdown(); /* NOUVEAU */
    if (g_hRealDll) {
        FreeLibrary(g_hRealDll);
        g_hRealDll = NULL;
    }
    TileTrack_Shutdown();
    LogShutdown();
}

/* ----------------------------------------------------------------------- *
 *  Exported functions (forwarded to the real DLL)
 *
 *  These names and signatures must exactly match the real d3d12.dll exports
 *  so that the application linking against "d3d12.dll" resolves them here
 *  transparently. See d3d12proxy.def for the actual export declarations.
 * ----------------------------------------------------------------------- */

HRESULT WINAPI D3D12CreateDevice(
    void *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
    DPROXY_REFIID riid, void **ppDevice)
{
    char flBufReq[24], flBufEff[24], guidBuf[48], hrBuf[16];

    /* ppDevice == NULL is the standard "just tell me if this would work"
     * capability probe (e.g. UE5's D3D12 RHI iterates adapters/feature
     * levels this way before picking one) -- distinct from an actual
     * device creation. Worth knowing which one we're looking at. */
    int isProbeOnly = (ppDevice == NULL);

    D3D_FEATURE_LEVEL effectiveLevel = MinimumFeatureLevel;

    if (g_Config.HasFeatureLevelOverride) {
        effectiveLevel = g_Config.EmulatedFeatureLevel;
        LogMsg("D3D12CreateDevice: overriding feature level %s -> %s",
               FormatFeatureLevel(MinimumFeatureLevel, flBufReq, sizeof(flBufReq)),
               FormatFeatureLevel(effectiveLevel, flBufEff, sizeof(flBufEff)));
    } else {
        LogMsg("D3D12CreateDevice: requested feature level %s (no override)",
               FormatFeatureLevel(MinimumFeatureLevel, flBufReq, sizeof(flBufReq)));
    }

    if (g_Config.LogDeviceCreation) {
        ConsoleMsg("[d3d12proxy] D3D12CreateDevice(adapter=%p, minFeatureLevel=%s, "
                   "riid=%s, %s) called",
                   pAdapter,
                   FormatFeatureLevel(MinimumFeatureLevel, flBufReq, sizeof(flBufReq)),
                   FormatGuid(riid, guidBuf, sizeof(guidBuf)),
                   isProbeOnly ? "ppDevice=NULL -> capability probe only"
                               : "ppDevice!=NULL -> real device requested");
    }

    if (!g_pfn_D3D12CreateDevice) {
        LogMsg("D3D12CreateDevice: real export not found, returning E_NOTIMPL");
        if (g_Config.LogDeviceCreation) {
            ConsoleMsg("[d3d12proxy] D3D12CreateDevice: real D3D12On7 export not "
                       "found in '%s' -- check DLLPath in d3d12proxy.ini",
                       g_Config.DLLPath);
        }
        return E_NOTIMPL;
    }

    HRESULT hr = g_pfn_D3D12CreateDevice(pAdapter, effectiveLevel, riid, ppDevice);
    LogMsg("D3D12CreateDevice: real call returned 0x%08lX", (unsigned long)hr);

    if (g_Config.LogDeviceCreation) {
        ConsoleMsg("[d3d12proxy] D3D12CreateDevice -> %s%s",
                   DecodeHR(hr, hrBuf, sizeof(hrBuf)),
                   FAILED(hr)
                       ? " <-- this is why the app thinks DX12 is unsupported"
                       : "");
    }

    if (SUCCEEDED(hr) && ppDevice && *ppDevice &&
        (g_Config.BypassRayTracingChecks || g_Config.BypassDLSSChecks ||
         g_Config.DisableTiledResources || g_Config.LogFeatureSupport ||
         g_Config.FixReservedResourceRefresh || g_Config.DisableSharedFences ||
         g_Config.StripUnsupportedHeapFlags || g_Config.FixReadbackInitialState)) {
        InstallDeviceHooks(*ppDevice);
    }

    return hr;
}

HRESULT WINAPI D3D12GetDebugInterface(DPROXY_REFIID riid, void **ppvDebug)
{
    LogMsg("D3D12GetDebugInterface called.");
    if (!g_pfn_D3D12GetDebugInterface) return E_NOTIMPL;
    return g_pfn_D3D12GetDebugInterface(riid, ppvDebug);
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    const void *pSrcData, SIZE_T SrcDataSizeInBytes,
    DPROXY_REFIID pRootSignatureDeserializerInterface,
    void **ppRootSignatureDeserializer)
{
    if (g_Config.LogPipelineStateCreation) {
        LONG64 n = InterlockedIncrement64(&g_RootSigDeserializerCount);
        LARGE_INTEGER freq = {0}, t0 = {0}, t1 = {0};
        int haveTimer = QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&t0);

        HRESULT hr = g_pfn_D3D12CreateRootSignatureDeserializer ?
            g_pfn_D3D12CreateRootSignatureDeserializer(
                pSrcData, SrcDataSizeInBytes,
                pRootSignatureDeserializerInterface, ppRootSignatureDeserializer)
            : E_NOTIMPL;

        if (haveTimer && QueryPerformanceCounter(&t1)) {
            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
            LogMsg("D3D12CreateRootSignatureDeserializer #%lld: %.2f ms, "
                   "blob=%zu bytes -> hr=0x%08lX",
                   (long long)n, ms, (size_t)SrcDataSizeInBytes, (unsigned long)hr);
        }
        return hr;
    }

    LogMsg("D3D12CreateRootSignatureDeserializer called.");
    if (!g_pfn_D3D12CreateRootSignatureDeserializer) return E_NOTIMPL;
    return g_pfn_D3D12CreateRootSignatureDeserializer(
        pSrcData, SrcDataSizeInBytes,
        pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    const void *pSrcData, SIZE_T SrcDataSizeInBytes,
    DPROXY_REFIID pRootSignatureDeserializerInterface,
    void **ppRootSignatureDeserializer)
{
    LogMsg("D3D12CreateVersionedRootSignatureDeserializer called.");
    if (!g_pfn_D3D12CreateVersionedRootSignatureDeserializer) return E_NOTIMPL;
    return g_pfn_D3D12CreateVersionedRootSignatureDeserializer(
        pSrcData, SrcDataSizeInBytes,
        pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

HRESULT WINAPI D3D12SerializeRootSignature(
    const void *pRootSignature, UINT Version, void **ppBlob, void **ppErrorBlob)
{
    LogMsg("D3D12SerializeRootSignature called.");
    if (!g_pfn_D3D12SerializeRootSignature) return E_NOTIMPL;

    /* ---- Strip unsupported root-signature flags (opt-in workaround) ----
     * D3D12On7 predates D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_
     * DIRECTLY_INDEXED (0x400) and D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_
     * HEAP_DIRECTLY_INDEXED (0x800) -- the "dynamic resources"/
     * ResourceDescriptorHeap[]/SamplerDescriptorHeap[] feature tied to
     * Shader Model 6.6. Its D3D12SerializeRootSignature rejects any
     * D3D12_ROOT_SIGNATURE_DESC::Flags value containing these bits
     * outright (observed: E_INVALIDARG, error blob text "Unsupported
     * bit-flag set (root signature flags 0x...)"). Some apps do not
     * check the HRESULT/error blob before dereferencing it, causing a
     * null-pointer crash in the app rather than a clean failure.
     * D3D12_ROOT_SIGNATURE_DESC's layout (root sig version 1.0, what
     * this entry point always takes) is:
     *   UINT                            NumParameters;      // +0x00
     *   const D3D12_ROOT_PARAMETER      *pParameters;       // +0x08
     *   UINT                            NumStaticSamplers;  // +0x10
     *   const D3D12_STATIC_SAMPLER_DESC *pStaticSamplers;   // +0x18
     *   D3D12_ROOT_SIGNATURE_FLAGS      Flags;              // +0x20
     * This only masks the two "directly indexed" bits; it cannot make
     * dynamic-resource-heap-indexing shaders actually WORK on D3D12On7
     * (nothing changes the compiled DXIL), only lets root-signature
     * creation succeed instead of crashing here. If the app's shaders
     * then genuinely rely on ResourceDescriptorHeap[]/
     * SamplerDescriptorHeap[] at draw time, expect corrupted rendering
     * or a later crash/device-removed in CreateGraphicsPipelineState or
     * during a draw call -- this is a "get past this exact crash and see
     * what's next" workaround, not a real fix for the underlying
     * hardware/runtime gap. */
    if (g_Config.StripUnsupportedRootSigFlags && pRootSignature) {
        UINT origFlags = *(const UINT *)((const BYTE *)pRootSignature + 0x20);
        UINT badBits   = origFlags & 0xC00u;
        if (badBits) {
            /* Copy the desc so we don't mutate the caller's own memory. */
            BYTE descCopy[0x28];
            CopyMemory(descCopy, pRootSignature, sizeof(descCopy));
            *(UINT *)(descCopy + 0x20) = origFlags & ~0xC00u;
            LogMsg("D3D12SerializeRootSignature: stripping unsupported flags "
                   "0x%X from Flags=0x%X (D3D12On7 does not support "
                   "CBV_SRV_UAV/SAMPLER_HEAP_DIRECTLY_INDEXED).",
                   badBits, origFlags);
            return g_pfn_D3D12SerializeRootSignature(descCopy, Version, ppBlob, ppErrorBlob);
        }
    }

    return g_pfn_D3D12SerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const void *pRootSignature, void **ppBlob, void **ppErrorBlob)
{
    LogMsg("D3D12SerializeVersionedRootSignature called.");
    if (!g_pfn_D3D12SerializeVersionedRootSignature) return E_NOTIMPL;
    return g_pfn_D3D12SerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT NumFeatures, const GUID *pIIDs,
    void *pConfigurationStructs, UINT *pConfigurationStructSizes)
{
    LogMsg("D3D12EnableExperimentalFeatures called.");
    if (!g_pfn_D3D12EnableExperimentalFeatures) return E_NOTIMPL;
    return g_pfn_D3D12EnableExperimentalFeatures(
        NumFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes);
}

/* ----------------------------------------------------------------------- *
 *  DLL entry point
 * ----------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hinstDLL;
        if (!ProxyInitialize()) {
            /* Returning FALSE here aborts the load of this DLL, which will
             * normally make the application fail to start with a "missing
             * or invalid DLL" style error. The MessageBox shown inside
             * ProxyInitialize() already explained why. */
            return FALSE;
        }
        break;

    case DLL_PROCESS_DETACH:
        ProxyShutdown();
        break;
    }

    return TRUE;
}