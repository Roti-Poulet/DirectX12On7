/*
 * d3d12.c - D3D12 implementation for Windows 7
 * Backend: Direct3D 11
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -DCOBJMACROS -DINITGUID -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 d3d12.c -shared -o d3d12.dll d3d12.def -ld3d11 -ldxgi -lole32 -luuid -lkernel32 -luser32
 *
 * FIX: Tous les types de structures sont correctement déclarés en avance
 *      pour éviter les erreurs de type incomplet.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef INITGUID
#define INITGUID
#endif

#include <windows.h>
#include <initguid.h>
#include <unknwn.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcommon.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ============================================================
 * SECTION 0 : D3D12 GUIDs et types minimaux
 * ============================================================ */

/* IIDs D3D12 */
DEFINE_GUID(IID_ID3D12Object,            0xc4fec28f,0x7966,0x4e95,0x9f,0x94,0x7d,0x85,0x14,0x78,0x38,0xd7);
DEFINE_GUID(IID_ID3D12DeviceChild,       0x905db94b,0xa00c,0x4140,0x9d,0xf5,0x2b,0x64,0xca,0x9e,0xa3,0x57);
DEFINE_GUID(IID_ID3D12RootSignature,     0xc54a6b62,0x72df,0x4ee8,0x8b,0xe5,0xa9,0x46,0xa1,0x42,0x92,0x14);
DEFINE_GUID(IID_ID3D12RootSignature1,    0xc54a6b66,0x72df,0x4ee8,0x8b,0xe5,0xa9,0x46,0xa1,0x42,0x92,0x14);
DEFINE_GUID(IID_ID3D12PipelineState,     0x765a30f3,0xf624,0x4c6f,0xa8,0x28,0xac,0xe9,0x48,0x62,0x24,0x45);
DEFINE_GUID(IID_ID3D12DescriptorHeap,    0x8efb471d,0x616c,0x4f49,0x90,0xf7,0x12,0x7b,0xb7,0x63,0xfa,0x51);
DEFINE_GUID(IID_ID3D12Resource,          0x696442be,0xa72e,0x4059,0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad);
DEFINE_GUID(IID_ID3D12CommandAllocator,  0x6102dee4,0xaf59,0x4b09,0xb9,0x99,0xb4,0x4d,0x73,0xf0,0x9b,0x24);
DEFINE_GUID(IID_ID3D12Fence,             0x0a753dcf,0xc4d8,0x4b91,0xad,0xf6,0xbe,0x5a,0x60,0xd9,0x5a,0x76);
DEFINE_GUID(IID_ID3D12CommandList,       0x7116d91c,0xe7e4,0x47ce,0xb8,0xc6,0xec,0x81,0x68,0xf4,0x37,0xe5);
DEFINE_GUID(IID_ID3D12GraphicsCommandList, 0x5b160d0f,0xac1b,0x4185,0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55);
DEFINE_GUID(IID_ID3D12CommandQueue,      0x0ec870a6,0x5d7e,0x4c22,0x8c,0xfc,0x5b,0xaa,0xe0,0x76,0x16,0xed);
DEFINE_GUID(IID_ID3D12Device,            0x189819f1,0x1db6,0x4b57,0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7);
DEFINE_GUID(IID_ID3D12Device1,           0x77acce80,0x638e,0x4e65,0x88,0x95,0xc1,0xf2,0x33,0x86,0x86,0x3e);
DEFINE_GUID(IID_ID3D12Device2,           0x30baa41e,0xb15b,0x475a,0xa7,0x55,0xb4,0x2d,0x72,0x43,0x49,0x03);

/* IIDs helpers */
DEFINE_GUID(IID_IUnknown_local, 0x00000000,0x0000,0x0000,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);

/* D3D12 enums de base (identiques à la version précédente - on les reprend) */
typedef enum D3D12_COMMAND_LIST_TYPE {
    D3D12_COMMAND_LIST_TYPE_DIRECT  = 0,
    D3D12_COMMAND_LIST_TYPE_BUNDLE  = 1,
    D3D12_COMMAND_LIST_TYPE_COMPUTE = 2,
    D3D12_COMMAND_LIST_TYPE_COPY    = 3,
} D3D12_COMMAND_LIST_TYPE;

typedef enum D3D12_DESCRIPTOR_HEAP_TYPE {
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 0,
    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER     = 1,
    D3D12_DESCRIPTOR_HEAP_TYPE_RTV         = 2,
    D3D12_DESCRIPTOR_HEAP_TYPE_DSV         = 3,
    D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES   = 4,
} D3D12_DESCRIPTOR_HEAP_TYPE;

typedef enum D3D12_DESCRIPTOR_HEAP_FLAGS {
    D3D12_DESCRIPTOR_HEAP_FLAG_NONE           = 0,
    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE = 0x1,
} D3D12_DESCRIPTOR_HEAP_FLAGS;

typedef enum D3D12_HEAP_TYPE {
    D3D12_HEAP_TYPE_DEFAULT  = 1,
    D3D12_HEAP_TYPE_UPLOAD   = 2,
    D3D12_HEAP_TYPE_READBACK = 3,
    D3D12_HEAP_TYPE_CUSTOM   = 4,
} D3D12_HEAP_TYPE;

typedef enum D3D12_RESOURCE_DIMENSION {
    D3D12_RESOURCE_DIMENSION_UNKNOWN   = 0,
    D3D12_RESOURCE_DIMENSION_BUFFER    = 1,
    D3D12_RESOURCE_DIMENSION_TEXTURE1D = 2,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
    D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4,
} D3D12_RESOURCE_DIMENSION;

typedef enum D3D12_RESOURCE_FLAGS {
    D3D12_RESOURCE_FLAG_NONE                  = 0,
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET   = 0x1,
    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL   = 0x2,
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 0x4,
    D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE  = 0x8,
    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER   = 0x10,
    D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS = 0x20,
} D3D12_RESOURCE_FLAGS;

typedef enum D3D12_RESOURCE_STATES {
    D3D12_RESOURCE_STATE_COMMON                = 0,
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER = 0x1,
    D3D12_RESOURCE_STATE_INDEX_BUFFER          = 0x2,
    D3D12_RESOURCE_STATE_RENDER_TARGET         = 0x4,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS      = 0x8,
    D3D12_RESOURCE_STATE_DEPTH_WRITE           = 0x10,
    D3D12_RESOURCE_STATE_DEPTH_READ            = 0x20,
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 0x80,
    D3D12_RESOURCE_STATE_COPY_DEST             = 0x400,
    D3D12_RESOURCE_STATE_COPY_SOURCE           = 0x800,
    D3D12_RESOURCE_STATE_RESOLVE_DEST          = 0x1000,
    D3D12_RESOURCE_STATE_RESOLVE_SOURCE        = 0x2000,
    D3D12_RESOURCE_STATE_PRESENT               = 0,
    D3D12_RESOURCE_STATE_GENERIC_READ          = 0x1|0x2|0x40|0x80|0x800|0x4000,
} D3D12_RESOURCE_STATES;

typedef enum D3D_ROOT_SIGNATURE_VERSION {
    D3D_ROOT_SIGNATURE_VERSION_1   = 0x1,
    D3D_ROOT_SIGNATURE_VERSION_1_0 = 0x1,
    D3D_ROOT_SIGNATURE_VERSION_1_1 = 0x2,
} D3D_ROOT_SIGNATURE_VERSION;

typedef enum D3D12_ROOT_PARAMETER_TYPE {
    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE = 0,
    D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS  = 1,
    D3D12_ROOT_PARAMETER_TYPE_CBV              = 2,
    D3D12_ROOT_PARAMETER_TYPE_SRV              = 3,
    D3D12_ROOT_PARAMETER_TYPE_UAV              = 4,
} D3D12_ROOT_PARAMETER_TYPE;

typedef enum D3D12_SHADER_VISIBILITY {
    D3D12_SHADER_VISIBILITY_ALL      = 0,
    D3D12_SHADER_VISIBILITY_VERTEX   = 1,
    D3D12_SHADER_VISIBILITY_HULL     = 2,
    D3D12_SHADER_VISIBILITY_DOMAIN   = 3,
    D3D12_SHADER_VISIBILITY_GEOMETRY = 4,
    D3D12_SHADER_VISIBILITY_PIXEL    = 5,
} D3D12_SHADER_VISIBILITY;

typedef enum D3D12_DESCRIPTOR_RANGE_TYPE {
    D3D12_DESCRIPTOR_RANGE_TYPE_SRV     = 0,
    D3D12_DESCRIPTOR_RANGE_TYPE_UAV     = 1,
    D3D12_DESCRIPTOR_RANGE_TYPE_CBV     = 2,
    D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER = 3,
} D3D12_DESCRIPTOR_RANGE_TYPE;

typedef enum D3D12_ROOT_SIGNATURE_FLAGS {
    D3D12_ROOT_SIGNATURE_FLAG_NONE                                = 0,
    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT = 0x1,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS     = 0x2,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS       = 0x4,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS     = 0x8,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS   = 0x10,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS      = 0x20,
    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT               = 0x40,
} D3D12_ROOT_SIGNATURE_FLAGS;

typedef enum D3D12_STATIC_BORDER_COLOR {
    D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK = 0,
    D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK      = 1,
    D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE      = 2,
} D3D12_STATIC_BORDER_COLOR;

typedef enum D3D12_FENCE_FLAGS {
    D3D12_FENCE_FLAG_NONE                = 0,
    D3D12_FENCE_FLAG_SHARED             = 0x1,
    D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER = 0x2,
} D3D12_FENCE_FLAGS;

/* D3D12 structs fondamentaux */
typedef struct D3D12_DESCRIPTOR_RANGE {
    D3D12_DESCRIPTOR_RANGE_TYPE RangeType;
    UINT                        NumDescriptors;
    UINT                        BaseShaderRegister;
    UINT                        RegisterSpace;
    UINT                        OffsetInDescriptorsFromTableStart;
} D3D12_DESCRIPTOR_RANGE;

typedef struct D3D12_ROOT_DESCRIPTOR_TABLE {
    UINT                          NumDescriptorRanges;
    const D3D12_DESCRIPTOR_RANGE *pDescriptorRanges;
} D3D12_ROOT_DESCRIPTOR_TABLE;

typedef struct D3D12_ROOT_CONSTANTS {
    UINT ShaderRegister;
    UINT RegisterSpace;
    UINT Num32BitValues;
} D3D12_ROOT_CONSTANTS;

typedef struct D3D12_ROOT_DESCRIPTOR {
    UINT ShaderRegister;
    UINT RegisterSpace;
} D3D12_ROOT_DESCRIPTOR;

typedef struct D3D12_ROOT_PARAMETER {
    D3D12_ROOT_PARAMETER_TYPE ParameterType;
    union {
        D3D12_ROOT_DESCRIPTOR_TABLE DescriptorTable;
        D3D12_ROOT_CONSTANTS        Constants;
        D3D12_ROOT_DESCRIPTOR       Descriptor;
    };
    D3D12_SHADER_VISIBILITY ShaderVisibility;
} D3D12_ROOT_PARAMETER;

typedef struct D3D12_STATIC_SAMPLER_DESC {
    D3D11_FILTER               Filter;
    D3D11_TEXTURE_ADDRESS_MODE AddressU;
    D3D11_TEXTURE_ADDRESS_MODE AddressV;
    D3D11_TEXTURE_ADDRESS_MODE AddressW;
    FLOAT                      MipLODBias;
    UINT                       MaxAnisotropy;
    D3D11_COMPARISON_FUNC      ComparisonFunc;
    D3D12_STATIC_BORDER_COLOR  BorderColor;
    FLOAT                      MinLOD;
    FLOAT                      MaxLOD;
    UINT                       ShaderRegister;
    UINT                       RegisterSpace;
    D3D12_SHADER_VISIBILITY    ShaderVisibility;
} D3D12_STATIC_SAMPLER_DESC;

typedef struct D3D12_ROOT_SIGNATURE_DESC {
    UINT                         NumParameters;
    const D3D12_ROOT_PARAMETER  *pParameters;
    UINT                         NumStaticSamplers;
    const D3D12_STATIC_SAMPLER_DESC *pStaticSamplers;
    D3D12_ROOT_SIGNATURE_FLAGS   Flags;
} D3D12_ROOT_SIGNATURE_DESC;

typedef enum D3D12_DESCRIPTOR_RANGE_FLAGS {
    D3D12_DESCRIPTOR_RANGE_FLAG_NONE = 0,
    D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE = 0x1,
    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE        = 0x2,
    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE = 0x4,
    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC          = 0x8,
} D3D12_DESCRIPTOR_RANGE_FLAGS;

typedef enum D3D12_ROOT_DESCRIPTOR_FLAGS {
    D3D12_ROOT_DESCRIPTOR_FLAG_NONE         = 0,
    D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE = 0x2,
    D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE = 0x4,
    D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC  = 0x8,
} D3D12_ROOT_DESCRIPTOR_FLAGS;

typedef struct D3D12_DESCRIPTOR_RANGE1 {
    D3D12_DESCRIPTOR_RANGE_TYPE  RangeType;
    UINT                         NumDescriptors;
    UINT                         BaseShaderRegister;
    UINT                         RegisterSpace;
    D3D12_DESCRIPTOR_RANGE_FLAGS Flags;
    UINT                         OffsetInDescriptorsFromTableStart;
} D3D12_DESCRIPTOR_RANGE1;

typedef struct D3D12_ROOT_DESCRIPTOR_TABLE1 {
    UINT                           NumDescriptorRanges;
    const D3D12_DESCRIPTOR_RANGE1 *pDescriptorRanges;
} D3D12_ROOT_DESCRIPTOR_TABLE1;

typedef struct D3D12_ROOT_DESCRIPTOR1 {
    UINT                        ShaderRegister;
    UINT                        RegisterSpace;
    D3D12_ROOT_DESCRIPTOR_FLAGS Flags;
} D3D12_ROOT_DESCRIPTOR1;

typedef struct D3D12_ROOT_PARAMETER1 {
    D3D12_ROOT_PARAMETER_TYPE ParameterType;
    union {
        D3D12_ROOT_DESCRIPTOR_TABLE1 DescriptorTable;
        D3D12_ROOT_CONSTANTS         Constants;
        D3D12_ROOT_DESCRIPTOR1       Descriptor;
    };
    D3D12_SHADER_VISIBILITY ShaderVisibility;
} D3D12_ROOT_PARAMETER1;

typedef struct D3D12_ROOT_SIGNATURE_DESC1 {
    UINT                          NumParameters;
    const D3D12_ROOT_PARAMETER1  *pParameters;
    UINT                          NumStaticSamplers;
    const D3D12_STATIC_SAMPLER_DESC *pStaticSamplers;
    D3D12_ROOT_SIGNATURE_FLAGS    Flags;
} D3D12_ROOT_SIGNATURE_DESC1;

typedef struct D3D12_VERSIONED_ROOT_SIGNATURE_DESC {
    D3D_ROOT_SIGNATURE_VERSION Version;
    union {
        D3D12_ROOT_SIGNATURE_DESC  Desc_1_0;
        D3D12_ROOT_SIGNATURE_DESC1 Desc_1_1;
    };
} D3D12_VERSIONED_ROOT_SIGNATURE_DESC;

typedef struct D3D12_DESCRIPTOR_HEAP_DESC {
    D3D12_DESCRIPTOR_HEAP_TYPE  Type;
    UINT                        NumDescriptors;
    D3D12_DESCRIPTOR_HEAP_FLAGS Flags;
    UINT                        NodeMask;
} D3D12_DESCRIPTOR_HEAP_DESC;

typedef struct D3D12_COMMAND_QUEUE_DESC {
    D3D12_COMMAND_LIST_TYPE Type;
    INT                     Priority;
    UINT                    Flags;
    UINT                    NodeMask;
} D3D12_COMMAND_QUEUE_DESC;

typedef UINT64 D3D12_GPU_VIRTUAL_ADDRESS;

typedef enum D3D12_TEXTURE_LAYOUT {
    D3D12_TEXTURE_LAYOUT_UNKNOWN                 = 0,
    D3D12_TEXTURE_LAYOUT_ROW_MAJOR               = 1,
    D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE  = 2,
    D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE   = 3,
} D3D12_TEXTURE_LAYOUT;

typedef enum D3D12_CPU_PAGE_PROPERTY {
    D3D12_CPU_PAGE_PROPERTY_UNKNOWN       = 0,
    D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE = 1,
    D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE = 2,
    D3D12_CPU_PAGE_PROPERTY_WRITE_BACK    = 3,
} D3D12_CPU_PAGE_PROPERTY;

typedef enum D3D12_MEMORY_POOL {
    D3D12_MEMORY_POOL_UNKNOWN = 0,
    D3D12_MEMORY_POOL_L0      = 1,
    D3D12_MEMORY_POOL_L1      = 2,
} D3D12_MEMORY_POOL;

typedef enum D3D_SHADER_MODEL {
    D3D_SHADER_MODEL_5_1 = 0x51,
    D3D_SHADER_MODEL_6_0 = 0x60,
    D3D_SHADER_MODEL_6_1 = 0x61,
    D3D_SHADER_MODEL_6_2 = 0x62,
    D3D_SHADER_MODEL_6_3 = 0x63,
    D3D_SHADER_MODEL_6_4 = 0x64,
    D3D_SHADER_MODEL_6_5 = 0x65,
} D3D_SHADER_MODEL;

typedef struct D3D12_RESOURCE_DESC {
    D3D12_RESOURCE_DIMENSION Dimension;
    UINT64                   Alignment;
    UINT64                   Width;
    UINT                     Height;
    UINT16                   DepthOrArraySize;
    UINT16                   MipLevels;
    DXGI_FORMAT              Format;
    DXGI_SAMPLE_DESC         SampleDesc;
    D3D12_TEXTURE_LAYOUT     Layout;
    D3D12_RESOURCE_FLAGS     Flags;
} D3D12_RESOURCE_DESC;

typedef struct D3D12_HEAP_PROPERTIES {
    D3D12_HEAP_TYPE         Type;
    D3D12_CPU_PAGE_PROPERTY CPUPageProperty;
    D3D12_MEMORY_POOL       MemoryPoolPreference;
    UINT                    CreationNodeMask;
    UINT                    VisibleNodeMask;
} D3D12_HEAP_PROPERTIES;

typedef struct { UINT64 ptr; } D3D12_GPU_DESCRIPTOR_HANDLE;
typedef struct { SIZE_T ptr; } D3D12_CPU_DESCRIPTOR_HANDLE;

typedef struct D3D12_SHADER_BYTECODE {
    const void *pShaderBytecode;
    SIZE_T      BytecodeLength;
} D3D12_SHADER_BYTECODE;

typedef enum D3D12_FEATURE {
    D3D12_FEATURE_D3D12_OPTIONS         = 0,
    D3D12_FEATURE_ARCHITECTURE          = 1,
    D3D12_FEATURE_FEATURE_LEVELS        = 2,
    D3D12_FEATURE_FORMAT_SUPPORT        = 3,
    D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS = 4,
    D3D12_FEATURE_FORMAT_INFO           = 5,
    D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT = 6,
    D3D12_FEATURE_SHADER_MODEL          = 7,
    D3D12_FEATURE_D3D12_OPTIONS1        = 8,
    D3D12_FEATURE_D3D12_OPTIONS2        = 18,
    D3D12_FEATURE_ROOT_SIGNATURE        = 12,
    D3D12_FEATURE_ARCHITECTURE1         = 16,
    D3D12_FEATURE_D3D12_OPTIONS3        = 20,
    D3D12_FEATURE_EXISTING_HEAPS        = 21,
    D3D12_FEATURE_D3D12_OPTIONS4        = 23,
    D3D12_FEATURE_SERIALIZATION         = 24,
    D3D12_FEATURE_CROSS_NODE            = 25,
    D3D12_FEATURE_D3D12_OPTIONS5        = 27,
} D3D12_FEATURE;

typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS {
    BOOL DoublePrecisionFloatShaderOps;
    BOOL OutputMergerLogicOp;
    UINT MinPrecisionSupport;
    UINT TiledResourcesTier;
    UINT ResourceBindingTier;
    BOOL PSSpecifiedStencilRefSupported;
    BOOL TypedUAVLoadAdditionalFormats;
    BOOL ROVsSupported;
    UINT ConservativeRasterizationTier;
    UINT MaxGPUVirtualAddressBitsPerResource;
    BOOL StandardSwizzle64KBSupported;
    UINT CrossNodeSharingTier;
    BOOL CrossAdapterRowMajorTextureSupported;
    BOOL VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation;
    UINT ResourceHeapTier;
} D3D12_FEATURE_DATA_D3D12_OPTIONS;

typedef struct D3D12_FEATURE_DATA_FEATURE_LEVELS {
    UINT                NumFeatureLevels;
    const D3D_FEATURE_LEVEL *pFeatureLevelsRequested;
    D3D_FEATURE_LEVEL   MaxSupportedFeatureLevel;
} D3D12_FEATURE_DATA_FEATURE_LEVELS;

typedef struct D3D12_FEATURE_DATA_SHADER_MODEL {
    D3D_SHADER_MODEL HighestShaderModel;
} D3D12_FEATURE_DATA_SHADER_MODEL;

typedef struct D3D12_FEATURE_DATA_ROOT_SIGNATURE {
    D3D_ROOT_SIGNATURE_VERSION HighestVersion;
} D3D12_FEATURE_DATA_ROOT_SIGNATURE;

typedef struct D3D12_RANGE { SIZE_T Begin; SIZE_T End; } D3D12_RANGE;
typedef struct D3D12_BOX { UINT left, top, front, right, bottom, back; } D3D12_BOX;

typedef struct D3D12_RESOURCE_BARRIER {
    UINT Type;
    UINT Flags;
    union {
        struct { void *pResource; UINT Subresource; UINT StateBefore; UINT StateAfter; } Transition;
        struct { void *pResourceBefore; void *pResourceAfter; } Aliasing;
        struct { void *pUAVResource; } UAV;
    };
} D3D12_RESOURCE_BARRIER;

typedef struct D3D12_VIEWPORT {
    FLOAT TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
} D3D12_VIEWPORT;

typedef struct D3D12_RECT { LONG left, top, right, bottom; } D3D12_RECT;

typedef struct D3D12_VERTEX_BUFFER_VIEW {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
    UINT                      SizeInBytes;
    UINT                      StrideInBytes;
} D3D12_VERTEX_BUFFER_VIEW;

typedef struct D3D12_INDEX_BUFFER_VIEW {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
    UINT                      SizeInBytes;
    DXGI_FORMAT               Format;
} D3D12_INDEX_BUFFER_VIEW;

/* Structures pour CreateGraphicsPipelineState */
typedef enum D3D12_INPUT_CLASSIFICATION {
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA = 0,
    D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA = 1,
} D3D12_INPUT_CLASSIFICATION;

typedef struct D3D12_INPUT_ELEMENT_DESC {
    const char *SemanticName;
    UINT SemanticIndex;
    DXGI_FORMAT Format;
    UINT InputSlot;
    UINT AlignedByteOffset;
    D3D12_INPUT_CLASSIFICATION InputSlotClass;
    UINT InstanceDataStepRate;
} D3D12_INPUT_ELEMENT_DESC;

typedef struct D3D12_INPUT_LAYOUT_DESC {
    const D3D12_INPUT_ELEMENT_DESC *pInputElementDescs;
    UINT NumElements;
} D3D12_INPUT_LAYOUT_DESC;

typedef struct D3D12_STREAM_OUTPUT_DESC {
    /* non utilisé */
} D3D12_STREAM_OUTPUT_DESC;

typedef struct D3D12_BLEND_DESC {
    /* non utilisé */
} D3D12_BLEND_DESC;

typedef struct D3D12_RASTERIZER_DESC {
    /* non utilisé */
} D3D12_RASTERIZER_DESC;

typedef struct D3D12_DEPTH_STENCIL_DESC {
    /* non utilisé */
} D3D12_DEPTH_STENCIL_DESC;

typedef enum D3D12_INDEX_BUFFER_STRIP_CUT_VALUE {
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED = 0,
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF = 1,
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF = 2,
} D3D12_INDEX_BUFFER_STRIP_CUT_VALUE;

typedef enum D3D12_PRIMITIVE_TOPOLOGY_TYPE {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED = 0,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT = 1,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE = 2,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE = 3,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH = 4,
} D3D12_PRIMITIVE_TOPOLOGY_TYPE;

