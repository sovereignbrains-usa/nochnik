# Сборка Ночника: rc + cl (MSVC), статический рантайм. Результат: build\Night.exe
# Запуск: powershell -ExecutionPolicy Bypass -File build.ps1 [-NoCheck]
param([switch]$NoCheck)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Окружение MSVC через vcvars64 (один раз на процесс).
if (-not $env:VSCMD_VER) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    if (-not $vs) { throw 'Visual Studio / Build Tools не найдены' }
    $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "chcp 65001 >nul & `"$bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
}

New-Item -ItemType Directory -Force build | Out-Null
rc /nologo /utf-8 /fo build\night.res night.rc
if ($LASTEXITCODE) { throw 'rc не собрал ресурсы' }

cl /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /W4 /permissive- /sdl /GS /guard:cf /Zc:__cplusplus `
    /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    /Fobuild\ /Fe:build\Night.exe `
    night.cpp base.cpp config.cpp display.cpp selfcheck.cpp startup.cpp build\night.res `
    /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT /guard:cf `
    user32.lib gdi32.lib gdiplus.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib dwmapi.lib wtsapi32.lib uxtheme.lib windowsapp.lib
if ($LASTEXITCODE) { throw 'cl не собрал' }

if (-not $NoCheck) {
    $out = Join-Path $env:TEMP 'night-check.txt'
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = Start-Process build\Night.exe -ArgumentList '--check', "`"$out`"" -Wait -PassThru
    Get-Content $out
    if ($p.ExitCode) { throw '--check не прошёл' }
}
Write-Host 'build\Night.exe готов'
