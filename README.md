# DirectX12On7
A patch to get DirectX12 running on Windows 7, using Microsoft DLLs (d3d12.dll and dxilconv7.dll) and some coding for the DirectX Graphics Interface (DXGI) wrapper.

**This is a real implementation of DirectX12, not just an emulation using Vulkan** (VKD3D), so it's a bit more optimized.

This does not need any D3D12On7 support inside the application itself, it *implements* D3D12On7 **outside** the game.

***How to build myself ?***
1) Download the source code ZIP ( https://github.com/Roti-Poulet/DirectX12On7/archive/refs/heads/main.zip )
2) Extract it
3) Open MinGW64, change the directory to where you extracted the ZIP /DXGI_Wrapper/Versions/(The latest version)
4) Execute this command to build the binary: x86_64-w64-mingw32-gcc -m64 -shared -O2 -std=c11 -o dxgw.dll dxgi_win7.c dxgi_win7.def -ldxgi -ld2d1 -loleaut32 -static

***Can I make my own fork and modify some files of this project ?***

Yes, but please give credits. If you want to help me adding new features and increasing the support, you can add me on Discord: roti_poulet-

***Tested games that does work:***
- SunTemple Unreal Engine 4 (D3D12 mode)
- Resident Evil Village/8 [Moving the window makes the game crash, but I got ingame and everything seems to work perfectly fine]

Made with love from France :D