typedef struct D3D12_CACHED_PIPELINE_STATE {
    const void *pCachedBlob;
    SIZE_T CachedBlobSizeInBytes;
} D3D12_CACHED_PIPELINE_STATE;

typedef enum D3D12_PIPELINE_STATE_FLAGS {
    D3D12_PIPELINE_STATE_FLAG_NONE = 0,
    D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG = 0x1,
} D3D12_PIPELINE_STATE_FLAGS;

typedef struct D3D12_GRAPHICS_PIPELINE_STATE_DESC {
    IUnknown *pRootSignature;  // casté plus tard en D12RootSig*
    D3D12_SHADER_BYTECODE VS;
    D3D12_SHADER_BYTECODE PS;
    D3D12_SHADER_BYTECODE DS;
    D3D12_SHADER_BYTECODE HS;
    D3D12_SHADER_BYTECODE GS;
    D3D12_STREAM_OUTPUT_DESC StreamOutput;
    D3D12_BLEND_DESC BlendState;
    UINT SampleMask;
    D3D12_RASTERIZER_DESC RasterizerState;
    D3D12_DEPTH_STENCIL_DESC DepthStencilState;
    D3D12_INPUT_LAYOUT_DESC InputLayout;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType;
    UINT NumRenderTargets;
    DXGI_FORMAT RTVFormats[8];
    DXGI_FORMAT DSVFormat;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT NodeMask;
    D3D12_CACHED_PIPELINE_STATE CachedPSO;
    D3D12_PIPELINE_STATE_FLAGS Flags;
} D3D12_GRAPHICS_PIPELINE_STATE_DESC;

/* ============================================================
 * SECTION 1 : Helpers COM & logging
 * ============================================================ */

#ifdef DEBUG
#define LOG(fmt, ...) do { char _b[512]; snprintf(_b,sizeof(_b),"[d3d12w7] " fmt "\n", ##__VA_ARGS__); OutputDebugStringA(_b); } while(0)
#define WARN(fmt, ...) do { char _b[512]; snprintf(_b,sizeof(_b),"[d3d12w7 WARN] " fmt "\n", ##__VA_ARGS__); OutputDebugStringA(_b); } while(0)
#define HOT_LOG(fmt, ...) ((void)0)   // Désactivé complètement pour perf
#else
#define LOG(fmt, ...) ((void)0)
#define WARN(fmt, ...) ((void)0)
#define HOT_LOG(fmt, ...) ((void)0)
#endif
/* TRACE_LOG : TOUJOURS actif meme en release, pour diagnostiquer les crashs D3D11.
 * A retirer une fois le bug identifie (impact perf en hot path). */
#define TRACE_LOG(fmt, ...) do { char _tb[512]; snprintf(_tb,sizeof(_tb),"[TRACE] " fmt "\n", ##__VA_ARGS__); OutputDebugStringA(_tb); } while(0)

#define SAFE_RELEASE(p) do { if(p){ ((IUnknown*)(void*)(p))->lpVtbl->Release((IUnknown*)(void*)(p)); (p)=NULL; } } while(0)

static inline void* d12_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static inline void d12_free(void *p) { if(p) HeapFree(GetProcessHeap(), 0, p); }

static volatile LONG g_ResourceIDCounter = 0;

/* ============================================================
 * SECTION 2 : Private data store
 * ============================================================ */

typedef struct PDE {
    GUID            guid;
    void           *data;
    UINT            size;
    BOOL            is_iface;
    struct PDE     *next;
} PDE;

typedef struct { PDE *head; CRITICAL_SECTION cs; } PDStore;

static void PDStore_Init(PDStore *s) {
    s->head = NULL;
    InitializeCriticalSection(&s->cs);
}

static void PDStore_Destroy(PDStore *s) {
    EnterCriticalSection(&s->cs);
    PDE *e = s->head;
    while (e) {
        PDE *next = e->next;
        if (e->is_iface && e->data)
            ((IUnknown*)e->data)->lpVtbl->Release((IUnknown*)e->data);
        else
            d12_free(e->data);
        d12_free(e);
        e = next;
    }
    s->head = NULL;
    LeaveCriticalSection(&s->cs);
    DeleteCriticalSection(&s->cs);
}

static HRESULT PDStore_Get(PDStore *s, REFGUID guid, UINT *pSize, void *pData) {
    EnterCriticalSection(&s->cs);
    for (PDE *e = s->head; e; e = e->next) {
        if (IsEqualGUID(&e->guid, guid)) {
            if (e->is_iface) {
                if (*pSize < sizeof(void*)) { LeaveCriticalSection(&s->cs); return E_INVALIDARG; }
                *pSize = sizeof(void*);
                if (pData) {
                    *(IUnknown**)pData = (IUnknown*)e->data;
                    ((IUnknown*)e->data)->lpVtbl->AddRef((IUnknown*)e->data);
                }
            } else {
                if (pData && *pSize < e->size) { LeaveCriticalSection(&s->cs); return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
                *pSize = e->size;
                if (pData) memcpy(pData, e->data, e->size);
            }
            LeaveCriticalSection(&s->cs);
            return S_OK;
        }
    }
    LeaveCriticalSection(&s->cs);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT PDStore_Set(PDStore *s, REFGUID guid, UINT size, const void *pData) {
    EnterCriticalSection(&s->cs);
    for (PDE *e = s->head; e; e = e->next) {
        if (IsEqualGUID(&e->guid, guid)) {
            if (e->is_iface && e->data) ((IUnknown*)e->data)->lpVtbl->Release((IUnknown*)e->data);
            else d12_free(e->data);
            e->is_iface = FALSE;
            if (pData && size > 0) {
                e->data = d12_alloc(size);
                e->size = size;
                if (e->data) memcpy(e->data, pData, size);
            } else { e->data = NULL; e->size = 0; }
            LeaveCriticalSection(&s->cs);
            return S_OK;
        }
    }
    PDE *ne = d12_alloc(sizeof(PDE));
    if (!ne) { LeaveCriticalSection(&s->cs); return E_OUTOFMEMORY; }
    ne->guid     = *guid;
    ne->is_iface = FALSE;
    ne->size     = size;
    ne->data     = (pData && size) ? d12_alloc(size) : NULL;
    if (ne->data) memcpy(ne->data, pData, size);
    ne->next     = s->head;
    s->head      = ne;
    LeaveCriticalSection(&s->cs);
    return S_OK;
}

static HRESULT PDStore_SetIface(PDStore *s, REFGUID guid, const IUnknown *pIface) {
    EnterCriticalSection(&s->cs);
    for (PDE *e = s->head; e; e = e->next) {
        if (IsEqualGUID(&e->guid, guid)) {
            if (e->is_iface && e->data) ((IUnknown*)e->data)->lpVtbl->Release((IUnknown*)e->data);
            else d12_free(e->data);
            e->is_iface = TRUE;
            e->data     = (void*)pIface;
            e->size     = sizeof(void*);
            if (pIface) ((IUnknown*)pIface)->lpVtbl->AddRef((IUnknown*)pIface);
            LeaveCriticalSection(&s->cs);
            return S_OK;
        }
    }
    PDE *ne = d12_alloc(sizeof(PDE));
    if (!ne) { LeaveCriticalSection(&s->cs); return E_OUTOFMEMORY; }
    ne->guid     = *guid;
    ne->is_iface = TRUE;
    ne->data     = (void*)pIface;
    ne->size     = sizeof(void*);
    if (pIface) ((IUnknown*)pIface)->lpVtbl->AddRef((IUnknown*)pIface);
    ne->next     = s->head;
    s->head      = ne;
    LeaveCriticalSection(&s->cs);
    return S_OK;
}

/* ============================================================
 * SECTION 3 : ID3DBlob
 * ============================================================ */

typedef struct {
    struct {
        HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
        ULONG   (WINAPI *AddRef)(IUnknown*);
        ULONG   (WINAPI *Release)(IUnknown*);
        LPVOID  (WINAPI *GetBufferPointer)(IUnknown*);
        SIZE_T  (WINAPI *GetBufferSize)(IUnknown*);
    } *lpVtbl;
    LONG   ref;
    void  *buf;
    SIZE_T size;
} D3DBlob;

static HRESULT WINAPI Blob_QI(IUnknown *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ID3DBlob)) {
        ((D3DBlob*)self)->ref++;
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI Blob_AddRef(IUnknown *self)  { return InterlockedIncrement(&((D3DBlob*)self)->ref); }
static ULONG WINAPI Blob_Release(IUnknown *self) {
    D3DBlob *b = (D3DBlob*)self;
    ULONG r = InterlockedDecrement(&b->ref);
    if (!r) { d12_free(b->buf); d12_free(b); }
    return r;
}
static LPVOID WINAPI Blob_GetBufferPointer(IUnknown *self) { return ((D3DBlob*)self)->buf; }
static SIZE_T WINAPI Blob_GetBufferSize(IUnknown *self)    { return ((D3DBlob*)self)->size; }

static const struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    LPVOID  (WINAPI *GetBufferPointer)(IUnknown*);
    SIZE_T  (WINAPI *GetBufferSize)(IUnknown*);
} g_BlobVtbl = { Blob_QI, Blob_AddRef, Blob_Release, Blob_GetBufferPointer, Blob_GetBufferSize };

static HRESULT CreateBlob(SIZE_T size, IUnknown **ppBlob) {
    D3DBlob *b = d12_alloc(sizeof(D3DBlob));
    if (!b) return E_OUTOFMEMORY;
    b->buf = d12_alloc(size);
    if (!b->buf) { d12_free(b); return E_OUTOFMEMORY; }
    b->lpVtbl = (void*)&g_BlobVtbl;
    b->ref    = 1;
    b->size   = size;
    *ppBlob   = (IUnknown*)b;
    return S_OK;
}


/* ============================================================
 * SECTION 4 : Root Signature Serialization
 * ============================================================ */

#pragma pack(push, 1)
typedef struct RS_HEADER {
    UINT32 Version;
    UINT32 NumParams;
    UINT32 ParamOffset;
    UINT32 NumSamplers;
    UINT32 SamplerOffset;
    UINT32 Flags;
} RS_HEADER;

typedef struct RS_PARAM_ENTRY {
    UINT32 Type;
    UINT32 Visibility;
    UINT32 PayloadOffset;
} RS_PARAM_ENTRY;

typedef struct RS_TABLE_PAYLOAD {
    UINT32 NumRanges;
    UINT32 RangesOffset;
} RS_TABLE_PAYLOAD;

typedef struct RS_RANGE_V1 {
    UINT32 RangeType;
    UINT32 NumDescriptors;
    UINT32 BaseShaderRegister;
    UINT32 RegisterSpace;
    UINT32 OffsetInDescriptorsFromTableStart;
} RS_RANGE_V1;

typedef struct RS_RANGE_V2 {
    UINT32 RangeType;
    UINT32 NumDescriptors;
    UINT32 BaseShaderRegister;
    UINT32 RegisterSpace;
    UINT32 Flags;
    UINT32 OffsetInDescriptorsFromTableStart;
} RS_RANGE_V2;

typedef struct RS_CONSTANTS_PAYLOAD { UINT32 Reg, Space, Count; } RS_CONSTANTS_PAYLOAD;
typedef struct RS_DESCRIPTOR_PAYLOAD_V1 { UINT32 Reg, Space; } RS_DESCRIPTOR_PAYLOAD_V1;
typedef struct RS_DESCRIPTOR_PAYLOAD_V2 { UINT32 Reg, Space, Flags; } RS_DESCRIPTOR_PAYLOAD_V2;
#pragma pack(pop)

static SIZE_T CalcBlobSize_V1(const D3D12_ROOT_SIGNATURE_DESC *d) {
    SIZE_T sz = sizeof(RS_HEADER);
    sz += d->NumParameters * sizeof(RS_PARAM_ENTRY);
    for (UINT i = 0; i < d->NumParameters; i++) {
        const D3D12_ROOT_PARAMETER *p = &d->pParameters[i];
        switch (p->ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            sz += sizeof(RS_TABLE_PAYLOAD);
            sz += p->DescriptorTable.NumDescriptorRanges * sizeof(RS_RANGE_V1);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            sz += sizeof(RS_CONSTANTS_PAYLOAD);
            break;
        default:
            sz += sizeof(RS_DESCRIPTOR_PAYLOAD_V1);
            break;
        }
    }
    sz += d->NumStaticSamplers * sizeof(D3D12_STATIC_SAMPLER_DESC);
    return sz;
}

static SIZE_T CalcBlobSize_V11(const D3D12_ROOT_SIGNATURE_DESC1 *d) {
    SIZE_T sz = sizeof(RS_HEADER);
    sz += d->NumParameters * sizeof(RS_PARAM_ENTRY);
    for (UINT i = 0; i < d->NumParameters; i++) {
        const D3D12_ROOT_PARAMETER1 *p = &d->pParameters[i];
        switch (p->ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            sz += sizeof(RS_TABLE_PAYLOAD);
            sz += p->DescriptorTable.NumDescriptorRanges * sizeof(RS_RANGE_V2);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            sz += sizeof(RS_CONSTANTS_PAYLOAD);
            break;
        default:
            sz += sizeof(RS_DESCRIPTOR_PAYLOAD_V2);
            break;
        }
    }
    sz += d->NumStaticSamplers * sizeof(D3D12_STATIC_SAMPLER_DESC);
    return sz;
}

static HRESULT SerializeRS_V1(const D3D12_ROOT_SIGNATURE_DESC *desc, IUnknown **ppBlob, IUnknown **ppErr) {
    if (ppErr) *ppErr = NULL;
    if (desc->NumParameters > 64) return E_INVALIDARG;
    if (desc->NumStaticSamplers > 2032) return E_INVALIDARG;
    SIZE_T total = CalcBlobSize_V1(desc);
    IUnknown *blob = NULL;
    HRESULT hr = CreateBlob(total, &blob);
    if (FAILED(hr)) return hr;
    BYTE *base = (BYTE*)((D3DBlob*)blob)->buf;
    UINT32 cursor = 0;
    RS_HEADER *hdr = (RS_HEADER*)(base + cursor);
    cursor += sizeof(RS_HEADER);
    hdr->Version    = 1;
    hdr->NumParams  = desc->NumParameters;
    hdr->NumSamplers = desc->NumStaticSamplers;
    hdr->Flags      = (UINT32)desc->Flags;
    hdr->ParamOffset = cursor;
    RS_PARAM_ENTRY *entries = (RS_PARAM_ENTRY*)(base + cursor);
    cursor += desc->NumParameters * sizeof(RS_PARAM_ENTRY);
    for (UINT i = 0; i < desc->NumParameters; i++) {
        const D3D12_ROOT_PARAMETER *p = &desc->pParameters[i];
        entries[i].Type       = (UINT32)p->ParameterType;
        entries[i].Visibility = (UINT32)p->ShaderVisibility;
        entries[i].PayloadOffset = cursor;
        switch (p->ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            RS_TABLE_PAYLOAD *tp = (RS_TABLE_PAYLOAD*)(base + cursor);
            cursor += sizeof(RS_TABLE_PAYLOAD);
            tp->NumRanges   = p->DescriptorTable.NumDescriptorRanges;
            tp->RangesOffset = cursor;
            for (UINT r = 0; r < p->DescriptorTable.NumDescriptorRanges; r++) {
                const D3D12_DESCRIPTOR_RANGE *rng = &p->DescriptorTable.pDescriptorRanges[r];
                RS_RANGE_V1 *rv1 = (RS_RANGE_V1*)(base + cursor);
                rv1->RangeType               = (UINT32)rng->RangeType;
                rv1->NumDescriptors          = rng->NumDescriptors;
                rv1->BaseShaderRegister      = rng->BaseShaderRegister;
                rv1->RegisterSpace           = rng->RegisterSpace;
                rv1->OffsetInDescriptorsFromTableStart = rng->OffsetInDescriptorsFromTableStart;
                cursor += sizeof(RS_RANGE_V1);
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            RS_CONSTANTS_PAYLOAD *cp = (RS_CONSTANTS_PAYLOAD*)(base + cursor);
            cp->Reg   = p->Constants.ShaderRegister;
            cp->Space = p->Constants.RegisterSpace;
            cp->Count = p->Constants.Num32BitValues;
            cursor += sizeof(RS_CONSTANTS_PAYLOAD);
            break;
        }
        default: {
            RS_DESCRIPTOR_PAYLOAD_V1 *dp = (RS_DESCRIPTOR_PAYLOAD_V1*)(base + cursor);
            dp->Reg   = p->Descriptor.ShaderRegister;
            dp->Space = p->Descriptor.RegisterSpace;
            cursor += sizeof(RS_DESCRIPTOR_PAYLOAD_V1);
            break;
        }
        }
    }
    hdr->SamplerOffset = cursor;
    if (desc->NumStaticSamplers > 0) {
        memcpy(base + cursor, desc->pStaticSamplers,
               desc->NumStaticSamplers * sizeof(D3D12_STATIC_SAMPLER_DESC));
        cursor += desc->NumStaticSamplers * sizeof(D3D12_STATIC_SAMPLER_DESC);
    }
    assert(cursor == (UINT32)total);
    *ppBlob = blob;
    return S_OK;
}

static HRESULT SerializeRS_V11(const D3D12_ROOT_SIGNATURE_DESC1 *desc, IUnknown **ppBlob, IUnknown **ppErr) {
    if (ppErr) *ppErr = NULL;
    if (desc->NumParameters > 64) return E_INVALIDARG;
    SIZE_T total = CalcBlobSize_V11(desc);
    IUnknown *blob = NULL;
    HRESULT hr = CreateBlob(total, &blob);
    if (FAILED(hr)) return hr;
    BYTE *base = (BYTE*)((D3DBlob*)blob)->buf;
    UINT32 cursor = 0;
    RS_HEADER *hdr = (RS_HEADER*)(base + cursor);
    cursor += sizeof(RS_HEADER);
    hdr->Version     = 2;
    hdr->NumParams   = desc->NumParameters;
    hdr->NumSamplers = desc->NumStaticSamplers;
    hdr->Flags       = (UINT32)desc->Flags;
    hdr->ParamOffset = cursor;
    RS_PARAM_ENTRY *entries = (RS_PARAM_ENTRY*)(base + cursor);
    cursor += desc->NumParameters * sizeof(RS_PARAM_ENTRY);
    for (UINT i = 0; i < desc->NumParameters; i++) {
        const D3D12_ROOT_PARAMETER1 *p = &desc->pParameters[i];
        entries[i].Type          = (UINT32)p->ParameterType;
        entries[i].Visibility    = (UINT32)p->ShaderVisibility;
        entries[i].PayloadOffset = cursor;
        switch (p->ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            RS_TABLE_PAYLOAD *tp = (RS_TABLE_PAYLOAD*)(base + cursor);
            cursor += sizeof(RS_TABLE_PAYLOAD);
            tp->NumRanges    = p->DescriptorTable.NumDescriptorRanges;
            tp->RangesOffset = cursor;
            for (UINT r = 0; r < p->DescriptorTable.NumDescriptorRanges; r++) {
                const D3D12_DESCRIPTOR_RANGE1 *rng = &p->DescriptorTable.pDescriptorRanges[r];
                RS_RANGE_V2 *rv2 = (RS_RANGE_V2*)(base + cursor);
                rv2->RangeType          = (UINT32)rng->RangeType;
                rv2->NumDescriptors     = rng->NumDescriptors;
                rv2->BaseShaderRegister = rng->BaseShaderRegister;
                rv2->RegisterSpace      = rng->RegisterSpace;
                rv2->Flags              = (UINT32)rng->Flags;
                rv2->OffsetInDescriptorsFromTableStart = rng->OffsetInDescriptorsFromTableStart;
                cursor += sizeof(RS_RANGE_V2);
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            RS_CONSTANTS_PAYLOAD *cp = (RS_CONSTANTS_PAYLOAD*)(base + cursor);
            cp->Reg = p->Constants.ShaderRegister;
            cp->Space = p->Constants.RegisterSpace;
            cp->Count = p->Constants.Num32BitValues;
            cursor += sizeof(RS_CONSTANTS_PAYLOAD);
            break;
        }
        default: {
            RS_DESCRIPTOR_PAYLOAD_V2 *dp = (RS_DESCRIPTOR_PAYLOAD_V2*)(base + cursor);
            dp->Reg   = p->Descriptor.ShaderRegister;
            dp->Space = p->Descriptor.RegisterSpace;
            dp->Flags = (UINT32)p->Descriptor.Flags;
            cursor += sizeof(RS_DESCRIPTOR_PAYLOAD_V2);
            break;
        }
        }
    }
    hdr->SamplerOffset = cursor;
    if (desc->NumStaticSamplers > 0) {
        memcpy(base + cursor, desc->pStaticSamplers,
               desc->NumStaticSamplers * sizeof(D3D12_STATIC_SAMPLER_DESC));
    }
    *ppBlob = blob;
    return S_OK;
}

typedef struct { D3D12_ROOT_SIGNATURE_DESC Desc; } RS_DESER_V1;

static HRESULT DeserializeRS_V1(const void *pData, SIZE_T sz, D3D12_ROOT_SIGNATURE_DESC **ppDesc) {
    if (!pData || sz < sizeof(RS_HEADER)) return E_INVALIDARG;
    const BYTE *base = (const BYTE*)pData;
    const RS_HEADER *hdr = (const RS_HEADER*)base;
    if (hdr->Version != 1) return E_INVALIDARG;
    RS_DESER_V1 *obj = d12_alloc(sizeof(RS_DESER_V1));
    if (!obj) return E_OUTOFMEMORY;
    D3D12_ROOT_SIGNATURE_DESC *d = &obj->Desc;
    d->NumParameters     = hdr->NumParams;
    d->NumStaticSamplers = hdr->NumSamplers;
    d->Flags             = (D3D12_ROOT_SIGNATURE_FLAGS)hdr->Flags;
    const RS_PARAM_ENTRY *entries = (const RS_PARAM_ENTRY*)(base + hdr->ParamOffset);
    D3D12_ROOT_PARAMETER *params = NULL;
    if (hdr->NumParams > 0) {
        params = d12_alloc(hdr->NumParams * sizeof(D3D12_ROOT_PARAMETER));
        if (!params) { d12_free(obj); return E_OUTOFMEMORY; }
        for (UINT i = 0; i < hdr->NumParams; i++) {
            params[i].ParameterType   = (D3D12_ROOT_PARAMETER_TYPE)entries[i].Type;
            params[i].ShaderVisibility = (D3D12_SHADER_VISIBILITY)entries[i].Visibility;
            const BYTE *payload = base + entries[i].PayloadOffset;
            switch (entries[i].Type) {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
                const RS_TABLE_PAYLOAD *tp = (const RS_TABLE_PAYLOAD*)payload;
                params[i].DescriptorTable.NumDescriptorRanges = tp->NumRanges;
                params[i].DescriptorTable.pDescriptorRanges   = (const D3D12_DESCRIPTOR_RANGE*)(base + tp->RangesOffset);
                break;
            }
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
                const RS_CONSTANTS_PAYLOAD *cp = (const RS_CONSTANTS_PAYLOAD*)payload;
                params[i].Constants.ShaderRegister = cp->Reg;
                params[i].Constants.RegisterSpace  = cp->Space;
                params[i].Constants.Num32BitValues = cp->Count;
                break;
            }
            default: {
                const RS_DESCRIPTOR_PAYLOAD_V1 *dp = (const RS_DESCRIPTOR_PAYLOAD_V1*)payload;
                params[i].Descriptor.ShaderRegister = dp->Reg;
                params[i].Descriptor.RegisterSpace  = dp->Space;
                break;
            }
            }
        }
    }
    d->pParameters    = params;
    d->pStaticSamplers = (hdr->NumSamplers > 0)
                         ? (const D3D12_STATIC_SAMPLER_DESC*)(base + hdr->SamplerOffset)
                         : NULL;
    *ppDesc = d;
    return S_OK;
}


/* ============================================================
 * SECTION 5 : Objets D3D12 (définitions complètes)
 * ============================================================ */

/* Cache pour une table de paramètres de Root Signature */
typedef struct D12RSTableCache {
    UINT                num_ranges;
    const RS_RANGE_V1  *ranges;   // pointeur directement dans le blob
} D12RSTableCache;

