# Рисует иконку и плитки MSIX в assets\. Запускать вручную, когда меняется форма; результат лежит в репозитории.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'assets'
New-Item -ItemType Directory -Force $out | Out-Null

# Форма как в старом Night.cs: два эллипса, заливка «чётное-нечётное» (месяц).
function New-Moon([int]$w, [int]$h, [double]$fill) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    # bbox формы: x 6..32, y 3..27 (26 x 24), центр (19, 15)
    $k = [Math]::Min($w, $h) * $fill / 26.0
    $g.TranslateTransform($w / 2.0, $h / 2.0)
    $g.ScaleTransform($k, $k)
    $g.TranslateTransform(-19, -15)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath ([System.Drawing.Drawing2D.FillMode]::Alternate)
    $path.AddEllipse(6, 5, 20, 22)
    $path.AddEllipse(14, 3, 18, 22)
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 176, 96))
    $g.FillPath($brush, $path)
    $g.Dispose()
    return $bmp
}

function Save-Png([System.Drawing.Bitmap]$bmp, [string]$name) {
    $bmp.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Имена, которые читает манифест (масштабы 100/200/400), поля вокруг месяца с запасом.
$tiles = @(
    @{ n = 'Square44x44Logo';   b = 44  },
    @{ n = 'Square150x150Logo'; b = 150 },
    @{ n = 'StoreLogo';         b = 50  }
)
foreach ($t in $tiles) {
    foreach ($sc in 100, 200, 400) {
        $px = [int]($t.b * $sc / 100)
        Save-Png (New-Moon $px $px 0.62) ("{0}.scale-{1}.png" -f $t.n, $sc)
    }
}
# Значок панели задач/списка приложений без плитки под ним.
foreach ($ts in 16, 24, 32, 48, 256) {
    Save-Png (New-Moon $ts $ts 0.9) ("Square44x44Logo.targetsize-{0}.png" -f $ts)
    Save-Png (New-Moon $ts $ts 0.9) ("Square44x44Logo.targetsize-{0}_altform-unplated.png" -f $ts)
}

# night.ico: PNG внутри, размеры 16..256.
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Moon $s $s 0.9
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
"assets: " + (Get-ChildItem $out | Measure-Object).Count + " files"
