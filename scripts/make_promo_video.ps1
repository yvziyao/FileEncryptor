<#
  从 promo.html 生成宣传视频 promo.mp4。

  做法（确定性，不依赖录屏，因此不会有窗口边框/弹窗/鼠标）：
    1. 用 Edge 无头模式，在 promo.html?still=N 下拍摄 8 张定格静帧（1280x720）
    2. ffmpeg 把静帧按各幕时长串起来，之间做 0.5s 交叉淡化，末尾淡出

  依赖：Edge（系统自带，路径不同可改 $edge）、ffmpeg（需在 PATH 中）
  用法：powershell -ExecutionPolicy Bypass -File scripts\make_promo_video.ps1
#>
param(
    [string]$OutFile = "promo.mp4",
    [string]$EdgePath = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$html = Join-Path $repo "promo.html"
$work = Join-Path $env:TEMP "fe_promo_frames"

if (-not (Test-Path $html)) { throw "缺少 promo.html" }
if (-not (Test-Path $EdgePath)) { throw "找不到 Edge: $EdgePath" }
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue)
if (-not $ffmpeg) { throw "找不到 ffmpeg，请先安装并加入 PATH" }

Remove-Item $work -Recurse -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null

# 1) 定格静帧
$url = "file:///" + ($html -replace '\\', '/')
$EAP0 = $ErrorActionPreference
$ErrorActionPreference = "Continue"   # Edge 会往 stderr 写调试信息，不要当错误
for ($i = 0; $i -lt 8; $i++) {
    $f = Join-Path $work ("s$i.png")
    & $EdgePath --headless=new --disable-gpu --window-size=1280,720 --virtual-time-budget=800 `
        --screenshot=$f "$url`?still=$i" 2>$null | Out-Null
    if (-not (Test-Path $f)) { throw "静帧 $i 拍摄失败" }
}
$ErrorActionPreference = $EAP0
Write-Host "已生成 8 张静帧"

# 2) ffmpeg 合成（时长与 promo.html 里 data-ms 对应）
$dur = @(2.6, 3.0, 3.4, 3.6, 3.2, 3.4, 3.0, 4.2)
$fade = 0.5
$ff = @("-loglevel", "error", "-y")
for ($i = 0; $i -lt 8; $i++) {
    $ff += @("-loop", "1", "-t", ("{0:N2}" -f $dur[$i]), "-i", (Join-Path $work "s$i.png"))
}

$parts = @()
for ($i = 0; $i -lt 8; $i++) {
    $parts += "[$i`:v]scale=1280:720,setsar=1,fps=30,format=yuv420p[v$i]"
}
$cur = "v0"; $acc = $dur[0]
for ($i = 1; $i -lt 8; $i++) {
    $off = $acc - $fade
    $parts += ("[${cur}][v$i]xfade=transition=fade:duration=$fade`:offset=" + ("{0:N2}" -f $off) + "[x$i]")
    $acc = $off + $dur[$i]
    $cur = "x$i"
}
$parts += ("[$cur]fade=t=out:st=" + ("{0:N2}" -f ($acc - 0.6)) + ":d=0.6[vout]")

$ff += @("-filter_complex", ($parts -join ";"), "-map", "[vout]",
           "-c:v", "libx264", "-preset", "medium", "-crf", "20",
           "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-r", "30",
           (Join-Path $repo $OutFile))

& $ffmpeg @ff
if ($LASTEXITCODE -ne 0) { throw "ffmpeg 合成失败" }

$out = Join-Path $repo $OutFile
$mb = [Math]::Round((Get-Item $out).Length / 1MB, 2)
Write-Host ("完成: $out  ($mb MB, 约 " + [Math]::Round($acc, 1) + " 秒)")
Remove-Item $work -Recurse -Force -EA SilentlyContinue
