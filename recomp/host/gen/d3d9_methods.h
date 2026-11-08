// D3D9 ABI method names and 32-bit argument counts from the public SDK declarations.
// Source: microsoft/win32metadata, generation/WinSDK/RecompiledIdlHeaders/shared/d3d9.h
// Header SHA256: a48e8132befefce1d14a8d0ffc277e7149c3c4b45d5f7427e2ac07605d65889e
#pragma once
static const char *const IDirect3D9_names[] = {"QueryInterface","AddRef","Release","RegisterSoftwareDevice","GetAdapterCount","GetAdapterIdentifier","GetAdapterModeCount","EnumAdapterModes","GetAdapterDisplayMode","CheckDeviceType","CheckDeviceFormat","CheckDeviceMultiSampleType","CheckDepthStencilMatch","CheckDeviceFormatConversion","GetDeviceCaps","GetAdapterMonitor","CreateDevice"};
static const unsigned char IDirect3D9_nargs[] = {2,0,0,1,0,3,2,4,2,5,6,6,5,4,3,1,6};
enum { IDirect3D9_COUNT = 17 };
static const char *const IDirect3DCubeTexture9_names[] = {"QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority","GetPriority","PreLoad","GetType","SetLOD","GetLOD","GetLevelCount","SetAutoGenFilterType","GetAutoGenFilterType","GenerateMipSubLevels","GetLevelDesc","GetCubeMapSurface","LockRect","UnlockRect","AddDirtyRect"};
static const unsigned char IDirect3DCubeTexture9_nargs[] = {2,0,0,1,4,3,1,1,0,0,0,1,0,0,1,0,0,2,3,5,2,2};
enum { IDirect3DCubeTexture9_COUNT = 22 };
static const char *const IDirect3DDevice9_names[] = {"QueryInterface","AddRef","Release","TestCooperativeLevel","GetAvailableTextureMem","EvictManagedResources","GetDirect3D","GetDeviceCaps","GetDisplayMode","GetCreationParameters","SetCursorProperties","SetCursorPosition","ShowCursor","CreateAdditionalSwapChain","GetSwapChain","GetNumberOfSwapChains","Reset","Present","GetBackBuffer","GetRasterStatus","SetDialogBoxMode","SetGammaRamp","GetGammaRamp","CreateTexture","CreateVolumeTexture","CreateCubeTexture","CreateVertexBuffer","CreateIndexBuffer","CreateRenderTarget","CreateDepthStencilSurface","UpdateSurface","UpdateTexture","GetRenderTargetData","GetFrontBufferData","StretchRect","ColorFill","CreateOffscreenPlainSurface","SetRenderTarget","GetRenderTarget","SetDepthStencilSurface","GetDepthStencilSurface","BeginScene","EndScene","Clear","SetTransform","GetTransform","MultiplyTransform","SetViewport","GetViewport","SetMaterial","GetMaterial","SetLight","GetLight","LightEnable","GetLightEnable","SetClipPlane","GetClipPlane","SetRenderState","GetRenderState","CreateStateBlock","BeginStateBlock","EndStateBlock","SetClipStatus","GetClipStatus","GetTexture","SetTexture","GetTextureStageState","SetTextureStageState","GetSamplerState","SetSamplerState","ValidateDevice","SetPaletteEntries","GetPaletteEntries","SetCurrentTexturePalette","GetCurrentTexturePalette","SetScissorRect","GetScissorRect","SetSoftwareVertexProcessing","GetSoftwareVertexProcessing","SetNPatchMode","GetNPatchMode","DrawPrimitive","DrawIndexedPrimitive","DrawPrimitiveUP","DrawIndexedPrimitiveUP","ProcessVertices","CreateVertexDeclaration","SetVertexDeclaration","GetVertexDeclaration","SetFVF","GetFVF","CreateVertexShader","SetVertexShader","GetVertexShader","SetVertexShaderConstantF","GetVertexShaderConstantF","SetVertexShaderConstantI","GetVertexShaderConstantI","SetVertexShaderConstantB","GetVertexShaderConstantB","SetStreamSource","GetStreamSource","SetStreamSourceFreq","GetStreamSourceFreq","SetIndices","GetIndices","CreatePixelShader","SetPixelShader","GetPixelShader","SetPixelShaderConstantF","GetPixelShaderConstantF","SetPixelShaderConstantI","GetPixelShaderConstantI","SetPixelShaderConstantB","GetPixelShaderConstantB","DrawRectPatch","DrawTriPatch","DeletePatch","CreateQuery"};
static const unsigned char IDirect3DDevice9_nargs[] = {2,0,0,0,0,0,1,1,2,1,3,3,1,2,2,0,1,4,4,2,1,3,2,8,9,7,6,6,8,8,4,2,2,2,5,3,6,2,2,1,1,0,0,6,2,2,2,1,1,1,1,2,2,2,2,2,2,2,2,2,0,1,1,1,2,2,3,3,3,3,1,2,2,1,1,1,1,1,0,1,0,3,6,4,8,6,2,1,1,1,1,2,1,1,3,3,3,3,3,3,4,4,2,2,1,1,2,1,1,3,3,3,3,3,3,3,3,1,2};
enum { IDirect3DDevice9_COUNT = 119 };
static const char *const IDirect3DIndexBuffer9_names[] = {"QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority","GetPriority","PreLoad","GetType","Lock","Unlock","GetDesc"};
static const unsigned char IDirect3DIndexBuffer9_nargs[] = {2,0,0,1,4,3,1,1,0,0,0,4,0,1};
enum { IDirect3DIndexBuffer9_COUNT = 14 };
static const char *const IDirect3DPixelShader9_names[] = {"QueryInterface","AddRef","Release","GetDevice","GetFunction"};
static const unsigned char IDirect3DPixelShader9_nargs[] = {2,0,0,1,2};
enum { IDirect3DPixelShader9_COUNT = 5 };
static const char *const IDirect3DQuery9_names[] = {"QueryInterface","AddRef","Release","GetDevice","GetType","GetDataSize","Issue","GetData"};
static const unsigned char IDirect3DQuery9_nargs[] = {2,0,0,1,0,0,1,3};
enum { IDirect3DQuery9_COUNT = 8 };
static const char *const IDirect3DStateBlock9_names[] = {"QueryInterface","AddRef","Release","GetDevice","Capture","Apply"};
static const unsigned char IDirect3DStateBlock9_nargs[] = {2,0,0,1,0,0};
enum { IDirect3DStateBlock9_COUNT = 6 };
static const char *const IDirect3DSurface9_names[] = {"QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority","GetPriority","PreLoad","GetType","GetContainer","GetDesc","LockRect","UnlockRect","GetDC","ReleaseDC"};
static const unsigned char IDirect3DSurface9_nargs[] = {2,0,0,1,4,3,1,1,0,0,0,2,1,3,0,1,1};
enum { IDirect3DSurface9_COUNT = 17 };
static const char *const IDirect3DSwapChain9_names[] = {"QueryInterface","AddRef","Release","Present","GetFrontBufferData","GetBackBuffer","GetRasterStatus","GetDisplayMode","GetDevice","GetPresentParameters"};
static const unsigned char IDirect3DSwapChain9_nargs[] = {2,0,0,5,1,3,1,1,1,1};
enum { IDirect3DSwapChain9_COUNT = 10 };
static const char *const IDirect3DTexture9_names[] = {"QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority","GetPriority","PreLoad","GetType","SetLOD","GetLOD","GetLevelCount","SetAutoGenFilterType","GetAutoGenFilterType","GenerateMipSubLevels","GetLevelDesc","GetSurfaceLevel","LockRect","UnlockRect","AddDirtyRect"};
static const unsigned char IDirect3DTexture9_nargs[] = {2,0,0,1,4,3,1,1,0,0,0,1,0,0,1,0,0,2,2,4,1,1};
enum { IDirect3DTexture9_COUNT = 22 };
static const char *const IDirect3DVertexBuffer9_names[] = {"QueryInterface","AddRef","Release","GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority","GetPriority","PreLoad","GetType","Lock","Unlock","GetDesc"};
static const unsigned char IDirect3DVertexBuffer9_nargs[] = {2,0,0,1,4,3,1,1,0,0,0,4,0,1};
enum { IDirect3DVertexBuffer9_COUNT = 14 };
static const char *const IDirect3DVertexDeclaration9_names[] = {"QueryInterface","AddRef","Release","GetDevice","GetDeclaration"};
static const unsigned char IDirect3DVertexDeclaration9_nargs[] = {2,0,0,1,2};
enum { IDirect3DVertexDeclaration9_COUNT = 5 };
static const char *const IDirect3DVertexShader9_names[] = {"QueryInterface","AddRef","Release","GetDevice","GetFunction"};
static const unsigned char IDirect3DVertexShader9_nargs[] = {2,0,0,1,2};
enum { IDirect3DVertexShader9_COUNT = 5 };
