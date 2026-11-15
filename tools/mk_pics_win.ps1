# mk_pics_win.ps1 - v0.10.5: image-viewer acceptance fixtures (ASCII only, on purpose:
# Windows PowerShell 5.1 reads a BOM-less file as ANSI, so any CJK comment can break
# parsing -- the repo rule is ".ps1 must be pure ASCII").
#
# Why System.Drawing instead of Python:
#   * BMP is easy to hand-write, but there is no stdlib JPEG *encoder* (PIL may not be
#     installed, and we do not want a third-party dependency just for a fixture);
#   * Windows PowerShell 5.1 ships .NET Framework System.Drawing: PNG/JPEG/BMP encoders.
#
# The pattern is deliberately "left half pure red (255,0,0), right half pure blue
# (0,0,255)":
#   * correct decode = both colours present, one half each -> pixel counts are provable;
#   * JPEG is lossy, so blocks are wide (64 px) and the test only counts pixels well
#     inside a block (chroma-subsampling edge error never reaches them);
#   * any "opened as text" regression or BGR/RGB channel swap shows up immediately
#     (red/blue swap is the classic bug in this kind of code).
#
# Output (build-time dir, never committed):
#   build64/pics/redblue.png   128x96      opened 1:1 (no scaling path)
#   build64/pics/redblue.jpg   128x96      JPEG decode path
#   build64/pics/redblue.bmp   128x96      BMP decode path
#   build64/pics/big.png       1600x1200   scaled down to the window (mode=fit)
param(
    [string]$OutDir = "build64\pics"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

function New-RedBlue {
    param([int]$W, [int]$H)
    $bmp = New-Object System.Drawing.Bitmap($W, $H)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $red = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0, 0))
    $blue = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(0, 0, 255))
    $half = [int]($W / 2)
    $g.FillRectangle($red, 0, 0, $half, $H)
    $g.FillRectangle($blue, $half, 0, $W - $half, $H)
    $g.Dispose()
    $red.Dispose()
    $blue.Dispose()
    return $bmp
}

$b = New-RedBlue -W 128 -H 96
$b.Save((Join-Path $OutDir "redblue.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$b.Save((Join-Path $OutDir "redblue.jpg"), [System.Drawing.Imaging.ImageFormat]::Jpeg)
$b.Save((Join-Path $OutDir "redblue.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
$b.Dispose()

$c = New-RedBlue -W 1600 -H 1200
$c.Save((Join-Path $OutDir "big.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$c.Dispose()

Get-ChildItem $OutDir | Sort-Object Name | ForEach-Object {
    "    pic {0} {1} B" -f $_.Name, $_.Length
}
