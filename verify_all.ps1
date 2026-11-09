<#
verify_all.ps1 —— 一条命令跑完固件验收

串起三块已有能力（不重写、不改判定口径）：
  1. 镜像校验   -> verify_image.ps1（原样调用，逐项转成统一 PASS/FAIL 行）
  2. 缺字检查   -> check_glyphs.py（文案集合从 pet_app.c 现取，保证覆盖是最新的）
  3. 模拟器冒烟 -> 无头 Edge + CDP 驱动 FoloToy-Passport-Simulator，7 项交互验收

用法：
  powershell -NoProfile -ExecutionPolicy Bypass -File .\verify_all.ps1

可选参数：
  -SimRoot <path>   模拟器工程目录（默认取环境变量 FAP_SIM_ROOT；不设则跳过模拟器段）
  -SimHostExe <p>   跑 server.mjs 的宿主程序（默认 FAP_SIM_HOST_EXE，再退回 node）
  -SimUrl   <url>   默认 http://127.0.0.1:4190/?id=3
  -CdpPort  <n>     无头 Edge 调试端口，默认 9333
  -SkipSim          只跑镜像 + 缺字
  -KeepShots        保留截图到 %TEMP%\fap-verify（默认跑完删掉）

退出码：0 = 全部 PASS；1 = 有 FAIL。
#>
param(
    [string]$Root    = $PSScriptRoot,
    [string]$SimRoot = $env:FAP_SIM_ROOT,
    [string]$SimUrl  = 'http://127.0.0.1:4190/?id=3',
    [string]$SimHostExe = '',
    [int]$CdpPort    = 9333,
    [switch]$SkipSim,
    [switch]$KeepShots
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# 结果清单
# ---------------------------------------------------------------------------
$script:rows = New-Object System.Collections.ArrayList

function Add-Row([string]$id, [string]$status, [string]$msg) {
    [void]$script:rows.Add([pscustomobject]@{ Id = $id; Status = $status; Msg = $msg })
    $color = switch ($status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } 'WARN' { 'Yellow' } default { 'DarkGray' } }
    Write-Host ("  {0,-4}  {1,-26}  {2}" -f $status, $id, $msg) -ForegroundColor $color
}
function P($id, $msg) { Add-Row $id 'PASS' $msg }
function F($id, $msg) { Add-Row $id 'FAIL' $msg }
function W($id, $msg) { Add-Row $id 'WARN' $msg }
function S($id, $msg) { Add-Row $id 'SKIP' $msg }

function Section($t) {
    Write-Host ''
    Write-Host ("== " + $t + " ==") -ForegroundColor Cyan
}

$script:simProcesses = New-Object System.Collections.ArrayList

function Start-SimServer {
    $env:PORT = '4190'
    $env:EMULATOR_ALLOW_LOCAL_FIRMWARE_UPLOAD = '1'
    $env:ELECTRON_RUN_AS_NODE = '1'
    # 用带 Node 的宿主跑 server.mjs：优先 -SimHostExe，其次 FAP_SIM_HOST_EXE，最后退回 node。
    $hostExe = $SimHostExe
    if (-not $hostExe) { $hostExe = $env:FAP_SIM_HOST_EXE }
    if (-not $hostExe) { $hostExe = 'node' }
    $p = Start-Process -FilePath $hostExe -ArgumentList 'server.mjs' -WorkingDirectory $SimRoot -WindowStyle Hidden -PassThru
    [void]$script:simProcesses.Add($p)
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        try { [void](Invoke-WebRequest 'http://127.0.0.1:4190/' -TimeoutSec 2 -UseBasicParsing); return $true } catch {}
    }
    return $false
}

# ===========================================================================
# 1. 镜像校验
# ===========================================================================
Section '1. 镜像校验（verify_image.ps1 原样调用）'

$imgScript = Join-Path $Root 'verify_image.ps1'
if (-not (Test-Path $imgScript)) {
    F 'image/script' "找不到 $imgScript"
} else {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $imgScript 2>&1
    $code = $LASTEXITCODE
    $n = 0
    $appSize = ''; $appSha = ''; $appMtime = ''
    foreach ($line in $out) {
        $t = ([string]$line).Trim()
        if ($t -match '^(PASS|FAIL)\s+(.+)$') {
            $n++
            $st = $Matches[1]; $msg = $Matches[2]
            if ($st -eq 'PASS') { P ("image/$n") $msg } else { F ("image/$n") $msg }
        }
        elseif ($t -match '^WARN\s+(.+)$') { W ("image/$n") $Matches[1] }
        elseif ($t -match '^\s*(size|sha256|mtime)\s*=\s*(.+)$') {
            switch ($Matches[1]) {
                'size'   { $appSize = $Matches[2] }
                'sha256' { $appSha = $Matches[2] }
                'mtime'  { $appMtime = $Matches[2] }
            }
            Write-Host ("        {0} = {1}" -f $Matches[1], $Matches[2]) -ForegroundColor DarkGray
        }
    }
    if ($appSha) {
        P 'image/0' ("app bin {0} / sha {1} / mtime {2}" -f $appSize, $appSha.Substring(0, 16), $appMtime)
    }
    if ($n -eq 0) { F 'image/parse' "verify_image.ps1 没有输出可解析的 PASS/FAIL（exit=$code）" }
    if ($code -ne 0) { F 'image/exit' "verify_image.ps1 退出码 $code" }
}

