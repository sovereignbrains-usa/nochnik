# Установка Ночника из MSIX. Лежит рядом с Night-*-x64.msix и Night.cer.
# Сертификат самоподписанный: его нужно один раз доверить на этом ПК (папка «Доверенные лица» компьютера).
# Для этого появится запрос UAC; сам пакет ставится от вашего пользователя.
# Запуск: правый клик -> «Выполнить с помощью PowerShell».
param([switch]$NoPause)
$ErrorActionPreference = 'Stop'

try {
    $here = $PSScriptRoot
    $msix = Get-ChildItem $here -Filter 'Night-*-x64.msix' | Sort-Object Name | Select-Object -Last 1
    $cer = Join-Path $here 'Night.cer'
    if (-not $msix -or -not (Test-Path $cer)) { throw 'Рядом должны лежать Night-*-x64.msix и Night.cer' }

    $thumb = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
    $trusted = { Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb" }
    if (-not (& $trusted)) {
        Write-Host 'Доверяю сертификату Ночника (нужно подтвердить запрос Windows)...'
        $cmd = "Import-Certificate -FilePath '$cer' -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null"
        Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $cmd
        if (-not (& $trusted)) { throw 'Сертификат не доверен: запрос Windows был отклонён.' }
    }

    # Прежняя сборка (.NET): остановить и убрать её запись автозапуска, иначе будет два экземпляра.
    Get-Process Night -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill(); $_.WaitForExit(3000) | Out-Null } catch {} }
    Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name Night -ErrorAction SilentlyContinue

    Add-AppxPackage -Path $msix.FullName -ForceUpdateFromAnyVersion
    Write-Host 'Ночник установлен. Он есть в меню «Пуск».'
}
catch {
    Write-Host "Ошибка: $($_.Exception.Message)" -ForegroundColor Red
}
if (-not $NoPause) { Read-Host 'Enter, чтобы закрыть' }
