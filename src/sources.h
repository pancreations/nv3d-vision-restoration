#pragma once
#include "core.h"
#include "platform.h"
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
namespace vision {
struct StereoFrame {
    ComPtr<ID3D11Texture2D> texture;
    HANDLE sharedHandle=nullptr; // legacy shared handle: not closed with CloseHandle
    unsigned width=0,height=0;
    uint64_t pairId=0;double timestamp=0;
    Packing packing=Packing::SideBySide;Encoding encoding=Encoding::SRGB;
    bool alignmentApplied=false;float sdrWhiteLevel=1;
};
struct SourceStatus { std::string message="Built-in calibration patterns";uint64_t frames=0,dropped=0;double lastFrame=0;bool running=false;
    // Whole-screen conversion: network input size, last inference time, age of the depth in use, time
    // to draw both eyes, depth maps received, and the helper's own message (model, device or error).
    unsigned netWidth=0,netHeight=0;double depthMs=0,depthAgeMs=0,convertMs=0;uint64_t depthFrames=0;std::string depthMessage; };
enum class SourceKind { Patterns, Image, Window, Screen };
struct SourceConfig { SourceKind kind=SourceKind::Patterns;std::filesystem::path file;HWND window=nullptr;Packing packing=Packing::SideBySide;Encoding encoding=Encoding::SRGB;bool captureHDR=true;float sdrWhiteLevel=1;
    // Screen: the display to convert, the conversion settings, the depth helper and its model.
    HMONITOR monitor=nullptr;ScreenSettings screen;std::filesystem::path depthHelper,depthModel,depthLog; };
class StereoSource {
    mutable std::mutex mutex_;std::shared_ptr<StereoFrame> latest_;SourceStatus status_;std::jthread thread_;ScreenSettings screen_;uint64_t screenRevision_=0;
public:
    ~StereoSource(){stop();}
    void start(const SourceConfig& config,LUID adapter);
    void stop();
    std::shared_ptr<StereoFrame> latest()const;
    SourceStatus status()const;
    void configureScreen(const ScreenSettings& settings); // applies live to a running screen conversion
private:
    using Publish=std::function<void(ID3D11Texture2D*,unsigned,unsigned,double,Encoding,unsigned,unsigned)>;
    void run(std::stop_token stop,SourceConfig config,LUID adapter);
    void runScreen(std::stop_token stop,const SourceConfig& config,LUID adapter,ID3D11Device* device,ID3D11DeviceContext* context,const Publish& publish); // screen_source.cpp
};
std::vector<std::pair<HWND,std::string>> captureWindows(HWND exclude);
}
