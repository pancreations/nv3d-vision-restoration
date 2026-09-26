// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_view.h"
#include <iostream>
#include <stdexcept>
using namespace vision;
static void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(){try{
    // A calibrated panel profile carries every shutter compensation at once.
    Settings s;
    s.hdr=true;s.bandHeight=.4f;s.bandCenter=.3f;s.convergence=.03f;s.imageGain=3.5f;s.blackFloor=.12f;
    s.guardLevel=.4f;s.cancelCrosstalk=true;s.leakProfile.fill(.5f);s.sequence=Sequence::BlackInsertion;
    s.phaseUs=3690;s.leftUs=2190;s.rightUs=2190;s.depth=.05f;s.peakNits=800;s.refresh=240;
    const auto view=streamViewSettings(s);
    const auto steady=steadyViewSettings(s);
    require(steady.hdr==s.hdr&&steady.imageGain==1&&steady.bandHeight==1&&steady.convergence==0&&steady.guardLevel==0,"Explicit steady diagnostic preserves output format and removes shutter compensation");
    require(!view.hdr,"A recorder takes SDR");
    require(view.bandHeight==1&&view.bandCenter==.5f,"The stream view shows the whole picture, not the stereo band");
    require(view.convergence==0,"A single eye has no eye shift");
    require(view.imageGain==1&&view.blackFloor==0&&view.guardLevel==0,"Panel brightness compensation is not streamed");
    require(!view.cancelCrosstalk&&view.leakProfile[0]==0&&view.leakProfile[7]==0,"Ghost subtraction belongs to the glasses");
    require(view.sequence==Sequence::Alternating,"The stream view has no black slots to fill");
    // Everything the presenter needs for its own output must survive untouched: the
    // stream view reads the calibration, it must never be a way to edit it.
    require(view.phaseUs==s.phaseUs&&view.leftUs==s.leftUs&&view.rightUs==s.rightUs,"Shutter timing is not the stream view's business");
    require(view.refresh==s.refresh&&view.depth==s.depth&&view.peakNits==s.peakNits,"Display and scene settings are carried through");

    // "Left" must mean the picture the left lens is given, so a profile that swaps
    // the eyes for the panel swaps what the stream view shows as well.
    Settings normal;normal.swapEyes=false;
    require(streamViewEye(normal,false)==Eye::Left&&streamViewEye(normal,true)==Eye::Right,"Unswapped eyes select directly");
    Settings swapped;swapped.swapEyes=true;
    require(streamViewEye(swapped,false)==Eye::Right&&streamViewEye(swapped,true)==Eye::Left,"A swapped profile swaps the streamed eye");
    require(streamViewSettings(swapped).swapEyes==false,"The eye is chosen once, not swapped twice");
    require(streamViewEye(normal,false)!=Eye::Black,"The stream view never shows a black slot");

    require(streamViewRate>0&&streamViewRate<=60,"The stream view must not outrun what a recorder encodes");
    std::cout<<"stream view: ok\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<"\n";return 1;}}
