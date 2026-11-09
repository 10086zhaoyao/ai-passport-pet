<#
verify_image.ps1 —— 出包前必跑：确认 merged 里的 app 段就是当前源码编出来的那个 bin。

为什么要跑：`idf.py build` 不生成 merged，merged 是单独 merge_bin 出来的。
只要 app 没重编（或编译失败）就合一次，就会产出「新镜像 + 旧 app」的包，
而且它时间戳是新的、体积正常、image_info 也全绿，肉眼完全看不出来。

用法：powershell -ExecutionPolicy Bypass -File .\verify_image.ps1
可选：-Merged <path> -App <path> -AppOffset 0x10000 -NvsOffset 0x700000
退出码：0 = 全部通过；1 = 有问题，禁止发布。
#>
param(
    [string]$Merged  = (Join-Path $PSScriptRoot 'build_ui\FoloToy-AI-Passport-merged.bin'),
    [string]$App     = (Join-Path $PSScriptRoot 'build_ui\FoloToy-AI-Passport.bin'),
    [int]$AppOffset  = 0x10000,
    [int]$NvsOffset  = 0x700000
)

$ErrorActionPreference = 'Stop'
$script:bad = 0

function Ok($m)  { Write-Host "  PASS  $m" -ForegroundColor Green }
function Bad($m) { Write-Host "  FAIL  $m" -ForegroundColor Red; $script:bad++ }

if (-not (Test-Path $Merged)) { Write-Host "FAIL merged 不存在: $Merged"; exit 1 }
if (-not (Test-Path $App))    { Write-Host "FAIL app bin 不存在: $App";  exit 1 }

$appFile = Get-Item $App
$sha = [Security.Cryptography.SHA256]::Create()
$m   = [IO.File]::ReadAllBytes($Merged)
$a   = [IO.File]::ReadAllBytes($App)
$appHash = ($sha.ComputeHash($a) | ForEach-Object { $_.ToString('x2') }) -join ''

Write-Host ''
Write-Host '== 1. app bin =='
Write-Host ("  size   = {0} B (0x{0:X})" -f $appFile.Length)
Write-Host ("  sha256 = {0}" -f $appHash)
Write-Host ("  mtime  = {0}" -f $appFile.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))

Write-Host '== 2. merged 内 app 段与 app bin 逐字节一致 =='
if ($m.Length -lt $AppOffset + $a.Length) {
    Bad ("merged 太短: {0} B，装不下 0x{1:X} + {2} B" -f $m.Length, $AppOffset, $a.Length)
} else {
    $seg = New-Object byte[] $a.Length
    [Array]::Copy($m, $AppOffset, $seg, 0, $a.Length)
    $segHash = ($sha.ComputeHash($seg) | ForEach-Object { $_.ToString('x2') }) -join ''
    if ($segHash -eq $appHash) {
        Ok ("merged[0x{0:X} .. 0x{1:X}] == app bin" -f $AppOffset, ($AppOffset + $a.Length - 1))
    } else {
        Bad ("merged[0x{0:X}..] 与 app bin 不一致" -f $AppOffset)
        Write-Host ("        merged 段 = {0}" -f $segHash) -ForegroundColor DarkGray
        Write-Host ("        app bin   = {0}" -f $appHash) -ForegroundColor DarkGray
    }
}

Write-Host '== 3. app bin 必须比所有源码新 =='
$files = @()
foreach ($d in @((Join-Path $PSScriptRoot 'main'), (Join-Path $PSScriptRoot 'components'))) {
    if (Test-Path $d) { $files += Get-ChildItem -Path $d -Recurse -File -ErrorAction SilentlyContinue }
}
$files += Get-ChildItem -Path $PSScriptRoot -File -ErrorAction SilentlyContinue
$src = $files | Where-Object {
    ($_.Extension -in '.c', '.h', '.csv', '.txt', '.cmake', '.html', '.js') -or
    ($_.Name -eq 'sdkconfig') -or ($_.Name -eq 'sdkconfig.defaults')
}
if (-not $src) {
    Write-Host '  WARN  没找到源码文件，跳过此项' -ForegroundColor Yellow
} else {
    $newest = $src | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($appFile.LastWriteTime -gt $newest.LastWriteTime) {
        Ok ("app bin 比最新源码晚 {0:0.0} 分钟（最新源码: {1}）" -f ($appFile.LastWriteTime - $newest.LastWriteTime).TotalMinutes, $newest.Name)
    } else {
        Bad 'app bin 不比源码新 —— 源码改过但没重编，禁止出包'
        $src | Where-Object { $_.LastWriteTime -ge $appFile.LastWriteTime } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 8 |
            ForEach-Object { Write-Host ("        {0}  {1}" -f $_.LastWriteTime.ToString('HH:mm:ss'), $_.FullName.Replace($PSScriptRoot + '\', '')) }
    }
}

Write-Host '== 4. merged 不得覆盖 nvs 存档区 =='
if ($m.Length -gt $NvsOffset) {
    Bad ("merged {0} B 已越过 nvs 区 0x{1:X}" -f $m.Length, $NvsOffset)
} else {
    Ok ("merged {0} B，止于 0x{0:X}，距 0x{1:X} 还有 {2} B" -f $m.Length, $NvsOffset, ($NvsOffset - $m.Length))
}

Write-Host ''
if ($script:bad -eq 0) {
    Write-Host '结果: 全部通过，可以出包。' -ForegroundColor Green
    exit 0
}
Write-Host ("结果: {0} 项不通过，禁止发布。" -f $script:bad) -ForegroundColor Red
exit 1
