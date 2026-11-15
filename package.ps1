# Упаковка Ночника в MSIX: build -> stage -> resources.pri -> makeappx -> подпись самоподписанным сертификатом.
# Результат в dist\: Night-<версия>-x64.msix, Night.cer, Install.ps1.
# Запуск: powershell -ExecutionPolicy Bypass -File package.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$subject = 'CN=Nochnik'
$ver = (Select-String -Path version.h -Pattern 'NIGHT_VER_STR\s+"([\d.]+)"').Matches[0].Groups[1].Value
$kit = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makeappx.exe" | Sort-Object FullName | Select-Object -Last 1 | Split-Path
$makeappx = Join-Path $kit 'makeappx.exe'
$makepri = Join-Path $kit 'makepri.exe'
$signtool = Join-Path $kit 'signtool.exe'

& "$PSScriptRoot\build.ps1"

$dist = Join-Path $PSScriptRoot 'dist'
$stage = Join-Path $dist 'stage'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\Assets" | Out-Null
Copy-Item build\Night.exe $stage
Copy-Item assets\*.png "$stage\Assets"

$xml = (Get-Content AppxManifest.xml -Raw -Encoding UTF8).Replace('{{VERSION}}', $ver).Replace('{{PUBLISHER}}', $subject)
[IO.File]::WriteAllText("$stage\AppxManifest.xml", $xml, (New-Object Text.UTF8Encoding $false))

# Индекс ресурсов: из масштабных файлов (scale-100/200/400) получается одно имя Assets\Square44x44Logo.png.
$cfg = Join-Path $dist 'priconfig.xml'
& $makepri createconfig /cf $cfg /dq ru-RU /o | Out-Null
if ($LASTEXITCODE) { throw 'makepri createconfig' }
& $makepri new /pr $stage /cf $cfg /mn "$stage\AppxManifest.xml" /of "$stage\resources.pri" /o | Out-Null
if ($LASTEXITCODE) { throw 'makepri new' }

$msix = Join-Path $dist "Night-$ver-x64.msix"
& $makeappx pack /d $stage /p $msix /o | Out-Null
if ($LASTEXITCODE) { throw 'makeappx pack' }

# Сертификат живёт в CurrentUser\My и переиспользуется: обновления подписаны тем же ключом.
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
    Sort-Object NotAfter | Select-Object -Last 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject $subject -KeyUsage DigitalSignature -FriendlyName 'Nochnik' `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(10) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}
Export-Certificate -Cert $cert -FilePath (Join-Path $dist 'Night.cer') | Out-Null

& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My $msix | Out-Null
if ($LASTEXITCODE) { throw 'signtool sign' }

Copy-Item "$PSScriptRoot\tools\Install.ps1" (Join-Path $dist 'Install.ps1') -Force
Remove-Item $stage -Recurse -Force
Write-Host "готово: $msix"
