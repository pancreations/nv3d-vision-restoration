// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <cstdint>
#include <string>
namespace vision::compatibility {
enum class Architecture { X86, X64 };
Architecture architecture(const std::filesystem::path& executable);
struct Inspection {
    Architecture architecture=Architecture::X64;
    bool geo11=false;
    bool migoto=false;
    std::string output,proxy,description;
};
Inspection inspect(const std::filesystem::path& executable);
struct Tuning { float depth=50,convergence=2;bool autoConvergence=false; };
Tuning tuning(const std::filesystem::path& executable);
std::string setTuning(const std::filesystem::path& executable,const Tuning& values);
std::filesystem::path processExecutable(uint32_t pid);
// Connect an existing Geo11 fix. Preserve its renderer bytes as VisionGeo11.dll
// behind an early d3d11 loader, and retain its shaders/NVAPI/other loaders.
// Reversible connection records own every changed file. Root contains x86/x64.
std::string connect(const std::filesystem::path& executable,const std::filesystem::path& runtimeRoot);
std::string disconnect(const std::filesystem::path& executable);
enum class CaptureMode { Sequential,Array,Katanga,SideBySide,TopBottom };
// Installs a reusable capture add-on and an explicit loader chain, retaining
// existing game fixes. The app's Direct 3D eyes source consumes its channel.
std::string connectCapture(const std::filesystem::path& executable,const std::filesystem::path& runtimeRoot,CaptureMode mode,bool rightFirst=false);
std::string disconnectCapture(const std::filesystem::path& executable);
}
