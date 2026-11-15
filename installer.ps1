# Собирает обычный установщик: dist\Night-<версия>-setup.exe (нужен Inno Setup 6).
# Сначала собирает Night.exe через build.ps1, версию берёт из version.h.
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$v = Get-Content version.h -Raw
$ver = ([regex]::Match($v, '#define NIGHT_VER_STR "([^"]+)"')).Groups[1].Value
if (-not $ver) { throw 'в version.h нет NIGHT_VER_STR' }

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1"; if ($LASTEXITCODE) { throw 'build.ps1 упал' } }

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 не найден (https://jrsoftware.org/isdl.php)' }

New-Item -ItemType Directory -Force dist | Out-Null
& $iscc "/DAppVersion=$ver" "/DSourceExe=$PSScriptRoot\build\Night.exe" "/DOutDir=$PSScriptRoot\dist" "$PSScriptRoot\installer\Night.iss"
if ($LASTEXITCODE) { throw 'ISCC упал' }
Write-Host "Готово: dist\Night-$ver-setup.exe"
