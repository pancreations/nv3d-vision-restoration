// SPDX-License-Identifier: GPL-3.0-or-later
// Optional VLC 3.x runtime: no SDK or VLC files are required to build/package.
#include "sources.h"
#include <malloc.h>
#include <cstring>
#include <chrono>

namespace vision {
namespace {
// Opaque C API objects and exact VLC 3.x ABI. Reject other major versions before
// resolving/using playback functions (VLC 4 changes several signatures).
struct libvlc_instance_t;struct libvlc_media_t;struct libvlc_media_player_t;
using Lock=void*(*)(void*,void**);
using Unlock=void(*)(void*,void*,void*const*);
using DisplayCallback=void(*)(void*,void*);
using Format=unsigned(*)(void**,char*,unsigned*,unsigned*,unsigned*,unsigned*);
using Cleanup=void(*)(void*);
struct Module {
    HMODULE handle=nullptr;
    ~Module(){if(handle)FreeLibrary(handle);}
    template<class T>T function(const char* name){auto address=GetProcAddress(handle,name);if(!address)throw std::runtime_error(std::string("VLC is missing ")+name);return reinterpret_cast<T>(address);}
};
std::filesystem::path vlcFolder(const std::filesystem::path& selected){
    if(!selected.empty())return std::filesystem::absolute(selected);
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,DWORD(std::size(exe)));
    std::vector<std::filesystem::path> candidates{std::filesystem::path(exe).parent_path()/L"vlc"};
    for(auto variable:{L"ProgramW6432",L"ProgramFiles",L"ProgramFiles(x86)"}){
        wchar_t folder[32768]{};DWORD n=GetEnvironmentVariableW(variable,folder,DWORD(std::size(folder)));
        if(n&&n<std::size(folder))candidates.emplace_back(std::filesystem::path(folder)/L"VideoLAN"/L"VLC");
    }
    for(const auto& folder:candidates)if(std::filesystem::is_regular_file(folder/L"libvlc.dll"))return folder;
    throw std::runtime_error("Install 64-bit VLC 3.x, or use Choose VLC... to select a portable vlc.exe.");
}
// VLC 3's vmem output calls Prepare/Display serially on its video-output thread.
// Keep its aligned write buffer separate from the complete, timed snapshot read
// by our GPU worker. Callbacks never touch D3D or the source's status mutex.
struct Pictures {
    std::mutex mutex;void* pixels=nullptr;std::vector<uint8_t> latest;
    unsigned width=0,height=0,pitch=0;uint64_t revision=0;bool invalid=false;Packing packing;
    explicit Pictures(Packing p):packing(p){}
    ~Pictures(){_aligned_free(pixels);}
    static unsigned format(void** opaque,char* chroma,unsigned* w,unsigned* h,unsigned* pitches,unsigned* lines) noexcept {
        auto& self=*static_cast<Pictures*>(*opaque);std::lock_guard lock(self.mutex);
        if(!*w||!*h||*w>8192||*h>8192||uint64_t(*w)*(*h)>32000000||
           (self.packing==Packing::SideBySide?*w%2:*h%2)){self.invalid=true;return 0;}
        const unsigned pitch=(*w*4+31)&~31u;const size_t bytes=size_t(pitch)*(*h);
        try{self.latest.resize(bytes);}catch(...){self.invalid=true;return 0;}
        _aligned_free(self.pixels);self.pixels=_aligned_malloc(bytes,32);
        if(!self.pixels){self.invalid=true;return 0;}
        self.width=*w;self.height=*h;self.pitch=pitch;
        std::memcpy(chroma,"RV32",4);pitches[0]=pitch;lines[0]=*h;return 1;
    }
    static void* lock(void* opaque,void** planes) noexcept {
        auto& self=*static_cast<Pictures*>(opaque);planes[0]=self.pixels;return nullptr;
    }
    static void display(void* opaque,void*) noexcept {
        auto& self=*static_cast<Pictures*>(opaque);std::lock_guard lock(self.mutex);
        if(self.pixels){std::memcpy(self.latest.data(),self.pixels,self.latest.size());++self.revision;}
    }
};
}
void StereoSource::runVlc(std::stop_token stop,const SourceConfig& config,ID3D11Device* device,ID3D11DeviceContext* context,const Publish& publish){
    if(!std::filesystem::is_regular_file(config.file))throw std::runtime_error("Choose a stereo movie file before starting VLC playback.");
    const auto folder=vlcFolder(config.vlcDirectory);
    constexpr DWORD flags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    Module core,vlc;core.handle=LoadLibraryExW((folder/L"libvlccore.dll").c_str(),nullptr,flags);
    if(!core.handle)throw std::runtime_error("Cannot load VLC. Select a complete 64-bit VLC 3.x installation with its plugins folder.");
    vlc.handle=LoadLibraryExW((folder/L"libvlc.dll").c_str(),nullptr,flags);
    if(!vlc.handle)throw std::runtime_error("Cannot load 64-bit libvlc.dll from the selected VLC folder.");
    auto version=vlc.function<const char*(*)()>("libvlc_get_version");
    if(std::strncmp(version(),"3.",2)!=0)throw std::runtime_error("Direct movie playback requires VLC 3.x (64-bit). This VLC version has a different API.");
    #define VLC_FUNCTION(name,result,...) auto name=vlc.function<result(*)(__VA_ARGS__)>("libvlc_" #name)
    auto create=vlc.function<libvlc_instance_t*(*)(int,const char*const*)>("libvlc_new");
    VLC_FUNCTION(release,void,libvlc_instance_t*);
    VLC_FUNCTION(media_new_path,libvlc_media_t*,libvlc_instance_t*,const char*);
    VLC_FUNCTION(media_release,void,libvlc_media_t*);
    VLC_FUNCTION(media_player_new_from_media,libvlc_media_player_t*,libvlc_media_t*);
    VLC_FUNCTION(media_player_release,void,libvlc_media_player_t*);
    VLC_FUNCTION(media_player_play,int,libvlc_media_player_t*);
    VLC_FUNCTION(media_player_stop,void,libvlc_media_player_t*);
    VLC_FUNCTION(media_player_set_pause,void,libvlc_media_player_t*,int);
    VLC_FUNCTION(media_player_set_time,void,libvlc_media_player_t*,int64_t);
    VLC_FUNCTION(media_player_get_time,int64_t,libvlc_media_player_t*);
    VLC_FUNCTION(media_player_get_length,int64_t,libvlc_media_player_t*);
    VLC_FUNCTION(media_player_get_state,int,libvlc_media_player_t*);
    VLC_FUNCTION(media_player_is_seekable,int,libvlc_media_player_t*);
    VLC_FUNCTION(audio_set_volume,int,libvlc_media_player_t*,int);
    VLC_FUNCTION(video_set_callbacks,void,libvlc_media_player_t*,Lock,Unlock,DisplayCallback,void*);
    VLC_FUNCTION(video_set_format_callbacks,void,libvlc_media_player_t*,Format,Cleanup);
    #undef VLC_FUNCTION
    const char* arguments[]{"--ignore-config","--no-video-title-show","--no-sub-autodetect-file","--avcodec-hw=none",config.mediaMute?"--no-audio":"--no-audio-time-stretch"};
    auto* instance=create(int(std::size(arguments)),arguments);
    if(!instance)throw std::runtime_error("VLC initialization failed. Check that its plugins folder is present.");
    struct InstanceGuard {libvlc_instance_t* value;decltype(release) destroy;~InstanceGuard(){destroy(value);}} instanceGuard{instance,release};
    auto* media=media_new_path(instance,utf8(std::filesystem::absolute(config.file).wstring()).c_str());
    if(!media)throw std::runtime_error("VLC could not open the movie.");
    auto* player=media_player_new_from_media(media);media_release(media);
    if(!player)throw std::runtime_error("VLC could not create the movie player.");
    Pictures pictures(config.packing);
    // The player must stop/release before callback storage or DLLs are destroyed,
    // including on errors, source changes, seeks and application shutdown.
    struct PlayerGuard {libvlc_media_player_t* value;decltype(media_player_stop) stop;decltype(media_player_release) destroy;~PlayerGuard(){stop(value);destroy(value);}} playerGuard{player,media_player_stop,media_player_release};
    video_set_callbacks(player,Pictures::lock,nullptr,Pictures::display,&pictures);
    video_set_format_callbacks(player,Pictures::format,nullptr);
    int volume;{std::lock_guard l(mutex_);volume=mediaVolume_;}audio_set_volume(player,volume);
    if(media_player_play(player)!=0)throw std::runtime_error("VLC failed to start movie playback.");
    ComPtr<ID3D11Texture2D> texture;unsigned width=0,height=0;uint64_t revision=0;bool paused=false;
    double lastStatus=0,start=qpc();
    while(!stop.stop_requested()){
        bool requestedPause;int requestedVolume;int64_t seek;
        {std::lock_guard l(mutex_);requestedPause=mediaPaused_;requestedVolume=mediaVolume_;seek=mediaSeekMs_;mediaSeekMs_=-1;}
        if(requestedPause!=paused){media_player_set_pause(player,requestedPause?1:0);paused=requestedPause;}
        if(requestedVolume!=volume){audio_set_volume(player,requestedVolume);volume=requestedVolume;}
        if(seek>=0&&media_player_is_seekable(player))media_player_set_time(player,seek);
        // Upload only a complete frame delivered at VLC's presentation time.
        // Release callback storage before waiting for shared-texture GPU copies.
        bool updated=false;
        {std::lock_guard l(pictures.mutex);
            if(pictures.invalid)throw std::runtime_error("VLC movie dimensions must split evenly into two eyes, be at most 8192 per side and 32 million pixels.");
            if(pictures.revision!=revision){
                if(width!=pictures.width||height!=pictures.height){
                    width=pictures.width;height=pictures.height;texture.Reset();
                    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
                    d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                    check(device->CreateTexture2D(&d,nullptr,&texture),"Create VLC video texture");
                }
                context->UpdateSubresource(texture.Get(),0,nullptr,pictures.latest.data(),pictures.pitch,0);
                revision=pictures.revision;updated=true;
            }
        }
        if(updated)publish(texture.Get(),width,height,qpc(),Encoding::SRGB,0,0);
        if(qpc()-lastStatus>.1){
            lastStatus=qpc();const int state=media_player_get_state(player);
            const auto time=media_player_get_time(player),length=media_player_get_length(player);
            const bool seekable=media_player_is_seekable(player)!=0;
            if(state==7)throw std::runtime_error("VLC could not decode this movie. Check that the file plays in VLC 3.x.");
            std::lock_guard l(mutex_);status_.mediaTimeMs=std::max(int64_t(0),time);status_.mediaLengthMs=std::max(int64_t(0),length);
            status_.mediaPaused=state==4;status_.mediaSeekable=seekable;
            status_.message=state==6?"VLC movie ended; holding last stereo pair":state==4?"VLC movie paused; holding last stereo pair":revision?"VLC stereo movie (SDR)":"VLC opening movie...";
            if(state==6){status_.running=false;break;}
            if(!revision&&qpc()-start>15)throw std::runtime_error("VLC produced no video frames. Choose a video file with a supported stereo layout.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}
