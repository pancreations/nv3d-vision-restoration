#pragma once
#include "core.h"
#include "platform.h"
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
};
struct SourceStatus { std::string message="Built-in calibration patterns";uint64_t frames=0,dropped=0;double lastFrame=0;bool running=false; };
enum class SourceKind { Patterns, Image, Window, Spout, SharedFrames };
struct SourceConfig { SourceKind kind=SourceKind::Patterns;std::filesystem::path file;HWND window=nullptr;std::string sender;Packing packing=Packing::SideBySide;Encoding encoding=Encoding::SRGB;bool captureHDR=false; };
class StereoSource {
    mutable std::mutex mutex_;std::shared_ptr<StereoFrame> latest_;SourceStatus status_;std::jthread thread_;
public:
    ~StereoSource(){stop();}
    void start(const SourceConfig& config,LUID adapter);
    void stop();
    std::shared_ptr<StereoFrame> latest()const;
    SourceStatus status()const;
private:
    void run(std::stop_token stop,SourceConfig config,LUID adapter);
};
std::vector<std::pair<HWND,std::string>> captureWindows(HWND exclude);
struct FrameChannelReport { bool outputRunning=false,emitterReady=false;std::string message; };
struct FrameChannelLink { bool present=false,active=false,visible=false;RECT view{};uint32_t pid=0;std::string name; };
// Host end of the frame channel's control block (frame_channel.h), polled from the UI loop.
class FrameChannelHost {
    HANDLE mapping_=nullptr;void* view_=nullptr;double nextOpen_=0,staleSince_=0;
    void close();
public:
    FrameChannelHost()=default;FrameChannelHost(const FrameChannelHost&)=delete;FrameChannelHost& operator=(const FrameChannelHost&)=delete;
    ~FrameChannelHost(){close();}
    FrameChannelLink poll(const FrameChannelReport& report);
};
}