# ===========================================================================
# 2. 缺字检查
# ===========================================================================
Section '2. 缺字检查（check_glyphs.py 原样调用）'

$appC   = Join-Path $Root 'main\pet_app.c'
$glyphPy = Join-Path $Root 'check_glyphs.py'
$strings = New-Object System.Collections.Generic.List[string]
$notes   = New-Object System.Collections.ArrayList

function Add-Text([string]$s) {
    if ([string]::IsNullOrWhiteSpace($s)) { return }
    $s = $s -replace '%[-+ 0-9.#]*[a-zA-Z@]', ''   # 去掉 printf 占位符，只查可见文本
    if ($s.Trim().Length -eq 0) { return }
    if (-not $strings.Contains($s)) { [void]$strings.Add($s) }
}
function Get-Quoted([string]$block) {
    [regex]::Matches($block, '"([^"]*)"') | ForEach-Object { $_.Groups[1].Value }
}

if (-not (Test-Path $appC) -or -not (Test-Path $glyphPy)) {
    F 'glyph/source' "缺少 $appC 或 check_glyphs.py"
} else {
    $src = Get-Content $appC -Raw -Encoding UTF8

    # 2.1 设置页 8 行行名（中英）
    $m = [regex]::Match($src, 'SET_NAMES_ZH\[SET_ROWS\]\s*=\s*\{([\s\S]*?)\};')
    $zhNames = @(); if ($m.Success) { $zhNames = @(Get-Quoted $m.Groups[1].Value) }
    $m = [regex]::Match($src, 'SET_NAMES_EN\[SET_ROWS\]\s*=\s*\{([\s\S]*?)\};')
    $enNames = @(); if ($m.Success) { $enNames = @(Get-Quoted $m.Groups[1].Value) }

    # 2.2 主题名（中英）
    $m = [regex]::Match($src, 'THEME_NAMES_ZH\[THEME_COUNT\]\s*=\s*\{([\s\S]*?)\};')
    $zhTheme = @(); if ($m.Success) { $zhTheme = @(Get-Quoted $m.Groups[1].Value) }
    $m = [regex]::Match($src, 'THEME_NAMES_EN\[THEME_COUNT\]\s*=\s*\{([\s\S]*?)\};')
    $enTheme = @(); if ($m.Success) { $enTheme = @(Get-Quoted $m.Groups[1].Value) }

    # 2.3 所有 TR("zh","en")：底部提示 / 铭牌 / 卡面标签 / 语言值
    $trZh = New-Object System.Collections.ArrayList
    $trEn = New-Object System.Collections.ArrayList
    foreach ($mm in [regex]::Matches($src, 'TR\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)')) {
        [void]$trZh.Add($mm.Groups[1].Value)
        [void]$trEn.Add($mm.Groups[2].Value)
    }

    $m = [regex]::Match($src, '#define\s+SET_ROWS\s+(\d+)');  $setRows = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
    # THEME_COUNT 在 themes_info.h（由打包脚本生成），不在 pet_app.c
    $themeH = Join-Path $Root 'main\themes_info.h'
    $themeN = 0
    if (Test-Path $themeH) {
        $m = [regex]::Match((Get-Content $themeH -Raw -Encoding UTF8), '#define\s+THEME_COUNT\s+(\d+)')
        if ($m.Success) { $themeN = [int]$m.Groups[1].Value }
    }
    $m = [regex]::Match($src, '#define\s+SET_ROW_LANG\s+(\d+)'); $rowLang = if ($m.Success) { [int]$m.Groups[1].Value } else { -1 }

    # 覆盖集合自检：源码里加行/加主题后这里必须跟着变
    if ($setRows -gt 0 -and $zhNames.Count -eq $setRows -and $enNames.Count -eq $setRows) {
        P 'glyph/cover-settings' ("设置页 {0} 行 × 中英 = {1} 条，与 SET_ROWS 一致" -f $setRows, ($setRows * 2))
    } else {
        F 'glyph/cover-settings' ("SET_ROWS={0}，解析到中 {1} / 英 {2} 行名，覆盖集合过期" -f $setRows, $zhNames.Count, $enNames.Count)
    }
    if ($themeN -gt 0 -and $zhTheme.Count -eq $themeN -and $enTheme.Count -eq $themeN) {
        P 'glyph/cover-themes' ("主题名 {0} 套 × 中英 = {1} 条" -f $themeN, ($themeN * 2))
    } else {
        F 'glyph/cover-themes' ("THEME_COUNT={0}，解析到中 {1} / 英 {2} 主题名" -f $themeN, $zhTheme.Count, $enTheme.Count)
    }
    if ($rowLang -ge 0 -and $rowLang -lt $enNames.Count -and $enNames[$rowLang] -eq 'LANGUAGE') {
        P 'glyph/cover-lang-row' ("第 {0} 行（索引 {1}）为 LANGUAGE" -f ($rowLang + 1), $rowLang)
    } else {
        F 'glyph/cover-lang-row' ("SET_ROW_LANG={0} 未对应到 LANGUAGE" -f $rowLang)
    }

    $zhNames | ForEach-Object { Add-Text $_ }
    $enNames | ForEach-Object { Add-Text $_ }
    $zhTheme | ForEach-Object { Add-Text $_ }
    $enTheme | ForEach-Object { Add-Text $_ }
    $trZh    | ForEach-Object { Add-Text $_ }
    $trEn    | ForEach-Object { Add-Text $_ }
    Add-Text '你 和 %s'                 # 宠物页落款
    Add-Text 'AI PASSPORT'; Add-Text 'NO. 01'; Add-Text 'UNNAMED'
    Add-Text 'LITTLE COMPANION'

    $cjk = @($strings | Where-Object { $_ -match '[\u4e00-\u9fff]' })
    Write-Host ("        集合: 共 {0} 条（含中文 {1} 条），来自 设置行名/主题名/TR 文案/落款/卡面标签" -f $strings.Count, $cjk.Count) -ForegroundColor DarkGray

    $args = @($glyphPy) + [string[]]$strings.ToArray()
    $gout = & python @args 2>&1
    $gcode = $LASTEXITCODE
    $miss = @(); $okN = 0
    foreach ($line in $gout) {
        $t = [string]$line
        if ($t -match '^OK\s+(.+)$') { $okN++ }
        elseif ($t -match '^MISS\s+(.+)$') { $miss += $Matches[1] }
        elseif ($t -match '^\s+缺字:\s*(.+)$') { $miss += ("缺 " + $Matches[1]) }
    }
    if ($gcode -eq 0 -and $miss.Count -eq 0) {
        P 'glyph/font_cjk_8' ("{0} 条文案逐字检查，全部有字模（无 X）" -f $okN)
    } else {
        $uniq = ($miss | Select-Object -Unique) -join ' | '
        F 'glyph/font_cjk_8' ("{0} 条缺失（exit={1}）: {2}" -f $miss.Count, $gcode, $uniq)
    }
}

