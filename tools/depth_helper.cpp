// SPDX-License-Identifier: GPL-3.0-or-later
// VisionDepth: the depth network host for the whole-screen conversion. Runs Depth Anything V2
// (or any single-picture ONNX depth model with an NCHW RGB input) through ONNX Runtime on
// DirectML, in its own process at a low GPU scheduling class so its work never delays the
// stereo presenter. Pictures arrive and depth maps leave through the shared channel described
// in src/depth_channel.h; the app starts and stops this process.
//   VisionDepth --host <pid> --model <file.onnx> [--adapter <high>:<low>] [--log <file>]
//   VisionDepth --bench <runs> --model <file.onnx> [--adapter <high>:<low>]   times the network alone
#include "depth_channel.h"
#include "screen_depth.h"
#include "platform.h"
#include <d3d12.h>
#include <d3dkmthk.h>
#define DML_TARGET_VERSION_USE_LATEST
#include <DirectML.h>
#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace vision;
namespace {
std::ofstream g_log;
void logLine(const std::string& line){if(!g_log)return;SYSTEMTIME t;GetLocalTime(&t);char stamp[32];snprintf(stamp,sizeof stamp,"%02u:%02u:%02u.%03u ",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);g_log<<stamp<<line<<'\n';g_log.flush();}
uint32_t load32(uint32_t& v){return std::atomic_ref<uint32_t>(v).load(std::memory_order_acquire);}
void store32(uint32_t& v,uint32_t x){std::atomic_ref<uint32_t>(v).store(x,std::memory_order_release);}
float halfToFloat(uint16_t h){const unsigned e=(h>>10)&31,m=h&1023;const float v=e==0?m/1024.f*std::pow(2.f,-14.f):e==31?(m?NAN:INFINITY):(1+m/1024.f)*std::pow(2.f,float(int(e)-15));return h&0x8000?-v:v;}
uint16_t floatToHalf(float f){
    uint32_t x;memcpy(&x,&f,4);const uint32_t sign=(x>>16)&0x8000;int exp=int((x>>23)&0xff)-127+15;uint32_t man=x&0x7fffff;
    if(((x>>23)&0xff)==0xff)return uint16_t(sign|0x7c00|(man?0x200:0));
    if(exp<=0){if(exp<-10)return uint16_t(sign);man|=0x800000;const uint32_t shift=uint32_t(14-exp);uint32_t half=man>>shift;if((man>>(shift-1))&1)half++;return uint16_t(sign|half);}
    if(exp>=31)return uint16_t(sign|0x7c00);
    uint32_t half=uint32_t(exp<<10)|(man>>13);if(man&0x1000)half++;return uint16_t(sign|half);
}
[[noreturn]] void fail(const std::string& why){throw std::runtime_error(why);}
std::filesystem::path moduleDirectory(){wchar_t path[32768]{};const DWORD n=GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));if(!n||n==std::size(path))fail("Cannot locate VisionDepth.exe");return std::filesystem::path(path).parent_path();}
// ONNX Runtime on DirectML with one loaded model: BGRA picture in, relative depth out.
class Engine {
    const OrtApi* api_=nullptr;OrtEnv* env_=nullptr;OrtSessionOptions* options_=nullptr;OrtSession* session_=nullptr;OrtMemoryInfo* memInfo_=nullptr;OrtAllocator* allocator_=nullptr;char* inputName_=nullptr;char* outputName_=nullptr;
    ComPtr<ID3D12Device> device_;ComPtr<ID3D12CommandQueue> queue_;ComPtr<IDMLDevice> dml_;
    ONNXTensorElementDataType inputType_=ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;std::vector<int64_t> inputDims_;std::vector<float> inputF_;std::vector<uint16_t> inputH_;
    void ok(OrtStatus* status,const char* what){if(!status)return;std::string message=std::string(what)+": "+api_->GetErrorMessage(status);api_->ReleaseStatus(status);fail(message);}
public:
    std::string adapterName;double loadMs=0;NetSize net;
    ~Engine(){if(!api_)return;if(inputName_)api_->AllocatorFree(allocator_,inputName_);if(outputName_)api_->AllocatorFree(allocator_,outputName_);if(memInfo_)api_->ReleaseMemoryInfo(memInfo_);if(session_)api_->ReleaseSession(session_);if(options_)api_->ReleaseSessionOptions(options_);if(env_)api_->ReleaseEnv(env_);}
    bool fp16()const{return inputType_==ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;}
    // Loads the model for pictures of this aspect ratio. The model is opened once on the CPU to read
    // its input's names and dimensions, then on DirectML with every free dimension pinned to the
    // chosen size: with the shapes static, DirectML fuses the whole graph into one GPU program,
    // while a dynamic-shape graph runs op by op with CPU round trips (90 ms instead of a few).
    void init(const std::filesystem::path& model,const LUID* luid,unsigned sourceWidth,unsigned sourceHeight,unsigned maxSide){
        const auto dir=moduleDirectory();
        HMODULE ort=LoadLibraryExW((dir/L"onnxruntime.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!ort)fail("onnxruntime.dll is missing beside VisionDepth.exe");
        auto getBase=reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(ort,"OrtGetApiBase"));if(!getBase)fail("onnxruntime.dll exports no OrtGetApiBase");
        api_=getBase()->GetApi(ORT_API_VERSION);if(!api_)fail("onnxruntime.dll is older than the headers VisionDepth was built with");
        const OrtDmlApi* dml=nullptr;ok(api_->GetExecutionProviderApi("DML",ORT_API_VERSION,reinterpret_cast<const void**>(&dml)),"DirectML provider");
        // Direct3D 12 and DirectML on the display's adapter, so the network runs on the GPU that presents.
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");ComPtr<IDXGIAdapter1> adapter;
        if(luid){ComPtr<IDXGIAdapter> a;if(SUCCEEDED(factory->EnumAdapterByLuid(*luid,IID_PPV_ARGS(&a))))a.As(&adapter);}
        if(!adapter)for(UINT i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;i++){DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))break;adapter.Reset();}
        if(!adapter)fail("No GPU for DirectML");
        DXGI_ADAPTER_DESC1 ad{};adapter->GetDesc1(&ad);adapterName=utf8(ad.Description);
        check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device_)),"Direct3D 12 device");
        D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;check(device_->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue_)),"Command queue");
        HMODULE dmlModule=LoadLibraryExW((dir/L"DirectML.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!dmlModule)dmlModule=LoadLibraryW(L"DirectML.dll");if(!dmlModule)fail("DirectML.dll is missing beside VisionDepth.exe");
        using CreateDml=HRESULT(WINAPI*)(ID3D12Device*,DML_CREATE_DEVICE_FLAGS,REFIID,void**);auto createDml=reinterpret_cast<CreateDml>(GetProcAddress(dmlModule,"DMLCreateDevice"));if(!createDml)fail("DirectML.dll exports no DMLCreateDevice");
        check(createDml(device_.Get(),DML_CREATE_DEVICE_FLAG_NONE,IID_PPV_ARGS(&dml_)),"DirectML device");
        ok(api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"VisionDepth",&env_),"ONNX Runtime environment");
        ok(api_->GetAllocatorWithDefaultOptions(&allocator_),"Allocator");
        const auto loadStart=std::chrono::steady_clock::now();
        std::vector<std::string> dimNames;
        {   // First pass on the CPU: the input's name, type, dimensions and free-dimension names.
            OrtSessionOptions* probeOptions=nullptr;ok(api_->CreateSessionOptions(&probeOptions),"Probe options");ok(api_->SetSessionGraphOptimizationLevel(probeOptions,ORT_DISABLE_ALL),"Probe optimisation");
            OrtSession* probe=nullptr;ok(api_->CreateSession(env_,model.c_str(),probeOptions,&probe),"Open the depth model");
            ok(api_->SessionGetInputName(probe,0,allocator_,&inputName_),"Input name");ok(api_->SessionGetOutputName(probe,0,allocator_,&outputName_),"Output name");
            OrtTypeInfo* info=nullptr;ok(api_->SessionGetInputTypeInfo(probe,0,&info),"Input type");const OrtTensorTypeAndShapeInfo* tensor=nullptr;ok(api_->CastTypeInfoToTensorInfo(info,&tensor),"Input tensor type");
            ok(api_->GetTensorElementType(tensor,&inputType_),"Input element type");size_t n=0;ok(api_->GetDimensionsCount(tensor,&n),"Input rank");inputDims_.resize(n);
            if(n){ok(api_->GetDimensions(tensor,inputDims_.data(),n),"Input shape");std::vector<const char*> names(n,nullptr);ok(api_->GetSymbolicDimensions(tensor,names.data(),n),"Input dimension names");for(auto* name:names)dimNames.push_back(name?name:"");}
            api_->ReleaseTypeInfo(info);api_->ReleaseSession(probe);api_->ReleaseSessionOptions(probeOptions);
        }
        if(inputDims_.size()!=4||(inputDims_[1]>0&&inputDims_[1]!=3))fail("The model does not take one NCHW RGB picture");
        if(inputType_!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT&&inputType_!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)fail("The model input is neither float32 nor float16");
        // The input size for this aspect ratio; a model with fixed dimensions dictates its own.
        net=chooseNetSize(sourceWidth,sourceHeight,std::clamp(maxSide,depth::patch,depth::maxSide),depth::patch);
        if(inputDims_[2]>0)net.height=unsigned(inputDims_[2]);if(inputDims_[3]>0)net.width=unsigned(inputDims_[3]);
        if(net.width>depth::maxSide||net.height>depth::maxSide)fail("The model's fixed input is larger than the channel allows");
        ok(api_->CreateSessionOptions(&options_),"Session options");
        // DirectML needs sequential execution without memory patterns.
        ok(api_->DisableMemPattern(options_),"Memory pattern");ok(api_->SetSessionExecutionMode(options_,ORT_SEQUENTIAL),"Execution mode");ok(api_->SetSessionGraphOptimizationLevel(options_,ORT_ENABLE_ALL),"Graph optimisation");
        const int64_t pinned[4]{1,3,int64_t(net.height),int64_t(net.width)};
        for(size_t i=0;i<4;i++)if(inputDims_[i]<=0&&!dimNames[i].empty())ok(api_->AddFreeDimensionOverrideByName(options_,dimNames[i].c_str(),pinned[i]),"Pin an input dimension");
        ok(dml->SessionOptionsAppendExecutionProvider_DML1(options_,dml_.Get(),queue_.Get()),"DirectML execution provider");
        ok(api_->CreateSession(env_,model.c_str(),options_,&session_),"Load the depth model");
        loadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-loadStart).count();
        ok(api_->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&memInfo_),"CPU memory");
    }
    // Runs the network on a BGRA picture of `net` size. `depth` receives net.width x net.height values,
    // larger nearer. `taken` is called once the picture has been read, so its buffer may be reused.
    bool run(const uint8_t* bgra,NetSize net,const std::function<void()>& taken,std::vector<float>& depth,double& ms,std::string& error){
        const size_t pixels=size_t(net.width)*net.height;inputF_.resize(pixels*3);
        static constexpr float mean[3]{.485f,.456f,.406f},inv[3]{1/.229f,1/.224f,1/.225f}; // ImageNet statistics, as Depth Anything expects
        for(size_t i=0;i<pixels;i++){const uint8_t* p=bgra+i*4;inputF_[i]=(p[2]/255.f-mean[0])*inv[0];inputF_[pixels+i]=(p[1]/255.f-mean[1])*inv[1];inputF_[2*pixels+i]=(p[0]/255.f-mean[2])*inv[2];}
        if(taken)taken();
        void* data=inputF_.data();size_t bytes=inputF_.size()*sizeof(float);
        if(fp16()){inputH_.resize(pixels*3);for(size_t i=0;i<inputF_.size();i++)inputH_[i]=floatToHalf(inputF_[i]);data=inputH_.data();bytes=inputH_.size()*sizeof(uint16_t);}
        const int64_t shape[4]{1,3,int64_t(net.height),int64_t(net.width)};
        OrtValue* input=nullptr;ok(api_->CreateTensorWithDataAsOrtValue(memInfo_,data,bytes,shape,4,inputType_,&input),"Input tensor");
        OrtValue* output=nullptr;const auto runStart=std::chrono::steady_clock::now();
        OrtStatus* status=api_->Run(session_,nullptr,&inputName_,&input,1,&outputName_,1,&output);api_->ReleaseValue(input);
        if(status){error=std::string("Network run failed: ")+api_->GetErrorMessage(status);api_->ReleaseStatus(status);return false;}
        ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-runStart).count();
        OrtTensorTypeAndShapeInfo* outInfo=nullptr;ok(api_->GetTensorTypeAndShape(output,&outInfo),"Output shape");
        size_t rank=0;api_->GetDimensionsCount(outInfo,&rank);std::vector<int64_t> outDims(rank);if(rank)api_->GetDimensions(outInfo,outDims.data(),rank);ONNXTensorElementDataType outType=ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;api_->GetTensorElementType(outInfo,&outType);api_->ReleaseTensorTypeAndShapeInfo(outInfo);
        const unsigned ow=rank>=2?unsigned(std::max<int64_t>(outDims[rank-1],0)):0,oh=rank>=2?unsigned(std::max<int64_t>(outDims[rank-2],0)):0;
        void* raw=nullptr;ok(api_->GetTensorMutableData(output,&raw),"Output data");
        bool usable=ow&&oh&&(outType==ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT||outType==ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
        if(usable){
            auto at=[&](unsigned x,unsigned y){const size_t i=size_t(y)*ow+x;return outType==ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16?halfToFloat(static_cast<const uint16_t*>(raw)[i]):static_cast<const float*>(raw)[i];};
            depth.resize(pixels);
            // The map is delivered at the input size; a model whose output differs is resampled to it.
            for(unsigned y=0;y<net.height;y++)for(unsigned x=0;x<net.width;x++)depth[size_t(y)*net.width+x]=at(ow==net.width?x:unsigned(uint64_t(x)*ow/net.width),oh==net.height?y:unsigned(uint64_t(y)*oh/net.height));
        }else error="The model produced no 2D float depth map";
        api_->ReleaseValue(output);return usable;
    }
};
}
int wmain(int argc,wchar_t** argv){
    DWORD hostPid=0;int bench=0;std::filesystem::path model,logPath;LUID luid{};bool haveLuid=false;
    for(int i=1;i+1<argc;i++){const std::wstring a=argv[i];
        if(a==L"--host")hostPid=DWORD(std::wcstoul(argv[++i],nullptr,10));else if(a==L"--bench")bench=std::max(1,_wtoi(argv[++i]));else if(a==L"--model")model=argv[++i];else if(a==L"--log")logPath=argv[++i];
        else if(a==L"--adapter"){const std::wstring v=argv[++i];const auto colon=v.find(L':');if(colon!=std::wstring::npos){luid.HighPart=LONG(std::wcstol(v.substr(0,colon).c_str(),nullptr,10));luid.LowPart=DWORD(std::wcstoul(v.substr(colon+1).c_str(),nullptr,10));haveLuid=true;}}}
    if(!logPath.empty()){std::error_code ec;std::filesystem::create_directories(logPath.parent_path(),ec);g_log.open(logPath,std::ios::app);}
    if(bench){
        // The network alone: a synthetic 16:9 picture at the default quality, timed per run.
        try{Engine engine;engine.init(model,haveLuid?&luid:nullptr,3840,2160,518);const NetSize net=engine.net;
            std::cout<<utf8(model.filename().wstring())<<" ("<<(engine.fp16()?"fp16":"fp32")<<" "<<net.width<<"x"<<net.height<<") on "<<engine.adapterName<<", loaded in "<<int(engine.loadMs)<<" ms\n";
            std::vector<uint8_t> picture(size_t(net.width)*net.height*4);for(size_t i=0;i<picture.size();i++)picture[i]=uint8_t((i*7)&255);
            std::vector<float> depth;double total=0;for(int i=0;i<bench;i++){double ms=0;std::string error;if(!engine.run(picture.data(),net,{},depth,ms,error)){std::cout<<"FAIL: "<<error<<"\n";return 1;}std::cout<<"run "<<i<<": "<<ms<<" ms\n";if(i)total+=ms;}
            if(bench>1)std::cout<<"mean after the first: "<<total/(bench-1)<<" ms\n";return 0;
        }catch(const std::exception& e){std::cout<<"FAIL: "<<e.what()<<"\n";return 1;}
    }
    logLine("VisionDepth starting for host "+std::to_string(hostPid)+", model "+utf8(model.wstring()));
    // Behind the presenter on both the CPU and the GPU: the app demotes a captured game the same way.
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);D3DKMTSetProcessSchedulingPriorityClass(GetCurrentProcess(),D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL);
    const std::wstring suffix=std::to_wstring(hostPid);
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,(std::wstring(depth::mappingPrefix)+suffix).c_str());if(!mapping){logLine("No depth channel for this host");return 2;}
    auto* view=static_cast<uint8_t*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,0));if(!view){logLine("Cannot map the depth channel");return 2;}
    auto* h=reinterpret_cast<depth::Header*>(view);if(h->magic!=depth::magic||h->version!=depth::version){logLine("Depth channel version mismatch");return 2;}
    HANDLE pictureEvent=OpenEventW(SYNCHRONIZE,FALSE,(std::wstring(depth::pictureEventPrefix)+suffix).c_str());HANDLE depthEvent=OpenEventW(EVENT_MODIFY_STATE,FALSE,(std::wstring(depth::depthEventPrefix)+suffix).c_str());
    if(!pictureEvent||!depthEvent){logLine("Cannot open the depth channel events");return 2;}
    HANDLE host=OpenProcess(SYNCHRONIZE,FALSE,hostPid);
    h->helperPid=GetCurrentProcessId();
    auto setMessage=[&](const std::string& m){strncpy_s(h->message,m.c_str(),_TRUNCATE);};
    try{
        Engine engine;engine.init(model,haveLuid?&luid:nullptr,h->sourceWidth,h->sourceHeight,h->maxNetSide);h->loadMs=engine.loadMs;
        const NetSize net=engine.net;h->netWidth=net.width;h->netHeight=net.height;
        {std::ostringstream m;m<<utf8(model.filename().wstring())<<" ("<<(engine.fp16()?"fp16":"fp32")<<" "<<net.width<<"x"<<net.height<<") on "<<engine.adapterName<<", loaded in "<<int(engine.loadMs)<<" ms";setMessage(m.str());logLine(m.str());}
        store32(h->state,depth::Ready);
        std::vector<float> depthMap;uint32_t taken=0;
        for(;;){
            HANDLE waits[2]{pictureEvent,host};const DWORD r=WaitForMultipleObjects(host?2:1,waits,FALSE,1000);
            if(host&&r==WAIT_OBJECT_0+1){logLine("Host exited");break;}
            const uint32_t seq=load32(h->pictureSeq);if(seq==taken)continue;
            const double timestamp=h->pictureTimestamp;double ms=0;std::string error;
            const bool got=engine.run(view+depth::headerBytes,net,[&]{store32(h->pictureTaken,seq);taken=seq;},depthMap,ms,error);
            if(!got){logLine(error);setMessage(error);continue;}
            h->inferenceMs=ms;
            store32(h->depthBusy,1);memcpy(view+depth::headerBytes+depth::pictureBytes,depthMap.data(),depthMap.size()*sizeof(float));
            h->depthTimestamp=timestamp;store32(h->depthSeq,load32(h->depthSeq)+1);store32(h->depthBusy,0);SetEvent(depthEvent);
        }
    }catch(const std::exception& e){logLine(std::string("Failed: ")+e.what());setMessage(e.what());store32(h->state,depth::Failed);}
    if(host)CloseHandle(host);CloseHandle(pictureEvent);CloseHandle(depthEvent);UnmapViewOfFile(view);CloseHandle(mapping);
    logLine("VisionDepth exiting");return 0;
}
