// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
namespace vision::compatibility {
enum class Architecture { X86, X64 };
Architecture architecture(const std::filesystem::path& executable);
struct Inspection {
    Architecture architecture=Architecture::X64;
    bool geo11=false;
    std::string output,proxy,description;
};
Inspection inspect(const std::filesystem::path& executable);
// Connect an existing Geo11 fix. Preserve its renderer bytes as VisionGeo11.dll
// behind an early d3d11 loader, and retain its shaders/NVAPI/other loaders.
// Reversible connection records own every changed file. Root contains x86/x64.
std::string connect(const std::filesystem::path& executable,const std::filesystem::path& runtimeRoot);
std::string disconnect(const std::filesystem::path& executable);
}
