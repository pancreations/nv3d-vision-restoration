// SPDX-License-Identifier: GPL-3.0-or-later
// All graphics initialization occurs on the first API call, outside DllMain.
// Load the unchanged community renderer only after installing our output hooks.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <filesystem>
#include <mutex>
#include <intrin.h>
extern "C" __declspec(dllexport) const char VisionStereoLoaderIdentity[]="VisionStereoLoader.v1";
namespace {
std::once_flag once;HMODULE geo=nullptr,output=nullptr;
void initialize(){std::call_once(once,[]{
    wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);auto dir=std::filesystem::path(exe).parent_path();
    output=LoadLibraryW((dir/L"VisionStereo11.dll").c_str());if(!output)return;
    auto prepare=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(output,"VisionStereoBootstrap"));if(!prepare||FAILED(prepare()))return;
    // Geo11 forces load_library_redirect=0 whenever proxy_d3d11 is set, and this
    // adapter always sets it. That redirect is what normally makes the game load
    // Geo11's NVAPI wrapper instead of the driver's; current drivers dropped 3D
    // Vision, so without the wrapper Geo11 cannot enter Direct Mode and the game
    // renders mono. Load the wrapper by full path before Geo11 initializes: it
    // then finds its own module, and later LoadLibrary("nvapi.dll") calls resolve
    // to this already-loaded module by base name.
    const wchar_t* wrapper=sizeof(void*)==8?L"nvapi64.dll":L"nvapi.dll";
    std::error_code wrapperError;
    if(std::filesystem::exists(dir/wrapper,wrapperError))LoadLibraryW((dir/wrapper).c_str());
    geo=LoadLibraryW((dir/L"VisionGeo11.dll").c_str());
    // Also cover games that created their DXGI factory before their device.
    // Geo11 now attaches around the output hooks installed by prepare().
    if(geo){IDXGIFactory* factory=nullptr;if(SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory),reinterpret_cast<void**>(&factory))))factory->Release();}
});}
HMODULE backend(void* caller){
    HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(caller),&module);
    // When loaded under a distinct filename, Geo11 hooks the loader's two
    // exports and calls their trampolines as its native backend. Forward that
    // return path to our output proxy, avoiding recursion/double wrapping.
    return module==geo?output:geo;
}
}
extern "C" HRESULT WINAPI VisionLoaderCreateDevice(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE sw,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context){
    initialize();auto module=backend(_ReturnAddress());auto fn=module?reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice")):nullptr;
    return fn?fn(adapter,type,sw,flags,levels,count,sdk,device,level,context):E_FAIL;
}
extern "C" HRESULT WINAPI VisionLoaderCreateDeviceAndSwapChain(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE sw,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** chain,ID3D11Device** device,D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context){
    initialize();auto module=backend(_ReturnAddress());auto fn=module?reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(module,"D3D11CreateDeviceAndSwapChain")):nullptr;
    return fn?fn(adapter,type,sw,flags,levels,count,sdk,desc,chain,device,level,context):E_FAIL;
}
