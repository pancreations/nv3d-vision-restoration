#pragma once
#include "platform.h"
#include "core.h"
#include "sources.h"
#include "emitter.h"
#include <mutex>
#include <thread>
namespace vision {
class Surface {
public:
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain2> swap;ComPtr<ID3D11RenderTargetView> target;HANDLE waitable=nullptr;
    unsigned width=0,height=0;bool hdr=false;
    ~Surface();
    void create(HWND window,const LUID* adapter,bool useHDR);
    void resize(unsigned w,unsigned h);
    void bind();
};
struct RenderStatus {
    bool running=false,preview=true,locked=false,timingPassed=false;std::string message="Output stopped";
    uint64_t presents=0,misses=0,resyncs=0,sourcePair=0;double elapsed=0,measuredHz=0,lastResyncSec=-1;
    double lastIntervalMs=0,maxIntervalMs=0;std::vector<double> intervals;unsigned leadBins[6]{};bool composed=false;uint64_t modeChanges=0; // trigger lead time ms: <0, 0-2, 2-4, 4-6, 6-8, >8
    // Frame jitter: scatter of the display's vblank timestamps around the fitted refresh clock,
    // and the standard deviation of the presenter's own present-to-present interval.
    double vblankJitterRmsUs=0,vblankJitterMaxUs=0,presentJitterUs=0;unsigned clockSamples=0;
    // Slips (misses) in the last whole second, and the GPU scheduling class the process obtained.
    unsigned slipsLastSecond=0;std::string gpuPriority;
    // Windows reports the output window as occluded whenever another window covers it, which is the
    // normal state while the viewer plays a side-by-side game with the pad focused on the game. The
    // presenter keeps running and keeps the emitter locked; this only reports the condition.
    bool occluded=false;
};
// One measurement of the display's vertical blanking against the presentation timestamps,
// made with the kernel scan-line counter (docs/TIMING-CALIBRATION.md).
struct VblankMeasurement {
    bool ok=false;std::string message;
    double signalHz=0,measuredHz=0,vblankLengthUs=0,syncAfterVblankStartUs=0,scanStartAfterSyncUs=0;
    double dxgiJitterRmsUs=0,dxgiJitterMaxUs=0,scanlineJitterRmsUs=0,wakeLatencyP50Us=0,wakeLatencyMaxUs=0;
    unsigned dxgiSamples=0,scanlineSamples=0;
};
VblankMeasurement measureVblank(const Display& display,double seconds);
class Presenter {
    mutable std::mutex mutex_;std::jthread worker_;Settings settings_;int pattern_=0;
    RenderStatus status_;uint64_t revision_=0;bool paused_=false,overlay_=false,resyncRequested_=false;
    StereoSource& source_;Emitter& emitter_;
public:
    Presenter(StereoSource& s,Emitter& e):source_(s),emitter_(e){}
    ~Presenter(){stop();}
    void start(HWND window,const Display& display,const Settings& settings,bool preview,int pattern);
    void stop();void configure(const Settings& settings,int pattern);void pause(bool paused);
    void resync(); // manual re-lock, requested from the UI or the global hotkey
    void showPhase(bool on); // draw the current phase value in both eye frames (calibration sweep)
    RenderStatus status()const;
    void exportReport(const std::filesystem::path& path,const Settings& settings,const std::string& source)const;
private:
    void run(std::stop_token stop,HWND window,Display display,bool preview);
};
bool runGpuSelfTest(const std::filesystem::path& reportDirectory);
void saveSurfacePng(Surface& surface,const std::filesystem::path& path);
}
