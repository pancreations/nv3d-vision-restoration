#pragma once
#include "platform.h"
#include "core.h"
#include "sources.h"
#include "emitter.h"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace vision {
class Surface {
public:
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain2> swap;ComPtr<ID3D11RenderTargetView> target;HANDLE waitable=nullptr;
    unsigned width=0,height=0;bool hdr=false;
    ~Surface();
    void create(HWND window,const LUID* adapter,bool useHDR,unsigned latency=3);
    // A second swap chain on an existing device, for the 2D stream view. Sharing the
    // presenter's device is what keeps the stream view off the shared-pair keyed mutex:
    // a second device would have to acquire pairs the presenter is still holding.
    void attach(ID3D11Device* existing,ID3D11DeviceContext* existingContext,HWND window,unsigned latency);
    void resize(unsigned w,unsigned h);
    void setHdr(bool useHDR);
    void bind();
private:
    void makeSwapChain(IDXGIFactory2* factory,HWND window,unsigned latency);
};
struct RenderStatus {
    bool running=false,preview=true,locked=false,timingPassed=false;std::string message="Output stopped";
    uint64_t presents=0,misses=0,resyncs=0,sourcePair=0;double elapsed=0,measuredHz=0,lastResyncSec=-1;
    uint64_t blackOnImage=0,clockReacquires=0;
    uint64_t clockOutliers=0,clockCorrections=0,statsDisjoints=0;
    double lastIntervalMs=0,maxIntervalMs=0;std::vector<double> intervals;unsigned leadBins[6]{};bool composed=false;uint64_t modeChanges=0; // trigger lead time ms: <0, 0-2, 2-4, 4-6, 6-8, >8
    // Frame jitter: scatter of the display's vblank timestamps around the fitted refresh clock,
    // and the standard deviation of the presenter's own present-to-present interval.
    double vblankJitterRmsUs=0,vblankJitterMaxUs=0,presentJitterUs=0;unsigned clockSamples=0;
    // Slips (misses) in the last whole second, and the GPU scheduling class the process obtained.
    unsigned slipsLastSecond=0;std::string gpuPriority;
    unsigned queueDepth=0;
    // 2D stream view: whether its swap chain is alive, how many pictures it has shown,
    // and why it is not running when it was asked for.
    bool streaming=false;uint64_t streamFrames=0;std::string streamMessage;
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
    HWND streamWindow_=nullptr;bool streamRightEye_=false,streamOpen_=false;std::condition_variable streamIdle_;
    StereoSource& source_;Emitter& emitter_;
public:
    struct Diagnostics {
        bool steadyTimingProbe=false; // One eye only, no black slots or emitter commands.
        bool simulateProbeTriggers=false; // Only accepted with a simulated emitter.
        unsigned queueDepth=0; // Zero selects the production refresh-based queue.
        std::filesystem::path trace;
    };
    Presenter(StereoSource& s,Emitter& e):source_(s),emitter_(e){}
    ~Presenter(){stop();}
    void start(HWND window,const Display& display,const Settings& settings,bool preview,int pattern);
    void start(HWND window,const Display& display,const Settings& settings,bool preview,int pattern,const Diagnostics& diagnostics);
    void stop();void configure(const Settings& settings,int pattern);void pause(bool paused);
    void resync(); // manual re-lock, requested from the UI or the global hotkey
    void showPhase(bool on); // draw the current phase value in both eye frames (calibration sweep)
    // 2D stream view. The presenter draws one eye into this second window because it
    // already holds the open stereo pair: a separate device would contend for the pair's
    // keyed mutex and make the presenter drop the frames it is holding. nullptr stops it.
    void streamTo(HWND window,bool rightEye);
    RenderStatus status()const;
    void exportReport(const std::filesystem::path& path,const Settings& settings,const std::string& source)const;
private:
    void run(std::stop_token stop,HWND window,Display display,bool preview,Diagnostics diagnostics);
};
bool runGpuSelfTest(const std::filesystem::path& reportDirectory);
void saveSurfacePng(Surface& surface,const std::filesystem::path& path);
// Writes an 8-bit or FP16 RGBA/BGRA texture as PNG (FP16 is clamped and sRGB encoded).
void saveTexturePng(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,const std::filesystem::path& path);
// Opens a published stereo pair on the given adapter and writes it as PNG (diagnostics).
void saveSharedFramePng(const StereoFrame& frame,const LUID& adapter,const std::filesystem::path& path);
}
