<#
  构建 FileEncryptor 的 MSIX 包。

  用法：
      powershell -ExecutionPolicy Bypass -File msix\build_msix.ps1
      powershell -ExecutionPolicy Bypass -File msix\build_msix.ps1 -Version 1.0.0.0

  产出：x64\msix\FileEncryptor.msix（已用本地自签名证书签名，可直接侧载测试）

  ⚠️ 上传到 Microsoft Store 前必须先把 msix\AppxManifest.xml 里的
     Publisher 换成 Partner Center 给的「发布者 ID」（形如 CN=8B1A...）。
     商店会用自己的证书重签，所以本地签名用哪张证书不影响上传；
     但 Publisher 必须与商店账号一致，否则包会被拒绝。
#>
param(
    [string]$Version = "1.0.0.0",
    [string]$CertPassword = "FileEncryptorMsix"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot          # 仓库根
$stage = Join-Path $repo "x64\msix"
$payload = Join-Path $stage "payload"
$msixOut = Join-Path $stage "FileEncryptor.msix"

function Find-SdkTool([string]$name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin",
               "$env:ProgramFiles\Windows Kits\10\bin",
               "D:\Windows Kits\10\bin")
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem $r -Recurse -Filter $name -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match '\\x64\\' } |
               Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$makeappx = Find-SdkTool "makeappx.exe"
$signtool = Find-SdkTool "signtool.exe"
if (-not $makeappx) { throw "找不到 makeappx.exe（需安装 Windows 10/11 SDK）" }
if (-not $signtool) { throw "找不到 signtool.exe（需安装 Windows 10/11 SDK）" }
Write-Host "makeappx: $makeappx"

# ---------------------------------------------------------------- 1) 准备负载
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $payload, (Join-Path $payload "Assets") | Out-Null

$gui = Join-Path $repo "x64\Release\FileEncryptor.exe"
$cli = Join-Path $repo "x64\Release\enc.exe"
foreach ($f in @($gui, $cli)) {
    if (-not (Test-Path $f)) { throw "缺少 $f，请先运行 build_setup.cmd" }
    Copy-Item $f $payload
}

# ---------------------------------------------------------------- 2) 生成商店图标
# MSIX 的磁贴/商店图标必须由清单指定的图片文件提供（Windows 不会像
# MSI/EXE 安装包那样自动从 exe 取图标），所以这些资源是**必需**的。
# 这里直接从程序真实图标（favicon.ico，也就是 exe 内嵌的那个）生成，
# 避免出现"占位美术"；以后要换成专门设计的商店图，替换 Assets\*.png 即可。
Add-Type -AssemblyName System.Drawing

$iconPath = Join-Path $repo "favicon.ico"
if (-not (Test-Path $iconPath)) { throw "缺少 favicon.ico（程序图标源）" }