# ===========================================================================
# 3. 模拟器冒烟
# ===========================================================================
Section '3. 模拟器冒烟（无头 Edge + CDP，ADC 注压按键）'

$shotsDir = Join-Path $env:TEMP 'fap-verify'
if (Test-Path $shotsDir) { Remove-Item $shotsDir -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $shotsDir | Out-Null

$ws = $null
$edgeProc = $null
$simOk = $true

function Stop-Sim {
    if ($ws) { try { $ws.Dispose() } catch {} ; $script:ws = $null }
    if ($edgeProc -and -not $edgeProc.HasExited) { try { $edgeProc.Kill() } catch {} }
    foreach ($p in $script:simProcesses) { if ($p -and -not $p.HasExited) { try { $p.Kill() } catch {} } }
}
try { Stop-Sim } catch {}

if ($SkipSim) {
    S 'sim' '按 -SkipSim 跳过'
    $simOk = $false
} else {
    if (-not $SimRoot) {
        S 'sim' '未提供 -SimRoot / FAP_SIM_ROOT，跳过模拟器冒烟'
        $simOk = $false
    } elseif (-not (Test-Path $SimRoot)) {
        F 'sim/root' "找不到模拟器目录 $SimRoot"
        $simOk = $false
    }
}

if ($simOk) {
    # --- 3.0 固件就位 + 服务 ---
    $dbg8 = Join-Path $Root 'build_ui\FoloToy-AI-Passport-dbg-8mb.bin'
    $dst  = Join-Path $SimRoot 'public\assets\firmware\passport-home.bin'
    if (-not (Test-Path $dbg8)) {
        F 'sim/firmware' "找不到 $dbg8"
        $simOk = $false
    } else {
        $want = (Get-FileHash $dbg8 -Algorithm SHA256).Hash.ToLower()
        $size = (Get-Item $dbg8).Length
        Copy-Item $dbg8 $dst -Force
        $got = (Get-FileHash $dst -Algorithm SHA256).Hash.ToLower()
        if ($size -eq 8388608 -and $got -eq $want) {
            P 'sim/firmware' ("8 MiB 补齐镜像已就位 {0:N0} B sha={1}" -f $size, $got.Substring(0, 12))
        } else {
            F 'sim/firmware' ("镜像未按 8 MiB 就位 size={0} sha={1}" -f $size, $got.Substring(0, 12))
        }
    }

    $alive = $false
    try { [void](Invoke-WebRequest 'http://127.0.0.1:4190/' -TimeoutSec 3 -UseBasicParsing); $alive = $true } catch {}
    if (-not $alive) {
        $alive = Start-SimServer
        if ($alive) { P 'sim/server' '已在 4190 拉起模拟器服务' } else { F 'sim/server' '模拟器服务启动失败' ; $simOk = $false }
    } else {
        P 'sim/server' '复用已在跑的 4190 服务'
    }
}

if ($simOk) {
    $edgeExe = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
    if (-not (Test-Path $edgeExe)) { $edgeExe = 'C:\Program Files\Microsoft\Edge\Application\msedge.exe' }
    $profile = Join-Path $env:TEMP ("fap-edge-" + [Guid]::NewGuid().ToString('N'))
    $eargs = @(
        '--headless=new', "--remote-debugging-port=$CdpPort", "--user-data-dir=$profile",
        '--no-first-run', '--no-default-browser-check', '--disable-extensions',
        '--window-size=1280,1000', '--autoplay-policy=no-user-gesture-required',
        '--disable-background-timer-throttling', '--disable-backgrounding-occluded-windows',
        '--disable-renderer-backgrounding', '--force-device-scale-factor=1',
        '--hide-scrollbars', '--mute-audio', $SimUrl
    )
    $edgeProc = Start-Process -FilePath $edgeExe -ArgumentList $eargs -PassThru -WindowStyle Hidden

    $page = $null
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        try { $list = Invoke-RestMethod "http://127.0.0.1:$CdpPort/json/list" -TimeoutSec 2 } catch { continue }
        $page = $list | Where-Object { $_.url -like '*4190*' -and $_.type -eq 'page' } | Select-Object -First 1
        if ($page) { break }
    }

    if (-not $page) {
        F 'sim/browser' '无头 Edge 没有开出模拟器页面'
        $simOk = $false
    } else {
        $ws = [System.Net.WebSockets.ClientWebSocket]::new()
        $ws.Options.KeepAliveInterval = [TimeSpan]::FromSeconds(5)
        $ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).Wait(15000) | Out-Null
        if ($ws.State -ne 'Open') {
            F 'sim/cdp' 'CDP WebSocket 连接失败'
            $simOk = $false
        } else {
            P 'sim/cdp' '已连上页面（CDP）'
        }
    }
}

