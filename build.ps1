param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$projectRoot=$PSScriptRoot
cmake -S $projectRoot -B "$projectRoot/build" -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
$targets=@('VisionRestoration','vision_stereo_setup','vision_tests','vision_compatibility_tests','vision_lcd_tests','vision_controls_tests','vision_menu_tests','vision_panel_tests','vision_platform_tests','vision_depth_tests','vision_screen_tests','vision_screen_bench','vision_overlay_probe','vision_firmware','VisionGameHook','vision_rp2040_tests','vision_rp2040_host_tests','vision_rp2040_probe')
# The AI screen depth helper needs ONNX Runtime and DirectML from tools/Get-Dependencies.ps1.
$targets+='vision_vlc_tests'
$targets+='vision_direct_eyes_tests'
$targets+='VisionStereoCapture'
if(Test-Path "$projectRoot/third_party/onnxruntime/build/native/include/onnxruntime_c_api.h"){$targets+='VisionDepth'}
cmake --build "$projectRoot/build" --config $Configuration --target $targets -- /m:1
if($LASTEXITCODE -ne 0){throw 'Build failed'}
ctest --test-dir "$projectRoot/build" -C $Configuration --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Tests failed'}
& "$projectRoot/tools/Build-StereoRuntime.ps1" -Configuration $Configuration