# 直接从 ICO 里解出最大的那一帧。
# 注意：System.Drawing.Icon 按尺寸构造时只会给到 48x48（它读不了 ICO 里的
# 256x256 大帧），用它做 310 宽的磁贴会糊，所以这里手工解 BMP 帧。
$srcBitmap = $null
$ico = [System.IO.File]::ReadAllBytes($iconPath)
if ($ico.Length -gt 6) {
    $count = [BitConverter]::ToUInt16($ico, 4)
    $best = $null
    for ($i = 0; $i -lt $count; $i++) {
        $e = 6 + $i * 16
        $w = [int]$ico[$e]; if ($w -eq 0) { $w = 256 }
        $h = [int]$ico[$e + 1]; if ($h -eq 0) { $h = 256 }
        $bpp = [BitConverter]::ToUInt16($ico, $e + 6)
        $size = [BitConverter]::ToUInt32($ico, $e + 8)
        $off = [BitConverter]::ToUInt32($ico, $e + 12)
        if ($bpp -ne 32) { continue }
        if (-not $best -or ($w * $h) -gt ($best.w * $best.h)) {
            $best = [pscustomobject]@{ w = $w; h = $h; off = $off; size = $size; bpp = $bpp }
        }
    }
    if ($best) {
        # 帧数据是 BITMAPINFOHEADER + 自下而上的 BGRA
        $bmp = New-Object System.Drawing.Bitmap($best.w, $best.h,
                                                [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $rowBytes = $best.w * 4
        for ($y = 0; $y -lt $best.h; $y++) {
            $src = $best.off + 40 + ($best.h - 1 - $y) * $rowBytes
            for ($x = 0; $x -lt $best.w; $x++) {
                $q = $src + $x * 4
                $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($ico[$q + 3], $ico[$q + 2],
                                                                      $ico[$q + 1], $ico[$q]))
            }
        }
        $srcBitmap = $bmp
        Write-Host ("图标源: favicon.ico 内 " + $best.w + "x" + $best.h + " 帧")
    }
}
if (-not $srcBitmap) {
    # 兜底：退回 .NET 能读到的尺寸
    $srcIcon = New-Object System.Drawing.Icon($iconPath)
    $srcBitmap = $srcIcon.ToBitmap()
    $srcIcon.Dispose()
    Write-Host ("图标源（兜底）: " + $srcBitmap.Width + "x" + $srcBitmap.Height)
}

function New-TileAsset([string]$path, [int]$w, [int]$h, [double]$fill, $backColor) {
    $bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    if ($backColor) { $g.Clear($backColor) } else { $g.Clear([System.Drawing.Color]::Transparent) }

    # 等比缩放放进画布中央，留出 fill 比例的内边距
    $box = [Math]::Min($w, $h) * $fill
    $scale = [Math]::Min($box / $srcBitmap.Width, $box / $srcBitmap.Height)
    $dw = [int]($srcBitmap.Width * $scale)
    $dh = [int]($srcBitmap.Height * $scale)
    $dx = [int](($w - $dw) / 2)
    $dy = [int](($h - $dh) / 2)
    $g.DrawImage($srcBitmap, $dx, $dy, $dw, $dh)

    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$brand = [System.Drawing.Color]::FromArgb(255, 37, 99, 235)
$assets = [ordered]@{
    "StoreLogo.png"         = @(50, 50, 0.86, $null)     # 商店列表：透明底
    "Square44x44Logo.png"   = @(44, 44, 0.86, $null)     # 任务栏/开始菜单小图标
    "Square71x71Logo.png"   = @(71, 71, 0.86, $null)
    "Square150x150Logo.png" = @(150, 150, 0.80, $null)   # 开始菜单磁贴
    "Wide310x150Logo.png"   = @(310, 150, 0.72, $null)   # 宽磁贴
    "SplashScreen.png"      = @(620, 300, 0.38, $brand)  # 启动画面（实底更稳）
}
foreach ($k in $assets.Keys) {
    $a = $assets[$k]
    New-TileAsset (Join-Path $payload "Assets\$k") $a[0] $a[1] $a[2] $a[3]
}
$srcBitmap.Dispose()
Write-Host "已从程序图标生成 $($assets.Count) 个 MSIX 图标资源"

# ---------------------------------------------------------------- 3) 清单
$manifestSrc = Join-Path $PSScriptRoot "AppxManifest.xml"
$manifest = Get-Content $manifestSrc -Raw -Encoding UTF8
if ($manifest -match "REPLACE_WITH_PARTNER_CENTER_PUBLISHER") {
    Write-Warning "AppxManifest.xml 里的 Publisher 还是占位符：本地侧载没问题，但**上传商店前必须替换**。"
}
# 只替换 <Identity ...> 里的 Version 属性。
# 不能用裸的 'Version="x.y.z.w"' 正则：它会同时匹配到 TargetDeviceFamily 里的
# MinVersion="10.0.17763.0"（"MinVersion" 也以 "Version" 结尾），
# 把平台最低版本错改成 1.0.0.0 —— 商店会直接拒收
# （"不可上传面向 Windows MinVersion <= 10.0.17134.0 的程序包"）。
$manifest = $manifest -replace '(<Identity[^>]*?\sVersion=")\d+\.\d+\.\d+\.\d+(")', "`${1}$Version`${2}"

# 校验：Identity 版本已更新，且 MinVersion 仍 >= 10.0.17763.0（商店的硬性下限）
$idVer = [regex]::Match($manifest, '<Identity[^>]*?\sVersion="([^"]+)"').Groups[1].Value
$minVer = [regex]::Match($manifest, 'MinVersion="([^"]+)"').Groups[1].Value
if ($idVer -ne $Version) { throw "清单 Identity/@Version 替换失败：得到 $idVer" }
if (-not $minVer -or [version]$minVer -lt [version]"10.0.17763.0") {
    throw "清单 MinVersion 无效（$minVer）：Microsoft Store 不接受 <= 10.0.17134.0 的包"
}
Write-Host ("清单版本 " + $idVer + " / 平台最低 " + $minVer)
# 以 UTF-8（无 BOM）写出，MakeAppx 不接受带 BOM 的清单
[System.IO.File]::WriteAllText((Join-Path $payload "AppxManifest.xml"), $manifest,
                               (New-Object System.Text.UTF8Encoding($false)))

# ---------------------------------------------------------------- 4) 打包
& $makeappx pack /d $payload /p $msixOut /o | Write-Host
if (-not (Test-Path $msixOut)) { throw "打包失败" }

# ---------------------------------------------------------------- 5) 签名（本地侧载用）
$publisherCn = ([regex]::Match($manifest, 'Publisher="([^"]+)"')).Groups[1].Value
$pfx = Join-Path $stage "FileEncryptor-test.pfx"
$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $publisherCn -and $_.HasPrivateKey } |
        Select-Object -First 1
if (-not $cert -and $publisherCn -notmatch "REPLACE_WITH") {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $publisherCn `
            -KeyUsage DigitalSignature -FriendlyName "FileEncryptor MSIX (test)" `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
    Write-Host "已创建自签名测试证书: $publisherCn"
}
if ($cert) {
    $pwd = ConvertTo-SecureString -String $CertPassword -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pwd | Out-Null
    & $signtool sign /fd SHA256 /f $pfx /p $CertPassword /a $msixOut | Write-Host
    Write-Host "已签名（本地测试证书）。侧载前需把该证书导入「受信任的根证书颁发机构」。"
}
else {
    Write-Warning "清单 Publisher 仍是占位符，跳过签名（商店上传时由微软签名，可忽略）。"
}

$item = Get-Item $msixOut
Write-Host ""
Write-Host ("MSIX 就绪: " + $item.FullName)
Write-Host ("大小: " + [Math]::Round($item.Length / 1KB) + " KB")
Write-Host ("SHA256: " + (Get-FileHash $msixOut -Algorithm SHA256).Hash)
Write-Host ""
Write-Host "侧载安装: Add-AppxPackage -Path ""$msixOut"""
Write-Host "商店上传: Partner Center → 提交 → 包 → 上传该 .msix"
