# Build and run the PC tests for the Google Search grounding path with MSVC.
# Usage (from the project root): powershell -File tools/host_tests/run.ps1
# Needs Visual Studio with the C++ workload (the same one the GSP simulator uses)
# and managed_components/espressif__cjson (created by the first device build).
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..\..")
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$out = Join-Path ([System.IO.Path]::GetTempPath()) "vox_host_tests"
New-Item -ItemType Directory -Force $out | Out-Null
$sources = @(
    "$here\test_grounding.c",
    "$root\components\app_gemini_live\gl_grounding.c",
    "$root\components\app_vox_session\vox_status_text.c",
    "$root\managed_components\espressif__cjson\cJSON\cJSON.c"
) | ForEach-Object { "`"$_`"" }
$includes = @(
    "$here\stubs",
    "$root\components\app_gemini_live",
    "$root\components\app_vox_session",
    "$root\managed_components\espressif__cjson\cJSON"
) | ForEach-Object { "/I`"$_`"" }
$cmd = "`"$vcvars`" >nul && cl /nologo /utf-8 /W4 /WX /wd4100 /wd4996 /D_CRT_SECURE_NO_WARNINGS " +
       "$($includes -join ' ') $($sources -join ' ') /Fo`"$out\\`" /Fe`"$out\test_grounding.exe`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed" }
& "$out\test_grounding.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$geo = "`"$vcvars`" >nul && cl /nologo /utf-8 /W4 /WX /I`"$root\components\app_camera`" " +
       "`"$here\test_camera_geometry.c`" `"$root\components\app_camera\camera_geometry.c`" " +
       "/Fo`"$out\\`" /Fe`"$out\test_camera_geometry.exe`""
cmd /c $geo
if ($LASTEXITCODE -ne 0) { throw "geometry build failed" }
& "$out\test_camera_geometry.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$ring = "`"$vcvars`" >nul && cl /nologo /utf-8 /W4 /WX /I`"$root\components\app_photos`" " +
        "`"$here\test_photos_ring.c`" `"$root\components\app_photos\photos_ring.c`" " +
        "/Fo`"$out\\`" /Fe`"$out\test_photos_ring.exe`""
cmd /c $ring
if ($LASTEXITCODE -ne 0) { throw "photo ring build failed" }
& "$out\test_photos_ring.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$rol = "`"$vcvars`" >nul && cl /nologo /utf-8 /W4 /WX " +
       "/I`"$root\components\app_vox_session`" /I`"$root\components\app_vox_eye`" " +
       "`"$here\test_rol.c`" `"$root\components\app_vox_session\vox_rol.c`" " +
       "`"$root\components\app_vox_eye\secret_text.c`" " +
       "/Fo`"$out\\`" /Fe`"$out\test_rol.exe`""
cmd /c $rol
if ($LASTEXITCODE -ne 0) { throw "rol build failed" }
& "$out\test_rol.exe"
exit $LASTEXITCODE
