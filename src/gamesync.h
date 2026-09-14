// SPDX-License-Identifier: GPL-3.0-or-later
// Host side of the in-game hook: keeps the shared-memory channel open while the app runs,
// and when a hooked game starts presenting frame-sequential stereo, times the emitter from
// the game's own DXGI frame statistics using the current calibration profile. The test
// presenter and this class never drive the emitter at the same time.
#pragma once
#include "emitter.h"
#include "platform.h"
#include <mutex>
#include <thread>
namespace vision {
struct GameSyncStatus {
    bool hosting=false,hooked=false,driving=false,composed=false,onDisplay=true,sequenceLimited=false;
    std::string message="Game hook idle",game;
    uint32_t pid=0,api=0,width=0,height=0;
    double measuredHz=0,frameAge=0;
    uint64_t frames=0,triggers=0,misses=0,guesses=0,samples=0;
};
class GameSync {
    Emitter& emitter_;mutable std::mutex mutex_;std::jthread worker_;Settings settings_;bool enabled_=true;HMONITOR monitor_{};GameSyncStatus status_;uint64_t generation_=0;
public:
    explicit GameSync(Emitter& e):emitter_(e){}
    ~GameSync(){stop();}
    void start();
    void stop();
    void enable(bool on);          // false while the built-in test output owns the emitter
    void configure(const Settings& s,HMONITOR display);
    GameSyncStatus status()const;
private:
    void run(std::stop_token stop);
};
// Copies ReShade (add-on build) as dxgi.dll plus the hook next to a game executable.
std::string installGameHook(const std::filesystem::path& gameExe,const std::filesystem::path& addon);
std::string removeGameHook(const std::filesystem::path& gameExe,const std::filesystem::path& addon);
}
