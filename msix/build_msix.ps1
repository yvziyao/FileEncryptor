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
# 用 .NET GDI+ 画：深蓝底 + 白色 FE 字样（占位美术，商店可后续替换）
Add-Type -AssemblyName System.Drawing
function New-Asset([string]$path, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"
    $g.TextRenderingHint = "AntiAliasGridFit"
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Rectangle(0, 0, $w, $h)),
        [System.Drawing.Color]::FromArgb(255, 37, 99, 235),
        [System.Drawing.Color]::FromArgb(255, 30, 64, 175), 45.0)
    $g.FillRectangle($bg, 0, 0, $w, $h)
    $bg.Dispose()
    $size = [int]([Math]::Min($w, $h) * 0.42)
    $font = New-Object System.Drawing.Font("Segoe UI", $size, [System.Drawing.FontStyle]::Bold,
                                           [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = "Center"; $fmt.LineAlignment = "Center"
    $g.DrawString("FE", $font, [System.Drawing.Brushes]::White,
                  (New-Object System.Drawing.RectangleF(0, 0, $w, $h)), $fmt)
    $font.Dispose(); $fmt.Dispose(); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$assets = [ordered]@{
    "StoreLogo.png"          = @(50, 50)
    "Square44x44Logo.png"    = @(44, 44)
    "Square71x71Logo.png"    = @(71, 71)
    "Square150x150Logo.png"  = @(150, 150)
    "Wide310x150Logo.png"    = @(310, 150)
    "SplashScreen.png"       = @(620, 300)
}
foreach ($k in $assets.Keys) {
    New-Asset (Join-Path $payload "Assets\$k") $assets[$k][0] $assets[$k][1]
}
Write-Host "已生成 $($assets.Count) 个图标资源"

# ---------------------------------------------------------------- 3) 清单
$manifestSrc = Join-Path $PSScriptRoot "AppxManifest.xml"
$manifest = Get-Content $manifestSrc -Raw -Encoding UTF8
if ($manifest -match "REPLACE_WITH_PARTNER_CENTER_PUBLISHER") {
    Write-Warning "AppxManifest.xml 里的 Publisher 还是占位符：本地侧载没问题，但**上传商店前必须替换**。"
}
$manifest = $manifest -replace 'Version="\d+\.\d+\.\d+\.\d+"', "Version=`"$Version`""
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
