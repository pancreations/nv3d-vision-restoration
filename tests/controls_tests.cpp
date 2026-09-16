#include "lcd_ui.h"
#include <imgui.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace vision;
namespace {
void require(bool pass,const char* message){if(!pass)throw std::runtime_error(message);}
}
int main(){try{
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={800,700};io.DeltaTime=1.f/60;
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    Settings s;s.phaseUs=4000;s.leftUs=s.rightUs=1500;s.sequence=Sequence::Repeated;
    bool fine=false,hdrAvailable=true,aiDesktop=false;ImVec2 timingOrigin{},imageOrigin{};
    auto frame=[&]{
        ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({700,650});
        ImGui::Begin("Controls",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
        timingOrigin=ImGui::GetCursorScreenPos();drawTimingControls(s,0,33332,3000,fine);
        imageOrigin=ImGui::GetCursorScreenPos();drawImageControls(s,hdrAvailable,aiDesktop);
        ImGui::End();ImGui::Render();
    };
    auto click=[&](float x,float y){io.AddMousePosEvent(x,y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();};
    frame();frame();
    const float row=ImGui::GetFrameHeightWithSpacing(),half=ImGui::GetFrameHeight()/2;
    auto imageSlider=[&](int index){click(480,imageOrigin.y+row*(index+1)+half);};
    // A real mouse click must change the same fields that drive the output.
    click(460,timingOrigin.y+row+ImGui::GetStyle().CellPadding.y+half);
    require(s.phaseUs!=4000&&s.leftUs==1500&&s.rightUs==1500,"phase click must affect phase alone");
    const auto phase=s.phaseUs;
    click(460,timingOrigin.y+row*2+ImGui::GetStyle().CellPadding.y+half);
    require(s.leftUs!=1500&&s.leftUs==s.rightUs&&s.phaseUs==phase,"shutter click must change both durations, preserving phase");
    const auto shutter=s.leftUs;
    imageSlider(0);require(s.imageGain>1&&s.phaseUs==phase&&s.leftUs==shutter,"brightness must work without retiming the glasses");
    // An invalid LCD draft must not disable independent image controls.
    s.lcd.enabled=true;s.lcd.settleUs=8000;s.lcd.durationUs=8000;const auto lcd=s.lcd;
    s.imageGain=1;imageSlider(0);require(s.imageGain>1&&s.lcd==lcd,"pending LCD timing cannot block brightness");
    imageSlider(2);require(s.blackFloor>0&&s.lcd==lcd,"black-level slider must affect image only");
    imageSlider(3);require(s.depth!=Settings{}.depth&&s.lcd==lcd,"depth slider remains accessible");
    imageSlider(4);require(s.convergence!=0&&s.lcd==lcd,"convergence remains accessible for any input");
    imageSlider(1);require(s.peakNits==400,"HDR highlights disabled in SDR");
    // Checkbox follows the section heading on the same line.
    click(imageOrigin.x+ImGui::CalcTextSize("BRIGHTNESS / HDR").x+ImGui::GetStyle().ItemSpacing.x+5,imageOrigin.y+half);
    require(s.hdr,"HDR checkbox must change output setting");
    imageSlider(1);require(s.peakNits!=400&&s.lcd==lcd,"HDR highlight slider must work without altering timing");
    hdrAvailable=false;s.hdr=false;frame();
    click(imageOrigin.x+ImGui::CalcTextSize("BRIGHTNESS / HDR").x+ImGui::GetStyle().ItemSpacing.x+5,imageOrigin.y+half);
    require(!s.hdr,"HDR cannot be enabled on an unavailable output");
    aiDesktop=true;const float sceneDepth=s.depth,alignment=s.convergence,oldStrength=s.screen.separation,oldPlane=s.screen.convergence;
    imageSlider(3);imageSlider(4);
    require(s.screen.separation!=oldStrength&&s.screen.convergence!=oldPlane,"AI controls must edit the actual generated depth and screen plane");
    require(s.depth==sceneDepth&&s.convergence==alignment,"AI controls must not edit shape depth or add a second alignment");
    ImGui::DestroyContext();std::cout<<"PASS: live timing/image controls, independent LCD edits, and AI depth/plane controls target the correct settings.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
