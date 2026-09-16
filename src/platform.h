#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <vector>
namespace vision {
template<class T> using ComPtr=Microsoft::WRL::ComPtr<T>;
inline void check(HRESULT hr,const char* operation){if(FAILED(hr))throw std::runtime_error(std::string(operation)+" (HRESULT "+std::to_string(uint32_t(hr))+")");}
inline double qpc(){LARGE_INTEGER v,f;QueryPerformanceCounter(&v);QueryPerformanceFrequency(&f);return double(v.QuadPart)/double(f.QuadPart);}
inline std::string utf8(const std::wstring& s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr);return r;}
inline std::wstring wide(const std::string& s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),r.data(),n);return r;}
struct Display {
    std::wstring gdiName;
    std::string name,id,connection,gpu;
    RECT rect{};
    LUID adapterLuid{};
    HMONITOR monitor{};
    unsigned width=0,height=0;
    double refresh=0;
    // Signal timing: active rows are transmitted over scanUs of each refresh; the remainder is
    // vertical blanking. A panel may re-time its own scan, so this is the signal, not a measurement.
    unsigned activeLines=0,totalLines=0;double scanUs=0;
    bool hdrSupported=false,hdrEnabled=false;
    float sdrWhiteNits=80;
    float maxNits=0;
    std::vector<DEVMODEW> modes;
};
std::vector<Display> enumerateDisplays();
class DisplayModeGuard {
    std::wstring name_; DEVMODEW original_{}; bool changed_=false;
public:
    ~DisplayModeGuard(){restore();}
    void apply(const Display& display,const DEVMODEW& mode);
    void restore();
};
}