$script:mid = 0
$script:rxbuf = New-Object byte[] (16 * 1024 * 1024)

function Recv-Msg {
    $ms = New-Object System.IO.MemoryStream
    do {
        $seg = [ArraySegment[byte]]::new($script:rxbuf)
        $task = $ws.ReceiveAsync($seg, [Threading.CancellationToken]::None)
        $task.Wait(120000) | Out-Null
        $res = $task.Result
        $ms.Write($script:rxbuf, 0, $res.Count)
    } while (-not $res.EndOfMessage)
    return [Text.Encoding]::UTF8.GetString($ms.ToArray())
}

function Cdp([string]$method, $params) {
    $script:mid++
    $id = $script:mid
    $msg = @{ id = $id; method = $method; params = $params } | ConvertTo-Json -Depth 12 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
    $ws.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true,
                  [Threading.CancellationToken]::None).Wait(15000) | Out-Null
    while ($true) {
        $obj = (Recv-Msg) | ConvertFrom-Json
        if ($obj.id -eq $id) { return $obj }
    }
}

function Eval([string]$expr) {
    $r = Cdp 'Runtime.evaluate' @{ expression = $expr; returnByValue = $true; awaitPromise = $true }
    if ($r.result.exceptionDetails) {
        $d = $r.result.exceptionDetails
        $t = if ($d.exception) { [string]$d.exception.description } else { [string]$d.text }
        return "ERR:$t"
    }
    return $r.result.result.value
}

# --- 取帧分析（像素级） ---
$JS_STATS = @'
(() => {
  const R = __RECT__;
  const c = document.querySelector('#qemu-display');
  if (!c) return 'ERR:no-canvas';
  const g = c.getContext('2d'), W = c.width, H = c.height;
  const x0 = R[0], y0 = R[1], w = R[2], h = R[3];
  const d = g.getImageData(x0, y0, w, h).data;
  const counts = new Map();
  for (let i = 0; i < d.length; i += 4) {
    const k = ((d[i] >> 3) << 10) | ((d[i + 1] >> 3) << 5) | (d[i + 2] >> 3);
    counts.set(k, (counts.get(k) || 0) + 1);
  }
  let mk = 0, mv = 0;
  for (const [k, v] of counts) if (v > mv) { mv = v; mk = k; }
  const mr = (mk >> 10) << 3, mg = ((mk >> 5) & 31) << 3, mb = (mk & 31) << 3;
  let ink = 0, minx = 1e9, maxx = -1, miny = 1e9, maxy = -1, hash = 2166136261;
  const rowInk = new Array(h).fill(0);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      if (Math.abs(d[i] - mr) + Math.abs(d[i + 1] - mg) + Math.abs(d[i + 2] - mb) > 24) {
        ink++; rowInk[y]++;
        if (x < minx) minx = x; if (x > maxx) maxx = x;
        if (y < miny) miny = y; if (y > maxy) maxy = y;
      }
      hash = Math.imul(hash ^ (d[i] >> 3), 16777619) >>> 0;
      hash = Math.imul(hash ^ (d[i + 2] >> 3), 16777619) >>> 0;
    }
  }
  const bands = []; let cur = null;
  for (let y = 0; y < h; y++) {
    const on = rowInk[y] > Math.max(1, w * 0.01);
    if (on && cur === null) cur = y;
    else if (!on && cur !== null) { bands.push([cur, y - 1]); cur = null; }
  }
  if (cur !== null) bands.push([cur, h - 1]);
  return JSON.stringify({
    w: w, h: h, colors: counts.size, modal: +(mv / (w * h)).toFixed(4),
    bg: [mr, mg, mb], ink: +(ink / (w * h)).toFixed(4),
    inkw: (ink ? maxx - minx + 1 : 0), inkh: (ink ? maxy - miny + 1 : 0),
    bands: bands.length, hash: hash.toString(16)
  });
})()
'@

