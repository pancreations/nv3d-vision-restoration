#include "platform.h"
#include <algorithm>
#include <set>
#include <tuple>
namespace vision {
std::vector<Display> enumerateDisplays(){
    UINT np=0,nm=0;std::vector<DISPLAYCONFIG_PATH_INFO> paths;std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    for(int retry=0;retry<3;retry++){
        if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&np,&nm)!=ERROR_SUCCESS)break;
        paths.resize(np);modes.resize(nm);
        LONG rc=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&np,paths.data(),&nm,modes.data(),nullptr);
        if(rc==ERROR_SUCCESS){paths.resize(np);modes.resize(nm);break;}
        paths.clear();modes.clear();if(rc!=ERROR_INSUFFICIENT_BUFFER)break;
    }
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create DXGI factory");
    std::vector<Display> result;
    for(UINT a=0;;a++){
        ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 ad{};adapter->GetDesc1(&ad);
        for(UINT o=0;;o++){
            ComPtr<IDXGIOutput> output;if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_OUTPUT_DESC od{};if(FAILED(output->GetDesc(&od)) || !od.AttachedToDesktop)continue;
            Display d;d.gdiName=od.DeviceName;d.rect=od.DesktopCoordinates;d.monitor=od.Monitor;d.adapterLuid=ad.AdapterLuid;d.gpu=utf8(ad.Description);d.name=utf8(d.gdiName);d.id=d.name;
            DEVMODEW current{};current.dmSize=sizeof(current);EnumDisplaySettingsW(d.gdiName.c_str(),ENUM_CURRENT_SETTINGS,&current);
            d.width=current.dmPelsWidth;d.height=current.dmPelsHeight;d.refresh=current.dmDisplayFrequency;
            for(const auto& p:paths){
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),p.sourceInfo.adapterId,p.sourceInfo.id};
                if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS || d.gdiName!=source.viewGdiDeviceName)continue;
                DISPLAYCONFIG_TARGET_DEVICE_NAME target{};target.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,sizeof(target),p.targetInfo.adapterId,p.targetInfo.id};
                if(DisplayConfigGetDeviceInfo(&target.header)==ERROR_SUCCESS){d.name=utf8(target.monitorFriendlyDeviceName);d.id=utf8(target.monitorDevicePath);}
                d.connection=std::to_string(int(p.targetInfo.outputTechnology))+":"+std::to_string(p.targetInfo.id);
                if(p.targetInfo.refreshRate.Denominator)d.refresh=double(p.targetInfo.refreshRate.Numerator)/p.targetInfo.refreshRate.Denominator;
                if(p.targetInfo.modeInfoIdx<modes.size() && modes[p.targetInfo.modeInfoIdx].infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET){
                    auto& signal=modes[p.targetInfo.modeInfoIdx].targetMode.targetVideoSignalInfo;
                    if(signal.vSyncFreq.Denominator)d.refresh=double(signal.vSyncFreq.Numerator)/signal.vSyncFreq.Denominator;
                    d.activeLines=signal.activeSize.cy;d.totalLines=signal.totalSize.cy;
                    if(d.totalLines&&d.refresh>0)d.scanUs=1e6/d.refresh*double(d.activeLines)/double(d.totalLines);
                }
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};color.header={DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,sizeof(color),p.targetInfo.adapterId,p.targetInfo.id};
                if(DisplayConfigGetDeviceInfo(&color.header)==ERROR_SUCCESS){d.hdrSupported=color.advancedColorSupported;d.hdrEnabled=color.advancedColorEnabled;}
                DISPLAYCONFIG_SDR_WHITE_LEVEL white{};white.header={DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,sizeof(white),p.targetInfo.adapterId,p.targetInfo.id};
                if(d.hdrEnabled&&DisplayConfigGetDeviceInfo(&white.header)==ERROR_SUCCESS&&white.SDRWhiteLevel)d.sdrWhiteNits=80.f*white.SDRWhiteLevel/1000.f;
            }
            ComPtr<IDXGIOutput6> out6;
            if(SUCCEEDED(output.As(&out6))){DXGI_OUTPUT_DESC1 ext{};if(SUCCEEDED(out6->GetDesc1(&ext)))d.maxNits=ext.MaxLuminance;}
            std::set<std::tuple<DWORD,DWORD,DWORD>> unique;
            for(DWORD i=0;;i++){
                DEVMODEW m{};m.dmSize=sizeof(m);if(!EnumDisplaySettingsW(d.gdiName.c_str(),i,&m))break;
                if(m.dmBitsPerPel!=32 || (m.dmDisplayFlags&DM_INTERLACED) || m.dmDisplayFrequency<60)continue;
                if(unique.emplace(m.dmPelsWidth,m.dmPelsHeight,m.dmDisplayFrequency).second)d.modes.push_back(m);
            }
            std::sort(d.modes.begin(),d.modes.end(),[](auto& x,auto& y){return std::tie(x.dmPelsWidth,x.dmPelsHeight,x.dmDisplayFrequency)>std::tie(y.dmPelsWidth,y.dmPelsHeight,y.dmDisplayFrequency);});
            result.push_back(std::move(d));
        }
    }return result;
}
void DisplayModeGuard::apply(const Display& d,const DEVMODEW& selected){
    restore();DEVMODEW target=selected;target.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY|DM_BITSPERPEL;
    if(ChangeDisplaySettingsExW(d.gdiName.c_str(),&target,nullptr,CDS_TEST,nullptr)!=DISP_CHANGE_SUCCESSFUL)throw std::runtime_error("Windows rejected this display mode.");
    original_={};original_.dmSize=sizeof(original_);if(!EnumDisplaySettingsW(d.gdiName.c_str(),ENUM_CURRENT_SETTINGS,&original_))throw std::runtime_error("Cannot record original display mode.");
    if(ChangeDisplaySettingsExW(d.gdiName.c_str(),&target,nullptr,CDS_FULLSCREEN,nullptr)!=DISP_CHANGE_SUCCESSFUL)throw std::runtime_error("Could not apply display mode.");
    name_=d.gdiName;changed_=true;
}
void DisplayModeGuard::restore(){if(changed_){ChangeDisplaySettingsExW(name_.c_str(),&original_,nullptr,0,nullptr);changed_=false;}}
}