/* Forward declarations pour tous les types de structures */
typedef struct D12Device D12Device;
typedef struct D12CmdQueue D12CmdQueue;
typedef struct D12CmdList D12CmdList;
typedef struct D12CmdAllocator D12CmdAllocator;
typedef struct D12Fence D12Fence;
typedef struct D12Resource D12Resource;
typedef struct D12DescHeap D12DescHeap;
typedef struct D12PSO D12PSO;
typedef struct D12RootSig D12RootSig;
typedef union D12Descriptor D12Descriptor;

/* Définition de D12Descriptor (union) avant de l'utiliser */
typedef union D12Descriptor {
    ID3D11RenderTargetView   *rtv;
    ID3D11DepthStencilView   *dsv;
    ID3D11ShaderResourceView *srv;
    ID3D11UnorderedAccessView *uav;
    ID3D11SamplerState       *sampler;
    struct { ID3D11Buffer *buf; UINT offset_bytes; UINT size_bytes; } cbv;
} D12Descriptor;

/* Structs descripteurs D3D12 minimales pour CBV/SRV */
typedef struct {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation; /* adresse GPU du buffer */
    UINT                      SizeInBytes;    /* taille en bytes (multiple de 256) */
} D3D12_CONSTANT_BUFFER_VIEW_DESC;

typedef enum {
    D3D12_SRV_DIMENSION_UNKNOWN   = 0,
    D3D12_SRV_DIMENSION_BUFFER    = 1,
    D3D12_SRV_DIMENSION_TEXTURE1D = 2,
    D3D12_SRV_DIMENSION_TEXTURE2D = 4,
} D3D12_SRV_DIMENSION;

typedef struct {
    UINT FirstElement;
    UINT NumElements;
    UINT StructureByteStride;
    UINT Flags;
} D3D12_BUFFER_SRV;

typedef struct {
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT PlaneSlice;
    FLOAT ResourceMinLODClamp;
} D3D12_TEX2D_SRV;

typedef struct {
    DXGI_FORMAT           Format;
    D3D12_SRV_DIMENSION   ViewDimension;
    UINT                  Shader4ComponentMapping;
    union {
        D3D12_BUFFER_SRV  Buffer;
        D3D12_TEX2D_SRV   Texture2D;
    };
} D3D12_SHADER_RESOURCE_VIEW_DESC;

/* Maintenant on peut définir D12Device (ne dépend de rien) */
struct D12Device {
    void                  *lpVtbl;
    LONG                   ref;
    PDStore                pd;
    ID3D11Device          *d11dev;
    ID3D11DeviceContext   *d11ctx;
    IDXGIDevice           *dxgi_dev;
    D3D_FEATURE_LEVEL      actual_fl;
    D3D_FEATURE_LEVEL      exposed_fl;
    UINT64                 next_gpu_va;
    UINT desc_inc[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
    D12DescHeap           *lastRTVHeap;
    D12DescHeap           *lastDSVHeap;
    struct D12Resource    *res_list_head; /* liste chainee pour FindResourceByVA */
};

/* D12CmdQueue utilise D12Device* (déjà défini) */
struct D12CmdQueue {
    void                  *lpVtbl;
    LONG                   ref;
    PDStore                pd;
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    D12Device             *device;
    ID3D11DeviceContext   *imm_ctx;
    IDXGIDevice           *dxgi_dev;
    UINT64                 last_fence_val;
    /* Swap chain D3D11 pour DLPresent (cree a la premiere presentation) */
    IDXGISwapChain        *present_sc;
    HWND                   present_hwnd;
    UINT                   present_w;
    UINT                   present_h;
};

/* D12CmdAllocator utilise D12Device* */
struct D12CmdAllocator {
    void              *lpVtbl;
    LONG               ref;
    PDStore            pd;
    D3D12_COMMAND_LIST_TYPE type;
    ID3D11DeviceContext    *deferred_ctx;
    D12Device              *device;
};

/* D12PSO utilise D12RootSig* (forward déclaré) */
struct D12PSO {
    void      *lpVtbl;
    LONG       ref;
    PDStore    pd;
    D12Device *device;
    ID3D11VertexShader   *vs;
    ID3D11PixelShader    *ps;
    ID3D11GeometryShader *gs;
    ID3D11HullShader     *hs;
    ID3D11DomainShader   *ds;
    ID3D11ComputeShader  *cs;
    ID3D11RasterizerState   *rast;
    ID3D11DepthStencilState *depth;
    ID3D11BlendState        *blend;
    ID3D11InputLayout       *input_layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D12RootSig *root_sig;
};

/* D12RootSig */
struct D12RootSig {
    void     *lpVtbl;
    LONG      ref;
    PDStore   pd;
    void     *blob;           // blob sérialisé
    SIZE_T    blob_sz;
    UINT      num_params;
    UINT      num_samplers;
    D3D12_ROOT_SIGNATURE_FLAGS flags;

    /* Cache pré-parsé pour les Descriptor Tables */
    D12RSTableCache *param_tables;  // tableau de taille num_params

    D12Device *device;
};

typedef struct D12RootSigParamTable {
    UINT                num_ranges;
    const RS_RANGE_V1  *ranges;   // pointeur directement dans le blob
} D12RootSigParamTable;

/* D12DescHeap utilise D12Descriptor* (défini) */
struct D12DescHeap {
    void                    *lpVtbl;
    LONG                     ref;
    PDStore                  pd;
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    D12Descriptor           *descs;
    UINT                     increment;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
};

/* D12Resource */
struct D12Resource {
    void               *lpVtbl;
    LONG                ref;
    PDStore             pd;
    UINT                id;
    D12Device          *device;
    D3D12_RESOURCE_DESC res_desc;
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_STATES state;
    ID3D11Resource     *d11res;
    ID3D11Buffer       *d11buf;
    ID3D11Texture2D    *d11tex;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_va;
    void               *mapped_ptr;
    BOOL                is_cbv;      /* cache: d11buf a BIND_CONSTANT_BUFFER */
    UINT                d11_bind_flags; /* cache complet des BindFlags D3D11 du buffer */
    struct D12Resource *res_list_next; /* pour FindResourceByVA */
};

/* D12CmdList utilise D12CmdAllocator*, D12PSO*, D12RootSig*, D12DescHeap* (tous définis ou forward) */
struct D12CmdList {
    void                  *lpVtbl;
    LONG                   ref;
    PDStore                pd;
    D3D12_COMMAND_LIST_TYPE type;
    bool                   closed;
    D12CmdAllocator       *allocator;
    D12Device             *device;
    D12PSO                *cur_pso;
    D12RootSig            *cur_root_sig;
    D12DescHeap           *desc_heaps[2];
    ID3D11DeviceContext   *ctx;
    ID3D11CommandList     *cmd_list;
    D3D12_VIEWPORT         viewports[16];
    UINT                   num_viewports;
    D3D12_RECT             scissors[16];
    UINT                   num_scissors;
};

/* D12Fence */
struct D12Fence {
    void      *lpVtbl;
    LONG       ref;
    PDStore    pd;
    UINT64     value;
    UINT64     completed;
    HANDLE     event;
    ID3D11Query *query;
    D12Device  *device;
};

/* ============================================================
 * HELPERS POUR DESCRIPTOR HANDLES (RTV/DSV indexés)
 * ============================================================ */

/* Handle valide = adresse user-space Windows 64-bit [0x10000..0x7FFFFFFEFFFF] */
static BOOL IsValidDescriptorPtr(SIZE_T ptr) {
    return ptr >= 0x10000ULL && ptr <= 0x7FFFFFFEFFFFull;
}

/* Cache statique pour retourner un pointeur-vers-pointeur meme quand le
 * handle recu contient DEJA la valeur ID3D11View* (bug cote appelant, ou
 * ABI struct-return non standard). On stocke la valeur dans une variable
 * locale et on retourne son adresse — thread-unsafe mais mieux que crash. */
/* tls_rtv_direct / tls_dsv_direct: plus utilisees depuis la suppression de
 * l'heuristique de correction dans GetRTVSlot/GetDSVSlot (voir plus bas). */

static ID3D11RenderTargetView** GetRTVSlot(D12Device *dev, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)dev;
    /* Maintenant que desc_inc == sizeof(D12Descriptor), h.ptr est TOUJOURS
     * l'adresse d'un slot dans le tableau descs[] du heap, jamais l'objet
     * COM lui-meme. L'ancienne heuristique de "correction" lisait *p puis
     * confondait le slot et l'objet, causant des resolutions vers de la
     * memoire aleatoire (vtable factice) -> crash dans OMSetRenderTargets. */
    if (!IsValidDescriptorPtr(h.ptr)) return NULL;
    return (ID3D11RenderTargetView**)(uintptr_t)h.ptr;
}

static ID3D11DepthStencilView** GetDSVSlot(D12Device *dev, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    (void)dev;
    if (!IsValidDescriptorPtr(h.ptr)) return NULL;
    return (ID3D11DepthStencilView**)(uintptr_t)h.ptr;
}

/* Macro COM boilerplate */
#define COM_QI_BEGIN(ClassName) \
    static HRESULT WINAPI __attribute__((unused)) ClassName##_QI(IUnknown *self, REFIID riid, void **ppv) { \
        if (!ppv) return E_POINTER; \
        ClassName *this_ = (ClassName*)self;

#define COM_QI_IFACE(IID_, IfaceName) \
        if (IsEqualGUID(riid, &IID_) || IsEqualGUID(riid, &IID_IUnknown_local)) { \
            InterlockedIncrement(&this_->ref); \
            *ppv = self; \
            return S_OK; \
        }

#define COM_QI_END() \
        LOG("QI: interface inconnue"); \
        *ppv = NULL; \
        return E_NOINTERFACE; \
    }

#define COM_ADDREF_RELEASE(ClassName, DestroyFn) \
    static ULONG WINAPI __attribute__((unused)) ClassName##_AddRef(IUnknown *self) { \
        return InterlockedIncrement(&((ClassName*)self)->ref); \
    } \
    static ULONG WINAPI __attribute__((unused)) ClassName##_Release(IUnknown *self) { \
        ClassName *this_ = (ClassName*)self; \
        ULONG r = InterlockedDecrement(&this_->ref); \
        if (!r) DestroyFn(this_); \
        return r; \
    }

#define IMPL_D3D12OBJECT(ClassName) \
    static HRESULT WINAPI __attribute__((unused)) ClassName##_GetPrivateData(IUnknown *s, REFGUID g, UINT *sz, void *p) \
        { return PDStore_Get(&((ClassName*)s)->pd, g, sz, p); } \
    static HRESULT WINAPI __attribute__((unused)) ClassName##_SetPrivateData(IUnknown *s, REFGUID g, UINT sz, const void *p) \
        { return PDStore_Set(&((ClassName*)s)->pd, g, sz, p); } \
    static HRESULT WINAPI __attribute__((unused)) ClassName##_SetPrivateDataInterface(IUnknown *s, REFGUID g, const IUnknown *p) \
        { return PDStore_SetIface(&((ClassName*)s)->pd, g, p); } \
    static HRESULT WINAPI __attribute__((unused)) ClassName##_SetName(IUnknown *s, LPCWSTR n) \
        { LOG("SetName %S", n); return S_OK; }

#define IMPL_GETDEVICE(ClassName) \
    static HRESULT WINAPI __attribute__((unused)) ClassName##_GetDevice(IUnknown *s, REFIID riid, void **ppv) { \
        ClassName *this_ = (ClassName*)s; \
        if (!ppv) return E_POINTER; \
        if (!this_->device) { *ppv = NULL; return E_FAIL; } \
        return ((IUnknown*)(void*)this_->device)->lpVtbl->QueryInterface( \
            (IUnknown*)(void*)this_->device, riid, ppv); \
    }


/* ============================================================
 * SECTION 6 : ID3D12Fence - implémentation
 * ============================================================ */

static void D12Fence_Destroy(D12Fence *f) {
    PDStore_Destroy(&f->pd);
    if (f->event) CloseHandle(f->event);
    SAFE_RELEASE(f->query);
    d12_free(f);
}

IMPL_D3D12OBJECT(D12Fence)
COM_ADDREF_RELEASE(D12Fence, D12Fence_Destroy)
COM_QI_BEGIN(D12Fence)
    COM_QI_IFACE(IID_ID3D12Fence, ID3D12Fence)
COM_QI_END()

static UINT64 WINAPI D12Fence_GetCompletedValue(IUnknown *s)
    { return ((D12Fence*)s)->completed; }

static HRESULT WINAPI D12Fence_SetEventOnCompletion(IUnknown *s, UINT64 value, HANDLE ev) {
    D12Fence *f = (D12Fence*)s;
    if (f->completed >= value) { SetEvent(ev); return S_OK; }
    f->event = ev;
    return S_OK;
}

static HRESULT WINAPI D12Fence_Signal(IUnknown *s, UINT64 value) {
    D12Fence *f = (D12Fence*)s;
    f->completed = value;
    if (f->event && f->completed >= f->value) { SetEvent(f->event); f->event = NULL; }
    return S_OK;
}


/* ============================================================
 * SECTION 7 : ID3D12RootSignature
 * ============================================================ */

/* Destruction */
static void D12RootSig_Destroy(D12RootSig *r) {
    PDStore_Destroy(&r->pd);
    d12_free(r->blob);
    d12_free(r->param_tables);
    d12_free(r);
}

/* QI */
static HRESULT WINAPI D12RootSig_QI(IUnknown *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    D12RootSig *this_ = (D12RootSig*)self;
    if (IsEqualGUID(riid, &IID_IUnknown_local) ||
        IsEqualGUID(riid, &IID_ID3D12RootSignature) ||
        IsEqualGUID(riid, &IID_ID3D12RootSignature1))
    {
        InterlockedIncrement(&this_->ref);
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

/* AddRef / Release */
static ULONG WINAPI D12RootSig_AddRef(IUnknown *self) {
    return InterlockedIncrement(&((D12RootSig*)self)->ref);
}

static ULONG WINAPI D12RootSig_Release(IUnknown *self) {
    D12RootSig *r = (D12RootSig*)self;
    ULONG ref = InterlockedDecrement(&r->ref);
    if (ref == 0) D12RootSig_Destroy(r);
    return ref;
}

/* GetDevice */
static HRESULT WINAPI D12RootSig_GetDevice(IUnknown *s, REFIID riid, void **ppv) {
    D12RootSig *rs = (D12RootSig*)s;
    if (!ppv) return E_POINTER;
    if (!rs->device) { *ppv = NULL; return E_FAIL; }
    return ((IUnknown*)(void*)rs->device)->lpVtbl->QueryInterface(
        (IUnknown*)(void*)rs->device, riid, ppv);
}

/* IMPL_D3D12OBJECT (GetPrivateData, SetPrivateData, etc.) */
IMPL_D3D12OBJECT(D12RootSig)


/* ============================================================
 * SECTION 8 : ID3D12CommandAllocator
 * ============================================================ */

static void D12CmdAllocator_Destroy(D12CmdAllocator *a) {
    PDStore_Destroy(&a->pd);
    SAFE_RELEASE(a->deferred_ctx);
    d12_free(a);
}

IMPL_D3D12OBJECT(D12CmdAllocator)
IMPL_GETDEVICE(D12CmdAllocator)
COM_ADDREF_RELEASE(D12CmdAllocator, D12CmdAllocator_Destroy)
COM_QI_BEGIN(D12CmdAllocator)
    COM_QI_IFACE(IID_ID3D12CommandAllocator, ID3D12CommandAllocator)
COM_QI_END()

static HRESULT WINAPI D12CmdAlloc_Reset(IUnknown *s) {
    D12CmdAllocator *a = (D12CmdAllocator*)s;
    SAFE_RELEASE(a->deferred_ctx);
    return S_OK;
}


/* ============================================================
 * SECTION 9 : ID3D12DescriptorHeap (version corrigée)
 * ============================================================ */

static void D12DescHeap_Destroy(D12DescHeap *h) {
    PDStore_Destroy(&h->pd);
    for (UINT i = 0; i < h->desc.NumDescriptors; i++) {
        D12Descriptor *d = &h->descs[i];
        switch (h->desc.Type) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: SAFE_RELEASE(d->rtv); break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: SAFE_RELEASE(d->dsv); break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: SAFE_RELEASE(d->sampler); break;
        default: break;
        }
    }
    d12_free(h->descs);
    d12_free(h);
}

IMPL_D3D12OBJECT(D12DescHeap)
COM_ADDREF_RELEASE(D12DescHeap, D12DescHeap_Destroy)
COM_QI_BEGIN(D12DescHeap)
    COM_QI_IFACE(IID_ID3D12DescriptorHeap, ID3D12DescriptorHeap)
COM_QI_END()

/* GetDesc - structure > 8 bytes => hidden return pointer, mais 'this' reste
 * le premier argument (RCX), le pointeur de retour vient en second (RDX). */
static void WINAPI D12DescHeap_GetDesc(IUnknown *s, D3D12_DESCRIPTOR_HEAP_DESC *pOut) {
    D12DescHeap *h = (D12DescHeap*)s;
    *pOut = h->desc;
    LOG("D12DescHeap_GetDesc: type=%u, num=%u, flags=%u",
        h->desc.Type, h->desc.NumDescriptors, h->desc.Flags);
}

/* GetCPUDescriptorHandleForHeapStart / GetGPUDescriptorHandleForHeapStart
 *
 * IMPORTANT ABI : D3D12_CPU_DESCRIPTOR_HANDLE et D3D12_GPU_DESCRIPTOR_HANDLE
 * ne contiennent qu'un seul SIZE_T (8 bytes total). Selon l'ABI Windows x64,
 * les structs de taille <= 8 bytes sont retournees PAR VALEUR dans RAX, pas
 * via un pointeur cache. Il n'y a donc PAS de hidden-pointer ici (a la
 * difference de D3D12_RESOURCE_DESC ou D3D12_DESCRIPTOR_HEAP_DESC qui font
 * plus de 8 bytes). 'this' reste le premier argument, retour normal. */
/* IMPORTANT: GetCPUDescriptorHandleForHeapStart / GetGPUDescriptorHandleForHeapStart
 * sont des cas particuliers connus de l'ABI D3D12 : bien que
 * D3D12_CPU/GPU_DESCRIPTOR_HANDLE ne fassent que 8 bytes, MSVC les traite
 * comme des structs non-triviales et les retourne via hidden-pointer,
 * PAS dans RAX. C'est une particularite documentee de ces deux methodes
 * precises (Wine et RenderDoc s'y sont deja heurtes). On applique donc
 * la convention COM standard : this en premier (RCX), pointeur de retour
 * cache en second (RDX), et on retourne aussi ce pointeur dans RAX. */
static D3D12_CPU_DESCRIPTOR_HANDLE* WINAPI D12DescHeap_GetCPUDescriptorHandleForHeapStart(
    IUnknown *s, D3D12_CPU_DESCRIPTOR_HANDLE *pOut)
{
    D12DescHeap *h = (D12DescHeap*)s;
    TRACE_LOG("GetCPUDescriptorHandleForHeapStart: this=%p cpu_base.ptr=%p pOut=%p", (void*)s, (void*)h->cpu_base.ptr, (void*)pOut);
    *pOut = h->cpu_base;
    return pOut;
}

static D3D12_GPU_DESCRIPTOR_HANDLE* WINAPI D12DescHeap_GetGPUDescriptorHandleForHeapStart(
    IUnknown *s, D3D12_GPU_DESCRIPTOR_HANDLE *pOut)
{
    D12DescHeap *h = (D12DescHeap*)s;
    TRACE_LOG("GetGPUDescriptorHandleForHeapStart: this=%p gpu_base.ptr=%p pOut=%p", (void*)s, (void*)h->gpu_base.ptr, (void*)pOut);
    *pOut = h->gpu_base;
    return pOut;
}

/* GetDevice pour D12DescHeap (slot 7) */
static HRESULT WINAPI D12DescHeap_GetDevice(IUnknown *s, REFIID riid, void **ppv) {
    (void)s; (void)riid;
    WARN("D12DescHeap_GetDevice: non implemente (pas de backref device)");
    if (ppv) *ppv = NULL;
    return E_NOTIMPL;
}

/* Vtable pour D12DescHeap - ORDRE: 0-2 IUnknown, 3-6 ID3D12Object,
 * 7 GetDevice, 8 GetDesc, 9 GetCPUDescriptorHandleForHeapStart,
 * 10 GetGPUDescriptorHandleForHeapStart */
typedef struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
    HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
    HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
    HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
    HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
    void    (WINAPI *GetDesc)(IUnknown*, D3D12_DESCRIPTOR_HEAP_DESC*); /* hidden pointer - >8 bytes, this first */
    D3D12_CPU_DESCRIPTOR_HANDLE* (WINAPI *GetCPUDescriptorHandleForHeapStart)(IUnknown*, D3D12_CPU_DESCRIPTOR_HANDLE*); /* hidden-pointer: particularite D3D12 */
    D3D12_GPU_DESCRIPTOR_HANDLE* (WINAPI *GetGPUDescriptorHandleForHeapStart)(IUnknown*, D3D12_GPU_DESCRIPTOR_HANDLE*); /* hidden-pointer: particularite D3D12 */
} DHVtbl;

static const DHVtbl g_DHVtbl = {
    (HRESULT(WINAPI*)(IUnknown*,REFIID,void**))D12DescHeap_QI,
    (ULONG(WINAPI*)(IUnknown*))D12DescHeap_AddRef,
    (ULONG(WINAPI*)(IUnknown*))D12DescHeap_Release,
    D12DescHeap_GetPrivateData,
    D12DescHeap_SetPrivateData,
    D12DescHeap_SetPrivateDataInterface,
    D12DescHeap_SetName,
    D12DescHeap_GetDevice,
    D12DescHeap_GetDesc,
    D12DescHeap_GetCPUDescriptorHandleForHeapStart,
    D12DescHeap_GetGPUDescriptorHandleForHeapStart,
};

static __attribute__((unused)) UINT D12DescHeap_IndexOf(D12DescHeap *heap, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    return (UINT)((h.ptr - heap->cpu_base.ptr) / heap->increment);
}


/* ============================================================
 * SECTION 10 : ID3D12Resource
 * ============================================================ */

static void D12Resource_Destroy(D12Resource *r) {
    PDStore_Destroy(&r->pd);
    SAFE_RELEASE(r->d11res);
    d12_free(r);
}

IMPL_D3D12OBJECT(D12Resource)
COM_ADDREF_RELEASE(D12Resource, D12Resource_Destroy)
COM_QI_BEGIN(D12Resource)
    COM_QI_IFACE(IID_ID3D12Resource, ID3D12Resource)
COM_QI_END()

/* GetDesc - convention COM Windows x64 pour retour de struct > 8 bytes:
 * (this*, hidden_ret_ptr*) - this en RCX, pointeur de retour en RDX. */
static void WINAPI D12Resource_GetDesc(IUnknown *s, D3D12_RESOURCE_DESC *pOut) {
    D12Resource *r = (D12Resource*)s;
    *pOut = r->res_desc;
    LOG("D12Resource_GetDesc: dim=%u, w=%llu, h=%u, fmt=%u",
        r->res_desc.Dimension, r->res_desc.Width, r->res_desc.Height, r->res_desc.Format);
}

static HRESULT WINAPI D12Resource_Map(IUnknown *s, UINT sub, const D3D12_RANGE *r, void **ppData) {
    D12Resource *res = (D12Resource*)s;
    char buf[512];
    snprintf(buf, sizeof(buf), "D12Resource_Map: id=%u, sub=%u, heapType=%d, dim=%u, width=%llu, d11buf=%p, d11tex=%p\n",
             res->id, sub, res->heap_props.Type, res->res_desc.Dimension, res->res_desc.Width, res->d11buf, res->d11tex);
    OutputDebugStringA(buf);
    if (!ppData) return E_INVALIDARG;
    *ppData = NULL;

    // Si déjà mappé, retourner le pointeur existant
    if (res->mapped_ptr) {
        *ppData = res->mapped_ptr;
        snprintf(buf, sizeof(buf), "  Map déjà mappé, retourne %p\n", res->mapped_ptr);
        OutputDebugStringA(buf);
        return S_OK;
    }

    // Allouer un buffer système pour tous les cas
    SIZE_T size = (SIZE_T)res->res_desc.Width;
    if (size == 0) size = 4096;
    void *ptr = malloc(size);
    if (!ptr) {
        static char dummy[4096];
        ptr = dummy;
    }
    res->mapped_ptr = ptr;
    *ppData = ptr;
    snprintf(buf, sizeof(buf), "  Map alloué %p taille %zu\n", ptr, size);
    OutputDebugStringA(buf);
    return S_OK;
}

