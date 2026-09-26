// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
namespace vision {
// The 2D stream view: one eye, every refresh, for a screen recorder or a voice
// chat's screen share.
//
// Frame-sequential 3D is unwatchable to anyone capturing the display. The output
// alternates left and right (and, in a black-frame sequence, black) once per
// refresh; a recorder sampling that at 30 or 60 frames a second lands on a
// different eye each time, so the picture jumps sideways by the parallax and
// goes dark whenever it catches a black slot. Only the glasses resolve the
// sequence. A capture never can.
//
// So the recorder is given a separate, ordinary window that holds one eye
// continuously. None of the shutter compensations belong in it: the stereo band,
// the eye shift, the crosstalk subtraction and the brightness gain all exist to
// make an alternating image look right through the glasses, and all of them look
// wrong on a flat picture.
inline Settings steadyViewSettings(const Settings& s){
    Settings v=s;
    v.bandHeight=1;v.bandCenter=.5f;  // the whole picture, never the stereo band
    v.convergence=0;                  // the per-eye shift has no meaning for a single eye
    v.imageGain=1;v.blackFloor=0;v.guardLevel=0;
    v.cancelCrosstalk=false;v.leakProfile.fill(0.f);
    v.sequence=Sequence::Alternating; // this window has no black slots to fill
    v.swapEyes=false;                 // the eye is chosen here, not swapped by the panel calibration
    return v;
}
inline Settings streamViewSettings(const Settings& s){
    auto v=steadyViewSettings(s);
    v.hdr=false; // Recorders take SDR; the steady display retains its HDR format.
    return v;
}
// Which eye the stream view shows. The eye swap that corrects the glasses is
// applied so that "left" means the same picture the left lens is given.
inline Eye streamViewEye(const Settings& s,bool rightEye){
    return (rightEye!=s.swapEyes)?Eye::Right:Eye::Left;
}
// Stream frames per second. The view exists to be re-encoded by something that
// tops out near 60; drawing it faster spends GPU time the presenter needs.
inline constexpr double streamViewRate=60;
}
