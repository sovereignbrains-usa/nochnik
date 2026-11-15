# Делает иконку и плитки MSIX в assets\ из рисунка assets\source\moon.png (месяц со звездой).
# Запускать вручную, когда меняется рисунок; результат лежит в репозитории.
# Рисунок режется в тёмный круг (края исходника рваные), для мелких размеров берётся ближе к месяцу.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'assets'
$srcPath = Join-Path $out 'source\moon.png'
$src = [System.Drawing.Bitmap]::FromFile($srcPath)

# Центр месяца со звездой в исходнике (светлые пиксели: x 360..882, y 351..855).
$cx = 621.0; $cy = 603.0

function New-Icon([int]$size, [double]$fill) {
    # Мелкие размеры: ближе к месяцу, чтобы он читался.
    $half = if ($size -le 32) { 380.0 } else { 440.0 }
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $d = $size * $fill
    $o = ($size - $d) / 2.0
    $clip = New-Object System.Drawing.Drawing2D.GraphicsPath
    $clip.AddEllipse([single]$o, [single]$o, [single]$d, [single]$d)
    $g.SetClip($clip)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 13, 11, 10))), 0, 0, $size, $size)
    $crop = $src.Clone((New-Object System.Drawing.RectangleF ([single]($cx - $half)), ([single]($cy - $half)), ([single](2 * $half)), ([single](2 * $half))), $src.PixelFormat)
    $g.DrawImage($crop, [single]$o, [single]$o, [single]$d, [single]$d)
    $crop.Dispose()
    $g.Dispose()
    return $bmp
}

function Save-Png([System.Drawing.Bitmap]$bmp, [string]$name) {
    $bmp.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Имена, которые читает манифест (масштабы 100/200/400), поля вокруг круга с запасом.
$tiles = @(
    @{ n = 'Square44x44Logo';   b = 44  },
    @{ n = 'Square150x150Logo'; b = 150 },
    @{ n = 'StoreLogo';         b = 50  }
)
foreach ($t in $tiles) {
    foreach ($sc in 100, 200, 400) {
        $px = [int]($t.b * $sc / 100)
        Save-Png (New-Icon $px 0.8) ("{0}.scale-{1}.png" -f $t.n, $sc)
    }
}
# Значок панели задач/списка приложений без плитки под ним.
foreach ($ts in 16, 24, 32, 48, 256) {
    Save-Png (New-Icon $ts 1.0) ("Square44x44Logo.targetsize-{0}.png" -f $ts)
    Save-Png (New-Icon $ts 1.0) ("Square44x44Logo.targetsize-{0}_altform-unplated.png" -f $ts)
}

# night.ico: PNG внутри, размеры 16..256.
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Icon $s 1.0
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $bmp.Dispose(); $ms.Dispose()
}
$fs = [System.IO.File]::Create((Join-Path $out 'night.ico'))
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Dispose(); $fs.Dispose()
$src.Dispose()
"assets: " + (Get-ChildItem $out -File).Count + " files"
