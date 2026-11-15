<#
  把 GitHub 上刚发布的产物同步到 Gitee Release。

  只在设置了 GITEE_TOKEN 时才会真正执行（否则直接跳过，不影响 GitHub 侧发布）。
  Token 获取：Gitee → 设置 → 私人令牌 → 生成新令牌，勾选 projects 权限。

  用法（由 .github/workflows/release.yml 调用）：
      $env:GITEE_TOKEN = "xxx"
      ./scripts/gitee_release.ps1 -Tag v1.0.0
#>
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Owner = "yvziyao",
    [string]$Repo = "FileEncryptor",
    [string]$TargetBranch = "master"
)

$ErrorActionPreference = "Stop"

$token = $env:GITEE_TOKEN
if (-not $token) {
    Write-Host "GITEE_TOKEN 未设置，跳过 Gitee 同步。"
    exit 0
}

$api = "https://gitee.com/api/v5/repos/$Owner/$Repo"

$files = @(
    "x64/Release/FileEncryptor.exe",
    "x64/Release/enc.exe",
    "x64/Release/FileEncryptorSetup.exe",
    "x64/Release/SHA256SUMS.txt",
    "x64/msix/FileEncryptor.msix"
) | Where-Object { Test-Path $_ }

Write-Host "待上传文件: $($files.Count) 个"

# ---------------------------------------------------------------- 1) 已有同名 release?
$releases = @(Invoke-RestMethod "$api/releases?access_token=$token&per_page=100")
$existing = $releases | Where-Object { $_.tag_name -eq $Tag } | Select-Object -First 1

if ($existing) {
    # 同一个 tag 重跑：Gitee 不允许重复创建，先删掉再重建。
    # 下载地址是 releases/download/<tag>/<文件名>，与 release id 无关，所以重建不影响
    # 程序里「检查更新」用的 URL。
    Write-Host "已存在 tag 为 $Tag 的 release (id=$($existing.id))，先删除再重建"
    Invoke-RestMethod -Method Delete "$api/releases/$($existing.id)?access_token=$token" | Out-Null
}

# ---------------------------------------------------------------- 2) 创建 release
$body = @{
    tag_name         = $Tag
    name             = "FileEncryptor $Tag"
    body             = "自动构建产物（GitHub Actions 构建后同步）。便携版、安装程序、CLI 与 MSIX。"
    target_commitish = $TargetBranch
    prerelease       = $false
} | ConvertTo-Json -Compress

$rel = Invoke-RestMethod -Method Post "$api/releases" `
        -Headers @{ "Content-Type" = "application/json" } -Body $body
Write-Host "已创建 Gitee release: $Tag (id=$($rel.id))"

# ---------------------------------------------------------------- 3) 上传附件
foreach ($f in $files) {
    $item = Get-Item $f
    $r = Invoke-RestMethod -Method Post "$api/releases/$($rel.id)/attach_files?access_token=$token" `
            -Form @{ file = $item }
    Write-Host ("  已上传 " + $r.name + " (" + [Math]::Round($item.Length / 1KB) + " KB)")
}

Write-Host "Gitee 同步完成: https://gitee.com/$Owner/$Repo/releases/tag/$Tag"
