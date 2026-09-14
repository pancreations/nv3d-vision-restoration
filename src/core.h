#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vision {
inline constexpr double nvidiaBaseDelayUs = 4774.25;
inline constexpr double minimumShutterUs = 250.0;
// NVIDIA emitter schedule, from the disassembled RAM firmware (docs/EMITTER.md):
// - the 4-byte value in every 0xAA eye command tells the emitter how far away its next
//   period boundary is; the emitter phase-locks its own free-running period timer to it,
// - the X register delays the open IR token after that boundary; the close token follows
//   after the first token and the Y countdown (Y - 196 us); token/lens latency
//   is not included in the user-facing command-window estimate,
// - if that sequence is still running at the next boundary the whole next eye is skipped.
// The user-facing phase is the effective window start after the command (the predicted
// vblank of the commanded frame), continuous over the complete two-eye cycle. nvidiaSchedule() splits it
// into boundary distance and X so both stay inside safe firmware ranges.
// The split can put the window in the following period: eyePacket compensates
// that period's eye parity, rather than discarding the cycle with modulo alone.
inline constexpr double nvidiaBoundaryMarginUs = 1000.0;   // command-to-boundary distance stays in [margin, period-margin]
inline constexpr double nvidiaDelayCenterUs = 1300.0;      // X sits in [center-margin, center+margin]
inline constexpr double nvidiaSequenceGuardUs = 1000.0;    // tokens + close delay + safety after X + shutter
inline constexpr double nvidiaLockBiasUs = 6.0;            // firmware adds 73 ticks (12 MHz) per command in steady state
struct NvidiaSchedule { double boundaryUs; double delayUs; double openAfterCommandUs; };
double nvidiaBandCorrectionUs(double refresh);
NvidiaSchedule nvidiaSchedule(double refresh, double phaseUs);
double nvidiaLegacyPhaseUs(double refresh, double registerPhaseUs);
enum class Eye : int { Left, Right, Black };
enum class Sequence : int { Alternating, BlackInsertion, Repeated };
enum class Packing : int { SideBySide, TopBottom };
enum class Encoding : int { SRGB, LinearScRGB, PQ2020 };
// How the panel lights its rows. A sample-and-hold panel shows every row continuously, so
// a row carries the previous image until the scan rewrites it. A strobed panel (LightBoost,
// black frame insertion, "Motion Clearness", scanning/pulsed backlight) lights the panel
// only during a pulse after the scan, so the clean window is that pulse.
enum class Illumination : int { SampleAndHold, Strobed };
struct Settings {
    std::string name = "New calibration";
    std::string displayId;
    std::string connection;
    std::string emitterId="unassigned", emitterFirmware="unassigned";
    int width = 3840, height = 2160;
    double refresh = 120;
    bool hdr = false, swapEyes = false;
    Sequence sequence = Sequence::Alternating;
    double phaseUs = 0, leftUs = 1500, rightUs = 1500;
    float depth = 0.035f, convergence = 0, peakNits = 400;
    // Multiplies the presented eye image. A black-frame or repeated sequence lights each eye on
    // fewer refreshes, so the picture is dimmer; this spends the display's remaining headroom on
    // it. Values above 1 clip to white in SDR and reach for the panel's peak in HDR.
    float imageGain = 1;
    // Right eye window offset relative to the left eye (microseconds). Applied through a per-frame
    // X/Y register write on the NVIDIA emitter, so each eye gets its own delay and duration.
    float rightOffsetUs = 0;
    // Stereo area: the fraction of the output height that carries the image (the rest is
    // black) and the vertical center of that band. A panel rewrites its rows top to bottom
    // over most of a refresh; only rows that have finished show one eye. A shorter band spans
    // less of that scan and leaves a longer settled window for the shutter.
    float bandHeight = 1, bandCenter = .5f;
    // Panel settling time after a row is rewritten, and an optional measured scan time that
    // overrides the signal timing (a TV may re-time its panel), both in microseconds.
    double panelResponseUs = 1000, panelScanUs = 0;
    // Panel illumination. For a strobed panel the pulse starts strobeStartUs after the start
    // of each refresh's scan and lasts strobeLengthUs. Both come from a sweep or a camera.
    Illumination illumination = Illumination::SampleAndHold;
    double strobeStartUs = 0, strobeLengthUs = 1500;
    // Measured start of the active scan relative to the presentation timestamp DXGI reports
    // for the refresh (microseconds; the timestamp sits inside the blanking interval).
    // Zero until the vblank probe measures it.
    double scanStartUs = 0;
    bool glassesConfirmed = false, eyeConfirmed = false;
    std::string assessment = "Not assessed";
    std::string monitorNotes;
    bool validated = false;
};
struct Slot { Eye eye; bool trigger; bool pairBoundary; };
unsigned cycleLength(Sequence sequence);
Slot sequenceSlot(Sequence sequence, uint64_t index, bool swap);
// DXGI reports the actual display refresh, including composed presentation.
uint64_t refreshForPresent(uint32_t present, uint32_t observedPresent, uint64_t observedRefresh);
uint32_t presentForRefresh(uint64_t refresh, uint32_t observedPresent, uint64_t observedRefresh);
double periodUs(double refresh);
double phaseCycleUs(double refresh, Sequence sequence);
double wrapPhase(double phase, double period);
double wrapSignedPhase(double phase, double period);
double calibrationMaxShutterUs(double refresh);
double nvidiaMaxShutterUs(double refresh);
// The emitter runs one period per stereo slot pair: one display refresh for Left/Right, two for
// the four-slot sequences. The shutter may stay open across that whole period, which is how a
// black-frame sequence recovers its light, so every shutter limit follows this rate, not the
// display's refresh.
double sequenceEmitterHz(double refresh, Sequence sequence);
// Settled window of the rows inside the stereo area, in microseconds after the vblank of the
// eye's first refresh: rows refresh top to bottom over scanUs, settle panelResponseUs later,
// and hold that eye until the next image's scan reaches the band's top row.
struct SettledWindow { double openUs; double closeUs; };
double eyeHoldUs(double refresh, Sequence sequence);
SettledWindow settledWindow(double refresh, Sequence sequence, double scanUs, double bandHeight, double bandCenter, double panelResponseUs);
double suggestedPhaseUs(double refresh, Sequence sequence, const SettledWindow& window, double shutterUs);
double bandHeightForShutter(double refresh, Sequence sequence, double scanUs, double shutterUs, double panelResponseUs, double marginUs);
// Numeric illumination model. Every row of the stereo area is followed through one stereo
// cycle: it receives each refresh's content when the scan reaches it, fades to it over the
// panel response, and is lit continuously (sample and hold) or only during the strobe pulse.
// A shutter [phase, phase + shutter], measured from the start of the eye's first refresh,
// collects the eye's own light and the other eye's light; leakage is their ratio. Black
// frames and repeated frames are handled by the same model, which is why software black
// frame insertion removes the other eye from the screen during the scan.
struct PanelTiming { double scanUs=8100, responseUs=200; Illumination illumination=Illumination::SampleAndHold; double strobeStartUs=0, strobeLengthUs=1500; };
struct LeakageEstimate {
    double brightness=0;                    // own light collected / (shutter time x rows), 0..1
    double top=1, center=1, bottom=1, mean=1; // other-eye light / own light per band third and overall
};
LeakageEstimate estimateLeakage(double refresh, Sequence sequence, const PanelTiming& panel, double bandHeight, double bandCenter, double phaseUs, double shutterUs);
// Phase (model origin) with the least leakage for this shutter; brightness breaks ties.
double bestModelPhaseUs(double refresh, Sequence sequence, const PanelTiming& panel, double bandHeight, double bandCenter, double shutterUs);
// Largest band (10 % steps down to 10 %) whose best phase keeps the mean leakage within limit.
double bandHeightForLeakage(double refresh, Sequence sequence, const PanelTiming& panel, double bandCenter, double shutterUs, double leakageLimit);
// Widest shutter (up to maxShutterUs) whose best phase still keeps the mean leakage within the
// limit, chosen for the most light actually collected rather than the largest lit fraction of
// the shutter. In a black-frame sequence a row is lit for one refresh and dark for the rest, so
// a shutter spanning the scan stagger collects every row's whole lit period without leaking.
double brightestShutterUs(double refresh, Sequence sequence, const PanelTiming& panel, double bandHeight, double bandCenter, double leakageLimit, double maxShutterUs);
// The emitter command is sent at the presentation timestamp of the triggering refresh, which
// is the eye's first refresh except for Left/Left/Right/Right (second). The model's origin is
// the start of the active scan: scanStartUs after that timestamp. USB latency delays the
// command's arrival. These convert between the model phase and the emitter phase.
double triggerOffsetUs(double refresh, Sequence sequence);
double emitterPhaseFromModel(double modelPhaseUs, double refresh, Sequence sequence, double scanStartUs, double usbLatencyUs);
double modelPhaseFromEmitter(double emitterPhaseUs, double refresh, Sequence sequence, double scanStartUs, double usbLatencyUs);
void applyTimingPreset(Settings& settings, unsigned hz);
bool refreshRatesMatch(double presetHz, double displayHz);
void validate(const Settings& settings);
std::vector<uint8_t> timingPacket(double refresh, double phaseUs, double durationUs);
Eye nvidiaCommandEye(Eye frameEye, double refresh, double phaseUs);
std::array<uint8_t, 8> eyePacket(Eye eye, double refresh, double phaseUs);
struct NvidiaEyeTiming { double delayUs; double durationUs; };
NvidiaEyeTiming nvidiaEyeTiming(double refresh, double phaseUs, Eye eye, double leftUs, double rightUs, double rightOffsetUs);
// 12-byte write of X and Y only (offset 4 of the timing block), sent per frame for per-eye timing.
std::vector<uint8_t> eyeTimingPacket(double refresh, double delayUs, double durationUs);
void saveProfile(const std::filesystem::path& path, const Settings& settings);
Settings loadProfile(const std::filesystem::path& path);
std::string profileKey(const Settings& settings);

// Refresh clock from retrospective presentation statistics. A least-squares line through the
// last refreshes (up to a window) gives the period and the vblank phase; each new sample's
// distance from that line is the presentation jitter. This estimator never claims optical lock.
struct TimingTracker {
    double period = 0, epoch = 0;      // epoch = fitted time of lastRefresh
    uint64_t lastRefresh = 0;
    unsigned samples = 0;
    double jitterRmsUs = 0, jitterMaxUs = 0, lastResidualUs = 0; // sample minus fit
    bool observe(uint64_t refresh, double qpcSeconds);
    double predict(uint64_t refresh) const;
    void reset();
private:
    static constexpr unsigned window = 240;
    std::vector<std::pair<uint64_t,double>> history_;
    double sumResidualSq_ = 0; unsigned residualCount_ = 0;
    void refit();
};
struct FirmwareBlock { uint16_t address; std::vector<uint8_t> data; };
std::vector<FirmwareBlock> parseFirmware(std::span<const uint8_t> data);
std::vector<uint8_t> extractFirmware(std::span<const uint8_t> driver);
std::vector<uint8_t> readBinary(const std::filesystem::path& path);
}
