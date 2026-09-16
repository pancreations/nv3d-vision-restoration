#include "lcd_ui.h"
#include <imgui.h>
#include <algorithm>
#include <fstream>
#include <iomanip>

namespace vision {
namespace {
bool milliseconds(const char* name,double& us,double low,double high,bool fine) {
    double ms=us/1000.,lo=low/1000.,hi=high/1000.;
    ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1);ImGui::PushID(name);
    bool changed=ImGui::SliderScalar("##value",ImGuiDataType_Double,&ms,&lo,&hi,"%.2f ms",ImGuiSliderFlags_AlwaysClamp);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Ctrl+click to type. Use - / + for %.2f ms steps.",fine?.01:.1);
    ImGui::TableSetColumnIndex(2);if(ImGui::SmallButton("-")){ms-=fine?.01:.1;changed=true;}
    ImGui::TableSetColumnIndex(3);if(ImGui::SmallButton("+")){ms+=fine?.01:.1;changed=true;}
    ImGui::PopID();if(changed)us=std::clamp(ms,lo,hi)*1000.;return changed;
}
void sliderColumns(float dpi) {
    ImGui::TableSetupColumn("Parameter",ImGuiTableColumnFlags_WidthFixed,128*dpi);
    ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Minus",ImGuiTableColumnFlags_WidthFixed,19*dpi);
    ImGui::TableSetupColumn("Plus",ImGuiTableColumnFlags_WidthFixed,19*dpi);
}
void timeline(const LcdExposure& e,double guard) {
    const float width=ImGui::GetContentRegionAvail().x;auto at=ImGui::GetCursorScreenPos();auto* d=ImGui::GetWindowDrawList();
    ImGui::Dummy({width,38});
    for(int eye=0;eye<2;++eye){float y=at.y+eye*19;d->AddRectFilled({at.x,y},{at.x+width,y+13},IM_COL32(53,61,72,255),3);
        auto x=[&](double us){return at.x+width*float(std::clamp(us/e.periodUs,0.,1.));};
        d->AddRectFilled({at.x,y},{x(guard),y+13},IM_COL32(146,91,49,255));
        d->AddRectFilled({x(e.periodUs-guard),y},{at.x+width,y+13},IM_COL32(146,91,49,255));
        if(e.valid)d->AddRectFilled({x(e.openUs[eye]),y},{x(e.closeUs[eye]),y+13},IM_COL32(97,180,143,255),2);
    }
    ImGui::TextDisabled("L %.2f - %.2f ms   R %.2f - %.2f ms",e.openUs[0]/1000,e.closeUs[0]/1000,e.openUs[1]/1000,e.closeUs[1]/1000);
}
}
bool drawImageControls(Settings& s,bool hdrAvailable,bool aiDesktop) {
    ImGui::PushID("image_controls");
    ImGui::TextUnformatted("BRIGHTNESS / HDR");ImGui::SameLine();
    ImGui::BeginDisabled(!hdrAvailable);
    bool changed=ImGui::Checkbox("HDR output",&s.hdr);
    ImGui::EndDisabled();
    if(!hdrAvailable&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Enable HDR for this display in Windows to use HDR output.");
    ImGui::SetNextItemWidth(-1);
    changed|=ImGui::SliderFloat("##brightness",&s.imageGain,1,8,"Image brightness %.2fx",ImGuiSliderFlags_AlwaysClamp);
    if(ImGui::IsItemHovered())ImGui::SetTooltip(s.hdr?"Brightens the presented image using HDR headroom. Ctrl+click to type.":"Brightens the presented image; SDR highlights clip at white. Ctrl+click to type.");
    ImGui::BeginDisabled(!s.hdr);ImGui::SetNextItemWidth(-1);
    changed|=ImGui::SliderFloat("##highlights",&s.peakNits,80,1000,"HDR highlights %.0f nits",ImGuiSliderFlags_AlwaysClamp);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("Peak brightness of the HDR brightness test pattern.");
    ImGui::EndDisabled();
    float floor=s.blackFloor*100;ImGui::SetNextItemWidth(-1);
    if(ImGui::SliderFloat("##black_floor",&floor,0,30,"Black level %.1f %%",ImGuiSliderFlags_AlwaysClamp)){s.blackFloor=floor/100;changed=true;}
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Raises the image's black level. Zero preserves black.");
    ImGui::SetNextItemWidth(-1);
    if(aiDesktop){
        float depth=s.screen.separation*100;
        if(ImGui::SliderFloat("##depth",&depth,0,10,"AI depth strength %.2f %%",ImGuiSliderFlags_AlwaysClamp)){s.screen.separation=depth/100;changed=true;}
        ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderFloat("##convergence",&s.screen.convergence,.01f,1,"Screen plane %.2f",ImGuiSliderFlags_AlwaysClamp);
    }else{
        changed|=ImGui::SliderFloat("##depth",&s.depth,0,.12f,"Shape depth %.3f",ImGuiSliderFlags_AlwaysClamp);
        float convergence=s.convergence*100;ImGui::SetNextItemWidth(-1);
        if(ImGui::SliderFloat("##convergence",&convergence,-5,5,"Convergence %+.2f %%",ImGuiSliderFlags_AlwaysClamp)){s.convergence=convergence/100;changed=true;}
    }
    ImGui::PopID();return changed;
}
bool drawTimingControls(Settings& s,double phaseMin,double phaseMax,double shutterMax,bool& fine) {
    bool changed=false;ImGui::PushID("primary_timing");
    ImGui::TextUnformatted("PHASE / SHUTTER");ImGui::SameLine();ImGui::Checkbox("0.01 ms steps",&fine);
    if(ImGui::BeginTable("timing",4,ImGuiTableFlags_SizingStretchProp)){
        sliderColumns(ImGui::GetFontSize()/16.f);
        double& phase=s.lcd.enabled?s.lcd.phaseUs:s.phaseUs;
        double& shutter=s.lcd.enabled?s.lcd.durationUs:s.leftUs;
        changed|=milliseconds("Phase",phase,phaseMin,phaseMax,fine);
        if(milliseconds("Shutter",shutter,minimumShutterUs,shutterMax,fine)){
            if(!s.lcd.enabled)s.rightUs=s.leftUs;
            changed=true;
        }
        ImGui::EndTable();
    }
    changed|=ImGui::Checkbox("Swap eyes",&s.swapEyes);
    ImGui::PopID();return changed;
}
void prepareLcdCalibration(LcdCalibrationUi& ui,const Settings& s) {
    const auto key=profileKey(s)+"/"+sequencePattern(s.sequence)+(s.swapEyes?"/swapped":"/normal");
    if(ui.context!=key){bool fine=ui.fine;ui={};ui.fine=fine;ui.context=key;}
    if(!sameLcdAperture(ui.ratedTiming,s.lcd)){ui.ratings.fill(0);ui.ratedTiming=s.lcd;ui.seen=false;}
}
bool startLcdSweep(LcdCalibrationUi& ui,Settings& s,double hz,double now) {
    prepareLcdCalibration(ui,s);
    ui.candidates=lcdSettleSweep(s.lcd,hz,s.signalScanUs);ui.candidate=0;
    if(ui.candidates.empty()){ui.message="No valid sweep: reduce phase, guard or scan compensation.";return false;}
    s.lcd=ui.candidates.front();ui.sweeping=true;ui.lastAdvance=now;ui.seen=false;return true;
}
bool tickLcdCalibration(LcdCalibrationUi& ui,Settings& s,double hz,double now,bool running) {
    prepareLcdCalibration(ui,s);
    if(running&&!ui.sweeping){ui.seen=true;ui.seenHz=hz;}
    if(!s.lcd.enabled||!running){ui.sweeping=false;return false;}
    if(!ui.sweeping||now-ui.lastAdvance<3)return false;
    if(++ui.candidate>=ui.candidates.size()){ui.sweeping=false;ui.message="Sweep complete. Rate this timing or repeat the sweep.";return false;}
    auto next=ui.candidates[ui.candidate];next.target=s.lcd.target;
    if(!lcdExposure(next,hz,s.signalScanUs).valid){ui.sweeping=false;ui.message="Refresh changed; restart the sweep.";return false;}
    s.lcd=next;ui.lastAdvance=now;return true;
}
bool drawLcdCalibration(LcdCalibrationUi& ui,Settings& s,const Settings& active,double measuredHz,bool running,float dpi,const std::filesystem::path& reports,double now) {
    prepareLcdCalibration(ui,s);bool changed=false;
    const auto& applied=active.lcd;double hz=apertureWindowHz(measuredHz,s.sequence);
    ImGui::TextUnformatted("TIMING");ImGui::SameLine();ImGui::Checkbox("0.01 ms steps",&ui.fine);
    ImGui::SameLine();changed|=ImGui::Checkbox("Swap eyes",&s.swapEyes);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(6*dpi,3*dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(7*dpi,3*dpi));
    if(ImGui::BeginTable("lcd_timing",4,ImGuiTableFlags_SizingStretchProp)) {
        sliderColumns(dpi);
        changed|=milliseconds("Settle delay",s.lcd.settleUs,0,8000,ui.fine);
        changed|=milliseconds("Shutter duration",s.lcd.durationUs,250,8000,ui.fine);
        changed|=milliseconds("Global phase",s.lcd.phaseUs,-1e6/hz,1e6/hz,ui.fine);
        changed|=milliseconds("Guard / blanking",s.lcd.guardUs,250,std::min(2000.,1e6/hz/2),ui.fine);
        changed|=milliseconds("Left correction",s.lcd.leftAdjustUs,-1000,1000,ui.fine);
        changed|=milliseconds("Right correction",s.lcd.rightAdjustUs,-1000,1000,ui.fine);
        ImGui::EndTable();
    }
    changed|=ImGui::Checkbox("Scanout compensation",&s.lcd.compensateScanout);
    ImGui::SameLine();ImGui::TextDisabled("0 ms = signal (%.2f ms)",s.signalScanUs/1000);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("The reference row shifts the shutter window. All rows are still exposed together. Signal scan time may differ from internal panel scanout.");
    if(ImGui::BeginTable("lcd_scan",4,ImGuiTableFlags_SizingStretchProp)) {
        sliderColumns(dpi);
        changed|=milliseconds("Scan time",s.lcd.scanoutUs,0,50000,ui.fine);
        ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Reference row");
        ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1);double lo=0,hi=1;
        changed|=ImGui::SliderScalar("##reference_row",ImGuiDataType_Double,&s.lcd.referencePosition,&lo,&hi,"%.2f (top 0 / bottom 1)",ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
    if(changed){ui.sweeping=false;ui.message.clear();}
    auto e=lcdExposure(s.lcd,hz,s.signalScanUs);
    bool pending=!sameLcdAperture(s.lcd,applied)||s.sequence!=active.sequence;
    if(pending||!e.valid){
        ImGui::TextColored({1,.75f,.25f,1},"PENDING - glasses keep the last valid timing");
        if(!e.valid)ImGui::TextWrapped("%s",e.message.c_str());
        if(ImGui::Button("Restore applied values")){s.lcd=applied;s.sequence=active.sequence;hz=apertureWindowHz(measuredHz,s.sequence);changed=true;pending=false;e=lcdExposure(s.lcd,hz,s.signalScanUs);}
        ImGui::TextDisabled("Applied: settle %.2f / duration %.2f / phase %.2f ms",applied.settleUs/1000,applied.durationUs/1000,applied.phaseUs/1000);
    }
    const auto appliedExposure=lcdExposure(applied,apertureWindowHz(measuredHz,active.sequence),s.signalScanUs);
    if(appliedExposure.periodUs>0)timeline(appliedExposure,applied.guardUs);
    if(appliedExposure.valid)ImGui::TextDisabled("Applied exposure %.1f%%; each lens %.1f%% total duty.",100*appliedExposure.durationUs/appliedExposure.periodUs,100*appliedExposure.durationUs/(1e6/measuredHz*cycleLength(active.sequence)));
    ImGui::Separator();ImGui::TextUnformatted("TEST REGION");
    const char* targets[]{"Full panel","Top","Center","Bottom"};
    for(int i=0;i<4;++i){if(i)ImGui::SameLine();if(ImGui::RadioButton(targets[i],s.lcd.target==i)){s.lcd.target=i;changed=true;}}
    ImGui::BeginDisabled(!running||!e.valid);
    if(ImGui::Button(ui.sweeping?"Pause sweep / keep timing":"Sweep settle delay")) {
        if(ui.sweeping)ui.sweeping=false;
        else changed|=startLcdSweep(ui,s,hz,now);
    }
    ImGui::SameLine();if(ImGui::Button("Next candidate")&&!ui.candidates.empty()){ui.sweeping=false;ui.candidate=(ui.candidate+1)%ui.candidates.size();int target=s.lcd.target;s.lcd=ui.candidates[ui.candidate];s.lcd.target=target;changed=true;}
    ImGui::EndDisabled();
    if(ui.sweeping)ImGui::Text("Candidate %zu / %zu (3 seconds each)",ui.candidate+1,ui.candidates.size());
    if(!ui.message.empty())ImGui::TextWrapped("%s",ui.message.c_str());
    (void)reports;return changed;
}
bool drawLcdObservations(LcdCalibrationUi& ui,Settings& s,double hz,const std::filesystem::path& reports) {
    prepareLcdCalibration(ui,s);bool changed=false;
    const char* targets[]{"Full panel","Top","Center","Bottom"};
    ImGui::TextUnformatted("CROSSTALK CHECK");
    ImGui::TextWrapped("For a full-panel check, use the fullscreen test. Rate each lens after viewing; the intended target must remain visible.");
    {

        ImGui::BeginDisabled(ui.sweeping||!ui.seen);
        if(ImGui::BeginTable("lcd_ratings",3,ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Region");ImGui::TableSetupColumn("Left lens");ImGui::TableSetupColumn("Right lens");ImGui::TableHeadersRow();
            for(int row=0;row<3;++row){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(targets[row+1]);
                for(int eye=0;eye<2;++eye){ImGui::TableSetColumnIndex(eye+1);ImGui::PushID(eye*3+row);ImGui::SetNextItemWidth(-1);ImGui::Combo("##rating",&ui.ratings[eye*3+row],"Not rated\0Clear\0Mild ghost\0Strong ghost\0");ImGui::PopID();}}
            ImGui::EndTable();
        }
        const auto windowFrames=1+sequenceBlack(s.sequence);
        LcdObservation observation{s.lcd,(ui.seenHz>0?ui.seenHz:hz)*windowFrames,s.signalScanUs,ui.ratings,windowFrames};
        ImGui::BeginDisabled(!lcdObservationComplete(observation));
        if(ImGui::Button("Record timing")){ui.observations.push_back(observation);ui.message="Observation recorded for both lenses and all three regions.";}
        ImGui::EndDisabled();ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(ui.observations.empty());
        if(ImGui::Button("Use best observed")){const auto* best=&ui.observations.front();for(auto& o:ui.observations)if(lcdObservationBetter(o,*best))best=&o;s.lcd=best->timing;ui.sweeping=false;changed=true;ui.message="Applied the least crosstalk observed across all regions; brightness breaks ties.";}
        ImGui::SameLine();if(ImGui::Button("Save results")) {
            std::filesystem::create_directories(reports);std::ofstream f(reports/"lcd-calibration.csv");
            f<<"refresh_hz,window_refreshes,settle_us,duration_us,phase_us,left_adjust_us,right_adjust_us,guard_us,scan_compensate,scan_us,reference_row,left_top,left_center,left_bottom,right_top,right_center,right_bottom\n"<<std::setprecision(12);
            for(auto& o:ui.observations){const auto& t=o.timing;f<<o.refresh<<','<<o.windowFrames<<','<<t.settleUs<<','<<t.durationUs<<','<<t.phaseUs<<','<<t.leftAdjustUs<<','<<t.rightAdjustUs<<','<<t.guardUs<<','<<t.compensateScanout<<','<<(t.scanoutUs>0?t.scanoutUs:o.signalScanUs)<<','<<t.referencePosition;for(int v:o.crosstalk)f<<','<<v;f<<'\n';}
            ui.message=f?"Saved reports/lcd-calibration.csv":"Could not save observations.";
        }
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Clear")){ui.observations.clear();ui.ratings.fill(0);}
        ImGui::TextDisabled("%zu saved trials. Separation first, brightness second.",ui.observations.size());
    }
    if(!ui.message.empty())ImGui::TextWrapped("%s",ui.message.c_str());
    return changed;
}
}