static void WINAPI D12Resource_Unmap(IUnknown *s, UINT sub, const D3D12_RANGE *r) {
    D12Resource *res = (D12Resource*)s;
    char buf[256];
    snprintf(buf, sizeof(buf), "D12Resource_Unmap: res=%p, sub=%u, mapped_ptr=%p\n", res, sub, res->mapped_ptr);
    OutputDebugStringA(buf);
    // Ne pas libérer (on pourrait, mais on ne sait pas quand)
}

static D3D12_GPU_VIRTUAL_ADDRESS WINAPI D12Resource_GetGPUVirtualAddress(IUnknown *s) {
    if (!s) return 0;
    D12Resource *res = (D12Resource*)s;
    /* Convention : GPU VA = adresse de l objet D12Resource lui-meme.
     * FindResourceByVA fait un cast direct O(1) : res = (D12Resource*)(uintptr_t)va */
    return (D3D12_GPU_VIRTUAL_ADDRESS)(uintptr_t)res;
}

/* GetDevice pour D12Resource (ID3D12DeviceChild::GetDevice, slot 7) */
static HRESULT WINAPI D12Resource_GetDevice(IUnknown *s, REFIID riid, void **ppv) {
    D12Resource *res = (D12Resource*)s;
    if (!ppv) return E_POINTER;
    if (!res->device) { *ppv = NULL; return E_FAIL; }
    return ((IUnknown*)(void*)res->device)->lpVtbl->QueryInterface(
        (IUnknown*)(void*)res->device, riid, ppv);
}

/* Vtable for D12Resource - ORDRE EXACT ID3D12Resource (verifie via spec MS) :
 * 0-2  IUnknown
 * 3-6  ID3D12Object (GetPrivateData, SetPrivateData, SetPrivateDataInterface, SetName)
 * 7    ID3D12DeviceChild::GetDevice
 * 8    Map
 * 9    Unmap
 * 10   GetDesc
 * 11   GetGPUVirtualAddress
 * 12   WriteToSubresource
 * 13   ReadFromSubresource
 * 14   GetHeapProperties
 */
typedef struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
    HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
    HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
    HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
    HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
    HRESULT (WINAPI *Map)(IUnknown*, UINT, const D3D12_RANGE*, void**);
    void    (WINAPI *Unmap)(IUnknown*, UINT, const D3D12_RANGE*);
    void    (WINAPI *GetDesc)(IUnknown*, D3D12_RESOURCE_DESC*);
    D3D12_GPU_VIRTUAL_ADDRESS (WINAPI *GetGPUVirtualAddress)(IUnknown*);
    HRESULT (WINAPI *WriteToSubresource)();
    HRESULT (WINAPI *ReadFromSubresource)();
    HRESULT (WINAPI *GetHeapProperties)();
} ResVtbl;

static const ResVtbl g_ResVtbl = {
    (HRESULT(WINAPI*)(IUnknown*,REFIID,void**))D12Resource_QI,
    (ULONG(WINAPI*)(IUnknown*))D12Resource_AddRef,
    (ULONG(WINAPI*)(IUnknown*))D12Resource_Release,
    D12Resource_GetPrivateData, D12Resource_SetPrivateData,
    D12Resource_SetPrivateDataInterface, D12Resource_SetName,
    D12Resource_GetDevice,
    D12Resource_Map, D12Resource_Unmap,
    D12Resource_GetDesc, D12Resource_GetGPUVirtualAddress,
    NULL, NULL, NULL
};


/* ============================================================
 * SECTION 11 : ID3D12PipelineState
 * ============================================================ */

static void D12PSO_Destroy(D12PSO *p) {
    PDStore_Destroy(&p->pd);
    SAFE_RELEASE(p->vs);
    SAFE_RELEASE(p->ps);
    SAFE_RELEASE(p->gs);
    SAFE_RELEASE(p->hs);
    SAFE_RELEASE(p->ds);
    SAFE_RELEASE(p->cs);
    SAFE_RELEASE(p->rast);
    SAFE_RELEASE(p->depth);
    SAFE_RELEASE(p->blend);
    SAFE_RELEASE(p->input_layout);
    if (p->root_sig) p->root_sig->lpVtbl = p->root_sig->lpVtbl;
    d12_free(p);
}

IMPL_D3D12OBJECT(D12PSO)
COM_ADDREF_RELEASE(D12PSO, D12PSO_Destroy)

static HRESULT WINAPI D12PSO_QI(IUnknown *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    D12PSO *this_ = (D12PSO*)self;
    if (IsEqualGUID(riid, &IID_IUnknown_local) ||
        IsEqualGUID(riid, &IID_ID3D12Object) ||
        IsEqualGUID(riid, &IID_ID3D12DeviceChild) ||
        IsEqualGUID(riid, &IID_ID3D12PipelineState))
    {
        InterlockedIncrement(&this_->ref);
        *ppv = self;
        return S_OK;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "D12PSO_QI: interface inconnue Data1=%08lX\n", (unsigned long)riid->Data1);
    OutputDebugStringA(buf);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI D12PSO_GetDevice(IUnknown *s, REFIID riid, void **ppv) {
    D12PSO *p = (D12PSO*)s;
    if (!ppv) return E_POINTER;
    if (!p->device) { *ppv = NULL; return E_FAIL; }
    return ((IUnknown*)(void*)p->device)->lpVtbl->QueryInterface(
        (IUnknown*)(void*)p->device, riid, ppv);
}

static HRESULT WINAPI __attribute__((unused)) D12PSO_GetCachedBlob(IUnknown *s, IUnknown **ppBlob)
    { (void)s; *ppBlob = NULL; return E_NOTIMPL; }


/* ============================================================
 * SECTION 12 : ID3D12GraphicsCommandList
 * ============================================================ */

/* Les fonctions sont définies après la structure D12CmdList, qui est maintenant complète */
/* Les déclarations de fonctions vont suivre ... */

static void D12CmdList_Destroy(D12CmdList *cl) {
    PDStore_Destroy(&cl->pd);
    SAFE_RELEASE(cl->ctx);
    SAFE_RELEASE(cl->cmd_list);
    d12_free(cl);
}

IMPL_D3D12OBJECT(D12CmdList)
COM_ADDREF_RELEASE(D12CmdList, D12CmdList_Destroy)
COM_QI_BEGIN(D12CmdList)
    COM_QI_IFACE(IID_ID3D12GraphicsCommandList, ID3D12GraphicsCommandList)
    COM_QI_IFACE(IID_ID3D12CommandList, ID3D12CommandList)
COM_QI_END()

/* Forward declaration - definie dans la section CreateCommittedResource */
static D12Resource *D12Device_FindResourceByVA(D12Device *dev, D3D12_GPU_VIRTUAL_ADDRESS va);

static HRESULT WINAPI D12CmdList_GetDevice(IUnknown *s, REFIID riid, void **ppv) {
    D12CmdList *cl = (D12CmdList*)s;
    if (!ppv) return E_POINTER;
    if (!cl->device) { *ppv = NULL; return E_FAIL; }
    return ((IUnknown*)(void*)cl->device)->lpVtbl->QueryInterface(
        (IUnknown*)(void*)cl->device, riid, ppv);
}

static D3D12_COMMAND_LIST_TYPE WINAPI D12CmdList_GetType(IUnknown *s) {
    D3D12_COMMAND_LIST_TYPE type = ((D12CmdList*)s)->type;
    char buf[64];
    snprintf(buf, sizeof(buf), "D12CmdList_GetType: %u\n", type);
    OutputDebugStringA(buf);
    return type;
}

static HRESULT WINAPI D12CmdList_Close(IUnknown *s) {
    D12CmdList *cl = (D12CmdList*)s;
    if (cl->closed) return S_OK;

    if (cl->ctx) {
        HRESULT hr = cl->ctx->lpVtbl->FinishCommandList(cl->ctx, FALSE, &cl->cmd_list);
        if (FAILED(hr)) {
            SAFE_RELEASE(cl->cmd_list);
        }
    }
    cl->closed = true;
    return S_OK;
}

static void WINAPI D12CmdList_SetPipelineState(IUnknown *s, IUnknown *pPSO);

static HRESULT WINAPI D12CmdList_Reset(IUnknown *s, IUnknown *pAlloc, IUnknown *pInitState) {
    D12CmdList *cl = (D12CmdList*)s;
    D12CmdAllocator *alloc = (D12CmdAllocator*)pAlloc;

    SAFE_RELEASE(cl->cmd_list);
    cl->closed = false;
    cl->cur_pso = NULL;
    cl->cur_root_sig = NULL;

    if (!cl->ctx && alloc && alloc->device && alloc->device->d11dev) {
        HRESULT hr = alloc->device->d11dev->lpVtbl->CreateDeferredContext(alloc->device->d11dev, 0, &alloc->deferred_ctx);
        if (SUCCEEDED(hr)) {
            cl->ctx = alloc->deferred_ctx;
            cl->ctx->lpVtbl->AddRef(cl->ctx);
        }
    } else if (cl->ctx) {
        cl->ctx->lpVtbl->ClearState(cl->ctx);
    }

    /* pInitState = PSO initial, equivalent a appeler SetPipelineState()
     * juste apres Reset(). Sans ca, aucun shader n'est bound tant que
     * l'app n'appelle pas explicitement SetPipelineState -> DrawIndexedInstanced
     * echoue avec DEVICE_DRAW_VERTEX_SHADER_NOT_SET. */
    if (pInitState) {
        D12CmdList_SetPipelineState(s, pInitState);
    }

    return S_OK;
}

static void WINAPI D12CmdList_IASetPrimitiveTopology(IUnknown *s, D3D11_PRIMITIVE_TOPOLOGY topology) {
    D12CmdList *cl = (D12CmdList*)s;
    char buf[128];
    snprintf(buf, sizeof(buf), "D12CmdList_IASetPrimitiveTopology: %u\n", topology);
    OutputDebugStringA(buf);
    if (cl->ctx) {
        cl->ctx->lpVtbl->IASetPrimitiveTopology(cl->ctx, topology);
    }
}

/* Structure D3D12 manquante (ajoute-la dans SECTION 0 si elle n'existe pas) */
typedef struct D3D12_STREAM_OUTPUT_BUFFER_VIEW {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
    UINT64                    SizeInBytes;
    UINT64                    BufferFilledSizeLocation;
} D3D12_STREAM_OUTPUT_BUFFER_VIEW;

/* ============================================================
 * SOSetTargets - implémentation réelle
 * ============================================================ */

static void WINAPI D12CmdList_SOSetTargets(IUnknown *s, UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) {
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx || NumViews == 0 || NumViews > 4 || StartSlot > 4) return;

    ID3D11Buffer *buffers[4] = {0};
    UINT offsets[4] = {0};

    for (UINT i = 0; i < NumViews && i < 4; i++) {
        if (pViews && pViews[i].BufferLocation) {
            D12Resource *res = D12Device_FindResourceByVA(cl->device, pViews[i].BufferLocation);
            if (res && res->d11buf) buffers[i] = res->d11buf;
        }
    }
    cl->ctx->lpVtbl->SOSetTargets(cl->ctx, NumViews, buffers, offsets);
}

/* Effets basiques (hot path) */

static void WINAPI D12CmdList_ClearRenderTargetView(IUnknown *s,
    D3D12_CPU_DESCRIPTOR_HANDLE RTV, const FLOAT color[4],
    UINT NumRects, const D3D12_RECT *pRects)
{
    (void)NumRects; (void)pRects;
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;

    ID3D11RenderTargetView **p = GetRTVSlot(cl->device, RTV);
    if (p && *p) {
        TRACE_LOG("TRACE: about to call ClearRenderTargetView(ctx=%p, rtv=%p)", cl->ctx, *p);
        cl->ctx->lpVtbl->ClearRenderTargetView(cl->ctx, *p, color);
        TRACE_LOG("TRACE: ClearRenderTargetView returned OK");
    } else {
        LOG("ClearRenderTargetView: handle invalide ou vue NULL, skip (RTV.ptr=%p)", (void*)RTV.ptr);
    }
}

static void WINAPI D12CmdList_ClearDepthStencilView(IUnknown *s,
    D3D12_CPU_DESCRIPTOR_HANDLE DSV, UINT ClearFlags, FLOAT Depth,
    UINT8 Stencil, UINT NumRects, const D3D12_RECT *pRects)
{
    (void)NumRects; (void)pRects;
    D12CmdList *cl = (D12CmdList*)s;
    TRACE_LOG("ClearDepthStencilView ENTRY: DSV.ptr=%p", (void*)DSV.ptr);
    if (!cl->ctx) return;

    ID3D11DepthStencilView **p = GetDSVSlot(cl->device, DSV);
    if (p && *p) {
        TRACE_LOG("TRACE: about to call ClearDepthStencilView(ctx=%p, dsv=%p)", cl->ctx, *p);
        cl->ctx->lpVtbl->ClearDepthStencilView(cl->ctx, *p, ClearFlags, Depth, Stencil);
        TRACE_LOG("TRACE: ClearDepthStencilView returned OK");
    } else {
        LOG("ClearDepthStencilView: handle invalide ou vue NULL, skip (DSV.ptr=%p)", (void*)DSV.ptr);
    }
}

static void WINAPI D12CmdList_OMSetRenderTargets(IUnknown *s,
    UINT NumRTV, const D3D12_CPU_DESCRIPTOR_HANDLE *pRTV, BOOL RTsSingle,
    const D3D12_CPU_DESCRIPTOR_HANDLE *pDSV)
{
    /* DIAGNOSTIC: capture des registres RCX/RDX/R8/R9 tels qu'ils sont
     * AU MOMENT DE L'ENTREE dans cette fonction (avant tout mouvement de
     * registre fait par le prologue du compilateur pour les parametres
     * nommes). Ceci nous dit la verite sur ce que l'appelant (MSVC) a
     * reellement mis dans chaque registre, independamment de la maniere
     * dont GCC choisit de les interpreter dans les variables C. */
    UINT64 raw_rcx, raw_rdx, raw_r8, raw_r9;
    __asm__ volatile (
        "movq %%rcx, %0\n\t"
        "movq %%rdx, %1\n\t"
        "movq %%r8,  %2\n\t"
        "movq %%r9,  %3\n\t"
        : "=r"(raw_rcx), "=r"(raw_rdx), "=r"(raw_r8), "=r"(raw_r9)
        :
        : "memory"
    );
    TRACE_LOG("RAW REGS: RCX=%p RDX=%p R8=%p R9=%p",
        (void*)raw_rcx, (void*)raw_rdx, (void*)raw_r8, (void*)raw_r9);

    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;

    TRACE_LOG("TRACE: OMSetRenderTargets NumRTV=%u pRTV=%p RTsSingle=%d pDSV=%p", NumRTV, (void*)pRTV, RTsSingle, (void*)pDSV);

    ID3D11RenderTargetView *rtvs[8] = {0};
    for (UINT i = 0; i < NumRTV && i < 8; i++) {
        if (pRTV) {
            TRACE_LOG("TRACE: OMSetRenderTargets RTV[%u].ptr=%p", i, (void*)pRTV[i].ptr);
            ID3D11RenderTargetView **p = GetRTVSlot(cl->device, pRTV[i]);
            if (p) rtvs[i] = *p;
            TRACE_LOG("TRACE: OMSetRenderTargets RTV[%u] resolved to %p", i, rtvs[i]);
        }
    }

    ID3D11DepthStencilView *dsv = NULL;
    if (pDSV) {
        TRACE_LOG("TRACE: OMSetRenderTargets DSV.ptr=%p", (void*)pDSV->ptr);
        ID3D11DepthStencilView **p = GetDSVSlot(cl->device, *pDSV);
        if (p) dsv = *p;
        TRACE_LOG("TRACE: OMSetRenderTargets DSV resolved to %p", dsv);
    }

    TRACE_LOG("TRACE: about to call ctx->OMSetRenderTargets(ctx=%p)", cl->ctx);
    cl->ctx->lpVtbl->OMSetRenderTargets(cl->ctx, NumRTV, rtvs, dsv);
    TRACE_LOG("TRACE: ctx->OMSetRenderTargets returned OK");
}

static void WINAPI D12CmdList_RSSetViewports(IUnknown *s, UINT n, const D3D12_VIEWPORT *vps) {
    char buf[256];
    snprintf(buf, sizeof(buf), "D12CmdList_RSSetViewports: n=%u\n", n);
    OutputDebugStringA(buf);
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;
    D3D11_VIEWPORT d11vp[16];
    for (UINT i = 0; i < n && i < 16; i++) {
        d11vp[i].TopLeftX = vps[i].TopLeftX;
        d11vp[i].TopLeftY = vps[i].TopLeftY;
        d11vp[i].Width    = vps[i].Width;
        d11vp[i].Height   = vps[i].Height;
        d11vp[i].MinDepth = vps[i].MinDepth;
        d11vp[i].MaxDepth = vps[i].MaxDepth;
    }
    cl->ctx->lpVtbl->RSSetViewports(cl->ctx, n, d11vp);
}

static void WINAPI D12CmdList_RSSetScissorRects(IUnknown *s, UINT n, const D3D12_RECT *rects) {
    char buf[256];
    snprintf(buf, sizeof(buf), "D12CmdList_RSSetScissorRects: n=%u\n", n);
    OutputDebugStringA(buf);
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;
    cl->ctx->lpVtbl->RSSetScissorRects(cl->ctx, n, (const D3D11_RECT*)rects);
}

static void WINAPI D12CmdList_SetPipelineState(IUnknown *s, IUnknown *pPSO) {
    OutputDebugStringA("D12CmdList_SetPipelineState called\n");
    D12CmdList *cl = (D12CmdList*)s;
    D12PSO *pso = (D12PSO*)pPSO;
    if (!cl->ctx || !pso) {
        WARN("SetPipelineState: ctx ou pso NULL");
        return;
    }
    cl->cur_pso = pso;
    cl->ctx->lpVtbl->VSSetShader(cl->ctx, pso->vs, NULL, 0);
    cl->ctx->lpVtbl->PSSetShader(cl->ctx, pso->ps, NULL, 0);
    cl->ctx->lpVtbl->GSSetShader(cl->ctx, pso->gs, NULL, 0);
    cl->ctx->lpVtbl->HSSetShader(cl->ctx, pso->hs, NULL, 0);
    cl->ctx->lpVtbl->DSSetShader(cl->ctx, pso->ds, NULL, 0);
    if (pso->rast)  cl->ctx->lpVtbl->RSSetState(cl->ctx, pso->rast);
    if (pso->depth) cl->ctx->lpVtbl->OMSetDepthStencilState(cl->ctx, pso->depth, 0);
    if (pso->blend) cl->ctx->lpVtbl->OMSetBlendState(cl->ctx, pso->blend, NULL, 0xFFFFFFFF);
    if (pso->input_layout) cl->ctx->lpVtbl->IASetInputLayout(cl->ctx, pso->input_layout);
    cl->ctx->lpVtbl->IASetPrimitiveTopology(cl->ctx, pso->topology);
}

static void WINAPI D12CmdList_IASetVertexBuffers(IUnknown *s, UINT StartSlot, UINT Num, const D3D12_VERTEX_BUFFER_VIEW *pViews) {
    D12CmdList *cl = (D12CmdList*)s;
    char buf[256];
    for (UINT i = 0; i < Num; i++) {
        snprintf(buf, sizeof(buf), "IASetVertexBuffers: slot=%u, BufferLocation=%llu, Size=%u, Stride=%u\n",
                 StartSlot + i, pViews[i].BufferLocation, pViews[i].SizeInBytes, pViews[i].StrideInBytes);
        OutputDebugStringA(buf);
    }
    if (!cl->ctx || !cl->device) return;
    ID3D11Buffer *bufs[16] = {0};
    UINT strides[16] = {0}, offsets[16] = {0};
    for (UINT i = 0; i < Num && i < 16; i++) {
        D12Resource *res = D12Device_FindResourceByVA(cl->device, pViews[i].BufferLocation);
        if (res && res->d11buf && (res->d11_bind_flags & D3D11_BIND_VERTEX_BUFFER)) {
            bufs[i] = res->d11buf;
        } else if (res) {
            LOG("IASetVertexBuffers: resource id=%u sans BIND_VERTEX_BUFFER, slot=%u laisse a NULL", res->id, StartSlot + i);
        }
        strides[i] = pViews[i].StrideInBytes;
        offsets[i] = 0;
    }
    cl->ctx->lpVtbl->IASetVertexBuffers(cl->ctx, StartSlot, Num, bufs, strides, offsets);
}

static void WINAPI D12CmdList_IASetIndexBuffer(IUnknown *s, const D3D12_INDEX_BUFFER_VIEW *pView) {
    D12CmdList *cl = (D12CmdList*)s;
    char buf[256];
    snprintf(buf, sizeof(buf), "IASetIndexBuffer: BufferLocation=%llu, Size=%u, Format=%u\n",
             pView ? pView->BufferLocation : 0, pView ? pView->SizeInBytes : 0, pView ? pView->Format : 0);
    OutputDebugStringA(buf);
    if (!cl->ctx || !pView || !cl->device) return;
    D12Resource *res = D12Device_FindResourceByVA(cl->device, pView->BufferLocation);
    if (!res || !res->d11buf) return;
    if (!(res->d11_bind_flags & D3D11_BIND_INDEX_BUFFER)) {
        LOG("IASetIndexBuffer: resource id=%u sans BIND_INDEX_BUFFER, skip", res->id);
        return;
    }
    DXGI_FORMAT fmt = (pView->Format == DXGI_FORMAT_R32_UINT) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    cl->ctx->lpVtbl->IASetIndexBuffer(cl->ctx, res->d11buf, fmt, 0);
}

static void WINAPI D12CmdList_DrawInstanced(IUnknown *s,
    UINT VertexCount, UINT InstanceCount, UINT StartVertex, UINT StartInstance)
{
    D12CmdList *cl = (D12CmdList*)s;
    char buf[256];
    snprintf(buf, sizeof(buf), "D12CmdList_DrawInstanced: VertexCount=%u, InstanceCount=%u, StartVertex=%u, StartInstance=%u\n",
             VertexCount, InstanceCount, StartVertex, StartInstance);
    OutputDebugStringA(buf);
    if (!cl->ctx) return;
    cl->ctx->lpVtbl->DrawInstanced(cl->ctx, VertexCount, InstanceCount, StartVertex, StartInstance);
}

static void WINAPI D12CmdList_DrawIndexedInstanced(IUnknown *s, UINT IndexCount, UINT InstanceCount, UINT StartIndex, INT BaseVertex, UINT StartInstance) {
    D12CmdList *cl = (D12CmdList*)s;
    if (cl->ctx) {
        cl->ctx->lpVtbl->DrawIndexedInstanced(cl->ctx, IndexCount, InstanceCount, StartIndex, BaseVertex, StartInstance);
    }
}

static void WINAPI D12CmdList_Dispatch(IUnknown *s, UINT x, UINT y, UINT z) {
    char buf[256];
    snprintf(buf, sizeof(buf), "D12CmdList_Dispatch: x=%u, y=%u, z=%u\n", x, y, z);
    OutputDebugStringA(buf);
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;
    cl->ctx->lpVtbl->Dispatch(cl->ctx, x, y, z);
}

static void WINAPI D12CmdList_ResourceBarrier(IUnknown *s, UINT n, const D3D12_RESOURCE_BARRIER *barriers) {
    char buf[512];
    snprintf(buf, sizeof(buf), "D12CmdList_ResourceBarrier: n=%u\n", n);
    OutputDebugStringA(buf);
    for (UINT i = 0; i < n; i++) {
        const D3D12_RESOURCE_BARRIER *b = &barriers[i];
        /* On suppose que la plupart des barrières sont de type Transition (Type=0) */
        if (b->Type == 0) { /* D3D12_RESOURCE_BARRIER_TYPE_TRANSITION */
            snprintf(buf, sizeof(buf), "  barrier[%u]: Type=Transition, Flags=%u, pResource=%p, Subresource=%u, StateBefore=%u, StateAfter=%u\n",
                     i, b->Flags, b->Transition.pResource, b->Transition.Subresource,
                     b->Transition.StateBefore, b->Transition.StateAfter);
        } else {
            snprintf(buf, sizeof(buf), "  barrier[%u]: Type=%u (non Transition), Flags=%u\n", i, b->Type, b->Flags);
        }
        OutputDebugStringA(buf);
    }
    (void)s;
}

