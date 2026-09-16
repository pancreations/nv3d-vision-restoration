#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: vision_compile_shaders input.hlsl output.h");
    std::ifstream input(std::filesystem::path(argv[1]),std::ios::binary);
    if(!input)throw std::runtime_error("Cannot read stereo shader source");
    const std::string source{std::istreambuf_iterator<char>(input),{}};
    std::string header="#pragma once\nnamespace vision::compiled {\n";
    for(const char* entry:{"vs","ps"}){
        ComPtr<ID3DBlob> code,error;const std::string target=std::string(entry)+"_5_0";
        if(FAILED(D3DCompile(source.data(),source.size(),"stereo_scene.hlsl",nullptr,nullptr,entry,target.c_str(),D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error)))
            throw std::runtime_error(error?static_cast<const char*>(error->GetBufferPointer()):"Shader compilation failed");
        header+="inline constexpr unsigned char "+std::string(entry)+"[]={\n";
        auto bytes=static_cast<const unsigned char*>(code->GetBufferPointer());
        for(size_t i=0;i<code->GetBufferSize();++i){header+=std::to_string(bytes[i])+",";if(i%24==23)header+='\n';}
        header+="\n};\n";
    }
    header+="}\n";
    std::ofstream output(std::filesystem::path(argv[2]),std::ios::binary|std::ios::trunc);
    output<<header;output.close();if(!output)throw std::runtime_error("Cannot write compiled shader header");
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
