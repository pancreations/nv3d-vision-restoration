// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace vision::elden {
// Camera layout reference: Logersnamed/FreecamMod, MIT, commit
// 99d44a75e9ebb316043635d4d8b98c007bffa7dd. See THIRD-PARTY.md.
struct alignas(16) Camera {
    std::byte prefix[0x10]{};
    float basis[4][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    float fov=1.0f, aspect=16.f/9.f, nearPlane=.1f, farPlane=1000.f;
};
static_assert(offsetof(Camera,basis)==0x10 && offsetof(Camera,fov)==0x50 && sizeof(Camera)==0x60);
inline bool valid(const Camera& c) {
    for(auto& row:c.basis)for(float v:row)if(!std::isfinite(v))return false;
    if(!std::isfinite(c.fov)||c.fov<.05f||c.fov>3.1f)return false;
    for(int a=0;a<3;++a){float len=0;for(int n=0;n<3;++n)len+=c.basis[a][n]*c.basis[a][n];if(std::abs(len-1.f)>.05f)return false;}
    for(int a=0;a<3;++a)for(int b=a+1;b<3;++b){float dot=0;for(int n=0;n<3;++n)dot+=c.basis[a][n]*c.basis[b][n];if(std::abs(dot)>.03f)return false;}
    return std::abs(c.basis[3][3]-1.f)<.05f;
}
inline Camera eyeCamera(Camera center,unsigned eye,float separation) {
    const float distance=(eye==0?-.5f:.5f)*separation;
    for(int n=0;n<3;++n)center.basis[3][n]+=center.basis[0][n]*distance;
    return center;
}
// Camera stamps, not the number of Present calls, identify rendered views.
// A duplicate/stale stamp cannot complete a pair. Reset abandons partial pairs.
struct PairClock {
    uint64_t lastStamp=0,leftStamp=0,pairs=0;
    bool pending=false,complete=false;
    enum class Action { Ignore,StageLeft,Complete };
    Action accept(uint64_t stamp,unsigned eye) {
        if(!stamp||stamp<=lastStamp)return Action::Ignore;
        lastStamp=stamp;
        if(eye==0){leftStamp=stamp;pending=true;return Action::StageLeft;}
        if(eye!=1||!pending||stamp!=leftStamp+1){pending=false;return Action::Ignore;}
        pending=false;complete=true;++pairs;return Action::Complete;
    }
    void reset(){pending=false;complete=false;leftStamp=0;}
};
}