static void WINAPI D12CmdList_CopyResource(IUnknown *s, IUnknown *pDst, IUnknown *pSrc) {
    OutputDebugStringA("D12CmdList_CopyResource called\n");
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;
    D12Resource *dst = (D12Resource*)pDst;
    D12Resource *src = (D12Resource*)pSrc;
    char buf[256];
    snprintf(buf, sizeof(buf), "  dst id=%u, src id=%u\n", dst->id, src->id);
    OutputDebugStringA(buf);
    cl->ctx->lpVtbl->CopyResource(cl->ctx, dst->d11res, src->d11res);
}

static void WINAPI D12CmdList_CopyBufferRegion(IUnknown *s,
    IUnknown *pDst, UINT64 DstOffset, IUnknown *pSrc, UINT64 SrcOffset, UINT64 NumBytes)
{
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;
    D12Resource *dst = (D12Resource*)pDst;
    D12Resource *src = (D12Resource*)pSrc;
    char buf[256];
    snprintf(buf, sizeof(buf), "D12CmdList_CopyBufferRegion: dst id=%u (d11buf=%p), src id=%u (d11buf=%p), size=%llu\n",
             dst->id, dst->d11buf, src->id, src->d11buf, NumBytes);
    OutputDebugStringA(buf);
    D3D11_BOX srcBox = { (UINT)SrcOffset, 0, 0, (UINT)(SrcOffset + NumBytes), 1, 1 };
    cl->ctx->lpVtbl->CopySubresourceRegion(cl->ctx, dst->d11res, 0, (UINT)DstOffset, 0, 0, src->d11res, 0, &srcBox);
}

static void WINAPI D12CmdList_ClearState(IUnknown *s, IUnknown *pInitState) {
    OutputDebugStringA("D12CmdList_ClearState called\n");
    (void)s; (void)pInitState;
}
static void WINAPI D12CmdList_CopyTiles(IUnknown *s, ...) {
    OutputDebugStringA("D12CmdList_CopyTiles called\n");
    (void)s; WARN("CopyTiles non implemente");
}
static void WINAPI D12CmdList_OMSetBlendFactor(IUnknown *s, const FLOAT blendFactor[4]) {
    OutputDebugStringA("D12CmdList_OMSetBlendFactor called\n");
    (void)s; (void)blendFactor;
}
static void WINAPI D12CmdList_OMSetStencilRef(IUnknown *s, UINT stencilRef) {
    OutputDebugStringA("D12CmdList_OMSetStencilRef called\n");
    (void)s; (void)stencilRef;
}

/* ClearUnorderedAccessViewFloat/Uint : signature D3D12 reelle :
 *   (this, GPUHandle, CPUHandle, pResource, Values[4], NumRects, pRects)
 * On utilise le CPU handle (pointeur direct vers D12Descriptor). */
/* Valide qu'un CPU handle pointe vers un D12Descriptor accessible en user-space.
 * Les handles RTV/DSV qu'un jeu passe par erreur à ClearUAV ont souvent
 * CPU.ptr < 0x10000 (0x0, 0x1, ...) ou très grand → on les rejette proprement
 * SANS déréférencer, pour éviter le crash. */
static void WINAPI D12CmdList_ClearUnorderedAccessViewFloat(
    IUnknown *s,
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
    IUnknown *pResource,
    const FLOAT values[4],
    UINT NumRects, const D3D12_RECT *pRects)
{
    (void)ViewGPUHandle; (void)pResource; (void)NumRects; (void)pRects;
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;

    /* Valider le CPU handle AVANT tout déréférencement */
    if (!IsValidDescriptorPtr(ViewCPUHandle.ptr)) {
        LOG("ClearUAVFloat: CPU handle invalide (ptr=%p), skip", (void*)ViewCPUHandle.ptr);
        return;
    }
    D12Descriptor *slot = (D12Descriptor*)(uintptr_t)ViewCPUHandle.ptr;

    /* Valider le pointeur uav AVANT de l'utiliser */
    if (!IsValidDescriptorPtr((SIZE_T)(uintptr_t)slot->uav)) {
        LOG("ClearUAVFloat: slot->uav invalide (%p), skip", slot->uav);
        return;
    }
    cl->ctx->lpVtbl->ClearUnorderedAccessViewFloat(cl->ctx, slot->uav, values);
}

static void WINAPI D12CmdList_ClearUnorderedAccessViewUint(
    IUnknown *s,
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
    IUnknown *pResource,
    const UINT values[4],
    UINT NumRects, const D3D12_RECT *pRects)
{
    (void)ViewGPUHandle; (void)pResource; (void)NumRects; (void)pRects;
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx) return;

    /* Valider le CPU handle AVANT tout déréférencement */
    if (!IsValidDescriptorPtr(ViewCPUHandle.ptr)) {
        LOG("ClearUAVUint: CPU handle invalide (ptr=%p), skip", (void*)ViewCPUHandle.ptr);
        return;
    }
    D12Descriptor *slot = (D12Descriptor*)(uintptr_t)ViewCPUHandle.ptr;

    /* Valider le pointeur uav AVANT de l'utiliser */
    if (!IsValidDescriptorPtr((SIZE_T)(uintptr_t)slot->uav)) {
        LOG("ClearUAVUint: slot->uav invalide (%p), skip", slot->uav);
        return;
    }
    cl->ctx->lpVtbl->ClearUnorderedAccessViewUint(cl->ctx, slot->uav, values);
}

/* Stubs */
#define CMD_STUB(name) static void WINAPI D12CmdList_##name(IUnknown *s, ...) { \
    char buf[256]; \
    snprintf(buf, sizeof(buf), "D12CmdList_" #name " called\n"); \
    OutputDebugStringA(buf); \
    WARN(#name " non implemente"); \
}
/* SetDescriptorHeaps : mémorise les heaps actifs dans la command list.
 * Le jeu passe 1 ou 2 heaps (CBV_SRV_UAV et/ou SAMPLER).
 * On les stocke pour les utiliser dans SetGraphicsRootDescriptorTable. */
static void WINAPI D12CmdList_SetDescriptorHeaps(IUnknown *s, UINT NumHeaps, IUnknown *const *ppHeaps) {
    D12CmdList *cl = (D12CmdList*)s;
    cl->desc_heaps[0] = NULL;
    cl->desc_heaps[1] = NULL;
    for (UINT i = 0; i < NumHeaps && i < 2; i++) {
        D12DescHeap *h = (D12DescHeap*)ppHeaps[i];
        if (!h) continue;
        if (h->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
            cl->desc_heaps[0] = h;
        else if (h->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            cl->desc_heaps[1] = h;
    }
}

/* SetGraphicsRootSignature / SetComputeRootSignature : stocke la root sig active */
static void WINAPI D12CmdList_SetGraphicsRootSignature(IUnknown *s, IUnknown *pRS) {
    D12CmdList *cl = (D12CmdList*)s;
    cl->cur_root_sig = (D12RootSig*)pRS;
}
static void WINAPI D12CmdList_SetComputeRootSignature(IUnknown *s, IUnknown *pRS) {
    D12CmdList *cl = (D12CmdList*)s;
    cl->cur_root_sig = (D12RootSig*)pRS;
}

static void WINAPI D12CmdList_SetGraphicsRootConstantBufferView(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx || !cl->device || BufferLocation == 0) return;

    D12Resource *res = D12Device_FindResourceByVA(cl->device, BufferLocation);
    if (!res || !res->d11buf) return;

    /* Verification O(1) via flag mis en cache a la creation — evite le
     * crash D3D11 (E06D7363) quand le jeu passe un buffer vertex/index
     * a SetGraphicsRootConstantBufferView (buffer non-CBV). */
    if (!res->is_cbv) {
        LOG("SetGraphicsRootCBV: resource id=%u n'est pas un CBV, skip", res->id);
        return;
    }

    UINT slot = RootParamIndex;
    cl->ctx->lpVtbl->VSSetConstantBuffers(cl->ctx, slot, 1, &res->d11buf);
    cl->ctx->lpVtbl->PSSetConstantBuffers(cl->ctx, slot, 1, &res->d11buf);
}
static void WINAPI D12CmdList_SetComputeRootConstantBufferView(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx || !cl->device) return;
    D12Resource *res = D12Device_FindResourceByVA(cl->device, BufferLocation);
    if (!res || !res->d11buf) {
        LOG("SetComputeRootCBV: resource non trouvee pour VA=%p", (void*)(uintptr_t)BufferLocation);
        return;
    }
    if (!res->is_cbv) {
        LOG("SetComputeRootCBV: resource id=%u n'est pas un CBV, skip", res->id);
        return;
    }
    cl->ctx->lpVtbl->CSSetConstantBuffers(cl->ctx, RootParamIndex, 1, &res->d11buf);
}


/* SetGraphicsRootDescriptorTable : parcourt les ranges de la root sig
 * et bind les vues D3D11 correspondantes depuis le heap CBV_SRV_UAV. */
static void WINAPI D12CmdList_SetGraphicsRootDescriptorTable(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx || !cl->desc_heaps[0] || !cl->cur_root_sig || !cl->cur_root_sig->param_tables) return;

    D12DescHeap *heap = cl->desc_heaps[0];
    if (!IsValidDescriptorPtr((SIZE_T)BaseDescriptor.ptr)) return;

    D12Descriptor *base = (D12Descriptor*)(uintptr_t)BaseDescriptor.ptr;
    UINT idx = (UINT)((BYTE*)base - (BYTE*)heap->descs) / sizeof(D12Descriptor);

    if (RootParamIndex >= cl->cur_root_sig->num_params) return;

    const D12RSTableCache *table = &cl->cur_root_sig->param_tables[RootParamIndex];
    if (table->num_ranges == 0) return;

    UINT cur_idx = idx;
    for (UINT r = 0; r < table->num_ranges; r++) {
        const RS_RANGE_V1 *rng = &table->ranges[r];
        UINT n = (rng->NumDescriptors == 0xFFFFFFFF) ? 1 : rng->NumDescriptors;
        UINT base_reg = rng->BaseShaderRegister;

        for (UINT d = 0; d < n && (cur_idx + d) < heap->desc.NumDescriptors; d++) {
            D12Descriptor *slot = &heap->descs[cur_idx + d];
            UINT reg = base_reg + d;

            switch (rng->RangeType) {
            case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
                if (slot->srv) {
                    cl->ctx->lpVtbl->VSSetShaderResources(cl->ctx, reg, 1, &slot->srv);
                    cl->ctx->lpVtbl->PSSetShaderResources(cl->ctx, reg, 1, &slot->srv);
                }
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                if (slot->cbv.buf) {
                    cl->ctx->lpVtbl->VSSetConstantBuffers(cl->ctx, reg, 1, &slot->cbv.buf);
                    cl->ctx->lpVtbl->PSSetConstantBuffers(cl->ctx, reg, 1, &slot->cbv.buf);
                }
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
                if (slot->uav)
                    cl->ctx->lpVtbl->CSSetUnorderedAccessViews(cl->ctx, reg, 1, &slot->uav, NULL);
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
                if (slot->sampler) {
                    cl->ctx->lpVtbl->VSSetSamplers(cl->ctx, reg, 1, &slot->sampler);
                    cl->ctx->lpVtbl->PSSetSamplers(cl->ctx, reg, 1, &slot->sampler);
                }
                break;
            }
        }
        cur_idx += n;
    }
}

static void WINAPI D12CmdList_SetComputeRootDescriptorTable(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    D12CmdList *cl = (D12CmdList*)s;
    if (!cl->ctx || !cl->desc_heaps[0] || !cl->cur_root_sig || !cl->cur_root_sig->param_tables) return;

    D12DescHeap *heap = cl->desc_heaps[0];
    if (!IsValidDescriptorPtr((SIZE_T)BaseDescriptor.ptr)) return;

    D12Descriptor *base = (D12Descriptor*)(uintptr_t)BaseDescriptor.ptr;
    UINT idx = (UINT)((BYTE*)base - (BYTE*)heap->descs) / sizeof(D12Descriptor);

    if (RootParamIndex >= cl->cur_root_sig->num_params) return;

    const D12RSTableCache *table = &cl->cur_root_sig->param_tables[RootParamIndex];
    if (table->num_ranges == 0) return;

    UINT cur_idx = idx;
    for (UINT r = 0; r < table->num_ranges; r++) {
        const RS_RANGE_V1 *rng = &table->ranges[r];
        UINT n = (rng->NumDescriptors == 0xFFFFFFFF) ? 1 : rng->NumDescriptors;
        UINT base_reg = rng->BaseShaderRegister;

        for (UINT d = 0; d < n && (cur_idx + d) < heap->desc.NumDescriptors; d++) {
            D12Descriptor *slot = &heap->descs[cur_idx + d];
            UINT reg = base_reg + d;

            switch (rng->RangeType) {
            case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
                if (slot->srv)
                    cl->ctx->lpVtbl->CSSetShaderResources(cl->ctx, reg, 1, &slot->srv);
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                if (slot->cbv.buf)
                    cl->ctx->lpVtbl->CSSetConstantBuffers(cl->ctx, reg, 1, &slot->cbv.buf);
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
                if (slot->uav)
                    cl->ctx->lpVtbl->CSSetUnorderedAccessViews(cl->ctx, reg, 1, &slot->uav, NULL);
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
                if (slot->sampler)
                    cl->ctx->lpVtbl->CSSetSamplers(cl->ctx, reg, 1, &slot->sampler);
                break;
            }
        }
        cur_idx += n;
    }
}

/* SetComputeRoot32BitConstant / SetGraphicsRoot32BitConstant (SINGULIER):
 * ecrit UNE seule valeur 32 bits a un offset donne. Methodes distinctes de
 * la version plurielle (...Constants) dans la vraie vtable D3D12 - il ne
 * faut PAS les fusionner ni les omettre, sous peine de decaler tout le
 * reste de la vtable (OMSetRenderTargets et suivants) d'un cran. */
static void WINAPI D12CmdList_SetComputeRoot32BitConstant(
    IUnknown *s, UINT RootParamIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    (void)s; (void)RootParamIndex; (void)SrcData; (void)DestOffsetIn32BitValues;
}
static void WINAPI D12CmdList_SetGraphicsRoot32BitConstant(
    IUnknown *s, UINT RootParamIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    (void)s; (void)RootParamIndex; (void)SrcData; (void)DestOffsetIn32BitValues;
}

/* SetGraphicsRoot32BitConstants : écrit des données dans un buffer CBV mappé */
static void WINAPI D12CmdList_SetGraphicsRoot32BitConstants(
    IUnknown *s, UINT RootParamIndex, UINT Num32BitValues,
    const void *pSrcData, UINT DestOffsetIn32BitValues)
{
    /* Pour l'instant : stub silencieux.
     * Implémenter proprement nécessite de connaître le buffer CBV associé au param.
     * TODO: stocker un buffer D3D11_USAGE_DYNAMIC par root param constants. */
    (void)s; (void)RootParamIndex; (void)Num32BitValues;
    (void)pSrcData; (void)DestOffsetIn32BitValues;
}
static void WINAPI D12CmdList_SetComputeRoot32BitConstants(
    IUnknown *s, UINT RootParamIndex, UINT Num32BitValues,
    const void *pSrcData, UINT DestOffsetIn32BitValues)
{
    (void)s; (void)RootParamIndex; (void)Num32BitValues;
    (void)pSrcData; (void)DestOffsetIn32BitValues;
}

/* SetGraphicsRootShaderResourceView / UnorderedAccessView : bind direct GPU VA */
static void WINAPI D12CmdList_SetGraphicsRootShaderResourceView(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    (void)s; (void)RootParamIndex; (void)BufferLocation;
    /* TODO: créer une SRV D3D11 pour le buffer et la binder */
}
static void WINAPI D12CmdList_SetComputeRootShaderResourceView(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    (void)s; (void)RootParamIndex; (void)BufferLocation;
}
static void WINAPI D12CmdList_SetGraphicsRootUnorderedAccessView(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    (void)s; (void)RootParamIndex; (void)BufferLocation;
}
static void WINAPI D12CmdList_SetComputeRootUnorderedAccessView(
    IUnknown *s, UINT RootParamIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    (void)s; (void)RootParamIndex; (void)BufferLocation;
}

CMD_STUB(ExecuteBundle)
CMD_STUB(SetMarker)
CMD_STUB(BeginEvent)
CMD_STUB(EndEvent)
CMD_STUB(CopyTextureRegion)
CMD_STUB(ResolveQueryData)
CMD_STUB(BeginQuery)
CMD_STUB(EndQuery)
CMD_STUB(ResolveSubresource)
CMD_STUB(ExecuteIndirect)
CMD_STUB(DiscardResource)
CMD_STUB(SetPredication)

/* ============================================================
 * SECTION 12b : ID3D12CommandQueueDownlevel
 * ============================================================ */

DEFINE_GUID(IID_ID3D12CommandQueueDownlevel,
    0x38a8c5ef,0x7ccb,0x4e81,0x91,0x4f,0xa6,0xe9,0xd0,0x72,0xc4,0x94);

typedef struct D12CQDownlevel {
    struct {
        HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
        ULONG   (WINAPI *AddRef)(IUnknown*);
        ULONG   (WINAPI *Release)(IUnknown*);
        HRESULT (WINAPI *Present)(IUnknown*, IUnknown*, IUnknown*, HWND, UINT);
    } *lpVtbl;
    LONG         ref;
    D12CmdQueue *queue;
} D12CQDownlevel;

static HRESULT WINAPI D12CQDl_QI(IUnknown *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown_local) ||
        IsEqualGUID(riid, &IID_ID3D12CommandQueueDownlevel))
    {
        InterlockedIncrement(&((D12CQDownlevel*)self)->ref);
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI D12CQDl_AddRef(IUnknown *self) {
    return InterlockedIncrement(&((D12CQDownlevel*)self)->ref);
}
static ULONG WINAPI D12CQDl_Release(IUnknown *self) {
    D12CQDownlevel *d = (D12CQDownlevel*)self;
    ULONG r = InterlockedDecrement(&d->ref);
    if (!r) d12_free(d);
    return r;
}

static HRESULT WINAPI D12CQDl_Present(IUnknown *self, IUnknown *pCmdList, IUnknown *pResource, HWND hwnd, UINT flags) {
    D12CQDownlevel *d = (D12CQDownlevel*)self;
    D12CmdQueue *q = d->queue;
    (void)flags;

    /* === Exécution de la command list (si présente) === */
    if (pCmdList) {
        D12CmdList *cl = (D12CmdList*)pCmdList;
        if (!cl->closed && cl->ctx) {
            cl->ctx->lpVtbl->FinishCommandList(cl->ctx, FALSE, &cl->cmd_list);
            cl->closed = true;
        }
        if (cl->cmd_list && q->imm_ctx) {
            q->imm_ctx->lpVtbl->ExecuteCommandList(q->imm_ctx, cl->cmd_list, FALSE);
            SAFE_RELEASE(cl->cmd_list);
            cl->closed = false;
        }
    }

    /* === Présentation === */
    D12Resource *res = (D12Resource*)pResource;
    if (!res || !res->d11tex) {
        if (q->imm_ctx) q->imm_ctx->lpVtbl->Flush(q->imm_ctx);
        return S_OK;
    }

    /* Création de la swapchain une seule fois */
    if (!q->present_sc && hwnd && q->device && q->device->dxgi_dev) {
        IDXGIAdapter *adapter = NULL;
        IDXGIFactory *factory = NULL;

        q->device->dxgi_dev->lpVtbl->GetAdapter(q->device->dxgi_dev, &adapter);
        if (adapter) {
            adapter->lpVtbl->GetParent(adapter, &IID_IDXGIFactory, (void**)&factory);
            adapter->lpVtbl->Release(adapter);
        }

        if (factory) {
            DXGI_SWAP_CHAIN_DESC sc_desc = {0};
            sc_desc.BufferDesc.Width            = (UINT)res->res_desc.Width;
            sc_desc.BufferDesc.Height           = res->res_desc.Height;
            sc_desc.BufferDesc.Format           = res->res_desc.Format;
            sc_desc.BufferDesc.RefreshRate.Numerator   = 60;
            sc_desc.BufferDesc.RefreshRate.Denominator = 1;
            sc_desc.SampleDesc.Count            = 1;
            sc_desc.BufferUsage                 = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc_desc.BufferCount                 = 2;           // double buffering
            sc_desc.OutputWindow                = hwnd;
            sc_desc.Windowed                    = TRUE;
            sc_desc.SwapEffect                  = DXGI_SWAP_EFFECT_DISCARD;

            HRESULT hr = factory->lpVtbl->CreateSwapChain(
                factory, (IUnknown*)q->device->d11dev, &sc_desc, &q->present_sc);

            factory->lpVtbl->Release(factory);

            if (SUCCEEDED(hr)) {
                q->present_hwnd = hwnd;
                q->present_w    = (UINT)res->res_desc.Width;
                q->present_h    = res->res_desc.Height;
                LOG("DLPresent: swap chain créée (%ux%u)", q->present_w, q->present_h);
            } else {
                WARN("DLPresent: CreateSwapChain échoué : %08lx", hr);
            }
        }
    }

    /* Copie vers backbuffer + Present sans VSync */
    if (q->present_sc) {
        ID3D11Texture2D *backbuf = NULL;
        HRESULT hr = q->present_sc->lpVtbl->GetBuffer(
            q->present_sc, 0, &IID_ID3D11Texture2D, (void**)&backbuf);

        if (SUCCEEDED(hr) && backbuf) {
            q->imm_ctx->lpVtbl->CopyResource(
                q->imm_ctx,
                (ID3D11Resource*)backbuf,
                (ID3D11Resource*)res->d11tex);
            backbuf->lpVtbl->Release(backbuf);
        }

        q->present_sc->lpVtbl->Present(q->present_sc, 0, 0);  // SyncInterval = 0 → VSync OFF
    } 
    else if (q->imm_ctx) {
        q->imm_ctx->lpVtbl->Flush(q->imm_ctx);
    }

    return S_OK;
}

static const struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    HRESULT (WINAPI *Present)(IUnknown*, IUnknown*, IUnknown*, HWND, UINT);
} g_CQDlVtbl = { D12CQDl_QI, D12CQDl_AddRef, D12CQDl_Release, D12CQDl_Present };

static HRESULT D12CmdQueue_CreateDownlevel(D12CmdQueue *q, void **ppv) {
    D12CQDownlevel *d = d12_alloc(sizeof(D12CQDownlevel));
    if (!d) return E_OUTOFMEMORY;
    d->lpVtbl = (void*)&g_CQDlVtbl;
    d->ref    = 1;
    d->queue  = q;
    *ppv = d;
    return S_OK;
}


/* ============================================================
 * SECTION 13 : ID3D12CommandQueue
 * ============================================================ */

/* Forward declarations for functions used in g_CQVtbl */
static HRESULT WINAPI D12CmdQueue_ExecuteCommandLists(IUnknown *s, UINT n, IUnknown *const *ppCL);
static HRESULT WINAPI D12CmdQueue_Signal(IUnknown *s, IUnknown *pFence, UINT64 val);
static HRESULT WINAPI D12CmdQueue_Wait(IUnknown *s, IUnknown *pFence, UINT64 val);
static HRESULT WINAPI D12CmdQueue_GetTimestampFrequency(IUnknown *s, UINT64 *pFreq);
static HRESULT WINAPI D12CmdQueue_GetClockCalibration(IUnknown *s, UINT64 *pGPU, UINT64 *pCPU);
static void WINAPI D12CmdQueue_GetDesc(IUnknown *s, D3D12_COMMAND_QUEUE_DESC *pOut);

static void D12CmdQueue_Destroy(D12CmdQueue *q) {
    PDStore_Destroy(&q->pd);
    SAFE_RELEASE(q->dxgi_dev);
    SAFE_RELEASE(q->present_sc);
    d12_free(q);
}

static HRESULT WINAPI D12CmdQueue_GetDevice(IUnknown *s, REFIID riid, void **ppv) {
    D12CmdQueue *q = (D12CmdQueue*)s;
    if (!ppv) return E_POINTER;
    if (!q->device) { *ppv = NULL; return E_FAIL; }
    return ((IUnknown*)(void*)q->device)->lpVtbl->QueryInterface(
        (IUnknown*)(void*)q->device, riid, ppv);
}

