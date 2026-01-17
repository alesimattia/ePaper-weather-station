#.\Convert-PngToGxEPD2.ps1 -InputPng img.png -OutputHeader image.h  (800x480 | fit)
#.\Convert-PngToGxEPD2.ps1 -InputPng img.png -OutputHeader image.h -Width 640 -Height 384 (fit)
# .\Convert-PngToGxEPD2.ps1 -InputPng img.png -OutputHeader image.h -Mode crop (800x480)
# [fit | crop | stretch]



<#
Convert-PngToGxEPD2.ps1
PNG -> GxEPD2_7C native (4bpp packed) -> image.h

Quantizzazione: nearest-color (sempre)
Dithering: Floyd–Steinberg (opzionale)

PARAMETRI:
  -InputPng      (obbligatorio) PNG di input
  -OutputHeader  (obbligatorio) image.h di output
  -Width         (default 800)
  -Height        (default 480)
  -Mode          fit|crop|stretch  (default fit)
  -Dither        switch: se presente abilita Floyd–Steinberg

ESEMPI:
  # default: fit, no dithering
  .\Convert-PngToGxEPD2.ps1 -InputPng .\in.png -OutputHeader .\image.h

  # crop + dithering
  .\Convert-PngToGxEPD2.ps1 -InputPng .\in.png -OutputHeader .\image.h -Mode crop -Dither

  # size custom + dithering
  .\Convert-PngToGxEPD2.ps1 -InputPng .\in.png -OutputHeader .\image.h -Width 1024 -Height 600 -Dither
#>

param(
  [Parameter(Mandatory = $true)] [string]$InputPng,
  [Parameter(Mandatory = $true)] [string]$OutputHeader,
  [int]$Width  = 800,
  [int]$Height = 480,
  [ValidateSet("fit","crop","stretch")] [string]$Mode = "fit",
  [switch]$Dither
)

Add-Type -AssemblyName System.Drawing

$Palette = @(
  @(255,255,255), # 0 WHITE
  @(0,0,0),       # 1 BLACK
  @(255,0,0),     # 2 RED
  @(0,255,0),     # 3 GREEN
  @(0,0,255),     # 4 BLUE
  @(255,255,0),   # 5 YELLOW
  @(255,128,0)    # 6 ORANGE
)

function Clamp8([double]$v) {
  if ($v -lt 0) { return 0 }
  if ($v -gt 255) { return 255 }
  return [int][Math]::Round($v)
}

function Get-NearestPaletteIndex([int]$R, [int]$G, [int]$B) {
  $bestIndex = 0
  $bestDist  = [double]::PositiveInfinity

  for ($i = 0; $i -lt $Palette.Count; $i++) {
    $pr = $Palette[$i][0]; $pg = $Palette[$i][1]; $pb = $Palette[$i][2]
    $dr = $R - $pr; $dg = $G - $pg; $db = $B - $pb
    $d  = $dr*$dr + $dg*$dg + $db*$db
    if ($d -lt $bestDist) { $bestDist = $d; $bestIndex = $i }
  }
  return $bestIndex
}

function Resize-ToCanvas([System.Drawing.Bitmap]$Source, [int]$W, [int]$H, [string]$Mode) {
  $dst = New-Object System.Drawing.Bitmap $W, $H, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
  $g = [System.Drawing.Graphics]::FromImage($dst)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.Clear([System.Drawing.Color]::White)

  $srcW = $Source.Width
  $srcH = $Source.Height
  $srcAR = $srcW / $srcH
  $dstAR = $W / $H

  switch ($Mode) {
    "stretch" { $g.DrawImage($Source, 0, 0, $W, $H) }
    "fit" {
      if ($srcAR -gt $dstAR) { $newW = $W; $newH = [int][Math]::Round($W / $srcAR) }
      else { $newH = $H; $newW = [int][Math]::Round($H * $srcAR) }
      $ox = [int](($W - $newW) / 2)
      $oy = [int](($H - $newH) / 2)
      $g.DrawImage($Source, $ox, $oy, $newW, $newH)
    }
    "crop" {
      if ($srcAR -gt $dstAR) { $newH = $H; $newW = [int][Math]::Round($H * $srcAR) }
      else { $newW = $W; $newH = [int][Math]::Round($W / $srcAR) }
      $ox = [int](($W - $newW) / 2)
      $oy = [int](($H - $newH) / 2)
      $g.DrawImage($Source, $ox, $oy, $newW, $newH)
    }
  }

  $g.Dispose()
  return $dst
}

if (-not (Test-Path $InputPng)) { throw "File non trovato: $InputPng" }
if (($Width * $Height) % 2 -ne 0) { throw "Width*Height deve essere pari (2 pixel per byte)." }