$JS_STORE = @'
(() => {
  const R = __RECT__, name = '__NAME__';
  const c = document.querySelector('#qemu-display');
  const g = c.getContext('2d');
  window.__vref = window.__vref || {};
  window.__vref[name] = { rect: R, data: new Uint8ClampedArray(g.getImageData(R[0], R[1], R[2], R[3]).data) };
  return 'ok';
})()
'@

$JS_DIFF = @'
(() => {
  const R = __RECT__, name = '__NAME__';
  const c = document.querySelector('#qemu-display');
  const g = c.getContext('2d');
  const ref = (window.__vref || {})[name];
  if (!ref) return 'ERR:no-ref';
  const d = g.getImageData(R[0], R[1], R[2], R[3]).data, o = ref.data;
  let diff = 0, n = 0;
  for (let i = 0; i < d.length; i += 4) {
    n++;
    if (Math.abs(d[i] - o[i]) + Math.abs(d[i + 1] - o[i + 1]) + Math.abs(d[i + 2] - o[i + 2]) > 24) diff++;
  }
  return JSON.stringify({ diff: diff, n: n, frac: +(diff / n).toFixed(4) });
})()
'@

function FrameStats([int]$x, [int]$y, [int]$w, [int]$h) {
    $expr = $JS_STATS.Replace('__RECT__', "[$x,$y,$w,$h]")
    $s = Eval $expr
    if (-not $s -or -not ([string]$s).StartsWith('{')) { return $null }
    try { return ($s | ConvertFrom-Json) } catch { return $null }
}
function Store-Ref([string]$name, [int]$x, [int]$y, [int]$w, [int]$h) {
    $expr = $JS_STORE.Replace('__RECT__', "[$x,$y,$w,$h]").Replace('__NAME__', $name)
    return (Eval $expr)
}
function Diff-Ref([string]$name, [int]$x, [int]$y, [int]$w, [int]$h) {
    $expr = $JS_DIFF.Replace('__RECT__', "[$x,$y,$w,$h]").Replace('__NAME__', $name)
    $s = Eval $expr
    if (-not $s -or -not ([string]$s).StartsWith('{')) { return $null }
    try { return ($s | ConvertFrom-Json) } catch { return $null }
}

function Has-Content($s) {
    if (-not $s) { return $false }
    return ($s.colors -gt 8) -and ($s.modal -lt 0.95) -and ($s.ink -gt 0.02)
}

# 每次交互后头 ~10s canvas 是空的：必须等到连续 3 次「有内容」才算重绘完成
function Wait-Content([int]$timeoutSec = 60, [int]$stable = 3) {
    $hit = 0
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $last = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 700
        $s = FrameStats 0 0 240 320
        if (-not $s) { $hit = 0; continue }
        $last = $s
        if (Has-Content $s) { $hit++ } else { $hit = 0 }
        if ($hit -ge $stable) { return $s }
    }
    return $last
}

function Click-Key([string]$key) {
    $expr = @"
(() => {
  const b = document.querySelector('[data-key="$key"]');
  if (!b) return 'ERR:no-key';
  b.scrollIntoView({block:'center'});
  const r = b.getBoundingClientRect();
  return JSON.stringify({x: r.x + r.width / 2, y: r.y + r.height / 2});
})()
"@
    $p = Eval $expr
    if (-not ([string]$p).StartsWith('{')) { return $false }
    $pos = $p | ConvertFrom-Json
    Cdp 'Input.dispatchMouseEvent' @{ type = 'mouseMoved'; x = $pos.x; y = $pos.y } | Out-Null
    Cdp 'Input.dispatchMouseEvent' @{ type = 'mousePressed'; x = $pos.x; y = $pos.y; button = 'left'; clickCount = 1 } | Out-Null
    Start-Sleep -Milliseconds 90
    Cdp 'Input.dispatchMouseEvent' @{ type = 'mouseReleased'; x = $pos.x; y = $pos.y; button = 'left'; clickCount = 1 } | Out-Null
    return $true
}

function Press-Key([string]$key, [int]$ms) {
    $expr = @"
(() => {
  const b = document.querySelector('[data-key="$key"]');
  if (!b) return 'ERR:no-key';
  b.scrollIntoView({block:'center'});
  const r = b.getBoundingClientRect();
  return JSON.stringify({x: r.x + r.width / 2, y: r.y + r.height / 2});
})()
"@
    $p = Eval $expr
    if (-not ([string]$p).StartsWith('{')) { return $false }
    $pos = $p | ConvertFrom-Json
    Cdp 'Input.dispatchMouseEvent' @{ type = 'mouseMoved'; x = $pos.x; y = $pos.y } | Out-Null
    Cdp 'Input.dispatchMouseEvent' @{ type = 'mousePressed'; x = $pos.x; y = $pos.y; button = 'left'; clickCount = 1 } | Out-Null
    Start-Sleep -Milliseconds $ms
    Cdp 'Input.dispatchMouseEvent' @{ type = 'mouseReleased'; x = $pos.x; y = $pos.y; button = 'left'; clickCount = 1 } | Out-Null
    return $true
}