IMPL_D3D12OBJECT(D12CmdQueue)
COM_ADDREF_RELEASE(D12CmdQueue, D12CmdQueue_Destroy)

static HRESULT WINAPI D12CmdQueue_QI(IUnknown *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    D12CmdQueue *q = (D12CmdQueue*)self;
    if (IsEqualGUID(riid, &IID_IUnknown_local) ||
        IsEqualGUID(riid, &IID_ID3D12CommandQueue))
    {
        InterlockedIncrement(&q->ref);
        *ppv = self;
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_ID3D12CommandQueueDownlevel)) {
        return D12CmdQueue_CreateDownlevel(q, ppv);
    }
    if (IsEqualGUID(riid, &IID_IDXGIDevice) || IsEqualGUID(riid, &IID_IDXGIObject)) {
        if (q->dxgi_dev) {
            q->dxgi_dev->lpVtbl->AddRef(q->dxgi_dev);
            *ppv = q->dxgi_dev;
            return S_OK;
        }
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

/* GetDesc - structure > 8 bytes => hidden pointer, 'this' en premier */
static void WINAPI D12CmdQueue_GetDesc(IUnknown *s, D3D12_COMMAND_QUEUE_DESC *pOut) {
    D12CmdQueue *q = (D12CmdQueue*)s;
    *pOut = q->queue_desc;
    LOG("D12CmdQueue_GetDesc: type=%u, priority=%d, flags=%u",
        q->queue_desc.Type, q->queue_desc.Priority, q->queue_desc.Flags);
}

/* Vtable for D12CmdQueue */
typedef struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
    HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
    HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
    HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
    HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
    HRESULT (WINAPI *UpdateTileMappings)();
    HRESULT (WINAPI *CopyTileMappings)();
    HRESULT (WINAPI *ExecuteCommandLists)(IUnknown*, UINT, IUnknown *const*);
    HRESULT (WINAPI *SetMarker)();
    HRESULT (WINAPI *BeginEvent)();
    HRESULT (WINAPI *EndEvent)();
    HRESULT (WINAPI *Signal)(IUnknown*, IUnknown*, UINT64);
    HRESULT (WINAPI *Wait)(IUnknown*, IUnknown*, UINT64);
    HRESULT (WINAPI *GetTimestampFrequency)(IUnknown*, UINT64*);
    HRESULT (WINAPI *GetClockCalibration)(IUnknown*, UINT64*, UINT64*);
    void    (WINAPI *GetDesc)(IUnknown*, D3D12_COMMAND_QUEUE_DESC*);
} CQVtbl;

static const CQVtbl g_CQVtbl = {
    D12CmdQueue_QI,
    (ULONG(WINAPI*)(IUnknown*))D12CmdQueue_AddRef,
    (ULONG(WINAPI*)(IUnknown*))D12CmdQueue_Release,
    D12CmdQueue_GetPrivateData, D12CmdQueue_SetPrivateData,
    D12CmdQueue_SetPrivateDataInterface, D12CmdQueue_SetName,
    D12CmdQueue_GetDevice,
    NULL, NULL,
    D12CmdQueue_ExecuteCommandLists,
    NULL, NULL, NULL,
    D12CmdQueue_Signal, D12CmdQueue_Wait,
    D12CmdQueue_GetTimestampFrequency, D12CmdQueue_GetClockCalibration,
    D12CmdQueue_GetDesc
};

static HRESULT WINAPI D12CmdQueue_ExecuteCommandLists(IUnknown *s, UINT n, IUnknown *const *ppCL) {
    D12CmdQueue *q = (D12CmdQueue*)s;
    for (UINT i = 0; i < n; i++) {
        D12CmdList *cl = (D12CmdList*)ppCL[i];
        if (!cl) continue;
        if (!cl->closed) D12CmdList_Close((IUnknown*)cl);
        if (cl->cmd_list && q->imm_ctx) {
            q->imm_ctx->lpVtbl->ExecuteCommandList(q->imm_ctx, cl->cmd_list, FALSE);
            SAFE_RELEASE(cl->cmd_list);
        }
    }
    return S_OK;
}

static HRESULT WINAPI D12CmdQueue_Signal(IUnknown *s, IUnknown *pFence, UINT64 val) {
    D12CmdQueue *q = (D12CmdQueue*)s;
    D12Fence *f    = (D12Fence*)pFence;
    (void)q;
    f->completed = val;
    if (f->event) { SetEvent(f->event); f->event = NULL; }
    return S_OK;
}

static HRESULT WINAPI D12CmdQueue_Wait(IUnknown *s, IUnknown *pFence, UINT64 val) {
    (void)s;
    D12Fence *f = (D12Fence*)pFence;
    while (f->completed < val) Sleep(0);
    return S_OK;
}

static HRESULT WINAPI D12CmdQueue_GetTimestampFrequency(IUnknown *s, UINT64 *pFreq)
    { (void)s; *pFreq = 1000000; return S_OK; }

static HRESULT WINAPI D12CmdQueue_GetClockCalibration(IUnknown *s, UINT64 *pGPU, UINT64 *pCPU)
    { (void)s; QueryPerformanceCounter((LARGE_INTEGER*)pCPU); *pGPU = *pCPU; return S_OK; }


/* ============================================================
 * SECTION 14 : ID3D12Device - implémentation
 * ============================================================ */

static void D12Device_Destroy(D12Device *dev) {
    if (!dev) return;

    LOG("D12Device_Destroy called - nettoyage complet");

    /* Nettoyage des ressources chainées */
    D12Resource *r = dev->res_list_head;
    while (r) {
        D12Resource *next = r->res_list_next;
        D12Resource_Destroy(r);
        r = next;
    }
    dev->res_list_head = NULL;

    /* Nettoyage des heaps */
    if (dev->lastRTVHeap) {
        ((IUnknown*)dev->lastRTVHeap)->lpVtbl->Release((IUnknown*)dev->lastRTVHeap);
        dev->lastRTVHeap = NULL;
    }
    if (dev->lastDSVHeap) {
        ((IUnknown*)dev->lastDSVHeap)->lpVtbl->Release((IUnknown*)dev->lastDSVHeap);
        dev->lastDSVHeap = NULL;
    }

    PDStore_Destroy(&dev->pd);

    SAFE_RELEASE(dev->dxgi_dev);
    SAFE_RELEASE(dev->d11ctx);
    SAFE_RELEASE(dev->d11dev);

    d12_free(dev);
}
IMPL_D3D12OBJECT(D12Device)
COM_ADDREF_RELEASE(D12Device, D12Device_Destroy)

