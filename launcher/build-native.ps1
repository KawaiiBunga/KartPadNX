# Builds the launcher ELF and NACP on Windows with a local devkitPro (no Docker), the same sources
# and libraries as launcher/CMakeLists.txt. aio\publish.ps1 runs it so launcher changes ship.
#
#   powershell -ExecutionPolicy Bypass -File launcher\build-native.ps1 [-DevkitPro E:\tools\devkitPro] [-Out DIR]
#
# Output: <Out>\mkw_launcher.elf and <Out>\mkw-launcher.nacp (default Out: build\launcher-native).
param([string]$DevkitPro = $(if ($env:DEVKITPRO -and (Test-Path $env:DEVKITPRO)) { $env:DEVKITPRO } else { "E:\tools\devkitPro" }),
      [string]$Out = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo "build\launcher-native" }
$gxx = Join-Path $DevkitPro "devkitA64\bin\aarch64-none-elf-g++.exe"
if (-not (Test-Path $gxx)) { throw "devkitA64 not found at $DevkitPro (pass -DevkitPro)" }
$obj = Join-Path $Out "obj"
New-Item -ItemType Directory -Force $obj | Out-Null

# Version, title and author from the same place the Docker build reads them.
$cmake = Get-Content (Join-Path $repo "launcher\CMakeLists.txt") -Raw
function CMakeVar($name) { if ($cmake -match "set\($name `"([^`"]*)`"\)") { $Matches[1] } else { throw "no $name in CMakeLists.txt" } }

$srcs = @(Get-ChildItem (Join-Path $repo "launcher\source") -Recurse -Include *.cpp, *.c | ForEach-Object FullName) +
        @("disc_extractor.cpp", "disc_reader.cpp", "wii_cluster.cpp", "wii_crypto.cpp", "wii_crypto_switch.cpp",
          "wii_common_key.cpp", "wii_fst.cpp" | ForEach-Object { Join-Path $repo "switch\src\runtime\$_" })
$arch = @("-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE")
$cflags = $arch + @("-O2", "-DNDEBUG", "-D__SWITCH__=1", "-DCURL_STATICLIB=1", "-ffunction-sections", "-fdata-sections", "-w",
    "-I$repo\launcher\include", "-I$repo\launcher\source", "-I$repo\switch\src\runtime",
    "-I$DevkitPro\libnx\include", "-I$DevkitPro\portlibs\switch\include", "-I$DevkitPro\portlibs\switch\include\SDL2",
    "-I$DevkitPro\portlibs\switch\include\freetype2", "-I$DevkitPro\portlibs\switch\include\harfbuzz")
$env:DEVKITPRO = $DevkitPro

$objs = @()
$i = 0
foreach ($s in $srcs) {
    $o = Join-Path $obj ("{0:d3}-{1}.o" -f $i++, [IO.Path]::GetFileNameWithoutExtension($s))
    $std = if ($s.EndsWith(".c")) { "-std=gnu17" } else { "-std=gnu++20" }
    & $gxx $std @cflags -c $s -o $o
    if ($LASTEXITCODE) { throw "compiling $s failed" }
    $objs += $o
}

$elf = Join-Path $Out "mkw_launcher.elf"
$link = @("-specs=$DevkitPro\libnx\switch.specs") + $arch + @("-Wl,--gc-sections") + $objs + @(
    "-L$DevkitPro\libnx\lib", "-L$DevkitPro\portlibs\switch\lib", "-Wl,--start-group",
    "-lSDL2_ttf", "-lSDL2_image", "-lSDL2", "-lharfbuzz", "-lfreetype", "-lbz2", "-lpng16", "-ljpeg", "-lwebp", "-lwebpdemux",
    "-lcurl", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-lsodium", "-lzstd", "-lz",
    "-lEGL", "-lglapi", "-ldrm_nouveau", "-lstdc++", "-lnx", "-lm", "-lpthread", "-Wl,--end-group", "-o", $elf)
& $gxx @link
if ($LASTEXITCODE) { throw "linking the launcher failed" }

$nacp = Join-Path $Out "mkw-launcher.nacp"
& (Join-Path $DevkitPro "tools\bin\nacptool.exe") --create (CMakeVar "LAUNCHER_TITLE") (CMakeVar "LAUNCHER_AUTHOR") (CMakeVar "LAUNCHER_VERSION") $nacp
if ($LASTEXITCODE) { throw "nacptool failed" }
Write-Host "launcher: $elf ($([math]::Round((Get-Item $elf).Length / 1MB, 1)) MB) and $nacp"
