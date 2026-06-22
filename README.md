# DirectX12On7
A patch to get DirectX12 running on Windows 7, using Microsoft DLLs (d3d12.dll and dxilconv7.dll) and some coding for the DirectX Graphics Interface (DXGI) wrapper.

**This is a real implementation of DirectX12, not just an emulation using Vulkan** (VKD3D), so it's a bit more optimized.

This will not need any D3D12On7 support inside the application itself.

Every application that uses CreateDXGIFactory1 and DirectX12 can be fixed with this wrapper.

Made with love from France :D