/* Définition manuelle de D12Device_QI pour ajouter du logging */
static HRESULT WINAPI D12Device_QI(IUnknown *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "D12Device_QI: riid=%p (Data1=%08lX)\n", riid, (unsigned long)riid->Data1);
        OutputDebugStringA(buf);
    }
    D12Device *dev = (D12Device*)self;
    if (IsEqualGUID(riid, &IID_IUnknown_local) ||
        IsEqualGUID(riid, &IID_ID3D12Device) ||
        IsEqualGUID(riid, &IID_ID3D12Device1) ||
        IsEqualGUID(riid, &IID_ID3D12Device2))
    {
        InterlockedIncrement(&dev->ref);
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static UINT WINAPI D12Dev_GetNodeCount(IUnknown *s) { (void)s; return 1; }

/* CreateCommandQueue */
static HRESULT WINAPI D12Dev_CreateCommandQueue(IUnknown *s,
    const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **ppv)
{
    D12Device   *dev = (D12Device*)s;
    D12CmdQueue *q   = d12_alloc(sizeof(D12CmdQueue));
    if (!q) return E_OUTOFMEMORY;

    q->lpVtbl     = (void*)&g_CQVtbl;
    q->ref        = 1;
    q->device     = dev;
    q->queue_desc = *desc;
    q->imm_ctx    = dev->d11ctx;
    PDStore_Init(&q->pd);
    ID3D11Device_QueryInterface(dev->d11dev, &IID_IDXGIDevice, (void**)&q->dxgi_dev);

    return D12CmdQueue_QI((IUnknown*)q, riid, ppv);
}

/* CreateCommandAllocator */
static HRESULT WINAPI D12Dev_CreateCommandAllocator(IUnknown *s,
    D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **ppv)
{
    D12Device       *dev = (D12Device*)s;
    D12CmdAllocator *a   = d12_alloc(sizeof(D12CmdAllocator));
    if (!a) return E_OUTOFMEMORY;

    typedef struct {
        HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
        ULONG   (WINAPI *AddRef)(IUnknown*);
        ULONG   (WINAPI *Release)(IUnknown*);
        HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
        HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
        HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
        HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
        HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
        HRESULT (WINAPI *Reset)(IUnknown*);
    } CAVtbl;
    static const CAVtbl g_CAVtbl = {
        (HRESULT(WINAPI*)(IUnknown*,REFIID,void**))D12CmdAllocator_QI,
        (ULONG(WINAPI*)(IUnknown*))D12CmdAllocator_AddRef,
        (ULONG(WINAPI*)(IUnknown*))D12CmdAllocator_Release,
        D12CmdAllocator_GetPrivateData, D12CmdAllocator_SetPrivateData,
        D12CmdAllocator_SetPrivateDataInterface, D12CmdAllocator_SetName,
        D12CmdAllocator_GetDevice,
        D12CmdAlloc_Reset
    };

    a->lpVtbl = (void*)&g_CAVtbl;
    a->ref    = 1;
    a->type   = type;
    a->device = dev;
    PDStore_Init(&a->pd);
    if (type != D3D12_COMMAND_LIST_TYPE_COPY) {
        HRESULT hr = dev->d11dev->lpVtbl->CreateDeferredContext(dev->d11dev, 0, &a->deferred_ctx);
        if (FAILED(hr)) { WARN("CreateDeferredContext: %08lx", hr); d12_free(a); return hr; }
    }
    return D12CmdAllocator_QI((IUnknown*)a, riid, ppv);
}

/* CreateCommandList */
static HRESULT WINAPI D12Dev_CreateCommandList(IUnknown *s, UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type, IUnknown *pAlloc, IUnknown *pInitPSO,
    REFIID riid, void **ppv)
{
    OutputDebugStringA("D12Dev_CreateCommandList called\n");
    char buf[256];
    snprintf(buf, sizeof(buf), "  type=%u, nodeMask=%u\n", type, nodeMask);
    OutputDebugStringA(buf);
	(void)nodeMask; (void)pInitPSO;
    D12Device       *dev   = (D12Device*)s;
    D12CmdAllocator *alloc = (D12CmdAllocator*)pAlloc;

    D12CmdList *cl = d12_alloc(sizeof(D12CmdList));
    if (!cl) return E_OUTOFMEMORY;

    typedef struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
    HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
    HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
    HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
    HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
    D3D12_COMMAND_LIST_TYPE (WINAPI *GetType)(IUnknown*);
    HRESULT (WINAPI *Close)(IUnknown*);
    HRESULT (WINAPI *Reset)(IUnknown*, IUnknown*, IUnknown*);
    void (WINAPI *ClearState)(IUnknown*, IUnknown*);
    void (WINAPI *DrawInstanced)(IUnknown*, UINT, UINT, UINT, UINT);
    void (WINAPI *DrawIndexedInstanced)(IUnknown*, UINT, UINT, UINT, INT, UINT);
    void (WINAPI *Dispatch)(IUnknown*, UINT, UINT, UINT);
    void (WINAPI *CopyBufferRegion)(IUnknown*, IUnknown*, UINT64, IUnknown*, UINT64, UINT64);
    void (WINAPI *CopyTextureRegion)(IUnknown*, ...);
    void (WINAPI *CopyResource)(IUnknown*, IUnknown*, IUnknown*);
    void (WINAPI *CopyTiles)(IUnknown*, ...);
    void (WINAPI *ResolveSubresource)(IUnknown*, ...);
    void (WINAPI *IASetPrimitiveTopology)(IUnknown*, D3D11_PRIMITIVE_TOPOLOGY);
    void (WINAPI *RSSetViewports)(IUnknown*, UINT, const D3D12_VIEWPORT*);
    void (WINAPI *RSSetScissorRects)(IUnknown*, UINT, const D3D12_RECT*);
    void (WINAPI *OMSetBlendFactor)(IUnknown*, const FLOAT[4]);
    void (WINAPI *OMSetStencilRef)(IUnknown*, UINT);
    void (WINAPI *SetPipelineState)(IUnknown*, IUnknown*);
    void (WINAPI *ResourceBarrier)(IUnknown*, UINT, const D3D12_RESOURCE_BARRIER*);
    void (WINAPI *ExecuteBundle)(IUnknown*, ...);
    void (WINAPI *SetDescriptorHeaps)(IUnknown*, UINT, IUnknown *const*);
    void (WINAPI *SetComputeRootSignature)(IUnknown*, IUnknown*);
    void (WINAPI *SetGraphicsRootSignature)(IUnknown*, IUnknown*);
    void (WINAPI *SetComputeRootDescriptorTable)(IUnknown*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    void (WINAPI *SetGraphicsRootDescriptorTable)(IUnknown*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    void (WINAPI *SetComputeRoot32BitConstant)(IUnknown*, UINT, UINT, UINT);
    void (WINAPI *SetGraphicsRoot32BitConstant)(IUnknown*, UINT, UINT, UINT);
    void (WINAPI *SetComputeRoot32BitConstants)(IUnknown*, UINT, UINT, const void*, UINT);
    void (WINAPI *SetGraphicsRoot32BitConstants)(IUnknown*, UINT, UINT, const void*, UINT);
    void (WINAPI *SetComputeRootConstantBufferView)(IUnknown*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (WINAPI *SetGraphicsRootConstantBufferView)(IUnknown*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (WINAPI *SetComputeRootShaderResourceView)(IUnknown*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (WINAPI *SetGraphicsRootShaderResourceView)(IUnknown*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (WINAPI *SetComputeRootUnorderedAccessView)(IUnknown*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (WINAPI *SetGraphicsRootUnorderedAccessView)(IUnknown*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (WINAPI *IASetIndexBuffer)(IUnknown*, const D3D12_INDEX_BUFFER_VIEW*);
    void (WINAPI *IASetVertexBuffers)(IUnknown*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
    void (WINAPI *SOSetTargets)(IUnknown*, UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*);
    void (WINAPI *OMSetRenderTargets)(IUnknown*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    void (WINAPI *ClearDepthStencilView)(IUnknown*, D3D12_CPU_DESCRIPTOR_HANDLE, UINT, FLOAT, UINT8, UINT, const D3D12_RECT*);
    void (WINAPI *ClearRenderTargetView)(IUnknown*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
    void (WINAPI *ClearUnorderedAccessViewUint)(IUnknown*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, IUnknown*, const UINT[4], UINT, const D3D12_RECT*);
    void (WINAPI *ClearUnorderedAccessViewFloat)(IUnknown*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, IUnknown*, const FLOAT[4], UINT, const D3D12_RECT*);
    void (WINAPI *DiscardResource)(IUnknown*, ...);
    void (WINAPI *BeginQuery)(IUnknown*, ...);
    void (WINAPI *EndQuery)(IUnknown*, ...);
    void (WINAPI *ResolveQueryData)(IUnknown*, ...);
    void (WINAPI *SetPredication)(IUnknown*, ...);
    void (WINAPI *SetMarker)(IUnknown*, ...);
    void (WINAPI *BeginEvent)(IUnknown*, ...);
    void (WINAPI *EndEvent)(IUnknown*, ...);
    void (WINAPI *ExecuteIndirect)(IUnknown*, ...);
} CLVtbl;


static const CLVtbl g_CLVtbl = {
    /* 0-2 */ (HRESULT(WINAPI*)(IUnknown*,REFIID,void**))D12CmdList_QI,
    (ULONG(WINAPI*)(IUnknown*))D12CmdList_AddRef,
    (ULONG(WINAPI*)(IUnknown*))D12CmdList_Release,
    D12CmdList_GetPrivateData, D12CmdList_SetPrivateData,
    D12CmdList_SetPrivateDataInterface, D12CmdList_SetName,
    D12CmdList_GetDevice,
    D12CmdList_GetType,
    D12CmdList_Close,
    D12CmdList_Reset,
    D12CmdList_ClearState,
    D12CmdList_DrawInstanced,
    D12CmdList_DrawIndexedInstanced,
    D12CmdList_Dispatch,
    D12CmdList_CopyBufferRegion,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_CopyTextureRegion,
    D12CmdList_CopyResource,
    D12CmdList_CopyTiles,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_ResolveSubresource,
    D12CmdList_IASetPrimitiveTopology,
    D12CmdList_RSSetViewports,
    D12CmdList_RSSetScissorRects,
    D12CmdList_OMSetBlendFactor,
    D12CmdList_OMSetStencilRef,
    D12CmdList_SetPipelineState,
    D12CmdList_ResourceBarrier,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_ExecuteBundle,
    D12CmdList_SetDescriptorHeaps,
    D12CmdList_SetComputeRootSignature,
    D12CmdList_SetGraphicsRootSignature,
    D12CmdList_SetComputeRootDescriptorTable,
    D12CmdList_SetGraphicsRootDescriptorTable,
    D12CmdList_SetComputeRoot32BitConstant,
    D12CmdList_SetGraphicsRoot32BitConstant,
    D12CmdList_SetComputeRoot32BitConstants,
    D12CmdList_SetGraphicsRoot32BitConstants,
    D12CmdList_SetComputeRootConstantBufferView,
    D12CmdList_SetGraphicsRootConstantBufferView,
    D12CmdList_SetComputeRootShaderResourceView,
    D12CmdList_SetGraphicsRootShaderResourceView,
    D12CmdList_SetComputeRootUnorderedAccessView,
    D12CmdList_SetGraphicsRootUnorderedAccessView,
    D12CmdList_IASetIndexBuffer,
    D12CmdList_IASetVertexBuffers,
    D12CmdList_SOSetTargets,               // <--- maintenant correct
    D12CmdList_OMSetRenderTargets,
    D12CmdList_ClearDepthStencilView,
    D12CmdList_ClearRenderTargetView,
    D12CmdList_ClearUnorderedAccessViewUint,
    D12CmdList_ClearUnorderedAccessViewFloat,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_DiscardResource,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_BeginQuery,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_EndQuery,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_ResolveQueryData,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_SetPredication,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_SetMarker,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_BeginEvent,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_EndEvent,
    (void(WINAPI*)(IUnknown*,...))D12CmdList_ExecuteIndirect,
};

    cl->lpVtbl   = (void*)&g_CLVtbl;
    cl->ref      = 1;
    cl->type     = type;
    cl->closed   = false;
    cl->allocator = alloc;
    cl->device    = dev;
    cl->ctx       = alloc ? alloc->deferred_ctx : dev->d11ctx;
    if (cl->ctx) cl->ctx->lpVtbl->AddRef(cl->ctx);
    PDStore_Init(&cl->pd);

    return D12CmdList_QI((IUnknown*)cl, riid, ppv);
}

static const struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
    HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
    HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
    HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
    HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
} g_RSVtbl = {
    (HRESULT(WINAPI*)(IUnknown*,REFIID,void**))D12RootSig_QI,
    (ULONG(WINAPI*)(IUnknown*))D12RootSig_AddRef,
    (ULONG(WINAPI*)(IUnknown*))D12RootSig_Release,
    D12RootSig_GetPrivateData,
    D12RootSig_SetPrivateData,
    D12RootSig_SetPrivateDataInterface,
    D12RootSig_SetName,
    D12RootSig_GetDevice,
};

static HRESULT WINAPI D12Dev_CreateDescriptorHeap(IUnknown *s,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **ppv)
{
    OutputDebugStringA("D12Dev_CreateDescriptorHeap called\n");
    LOG("D12Dev_CreateDescriptorHeap: type=%u, num=%u, flags=%u",
        desc->Type, desc->NumDescriptors, desc->Flags);
    D12Device   *dev = (D12Device*)s;
    D12DescHeap *h   = d12_alloc(sizeof(D12DescHeap));
    if (!h) return E_OUTOFMEMORY;

    h->lpVtbl = (void*)&g_DHVtbl;
    h->ref    = 1;
    h->desc   = *desc;
    h->increment = dev->desc_inc[desc->Type];
    PDStore_Init(&h->pd);

    h->descs = d12_alloc(desc->NumDescriptors * sizeof(D12Descriptor));
    if (!h->descs) { d12_free(h); return E_OUTOFMEMORY; }

    h->cpu_base.ptr = (SIZE_T)h->descs;
    h->gpu_base.ptr = (UINT64)(uintptr_t)h->descs;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "D12Dev_CreateDescriptorHeap: cpu_base.ptr=%p, gpu_base.ptr=%p\n",
            (void*)h->cpu_base.ptr, (void*)h->gpu_base.ptr);
        OutputDebugStringA(buf);
    }

    // Stocker le heap RTV
    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) {
        if (dev->lastRTVHeap) {
            ((IUnknown*)dev->lastRTVHeap)->lpVtbl->Release((IUnknown*)dev->lastRTVHeap);
        }
        ((IUnknown*)h)->lpVtbl->AddRef((IUnknown*)h);
        dev->lastRTVHeap = h;
        OutputDebugStringA("Stored lastRTVHeap\n");
    }

    // Stocker le heap DSV
    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
        if (dev->lastDSVHeap) {
            ((IUnknown*)dev->lastDSVHeap)->lpVtbl->Release((IUnknown*)dev->lastDSVHeap);
        }
        ((IUnknown*)h)->lpVtbl->AddRef((IUnknown*)h);
        dev->lastDSVHeap = h;
        OutputDebugStringA("Stored lastDSVHeap\n");
    }

    return D12DescHeap_QI((IUnknown*)h, riid, ppv);
}

static UINT WINAPI D12Dev_GetDescriptorHandleIncrementSize(IUnknown *s, D3D12_DESCRIPTOR_HEAP_TYPE t)
    { return ((D12Device*)s)->desc_inc[t]; }

/* CreateCommittedResource */
static HRESULT WINAPI D12Dev_CreateCommittedResource(IUnknown *s,
    const D3D12_HEAP_PROPERTIES *heapProps,
    UINT heapFlags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initState,
    const void *pClearValue,
    REFIID riid, void **ppv)
{
    D12Device *dev = (D12Device*)s;
    D12Resource *res = d12_alloc(sizeof(D12Resource));
    if (!res) return E_OUTOFMEMORY;
    (void)heapFlags; (void)pClearValue;

    // Attribuer un ID unique
    res->id = (UINT)InterlockedIncrement(&g_ResourceIDCounter);

    char logbuf[512];
    snprintf(logbuf, sizeof(logbuf), "D12Dev_CreateCommittedResource: id=%u, dim=%u, width=%llu, height=%u, fmt=%u, heapType=%d\n",
             res->id, desc->Dimension, desc->Width, desc->Height, desc->Format, heapProps->Type);
    OutputDebugStringA(logbuf);

    res->lpVtbl = (void*)&g_ResVtbl;
    res->ref = 1;
    res->res_desc = *desc;
    res->heap_props = *heapProps;
    res->state = initState;
    res->gpu_va = dev->next_gpu_va;
    res->device = dev;
    /* Chainer dans la liste pour FindResourceByVA */
    res->res_list_next  = dev->res_list_head;
    dev->res_list_head  = res;
    PDStore_Init(&res->pd);

    HRESULT hr = S_OK;

    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = (UINT)desc->Width;
        bd.StructureByteStride = 0;

        if (heapProps->Type == D3D12_HEAP_TYPE_UPLOAD) {
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (bd.ByteWidth <= 65536 && (bd.ByteWidth % 256 == 0)) {
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                OutputDebugStringA("  Buffer UPLOAD traité comme constant buffer\n");
            } else {
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
                OutputDebugStringA("  Buffer UPLOAD traité comme vertex/index buffer\n");
            }
        } else if (heapProps->Type == D3D12_HEAP_TYPE_READBACK) {
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bd.BindFlags = 0;
        } else { // DEFAULT
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE;
            if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                bd.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        }

        hr = dev->d11dev->lpVtbl->CreateBuffer(dev->d11dev, &bd, NULL, &res->d11buf);
        res->d11res = (ID3D11Resource*)res->d11buf;
        res->is_cbv = (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? TRUE : FALSE;
        res->d11_bind_flags = bd.BindFlags;
        if (SUCCEEDED(hr)) {
            snprintf(logbuf, sizeof(logbuf), "  CreateBuffer OK, d11buf=%p\n", res->d11buf);
            OutputDebugStringA(logbuf);
            if (heapProps->Type == D3D12_HEAP_TYPE_UPLOAD) {
                D3D11_MAPPED_SUBRESOURCE mapped = {0};
                HRESULT hrMap = dev->d11ctx->lpVtbl->Map(dev->d11ctx, res->d11res, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (FAILED(hrMap)) {
                    WARN("Map D3D11 échoué: %08lx", hrMap);
                    res->mapped_ptr = NULL;
                } else {
                    res->mapped_ptr = mapped.pData;
                    snprintf(logbuf, sizeof(logbuf), "  Map réussi, mapped.pData=%p\n", mapped.pData);
                    OutputDebugStringA(logbuf);
                }
            }
        } else {
            WARN("CreateBuffer échoué: %08lx", hr);
        }
        dev->next_gpu_va += desc->Width;

    } else if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = (UINT)desc->Width;
        td.Height = desc->Height;
        td.MipLevels = desc->MipLevels ? desc->MipLevels : 1;
        td.ArraySize = desc->DepthOrArraySize ? desc->DepthOrArraySize : 1;
        td.Format = desc->Format;
        td.SampleDesc = desc->SampleDesc;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
            td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
            td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

        hr = dev->d11dev->lpVtbl->CreateTexture2D(dev->d11dev, &td, NULL, &res->d11tex);
        res->d11res = (ID3D11Resource*)res->d11tex;
        if (SUCCEEDED(hr)) {
            snprintf(logbuf, sizeof(logbuf), "  CreateTexture2D OK, d11tex=%p\n", res->d11tex);
            OutputDebugStringA(logbuf);
        }
        dev->next_gpu_va += (UINT64)desc->Width * desc->Height * 4;
    } else {
        WARN("CreateCommittedResource: dimension %d non supportee", desc->Dimension);
        d12_free(res);
        return E_NOTIMPL;
    }

    if (FAILED(hr)) {
        WARN("CreateCommittedResource D3D11: %08lx", hr);
        d12_free(res);
        return hr;
    }

    return D12Resource_QI((IUnknown*)res, riid, ppv);
}

/* CreateRootSignature */

static HRESULT WINAPI D12Dev_CreateRootSignature(IUnknown *s,
    UINT nodeMask, const void *pBlob, SIZE_T BlobSize, REFIID riid, void **ppv)
{
    (void)nodeMask;
    D12Device  *dev = (D12Device*)s;
    D12RootSig *rs = d12_alloc(sizeof(D12RootSig));
    if (!rs) return E_OUTOFMEMORY;

    rs->lpVtbl  = (void*)&g_RSVtbl;
    rs->ref     = 1;
    rs->device  = dev;
    rs->blob_sz = BlobSize;
    rs->blob    = d12_alloc(BlobSize);
    PDStore_Init(&rs->pd);

    if (!rs->blob) { d12_free(rs); return E_OUTOFMEMORY; }
    memcpy(rs->blob, pBlob, BlobSize);

    const RS_HEADER *hdr = (const RS_HEADER*)rs->blob;
    rs->num_params   = hdr->NumParams;
    rs->num_samplers = hdr->NumSamplers;
    rs->flags        = (D3D12_ROOT_SIGNATURE_FLAGS)hdr->Flags;

    /* Pré-parsing des tables de descripteurs */
    rs->param_tables = NULL;
    if (rs->num_params > 0) {
        rs->param_tables = d12_alloc(rs->num_params * sizeof(D12RSTableCache));
        if (rs->param_tables) {
            const RS_PARAM_ENTRY *entries = (const RS_PARAM_ENTRY*)(rs->blob + hdr->ParamOffset);
            for (UINT i = 0; i < rs->num_params; i++) {
                if (entries[i].Type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                    const RS_TABLE_PAYLOAD *tp = (const RS_TABLE_PAYLOAD*)(rs->blob + entries[i].PayloadOffset);
                    rs->param_tables[i].num_ranges = tp->NumRanges;
                    rs->param_tables[i].ranges     = (const RS_RANGE_V1*)(rs->blob + tp->RangesOffset);
                } else {
                    rs->param_tables[i].num_ranges = 0;
                    rs->param_tables[i].ranges     = NULL;
                }
            }
        }
    }

    return D12RootSig_QI((IUnknown*)rs, riid, ppv);
}

/* CreateFence */
static HRESULT WINAPI D12Fence_GetDevice(IUnknown *s, REFIID riid, void **ppv) {
    D12Fence *f = (D12Fence*)s;
    if (!ppv) return E_POINTER;
    if (!f->device) { *ppv = NULL; return E_FAIL; }
    return ((IUnknown*)(void*)f->device)->lpVtbl->QueryInterface(
        (IUnknown*)(void*)f->device, riid, ppv);
}

static HRESULT WINAPI D12Dev_CreateFence(IUnknown *s, UINT64 initVal, D3D12_FENCE_FLAGS flags, REFIID riid, void **ppv) {
    D12Device *dev = (D12Device*)s;
    (void)flags;
    D12Fence *f = d12_alloc(sizeof(D12Fence));
    if (!f) return E_OUTOFMEMORY;

    typedef struct {
        HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
        ULONG   (WINAPI *AddRef)(IUnknown*);
        ULONG   (WINAPI *Release)(IUnknown*);
        HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
        HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
        HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
        HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
        HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
        UINT64  (WINAPI *GetCompletedValue)(IUnknown*);
        HRESULT (WINAPI *SetEventOnCompletion)(IUnknown*, UINT64, HANDLE);
        HRESULT (WINAPI *Signal)(IUnknown*, UINT64);
    } FenceVtbl;
    static const FenceVtbl g_FVtbl = {
        (HRESULT(WINAPI*)(IUnknown*,REFIID,void**))D12Fence_QI,
        (ULONG(WINAPI*)(IUnknown*))D12Fence_AddRef,
        (ULONG(WINAPI*)(IUnknown*))D12Fence_Release,
        D12Fence_GetPrivateData, D12Fence_SetPrivateData,
        D12Fence_SetPrivateDataInterface, D12Fence_SetName,
        D12Fence_GetDevice,
        D12Fence_GetCompletedValue,
        D12Fence_SetEventOnCompletion,
        D12Fence_Signal,
    };

    f->lpVtbl    = (void*)&g_FVtbl;
    f->ref       = 1;
    f->value     = initVal;
    f->completed = initVal;
    f->device    = dev;
    PDStore_Init(&f->pd);
    return D12Fence_QI((IUnknown*)f, riid, ppv);
}

/* CheckFeatureSupport */
static HRESULT WINAPI D12Dev_CheckFeatureSupport(IUnknown *s,
    D3D12_FEATURE Feature, void *pFData, UINT DataSize)
{
    D12Device *dev = (D12Device*)s;
    switch (Feature) {
    case D3D12_FEATURE_D3D12_OPTIONS: {
        if (DataSize < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) return E_INVALIDARG;
        D3D12_FEATURE_DATA_D3D12_OPTIONS *o = pFData;
        memset(o, 0, sizeof(*o));
        o->TiledResourcesTier     = 2;
        o->ResourceBindingTier    = 3;
        o->ResourceHeapTier       = 2;
        o->ConservativeRasterizationTier = 1;
        o->ROVsSupported          = TRUE;
        o->TypedUAVLoadAdditionalFormats = TRUE;
        o->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
        o->OutputMergerLogicOp    = TRUE;
        o->CrossNodeSharingTier   = 0;
        o->CrossAdapterRowMajorTextureSupported = FALSE;
        return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
        if (DataSize < sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) return E_INVALIDARG;
        D3D12_FEATURE_DATA_FEATURE_LEVELS *fl = pFData;
        fl->MaxSupportedFeatureLevel = dev->exposed_fl;
        return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
        if (DataSize < sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)) return E_INVALIDARG;
        ((D3D12_FEATURE_DATA_SHADER_MODEL*)pFData)->HighestShaderModel = D3D_SHADER_MODEL_5_1;
        return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
        if (DataSize < sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)) return E_INVALIDARG;
        ((D3D12_FEATURE_DATA_ROOT_SIGNATURE*)pFData)->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        return S_OK;
    }
    default:
        WARN("CheckFeatureSupport: feature %d non geree", Feature);
        return E_NOTIMPL;
    }
}

/* GetDeviceRemovedReason */
static HRESULT WINAPI D12Dev_GetDeviceRemovedReason(IUnknown *s) {
    (void)s; return S_OK;
}

/* Stubs */
#define DEV_STUB(name) static HRESULT WINAPI D12Dev_##name(IUnknown *s, ...) \
    { (void)s; WARN("ID3D12Device::" #name " non implemente"); return E_NOTIMPL; }

DEV_STUB(CreateComputePipelineState)
DEV_STUB(CreateSharedHandle)
DEV_STUB(OpenSharedHandle)
DEV_STUB(OpenSharedHandleByName)
DEV_STUB(MakeResident)
DEV_STUB(Evict)
/* CreateQueryHeap: meme raisonnement que CreateCommandSignature ci-dessous -
 * on force *ppv=NULL explicitement plutot que de laisser un stub variadique
 * qui ne peut pas toucher au pointeur de sortie. */
static HRESULT WINAPI D12Dev_CreateQueryHeap(IUnknown *s,
    const void *pDesc, REFIID riid, void **ppv)
{
    (void)s; (void)pDesc; (void)riid;
    WARN("ID3D12Device::CreateQueryHeap non implemente");
    if (ppv) *ppv = NULL;
    return E_NOTIMPL;
}
DEV_STUB(SetStablePowerState)
/* CreateCommandSignature: PAS un stub variadique generique - le jeu (RE8 avec
 * ExecuteIndirect) peut dereferencer le ppv de sortie sans checker le HRESULT.
 * On force explicitement *ppv=NULL pour transformer un potentiel comportement
 * indefini (pile non initialisee) en NULL bien defini -> crash net et rapide
 * si le jeu l'utilise quand meme, plutot qu'une corruption memoire aleatoire. */
static HRESULT WINAPI D12Dev_CreateCommandSignature(IUnknown *s,
    const void *pDesc, IUnknown *pRootSignature, REFIID riid, void **ppv)
{
    (void)s; (void)pDesc; (void)pRootSignature; (void)riid;
    WARN("ID3D12Device::CreateCommandSignature non implemente");
    if (ppv) *ppv = NULL;
    return E_NOTIMPL;
}
DEV_STUB(GetResourceTiling)
DEV_STUB(CreateHeap)
DEV_STUB(CreatePlacedResource)
DEV_STUB(CreateReservedResource)

/* Cherche le D12Resource dont mapped_ptr == va (ce que GetGPUVirtualAddress retourne).
 * Parcourt la liste chainee dev->res_list_head. */
static D12Resource *D12Device_FindResourceByVA(D12Device *dev, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (!va || !IsValidDescriptorPtr((SIZE_T)(uintptr_t)va)) return NULL;

    /* Convention nouvelle : GPU VA = (uintptr_t)D12Resource*.
     * On vérifie la sanité via lpVtbl (non-NULL et aligné sur 8 bytes). */
    D12Resource *res = (D12Resource*)(uintptr_t)va;
    if (((uintptr_t)res->lpVtbl & 7) == 0 && res->lpVtbl != NULL &&
        (res->d11buf != NULL || res->d11tex != NULL)) {
        return res;
    }

    /* Fallback : ancienne convention mapped_ptr — parcourir la liste */
    for (D12Resource *r = dev->res_list_head; r; r = r->res_list_next) {
        if (!r->mapped_ptr) continue;
        SIZE_T start = (SIZE_T)(uintptr_t)r->mapped_ptr;
        SIZE_T end   = start + (SIZE_T)r->res_desc.Width;
        if ((SIZE_T)va >= start && (SIZE_T)va < end)
            return r;
    }
    return NULL;
}

/* CreateConstantBufferView
 * BufferLocation est une GPU VA = pointeur CPU vers les données mappées
 * (notre GetGPUVirtualAddress retourne mapped_ptr directement).
 * On retrouve le D12Resource correspondant via une recherche dans la VA map,
 * ou plus simplement on stocke dans le slot le buffer D3D11 + offset/size
 * que SetGraphicsRootDescriptorTable utilisera pour VSSetConstantBuffers. */
static void WINAPI D12Dev_CreateConstantBufferView(IUnknown *s,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    D12Device *dev = (D12Device*)s;
    (void)dev;

    if (!IsValidDescriptorPtr(DestDescriptor.ptr)) {
        LOG("CreateCBV: handle invalide ptr=%p", (void*)DestDescriptor.ptr);
        return;
    }
    D12Descriptor *slot = (D12Descriptor*)(uintptr_t)DestDescriptor.ptr;
    memset(slot, 0, sizeof(*slot));

    if (!pDesc) {
        /* CBV null descriptor - laisser le slot à zéro */
        LOG("CreateCBV: pDesc NULL (null descriptor)");
        return;
    }

    /* BufferLocation = mapped_ptr du D12Resource (notre convention GetGPUVA).
     * On cherche le D12Resource dont mapped_ptr == BufferLocation dans la
     * liste des ressources allouées. Comme on n'a pas de liste globale,
     * on utilise BufferLocation directement comme adresse CPU de D3D11Map,
     * et on retrouve le buf via la structure D12Resource embedée avant mapped_ptr.
     * Plus simple: on stocke l'adresse GPU telle quelle, SetGraphicsRootCBV la lira. */

    /* Trouver le D12Resource via la gpu_va map du device */
    D12Resource *res = D12Device_FindResourceByVA(dev, pDesc->BufferLocation);
    if (!res || !res->d11buf) {
        LOG("CreateCBV: resource non trouvee pour VA=%p", (void*)(uintptr_t)pDesc->BufferLocation);
        return;
    }

    UINT offset = (UINT)(pDesc->BufferLocation - res->gpu_va);
    slot->cbv.buf          = res->d11buf;
    slot->cbv.offset_bytes = offset;
    slot->cbv.size_bytes   = pDesc->SizeInBytes;

    LOG("CreateCBV: slot=%p buf=%p offset=%u size=%u",
        (void*)slot, res->d11buf, offset, pDesc->SizeInBytes);
}

/* CreateShaderResourceView */
static void WINAPI D12Dev_CreateShaderResourceView(IUnknown *s,
    IUnknown *pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    D12Device *dev = (D12Device*)s;

    if (!IsValidDescriptorPtr(DestDescriptor.ptr)) {
        LOG("CreateSRV: handle invalide ptr=%p", (void*)DestDescriptor.ptr);
        return;
    }
    D12Descriptor *slot = (D12Descriptor*)(uintptr_t)DestDescriptor.ptr;
    memset(slot, 0, sizeof(*slot));

    if (!pResource) {
        LOG("CreateSRV: pResource NULL (null descriptor)");
        return;
    }

    D12Resource *res = (D12Resource*)pResource;

    D3D11_SHADER_RESOURCE_VIEW_DESC d11desc;
    memset(&d11desc, 0, sizeof(d11desc));

    if (pDesc) {
        d11desc.Format = pDesc->Format;
        if (pDesc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            d11desc.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURE2D;
            d11desc.Texture2D.MostDetailedMip   = pDesc->Texture2D.MostDetailedMip;
            d11desc.Texture2D.MipLevels         = pDesc->Texture2D.MipLevels;
        } else if (pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
            d11desc.ViewDimension               = D3D11_SRV_DIMENSION_BUFFER;
            d11desc.Buffer.FirstElement         = pDesc->Buffer.FirstElement;
            d11desc.Buffer.NumElements          = pDesc->Buffer.NumElements;
        } else {
            /* Inférer depuis la ressource */
            if (res->d11tex) {
                d11desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                d11desc.Format        = res->res_desc.Format;
                d11desc.Texture2D.MipLevels = res->res_desc.MipLevels ? res->res_desc.MipLevels : 1;
            } else {
                d11desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                d11desc.Format        = DXGI_FORMAT_UNKNOWN;
            }
        }
    } else {
        /* Pas de desc: inférer */
        if (res->d11tex) {
            d11desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            d11desc.Format                    = res->res_desc.Format;
            d11desc.Texture2D.MipLevels       = res->res_desc.MipLevels ? res->res_desc.MipLevels : 1;
        } else {
            d11desc.ViewDimension             = D3D11_SRV_DIMENSION_BUFFER;
            d11desc.Format                    = DXGI_FORMAT_UNKNOWN;
            d11desc.Buffer.NumElements        = (UINT)(res->res_desc.Width / 4);
        }
    }

    HRESULT hr = dev->d11dev->lpVtbl->CreateShaderResourceView(
        dev->d11dev, res->d11res, &d11desc, &slot->srv);
    if (FAILED(hr)) {
        WARN("CreateSRV: D3D11 CreateSRV failed: %08lx", hr);
        slot->srv = NULL;
    } else {
        LOG("CreateSRV: slot=%p srv=%p", (void*)slot, slot->srv);
    }
}

static void WINAPI D12Dev_CreateSampler(IUnknown *s, const void *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    OutputDebugStringA("D12Dev_CreateSampler called\n");
    (void)s; (void)pDesc; (void)DestDescriptor;
    /* TODO: créer ID3D11SamplerState et stocker dans slot->sampler */
}

static HRESULT WINAPI D12Dev_CreateGraphicsPipelineState(IUnknown *s,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc,
    REFIID riid, void **ppv)
{
    OutputDebugStringA("D12Dev_CreateGraphicsPipelineState called\n");
    if (!ppv || !pDesc) return E_INVALIDARG;
    *ppv = NULL;

    D12Device *dev = (D12Device*)s;
    D12PSO *pso = d12_alloc(sizeof(D12PSO));
    if (!pso) return E_OUTOFMEMORY;

    // Initialiser la vtable (identique à celle utilisée pour D12PSO)
    typedef struct {
        HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
        ULONG   (WINAPI *AddRef)(IUnknown*);
        ULONG   (WINAPI *Release)(IUnknown*);
        HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
        HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
        HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
        HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
        HRESULT (WINAPI *GetDevice)(IUnknown*, REFIID, void**);
        HRESULT (WINAPI *GetCachedBlob)(IUnknown*, IUnknown**);
    } PSOVtbl;
    static const PSOVtbl g_PSOVtbl = {
        (HRESULT(WINAPI*)(IUnknown*,REFIID,void**))D12PSO_QI,
        (ULONG(WINAPI*)(IUnknown*))D12PSO_AddRef,
        (ULONG(WINAPI*)(IUnknown*))D12PSO_Release,
        D12PSO_GetPrivateData, D12PSO_SetPrivateData,
        D12PSO_SetPrivateDataInterface, D12PSO_SetName,
        D12PSO_GetDevice,
        D12PSO_GetCachedBlob,
    };

    pso->lpVtbl = (void*)&g_PSOVtbl;
    pso->ref = 1;
    pso->device = dev;
    PDStore_Init(&pso->pd);

    // Créer les shaders D3D11
    HRESULT hr = S_OK;

    if (pDesc->VS.pShaderBytecode && pDesc->VS.BytecodeLength > 0) {
        hr = dev->d11dev->lpVtbl->CreateVertexShader(dev->d11dev,
                pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength,
                NULL, &pso->vs);
        if (FAILED(hr)) {
            WARN("CreateVertexShader failed: %08lx", hr);
            D12PSO_Destroy(pso);
            return hr;
        }
    }

    if (pDesc->PS.pShaderBytecode && pDesc->PS.BytecodeLength > 0) {
        hr = dev->d11dev->lpVtbl->CreatePixelShader(dev->d11dev,
                pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength,
                NULL, &pso->ps);
        if (FAILED(hr)) {
            WARN("CreatePixelShader failed: %08lx", hr);
            D12PSO_Destroy(pso);
            return hr;
        }
    }

    // On ignore les autres shaders pour l'instant (GS, HS, DS, CS)
    // On ignore rasterizer, blend, depth-stencil pour simplifier.
    // Plus tard on pourra les ajouter.

    // Input Layout : conversion D3D12_INPUT_ELEMENT_DESC[] -> D3D11_INPUT_ELEMENT_DESC[]
    // Necessite le bytecode du VS pour la validation de signature D3D11.
    pso->input_layout = NULL;
    if (pDesc->InputLayout.NumElements > 0 && pDesc->InputLayout.pInputElementDescs
        && pDesc->VS.pShaderBytecode && pDesc->VS.BytecodeLength > 0)
    {
        const D3D12_INPUT_ELEMENT_DESC *src = pDesc->InputLayout.pInputElementDescs;
        UINT n = pDesc->InputLayout.NumElements;

        D3D11_INPUT_ELEMENT_DESC d11elems[32];
        if (n > 32) n = 32; /* garde-fou */

        for (UINT i = 0; i < n; i++) {
            d11elems[i].SemanticName         = src[i].SemanticName;
            d11elems[i].SemanticIndex        = src[i].SemanticIndex;
            d11elems[i].Format               = src[i].Format; /* DXGI_FORMAT identique D3D11/D3D12 */
            d11elems[i].InputSlot            = src[i].InputSlot;
            d11elems[i].AlignedByteOffset    = src[i].AlignedByteOffset;
            d11elems[i].InputSlotClass       = (D3D11_INPUT_CLASSIFICATION)src[i].InputSlotClass;
            d11elems[i].InstanceDataStepRate = src[i].InstanceDataStepRate;
        }

        hr = dev->d11dev->lpVtbl->CreateInputLayout(dev->d11dev,
                d11elems, n,
                pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength,
                &pso->input_layout);
        if (FAILED(hr)) {
            WARN("CreateInputLayout failed: %08lx", hr);
            pso->input_layout = NULL;
            /* Non-fatal : on continue sans layout plutot que d'echouer tout le PSO,
             * mais le rendu affichera alors l'erreur DEVICE_DRAW_INPUTLAYOUT_NOT_SET. */
        } else {
            LOG("CreateInputLayout OK: n=%u -> input_layout=%p", n, pso->input_layout);
        }
    } else {
        WARN("CreateGraphicsPipelineState: InputLayout absent ou VS bytecode manquant (n=%u)",
             pDesc->InputLayout.NumElements);
    }

    // Stocker la root signature si présente
    if (pDesc->pRootSignature) {
        pso->root_sig = (D12RootSig*)pDesc->pRootSignature;
        ((IUnknown*)pso->root_sig)->lpVtbl->AddRef((IUnknown*)pso->root_sig);
    }

    // Topologie par défaut (pourrait être modifiée plus tard)
    pso->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // Retourner l'objet
    hr = D12PSO_QI((IUnknown*)pso, riid, ppv);
    D12PSO_Release((IUnknown*)pso); // libère la ref locale
    return hr;
}

static void WINAPI D12Dev_CreateUnorderedAccessView(IUnknown *s, IUnknown *pResource, IUnknown *pCounterResource, const void *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    OutputDebugStringA("D12Dev_CreateUnorderedAccessView called\n");
    (void)pCounterResource; // pas géré pour l’instant
    D12Device *dev = (D12Device*)s;
    D12Resource *res = (D12Resource*)pResource;
    D12Descriptor *slot = (D12Descriptor*)(uintptr_t)DestDescriptor.ptr;
    if (!slot) { WARN("CreateUAV: slot NULL"); return; }
    if (slot->uav) { slot->uav->lpVtbl->Release(slot->uav); slot->uav = NULL; }
    if (!res || !res->d11res) { WARN("CreateUAV: ressource invalide"); return; }
    HRESULT hr = dev->d11dev->lpVtbl->CreateUnorderedAccessView(dev->d11dev, res->d11res, NULL, &slot->uav);
    if (FAILED(hr)) {
        WARN("CreateUAV: ID3D11Device::CreateUnorderedAccessView failed: %08lx", hr);
        slot->uav = NULL;
    } else {
        LOG("CreateUAV: success, slot->uav=%p", slot->uav);
    }
}

/* CreateRenderTargetView */
static void WINAPI D12Dev_CreateRenderTargetView(IUnknown *s, IUnknown *pResource,
    const void *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "D12Dev_CreateRenderTargetView: DestDescriptor.ptr=%p\n", (void*)DestDescriptor.ptr);
    OutputDebugStringA(buf);

    (void)pDesc;
    D12Device   *dev = (D12Device*)s;
    D12Resource *res = (D12Resource*)pResource;
    ID3D11RenderTargetView **pSlot = NULL;

    // Si le handle est 0, 8 ou autre petit nombre, utiliser le dernier heap RTV
    if (DestDescriptor.ptr < 0x1000) {
        if (dev->lastRTVHeap) {
            // L'index est donné par DestDescriptor.ptr / increment (pas 8 en dur)
            UINT inc = dev->desc_inc[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];
            UINT index = inc ? (UINT)(DestDescriptor.ptr / inc) : 0;
            if (index < dev->lastRTVHeap->desc.NumDescriptors) {
                pSlot = &dev->lastRTVHeap->descs[index].rtv;
                OutputDebugStringA("Using lastRTVHeap fallback\n");
                snprintf(buf, sizeof(buf), "  slot index=%u, pSlot=%p\n", index, pSlot);
                OutputDebugStringA(buf);
            }
        }
        if (!pSlot) {
            WARN("CreateRenderTargetView: impossible de trouver un slot valide");
            return;
        }
    } else {
        // Sinon, utiliser le handle tel quel
        pSlot = (ID3D11RenderTargetView**)(uintptr_t)DestDescriptor.ptr;
        if (!pSlot) {
            WARN("CreateRenderTargetView: handle CPU invalide");
            return;
        }
    }

    if (*pSlot) { (*pSlot)->lpVtbl->Release(*pSlot); *pSlot = NULL; }
    if (!res || !res->d11tex) {
        WARN("CreateRenderTargetView: ressource invalide (res=%p, d11tex=%p)", res, res ? res->d11tex : NULL);
        return;
    }

    HRESULT hr = dev->d11dev->lpVtbl->CreateRenderTargetView(dev->d11dev,
                res->d11res, NULL, pSlot);
    if (FAILED(hr)) {
        WARN("CreateRenderTargetView: ID3D11Device::CreateRenderTargetView a echoue hr=0x%08X", (unsigned)hr);
        *pSlot = NULL;
    } else {
        LOG("CreateRenderTargetView: success, pSlot=%p, *pSlot=%p", pSlot, *pSlot);
        snprintf(buf, sizeof(buf), "  RTV créé avec succès: *pSlot=%p\n", *pSlot);
        OutputDebugStringA(buf);
    }
}

/* CreateDepthStencilView */
static void WINAPI D12Dev_CreateDepthStencilView(IUnknown *s, IUnknown *pResource,
    const void *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "D12Dev_CreateDepthStencilView: DestDescriptor.ptr=%p\n", (void*)DestDescriptor.ptr);
    OutputDebugStringA(buf);

    (void)pDesc;
    D12Device   *dev = (D12Device*)s;
    D12Resource *res = (D12Resource*)pResource;
    ID3D11DepthStencilView **pSlot = NULL;

    // Si le handle est 0, 8 ou autre petit nombre, utiliser le dernier heap DSV
    if (DestDescriptor.ptr < 0x1000) {
        if (dev->lastDSVHeap) {
            UINT inc = dev->desc_inc[D3D12_DESCRIPTOR_HEAP_TYPE_DSV];
            UINT index = inc ? (UINT)(DestDescriptor.ptr / inc) : 0;
            if (index < dev->lastDSVHeap->desc.NumDescriptors) {
                pSlot = &dev->lastDSVHeap->descs[index].dsv;
                OutputDebugStringA("Using lastDSVHeap fallback\n");
                snprintf(buf, sizeof(buf), "  slot index=%u, pSlot=%p\n", index, pSlot);
                OutputDebugStringA(buf);
            }
        }
        if (!pSlot) {
            WARN("CreateDepthStencilView: impossible de trouver un slot valide");
            return;
        }
    } else {
        pSlot = (ID3D11DepthStencilView**)(uintptr_t)DestDescriptor.ptr;
        if (!pSlot) {
            WARN("CreateDepthStencilView: handle CPU invalide");
            return;
        }
    }

    if (*pSlot) { (*pSlot)->lpVtbl->Release(*pSlot); *pSlot = NULL; }
    if (!res || !res->d11tex) {
        WARN("CreateDepthStencilView: ressource invalide (res=%p, d11tex=%p)", res, res ? res->d11tex : NULL);
        return;
    }

    HRESULT hr = dev->d11dev->lpVtbl->CreateDepthStencilView(dev->d11dev,
                res->d11res, NULL, pSlot);
    if (FAILED(hr)) {
        WARN("CreateDepthStencilView: ID3D11Device::CreateDepthStencilView a echoue hr=0x%08X", (unsigned)hr);
        *pSlot = NULL;
    } else {
        LOG("CreateDepthStencilView: success, pSlot=%p, *pSlot=%p", pSlot, *pSlot);
        snprintf(buf, sizeof(buf), "  DSV créé avec succès: *pSlot=%p\n", *pSlot);
        OutputDebugStringA(buf);
    }
}
DEV_STUB(CopyDescriptors)
DEV_STUB(CopyDescriptorsSimple)

static void WINAPI D12Dev_GetCopyableFootprints(IUnknown *s, ...) { (void)s; WARN("GetCopyableFootprints non implemente"); }
static void WINAPI D12Dev_GetResourceAllocationInfo(IUnknown *s, ...) { (void)s; WARN("GetResourceAllocationInfo non implemente"); }
static void WINAPI D12Dev_GetCustomHeapProperties(IUnknown *s, ...) { (void)s; }
static LUID  WINAPI D12Dev_GetAdapterLuid(IUnknown *s) {
    LUID luid = {0,0};
    D12Device *dev = (D12Device*)s;
    IDXGIDevice *ddev = dev->dxgi_dev;
    if (ddev) {
        IDXGIAdapter *adapter = NULL;
        ddev->lpVtbl->GetAdapter(ddev, &adapter);
        if (adapter) {
            DXGI_ADAPTER_DESC adesc;
            adapter->lpVtbl->GetDesc(adapter, &adesc);
            luid = adesc.AdapterLuid;
            adapter->lpVtbl->Release(adapter);
        }
    }
    return luid;
}

/* Device vtable */
typedef struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    HRESULT (WINAPI *GetPrivateData)(IUnknown*, REFGUID, UINT*, void*);
    HRESULT (WINAPI *SetPrivateData)(IUnknown*, REFGUID, UINT, const void*);
    HRESULT (WINAPI *SetPrivateDataInterface)(IUnknown*, REFGUID, const IUnknown*);
    HRESULT (WINAPI *SetName)(IUnknown*, LPCWSTR);
    UINT    (WINAPI *GetNodeCount)(IUnknown*);
    HRESULT (WINAPI *CreateCommandQueue)(IUnknown*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
    HRESULT (WINAPI *CreateCommandAllocator)(IUnknown*, D3D12_COMMAND_LIST_TYPE, REFIID, void**);
    HRESULT (WINAPI *CreateGraphicsPipelineState)(IUnknown*, ...);
    HRESULT (WINAPI *CreateComputePipelineState)(IUnknown*, ...);
    HRESULT (WINAPI *CreateCommandList)(IUnknown*, UINT, D3D12_COMMAND_LIST_TYPE, IUnknown*, IUnknown*, REFIID, void**);
    HRESULT (WINAPI *CheckFeatureSupport)(IUnknown*, D3D12_FEATURE, void*, UINT);
    HRESULT (WINAPI *CreateDescriptorHeap)(IUnknown*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
    UINT    (WINAPI *GetDescriptorHandleIncrementSize)(IUnknown*, D3D12_DESCRIPTOR_HEAP_TYPE);
    HRESULT (WINAPI *CreateRootSignature)(IUnknown*, UINT, const void*, SIZE_T, REFIID, void**);
    void    (WINAPI *CreateConstantBufferView)(IUnknown*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    void    (WINAPI *CreateShaderResourceView)(IUnknown*, IUnknown*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    void    (WINAPI *CreateUnorderedAccessView)(IUnknown*, ...);
    void    (WINAPI *CreateRenderTargetView)(IUnknown*, IUnknown*, const void*, D3D12_CPU_DESCRIPTOR_HANDLE);
    void    (WINAPI *CreateDepthStencilView)(IUnknown*, IUnknown*, const void*, D3D12_CPU_DESCRIPTOR_HANDLE);
    void    (WINAPI *CreateSampler)(IUnknown*, ...);
    void    (WINAPI *CopyDescriptors)(IUnknown*, ...);
    void    (WINAPI *CopyDescriptorsSimple)(IUnknown*, ...);
    void    (WINAPI *GetResourceAllocationInfo)(IUnknown*, ...);
    void    (WINAPI *GetCustomHeapProperties)(IUnknown*, ...);
    HRESULT (WINAPI *CreateCommittedResource)(IUnknown*, const D3D12_HEAP_PROPERTIES*, UINT, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const void*, REFIID, void**);
    HRESULT (WINAPI *CreateHeap)(IUnknown*, ...);
    HRESULT (WINAPI *CreatePlacedResource)(IUnknown*, ...);
    HRESULT (WINAPI *CreateReservedResource)(IUnknown*, ...);
    HRESULT (WINAPI *CreateSharedHandle)(IUnknown*, ...);
    HRESULT (WINAPI *OpenSharedHandle)(IUnknown*, ...);
    HRESULT (WINAPI *OpenSharedHandleByName)(IUnknown*, ...);
    HRESULT (WINAPI *MakeResident)(IUnknown*, ...);
    HRESULT (WINAPI *Evict)(IUnknown*, ...);
    HRESULT (WINAPI *CreateFence)(IUnknown*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
    HRESULT (WINAPI *GetDeviceRemovedReason)(IUnknown*);
    void    (WINAPI *GetCopyableFootprints)(IUnknown*, ...);
    HRESULT (WINAPI *CreateQueryHeap)(IUnknown*, ...);
    HRESULT (WINAPI *SetStablePowerState)(IUnknown*, ...);
    HRESULT (WINAPI *CreateCommandSignature)(IUnknown*, ...);
    void    (WINAPI *GetResourceTiling)(IUnknown*, ...);
    LUID    (WINAPI *GetAdapterLuid)(IUnknown*);
} D12DeviceVtbl;

static const D12DeviceVtbl g_DevVtbl = {
    (HRESULT(WINAPI*)(IUnknown*,REFIID,void**)) D12Device_QI,
    (ULONG(WINAPI*)(IUnknown*))                 D12Device_AddRef,
    (ULONG(WINAPI*)(IUnknown*))                 D12Device_Release,
    D12Device_GetPrivateData,
    D12Device_SetPrivateData,
    D12Device_SetPrivateDataInterface,
    D12Device_SetName,
    D12Dev_GetNodeCount,
    D12Dev_CreateCommandQueue,
    D12Dev_CreateCommandAllocator,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreateGraphicsPipelineState,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreateComputePipelineState,
    D12Dev_CreateCommandList,
    D12Dev_CheckFeatureSupport,
    D12Dev_CreateDescriptorHeap,
    D12Dev_GetDescriptorHandleIncrementSize,
    D12Dev_CreateRootSignature,
    D12Dev_CreateConstantBufferView,
    D12Dev_CreateShaderResourceView,
    (void(WINAPI*)(IUnknown*,...))D12Dev_CreateUnorderedAccessView,
    D12Dev_CreateRenderTargetView,
    D12Dev_CreateDepthStencilView,
    (void(WINAPI*)(IUnknown*,...))D12Dev_CreateSampler,
    (void(WINAPI*)(IUnknown*,...))D12Dev_CopyDescriptors,
    (void(WINAPI*)(IUnknown*,...))D12Dev_CopyDescriptorsSimple,
    (void(WINAPI*)(IUnknown*,...))D12Dev_GetResourceAllocationInfo,
    (void(WINAPI*)(IUnknown*,...))D12Dev_GetCustomHeapProperties,
    D12Dev_CreateCommittedResource,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreateHeap,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreatePlacedResource,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreateReservedResource,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreateSharedHandle,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_OpenSharedHandle,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_OpenSharedHandleByName,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_MakeResident,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_Evict,
    D12Dev_CreateFence,
    D12Dev_GetDeviceRemovedReason,
    (void(WINAPI*)(IUnknown*,...))D12Dev_GetCopyableFootprints,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreateQueryHeap,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_SetStablePowerState,
    (HRESULT(WINAPI*)(IUnknown*,...))D12Dev_CreateCommandSignature,
    (void(WINAPI*)(IUnknown*,...))D12Dev_GetResourceTiling,
    D12Dev_GetAdapterLuid,
};


/* ============================================================
 * SECTION 15 : D3D12CreateDevice - Point d'entree principal
 * ============================================================ */

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static PFN_D3D11CreateDevice g_pfnD3D11Create = NULL;
static HMODULE               g_hD3D11 = NULL;

static BOOL D3D11_Load(void) {
    if (g_pfnD3D11Create) return TRUE;
    g_hD3D11 = LoadLibraryA("d3d11.dll");
    if (!g_hD3D11) return FALSE;
    g_pfnD3D11Create = (PFN_D3D11CreateDevice)GetProcAddress(g_hD3D11, "D3D11CreateDevice");
    return g_pfnD3D11Create != NULL;
}

HRESULT WINAPI D3D12CreateDevice(
    IUnknown          *pAdapter,
    D3D_FEATURE_LEVEL  MinimumFeatureLevel,
    REFIID             riid,
    void             **ppDevice)
{
    LOG("D3D12CreateDevice fl=%x", MinimumFeatureLevel);
    if (!ppDevice) return S_FALSE;
    if (!D3D11_Load()) {
        WARN("d3d11.dll non disponible");
        return DXGI_ERROR_SDK_COMPONENT_MISSING;
    }

    D3D_FEATURE_LEVEL d11_fls[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    ID3D11Device        *d11dev = NULL;
    ID3D11DeviceContext *d11ctx = NULL;
    D3D_FEATURE_LEVEL    actual_fl;

    UINT flags = 0;
#ifdef DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = g_pfnD3D11Create(
        (IDXGIAdapter*)pAdapter,
        pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        NULL, flags,
        d11_fls, ARRAYSIZE(d11_fls),
        D3D11_SDK_VERSION,
        &d11dev, &actual_fl, &d11ctx);

    if (FAILED(hr)) {
        WARN("D3D11CreateDevice echoue: %08lx", hr);
        return hr;
    }

    LOG("D3D11 cree, feature level: %x", actual_fl);

    D12Device *dev = d12_alloc(sizeof(D12Device));
    if (!dev) {
        ID3D11Device_Release(d11dev);
        ID3D11DeviceContext_Release(d11ctx);
        return E_OUTOFMEMORY;
    }

    dev->lpVtbl  = (void*)&g_DevVtbl;
    dev->ref     = 1;
    dev->d11dev  = d11dev;
    dev->d11ctx  = d11ctx;
    dev->actual_fl  = actual_fl;
    dev->next_gpu_va = 0x10000;
    PDStore_Init(&dev->pd);

    if (actual_fl >= D3D_FEATURE_LEVEL_11_0) {
        dev->exposed_fl = D3D_FEATURE_LEVEL_12_1;
    } else if (actual_fl >= D3D_FEATURE_LEVEL_10_1) {
        dev->exposed_fl = D3D_FEATURE_LEVEL_12_0;
    } else {
        dev->exposed_fl = D3D_FEATURE_LEVEL_11_0;
    }

    if (dev->exposed_fl < MinimumFeatureLevel) {
        WARN("Feature level demande (%x) > level disponible (%x)",
             MinimumFeatureLevel, dev->exposed_fl);
        D12Device_Destroy(dev);
        return E_FAIL;
    }

    /* Descriptor increments (standard values) */
    /* CRITIQUE: increment doit correspondre a sizeof(D12Descriptor) car
     * cpu_base.ptr pointe vers un tableau de D12Descriptor (union de 16 bytes
     * sur x64 a cause du membre cbv {buf(8)+offset(4)+size(4)}). Avec un
     * increment de 8 au lieu de 16, l'app indexe au MILIEU de chaque
     * descripteur des le slot 1, corrompant tout acces suivant. */
    dev->desc_inc[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] = sizeof(D12Descriptor);
    dev->desc_inc[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER]     = sizeof(D12Descriptor);
    dev->desc_inc[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]         = sizeof(D12Descriptor);
    dev->desc_inc[D3D12_DESCRIPTOR_HEAP_TYPE_DSV]         = sizeof(D12Descriptor);

    ID3D11Device_QueryInterface(d11dev, &IID_IDXGIDevice, (void**)&dev->dxgi_dev);

    hr = D12Device_QI((IUnknown*)dev, riid, ppDevice);
    if (FAILED(hr)) D12Device_Destroy(dev);

    LOG("D3D12CreateDevice OK, exposed FL=%x", dev->exposed_fl);
    return hr;
}


/* ============================================================
 * SECTION 16 : Fonctions exportees (hors D3D12CreateDevice)
 * ============================================================ */

HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **ppvDebug) {
    (void)riid;
    WARN("D3D12GetDebugInterface: debug layer non disponible (Win7)");
    if (ppvDebug) *ppvDebug = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION       Version,
    IUnknown                       **ppBlob,
    IUnknown                       **ppErrorBlob)
{
    if (!pRootSignature || !ppBlob) return E_INVALIDARG;
    if (Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        WARN("SerializeRootSignature V1.1 demande avec desc V1.0, conversion...");
    }
    return SerializeRS_V1(pRootSignature, ppBlob, ppErrorBlob);
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
    IUnknown                                 **ppBlob,
    IUnknown                                 **ppErrorBlob)
{
    if (!pRootSignature || !ppBlob) return E_INVALIDARG;
    switch (pRootSignature->Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
        return SerializeRS_V1(&pRootSignature->Desc_1_0, ppBlob, ppErrorBlob);
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
        return SerializeRS_V11(&pRootSignature->Desc_1_1, ppBlob, ppErrorBlob);
    default:
        return E_INVALIDARG;
    }
}

/* Deserializer */
typedef struct {
    void *lpVtbl;
    LONG  ref;
    void *blob_copy;
    SIZE_T blob_sz;
    D3D12_ROOT_SIGNATURE_DESC *desc;
} RS_Deser;

DEFINE_GUID(IID_ID3D12RootSignatureDeserializer, 0x34ab647b,0x3cc8,0x46ac,0x84,0x1b,0xc0,0x96,0x56,0x45,0xc0,0x46);
DEFINE_GUID(IID_ID3D12VersionedRootSignatureDeserializer, 0x7f91ce67,0x090c,0x4bb7,0xb7,0x8e,0xed,0x8f,0xf2,0xe3,0x1d,0xa0);

static void RSDeser_Destroy(RS_Deser *d) {
    d12_free(d->desc ? (void*)d->desc->pParameters : NULL);
    d12_free(d->blob_copy);
    d12_free(d);
}
static HRESULT WINAPI RSDeser_QI(IUnknown *s, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown_local) || IsEqualGUID(riid, &IID_ID3D12RootSignatureDeserializer)) {
        InterlockedIncrement(&((RS_Deser*)s)->ref); *ppv = s; return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI RSDeser_AddRef(IUnknown *s)  { return InterlockedIncrement(&((RS_Deser*)s)->ref); }
static ULONG WINAPI RSDeser_Release(IUnknown *s) {
    RS_Deser *d = (RS_Deser*)s;
    ULONG r = InterlockedDecrement(&d->ref);
    if (!r) RSDeser_Destroy(d);
    return r;
}
static const D3D12_ROOT_SIGNATURE_DESC* WINAPI RSDeser_GetRootSignatureDesc(IUnknown *s)
    { return ((RS_Deser*)s)->desc; }

static const struct {
    HRESULT (WINAPI *QI)(IUnknown*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(IUnknown*);
    ULONG   (WINAPI *Release)(IUnknown*);
    const D3D12_ROOT_SIGNATURE_DESC* (WINAPI *GetRootSignatureDesc)(IUnknown*);
} g_RSDeserVtbl = { RSDeser_QI, RSDeser_AddRef, RSDeser_Release, RSDeser_GetRootSignatureDesc };

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    const void *pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface, void **ppRootSignatureDeserializer)
{
    if (!pSrcData || !ppRootSignatureDeserializer) return E_INVALIDARG;

    RS_Deser *d = d12_alloc(sizeof(RS_Deser));
    if (!d) return E_OUTOFMEMORY;

    d->lpVtbl   = (void*)&g_RSDeserVtbl;
    d->ref      = 1;
    d->blob_sz  = SrcDataSizeInBytes;
    d->blob_copy = d12_alloc(SrcDataSizeInBytes);
    if (!d->blob_copy) { d12_free(d); return E_OUTOFMEMORY; }
    memcpy(d->blob_copy, pSrcData, SrcDataSizeInBytes);

    HRESULT hr = DeserializeRS_V1(d->blob_copy, SrcDataSizeInBytes, &d->desc);
    if (FAILED(hr)) { d12_free(d->blob_copy); d12_free(d); return hr; }

    return RSDeser_QI((IUnknown*)d, pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    const void *pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface, void **ppRootSignatureDeserializer)
{
    return D3D12CreateRootSignatureDeserializer(
        pSrcData, SrcDataSizeInBytes,
        pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT NumFeatures, const IID *pIIDs,
    void *pConfigurationStructs, UINT *pConfigurationStructSizes)
{
    (void)NumFeatures; (void)pIIDs; (void)pConfigurationStructs; (void)pConfigurationStructSizes;
    LOG("D3D12EnableExperimentalFeatures: accepte");
    return S_OK;
}

HRESULT WINAPI D3D12CoreCreateLayeredDevice(
    const void *unknown1, DWORD unk2, const void *unknown3,
    REFIID riid, void **ppv)
{
    (void)unknown1; (void)unk2; (void)unknown3; (void)riid; (void)ppv;
    return E_NOTIMPL;
}

UINT WINAPI D3D12CoreGetLayeredDeviceSize(const void *unknown1, DWORD unk2) {
    (void)unknown1; (void)unk2; return 0;
}

HRESULT WINAPI D3D12CoreRegisterLayers(const void *unknown, DWORD unk2) {
    (void)unknown; (void)unk2; return S_OK;
}

HRESULT WINAPI OpenAdapter10_2(void *pData) {
    (void)pData;
    WARN("OpenAdapter10_2: non supporte sur Win7 sans WDDMv2");
    return E_NOTIMPL;
}


/* Stubs pour les exports manquants */

HRESULT WINAPI D3D12CreateDeviceAndSwapChain(
    IUnknown *pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riidDevice,
    void **ppDevice,
    REFIID riidSwapChain,
    void **ppSwapChain)
{
    (void)pAdapter; (void)MinimumFeatureLevel; (void)riidDevice; (void)ppDevice;
    (void)riidSwapChain; (void)ppSwapChain;
    WARN("D3D12CreateDeviceAndSwapChain non implémenté (utilise D3D12CreateDevice + DXGI)");
    return E_NOTIMPL;
}

HRESULT WINAPI D3D12DeviceRemovedExtendedData(void) {
    WARN("D3D12DeviceRemovedExtendedData called");
    return E_NOTIMPL;
}

HRESULT WINAPI D3D12GetInterface(REFIID riid, void **ppv) {
    (void)riid; (void)ppv;
    WARN("D3D12GetInterface called");
    if (ppv) *ppv = NULL;
    return E_NOINTERFACE;
}

/* ============================================================
 * PIX event exports (requis par le .def, non implementes sur Win7)
 * ============================================================ */

void* WINAPI D3D12PIXEventsReplaceBlock(void *blockInfo, BOOL getEarliestTime) {
    (void)blockInfo; (void)getEarliestTime;
    WARN("D3D12PIXEventsReplaceBlock called (stub)");
    return NULL;
}

void WINAPI D3D12PIXGetThreadInfo(void *pInfo) {
    (void)pInfo;
    WARN("D3D12PIXGetThreadInfo called (stub)");
}

void WINAPI D3D12PIXNotifyWakeFromFenceSignal(HANDLE hEvent) {
    (void)hEvent;
    WARN("D3D12PIXNotifyWakeFromFenceSignal called (stub)");
}

HRESULT WINAPI D3D12PIXReportCounter(UINT64 name, UINT64 value) {
    (void)name; (void)value;
    WARN("D3D12PIXReportCounter called (stub)");
    return S_OK;
}

/* ============================================================
 * SECTION 17 : DllMain
 * ============================================================ */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        LOG("d3d12.dll charge (Win7 DX11 backend)");
        break;
    case DLL_PROCESS_DETACH:
        if (g_hD3D11) { FreeLibrary(g_hD3D11); g_hD3D11 = NULL; }
        LOG("d3d12.dll decharge");
        break;
    }
    return TRUE;
}