$src = [System.Drawing.Bitmap]::FromFile($InputPng)
try {
  $bmp = Resize-ToCanvas $src $Width $Height $Mode

  # output packed bytes
  $outBytes = New-Object byte[] (($Width * $Height) / 2)

  if (-not $Dither) {
    # nearest-only
    $k = 0
    for ($y=0; $y -lt $Height; $y++) {
      for ($x=0; $x -lt $Width; $x += 2) {
        $c1 = $bmp.GetPixel($x,     $y)
        $c2 = $bmp.GetPixel($x + 1, $y)
        $hi = (Get-NearestPaletteIndex $c1.R $c1.G $c1.B) -band 0x0F
        $lo = (Get-NearestPaletteIndex $c2.R $c2.G $c2.B) -band 0x0F
        $outBytes[$k++] = [byte](($hi -shl 4) -bor $lo)
      }
    }
  }
  else {
    # Floyd–Steinberg: lavoriamo su buffer float RGB
    $bufR = New-Object double[] ($Width * $Height)
    $bufG = New-Object double[] ($Width * $Height)
    $bufB = New-Object double[] ($Width * $Height)

    for ($y=0; $y -lt $Height; $y++) {
      for ($x=0; $x -lt $Width; $x++) {
        $i = $y*$Width + $x
        $c = $bmp.GetPixel($x,$y)
        $bufR[$i] = $c.R
        $bufG[$i] = $c.G
        $bufB[$i] = $c.B
      }
    }

    function Add-Err([int]$x,[int]$y,[double]$er,[double]$eg,[double]$eb,[double]$f) {
      if ($x -lt 0 -or $x -ge $Width -or $y -lt 0 -or $y -ge $Height) { return }
      $j = $y*$Width + $x
      $bufR[$j] = Clamp8($bufR[$j] + $er*$f)
      $bufG[$j] = Clamp8($bufG[$j] + $eg*$f)
      $bufB[$j] = Clamp8($bufB[$j] + $eb*$f)
    }

    # prima calcoliamo tutti gli indici (byte per pixel), poi pack
    $idx = New-Object byte[] ($Width * $Height)

    for ($y=0; $y -lt $Height; $y++) {
      for ($x=0; $x -lt $Width; $x++) {
        $i = $y*$Width + $x
        $r = [int]$bufR[$i]; $g = [int]$bufG[$i]; $b = [int]$bufB[$i]
        $pi = Get-NearestPaletteIndex $r $g $b
        $idx[$i] = [byte]$pi

        $pr = $Palette[$pi][0]; $pg = $Palette[$pi][1]; $pb = $Palette[$pi][2]
        $er = $r - $pr; $eg = $g - $pg; $eb = $b - $pb

        Add-Err ($x+1) $y     $er $eg $eb (7.0/16.0)
        Add-Err ($x-1) ($y+1) $er $eg $eb (3.0/16.0)
        Add-Err $x     ($y+1) $er $eg $eb (5.0/16.0)
        Add-Err ($x+1) ($y+1) $er $eg $eb (1.0/16.0)
      }
    }

    $k = 0
    for ($y=0; $y -lt $Height; $y++) {
      $row = $y*$Width
      for ($x=0; $x -lt $Width; $x += 2) {
        $hi = $idx[$row + $x] -band 0x0F
        $lo = $idx[$row + $x + 1] -band 0x0F
        $outBytes[$k++] = [byte](($hi -shl 4) -bor $lo)
      }
    }
  }

  # write header
  $sb = New-Object System.Text.StringBuilder
  $sb.AppendLine("// Auto-generated for GxEPD2_7C (4bpp packed: 2 pixels per byte)") | Out-Null
  $sb.AppendLine("// Source: " + [IO.Path]::GetFileName($InputPng)) | Out-Null
  $sb.AppendLine("// Size: " + $Width + "x" + $Height) | Out-Null
  $sb.AppendLine("") | Out-Null
  $sb.AppendLine("#pragma once") | Out-Null
  $sb.AppendLine("#include <Arduino.h>") | Out-Null
  $sb.AppendLine("") | Out-Null
  $sb.AppendLine("const uint16_t IMAGE_W = $Width;") | Out-Null
  $sb.AppendLine("const uint16_t IMAGE_H = $Height;") | Out-Null
  $sb.AppendLine("") | Out-Null
  $sb.AppendLine("const uint8_t image_data[] PROGMEM = {") | Out-Null

  for ($i=0; $i -lt $outBytes.Length; $i++) {
    if (($i % 16) -eq 0) { $sb.Append("  ") | Out-Null }
    $sb.Append(("0x{0:X2}" -f $outBytes[$i])) | Out-Null
    if ($i -ne $outBytes.Length - 1) { $sb.Append(", ") | Out-Null }
    if (($i % 16) -eq 15) { $sb.AppendLine("") | Out-Null }
  }
  if (($outBytes.Length % 16) -ne 0) { $sb.AppendLine("") | Out-Null }

  $sb.AppendLine("};") | Out-Null
  $sb.AppendLine("") | Out-Null
  $sb.AppendLine("// Use with: display.drawNative(image_data, 0, 0, 0, IMAGE_W, IMAGE_H, false, false, true);") | Out-Null

  [IO.File]::WriteAllText($OutputHeader, $sb.ToString(), [Text.E]()