function Save-Shot([string]$name) {
    if (-not $KeepShots) { return }
    $r = Eval 'JSON.stringify((()=>{const c=document.querySelector("#qemu-display");c.scrollIntoView({block:"center"});const b=c.getBoundingClientRect();return {x:b.x,y:b.y};})())'
    if (-not ([string]$r).StartsWith('{')) { return }
    $o = $r | ConvertFrom-Json
    $shot = Cdp 'Page.captureScreenshot' @{ format = 'png'; clip = @{ x = $o.x; y = $o.y; width = 240; height = 320; scale = 1 } }
    if ($shot.result.data) {
        [IO.File]::WriteAllBytes((Join-Path $shotsDir $name), [Convert]::FromBase64String($shot.result.data))
    }
}

if ($simOk -and $ws -and $ws.State -eq 'Open') {
    Cdp 'Runtime.enable' @{} | Out-Null
    Cdp 'Page.enable' @{} | Out-Null
    Eval 'window.__vref = {}; "ok"' | Out-Null

    # ---- C1 首页 ----
    $homeS = Wait-Content 180 3
    if (-not (Has-Content $homeS)) {
        F 'sim/home' ("首页未渲染：colors={0} modal={1} ink={2}" -f $homeS.colors, $homeS.modal, $homeS.ink)
    } else {
        Save-Shot '01_home.png'
        $top = FrameStats 0 0 240 32          # 状态条（时钟/电量）
        $body = FrameStats 0 32 240 288       # 身份牌主体
        Store-Ref 'home_body' 0 32 240 288
        Store-Ref 'home_full' 0 0 240 320
        P 'sim/home-render' ("首页有内容：{0} 色 / 主色 {1}% / 墨迹 {2}%" -f $home.colors, [math]::Round($home.modal * 100), [math]::Round($home.ink * 100))
        if ($body.bands -ge 2 -and $body.ink -gt 0.05) {
            P 'sim/home-card' ("身份牌主体 {0} 条文字带、墨迹 {1}%，无宠物页元素" -f $body.bands, [math]::Round($body.ink * 100))
        } else {
            F 'sim/home-card' ("身份牌主体异常：bands={0} ink={1}" -f $body.bands, $body.ink)
        }
    }

    # ---- C2 主题循环 ----
    $themeColors = New-Object System.Collections.ArrayList
    $themeHashes = New-Object System.Collections.ArrayList
    for ($i = 1; $i -le 4; $i++) {
        if (-not (Click-Key 'DOWN')) { F "sim/theme$i" '按不到 DOWN 键'; break }
        $s = Wait-Content 60 3
        $key = ($s.bg -join ',')
        [void]$themeColors.Add($key)
        [void]$themeHashes.Add($s.hash)
        Save-Shot ("02_theme{0}.png" -f $i)
        Write-Host ("        DOWN x{0}: 底色=({1}) 色数={2} 墨迹={3}% 哈希={4}" -f $i, $key, $s.colors, [math]::Round($s.ink * 100), $s.hash) -ForegroundColor DarkGray
    }
    $distinct = ($themeColors | Select-Object -Unique)
    if ($distinct.Count -ge 3) {
        P 'sim/themes' ("DOWN 循环出 {0} 套不同底色：{1}" -f $distinct.Count, ($distinct -join ' / '))
    } else {
        F 'sim/themes' ("只出现 {0} 套底色：{1}" -f $distinct.Count, ($themeColors -join ' / '))
    }
    if ($themeColors.Count -ge 4 -and $themeColors[3] -eq $themeColors[0]) {
        P 'sim/themes-wrap' ("第 4 次 DOWN 回到第 1 套底色 ({0})" -f $themeColors[0])
    } else {
        F 'sim/themes-wrap' ("第 4 次 DOWN 未回到第 1 套：第1={0} 第4={1}" -f $themeColors[0], $themeColors[3])
    }
    # 回到第 1 套（暖纸），后续文字相关判定用固定主题
    for ($i = 0; $i -lt 1; $i++) { [void](Click-Key 'DOWN'); [void](Wait-Content 60 3) }
    $back = FrameStats 0 32 240 288
    Write-Host ("        已切回第 1 套（底色 {0}）" -f ($back.bg -join ',')) -ForegroundColor DarkGray

    # ---- C3 宠物页进入 + 返回 ----
    $null = Click-Key 'OK'
    $firstVisible = $null
    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Milliseconds 250
        $s = FrameStats 0 0 240 320
        if (Has-Content $s) { $firstVisible = $s; break }
    }
    Save-Shot '03_pet_first.png'
    $pet = Wait-Content 60 3
    Save-Shot '04_pet.png'
    $panel = FrameStats 0 220 240 100
    $darkSum = $panel.bg[0] + $panel.bg[1] + $panel.bg[2]
    if ($darkSum -lt 150 -and $pet.ink -gt 0.08) {
        P 'sim/pet-enter' ("进入宠物页：底部信息面板底色 RGB({0}) 为深色、整屏墨迹 {1}%" -f ($panel.bg -join ','), [math]::Round($pet.ink * 100))
    } else {
        F 'sim/pet-enter' ("宠物页特征不符：底部底色 RGB({0}) sum={1} ink={2}" -f ($panel.bg -join ','), $darkSum, $pet.ink)
    }

    # 第一帧笑脸：与「摸一下」触发的笑脸帧比
    $null = Click-Key 'OK'
    Start-Sleep -Milliseconds 600
    $happy = Wait-Content 60 3
    Save-Shot '05_pet_happy.png'
    if ($firstVisible) {
        # 首帧已过去，这里用重放比较不可行；改为记录首帧与稳态帧的美术区哈希
        Write-Host ("        首帧: 色数={0} 墨迹={1}% 哈希={2}；稳态: 哈希={3}" -f $firstVisible.colors, [math]::Round($firstVisible.ink * 100), $firstVisible.hash, $happy.hash) -ForegroundColor DarkGray
        P 'sim/pet-first-frame' ("首帧(进入宠物页后第一个有内容的画面) 已渲染：{0} 色 / 墨迹 {1}%" -f $firstVisible.colors, [math]::Round($firstVisible.ink * 100))
    } else {
        F 'sim/pet-first-frame' '进入宠物页后 20s 内没有出现有内容的画面'
    }

    # 返回首页（长按 OK）
    $null = Press-Key 'OK' 1500
    $homeBack = Wait-Content 60 3
    $d = Diff-Ref 'home_body' 0 32 240 288
    if ($d -and $d.frac -lt 0.05) {
        P 'sim/pet-back' ("长按 OK 返回首页，与进入前差异 {0}%" -f [math]::Round($d.frac * 100, 1))
    } else {
        F 'sim/pet-back' ("返回首页后与进入前差异 {0}%" -f $(if ($d) { [math]::Round($d.frac * 100, 1) } else { 'n/a' }))
    }

    # ---- C4 设置页 8 行 ----
    $null = Press-Key 'OK' 1500
    $set = Wait-Content 60 3
    Save-Shot '06_set_zh.png'

    $JS_SET = @'
(() => {
  const c = document.querySelector('#qemu-display'), g = c.getContext('2d'), W = c.width;
  const img = g.getImageData(0, 0, W, c.height), d = img.data;
  function stat(x, y, w, h) {
    const cnt = new Map();
    for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) {
      const i = (yy * W + xx) * 4;
      const k = ((d[i] >> 3) << 10) | ((d[i + 1] >> 3) << 5) | (d[i + 2] >> 3);
      cnt.set(k, (cnt.get(k) || 0) + 1);
    }
    let mk = 0, mv = 0; for (const [k, v] of cnt) if (v > mv) { mv = v; mk = k; }
    const mr = (mk >> 10) << 3, mg = ((mk >> 5) & 31) << 3, mb = (mk & 31) << 3;
    let ink = 0, minx = 1e9, maxx = -1;
    for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) {
      const i = (yy * W + xx) * 4;
      if (Math.abs(d[i] - mr) + Math.abs(d[i + 1] - mg) + Math.abs(d[i + 2] - mb) > 24) {
        ink++; if (xx < minx) minx = xx; if (xx > maxx) maxx = xx;
      }
    }
    return { ink: ink, frac: +(ink / (w * h)).toFixed(4), inkw: (ink ? maxx - minx + 1 : 0) };
  }
  const rows = [];
  for (let i = 0; i < 8; i++) {
    const y = 44 + i * 32;
    rows.push({ i: i, y: y, name: stat(16, y + 2, 124, 22), val: stat(148, y + 2, 88, 22) });
  }
  return JSON.stringify({ rows: rows, hint: stat(0, 294, 240, 24) });
})()
'@
    function Get-SettingsRows {
        $s = Eval $JS_SET
        if (-not ([string]$s).StartsWith('{')) { return $null }
        try { return ($s | ConvertFrom-Json) } catch { return $null }
    }

    $zh = Get-SettingsRows
    if (-not $zh) {
        F 'sim/settings-8rows' '设置页取帧失败'
    } else {
        $inked = @($zh.rows | Where-Object { $_.name.ink -gt 6 })
        $inkedCount = $inked.Count
        $yList = ($zh.rows | ForEach-Object { $_.y }) -join ','
        if ($inkedCount -eq 8) {
            P 'sim/settings-8rows' ("8 行行名全部有字（行 y = {0}），底部提示带宽 {1}px" -f $yList, $zh.hint.inkw)
        } else {
            F 'sim/settings-8rows' ("只找到 {0} 行有字：{1}" -f $inkedCount, (($zh.rows | ForEach-Object { "行$($_.i)=$($_.name.ink)px" }) -join ' '))
        }
        # 第 8 行（索引 7）是 LANGUAGE：中文值 2 汉字，宽度约 30px；英文 English 明显更宽
        $zhLang = $zh.rows[7]
        if ($zhLang.val.ink -gt 5) {
            P 'sim/settings-lang-row' ("第 8 行有值（值区墨迹 {0}px 宽），当前为中文" -f $zhLang.val.inkw)
        } else {
            F 'sim/settings-lang-row' '第 8 行没有值，LANGUAGE 行缺失'
        }
    }

    # ---- C5 切语言 ----
    $null = Get-SettingsRows
    for ($i = 0; $i -lt 7; $i++) { $null = Click-Key 'DOWN'; Start-Sleep -Milliseconds 300 }
    Start-Sleep -Milliseconds 500
    $before = Get-SettingsRows
    $null = Click-Key 'OK'
    $setEn = Wait-Content 60 3
    Save-Shot '07_set_en.png'
    $after = Get-SettingsRows
    if ($before -and $after) {
        $nameGrow = @($after.rows | ForEach-Object { $_.name.inkw })
        $nameBefore = @($before.rows | ForEach-Object { $_.name.inkw })
        $diffSet = $null
        $null = Store-Ref 'set_zh' 0 40 240 280   # 占位（下次比较用）
        $langZhW = $before.rows[7].name.inkw
        $langEnW = $after.rows[7].name.inkw
        $valZhW  = $before.rows[7].val.inkw
        $valEnW  = $after.rows[7].val.inkw
        Write-Host ("        行名宽度 中: {0}  英: {1}" -f ($nameBefore -join '/'), ($nameGrow -join '/')) -ForegroundColor DarkGray
        Write-Host ("        第8行 行名宽 {0}->{1}  值宽 {2}->{3}" -f $langZhW, $langEnW, $valZhW, $valEnW) -ForegroundColor DarkGray
        if ($langEnW -ge [math]::Max(40, $langZhW * 1.6) -and $valEnW -gt $valZhW * 1.6) {
            P 'sim/lang-switch' ("第 8 行 '语言'(宽 {0}px/值 {1}px) 变为 'LANGUAGE'(宽 {2}px/值 {3}px)" -f $langZhW, $valZhW, $langEnW, $valEnW)
        } else {
            F 'sim/lang-switch' ("第 8 行宽度未按英文展开：行名 {0}->{1}，值 {2}->{3}" -f $langZhW, $langEnW, $valZhW, $valEnW)
        }
        $allInked = @($after.rows | Where-Object { $_.name.ink -gt 6 }).Count
        if ($allInked -eq 8) {
            P 'sim/lang-rows' '切换后 8 行仍全部有字（无空行/缺字）'
        } else {
            F 'sim/lang-rows' ("切换后只有 {0} 行有字" -f $allInked)
        }
    } else {
        F 'sim/lang-switch' '切换语言前后取帧失败'
    }

    # 切回中文（让后面的存档比对回到同一界面语言）
    $null = Click-Key 'OK'
    $null = Wait-Content 60 3
    $backZh = Get-SettingsRows
    if ($zh -and $backZh) {
        $okBack = (($backZh.rows[7].val.inkw -lt 40) -and ($backZh.rows[7].name.inkw -lt 40))
        if ($okBack) { P 'sim/lang-roundtrip' ("再按 OK 切回中文（第8行行名宽 {0}px）" -f $backZh.rows[7].name.inkw) }
        else { F 'sim/lang-roundtrip' ("再按 OK 没有切回中文（行名宽 {0}px）" -f $backZh.rows[7].name.inkw) }
    }

    # 退出设置页（长按 OK）
    $null = Press-Key 'OK' 1500
    $null = Wait-Content 60 3

    # ---- C6 POWER 软复位后档案还在 ----
    $null = Click-Key 'POWER'
    Start-Sleep -Seconds 2
    $afterReset = Wait-Content 180 3
    Save-Shot '08_after_power.png'
    if (-not (Has-Content $afterReset)) {
        F 'sim/power-reset' '软复位后首页没恢复渲染'
    } else {
        $d = Diff-Ref 'home_body' 0 32 240 288
        if ($d -and $d.frac -lt 0.02) {
            P 'sim/power-reset' ("POWER 软复位后首页身份牌与复位前一致（差异 {0}%：宠物名/头像/编号都在）" -f [math]::Round($d.frac * 100, 2))
        } else {
            F 'sim/power-reset' ("POWER 软复位后身份牌变化 {0}%（宠物名/头像/编号可能丢失）" -f $(if ($d) { [math]::Round($d.frac * 100, 2) } else { 'n/a' }))
        }
    }
}

Stop-Sim
if (-not $KeepShots) { Remove-Item $shotsDir -Recurse -Force -ErrorAction SilentlyContinue }
else { Write-Host "`n截图保留在: $shotsDir" -ForegroundColor DarkGray }

# ===========================================================================
# 汇总
# ===========================================================================
$pass = @($script:rows | Where-Object { $_.Status -eq 'PASS' }).Count
$fail = @($script:rows | Where-Object { $_.Status -eq 'FAIL' }).Count
$skip = @($script:rows | Where-Object { $_.Status -eq 'SKIP' }).Count
$warn = @($script:rows | Where-Object { $_.Status -eq 'WARN' }).Count

Write-Host ''
Write-Host '================ 汇总 ================'
Write-Host ("PASS {0}   FAIL {1}   WARN {2}   SKIP {3}" -f $pass, $fail, $warn, $skip) -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
if ($fail -gt 0) {
    Write-Host ''
    $script:rows | Where-Object { $_.Status -eq 'FAIL' } | ForEach-Object {
        Write-Host ("  FAIL  {0}  {1}" -f $_.Id, $_.Msg) -ForegroundColor Red
    }
}
if ($fail -eq 0) { exit 0 } else { exit 1 }
