# package-win.ps1 — Lock-safe, atomic Windows packager for BrainTerm.
# Stages into dist\.staging, swaps into dist\brainterm, verifies SHA-256 + build
# stamp, and optionally runs the §11.7 isolated-PATH smoke test.
# Compatible with Windows PowerShell 5.1 and PowerShell 7+.
param(
  [string]$BuildDir = "build",
  [string]$OutDir = "dist\brainterm",
  [string]$MsysRoot = "C:\msys64\ucrt64",
  [switch]$SmokeTest,
  [switch]$KillRunning
)

$ErrorActionPreference = "Stop"

function Copy-FileStrict {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination,
    [switch]$Recurse
  )
  if (-not (Test-Path -LiteralPath $Source)) {
    throw "Required source file/directory missing: $Source"
  }
  try {
    if ($Recurse) {
      Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force -ErrorAction Stop
    } else {
      Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop
    }
  } catch {
    throw ("Copy failed for '{0}' -> '{1}': {2}" -f $Source, $Destination, $_.Exception.Message)
  }
}

function Get-ProcessesInDir {
  param([Parameter(Mandatory = $true)][string]$DirPath)
  $prefix = $DirPath.TrimEnd('\', '/') + '\'
  $procs = Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue
  $hits = @()
  foreach ($p in $procs) {
    if ($p.ExecutablePath -and $p.ExecutablePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
      $hits += $p
    }
  }
  return $hits
}

function Test-FileUnlocked {
  param([Parameter(Mandatory = $true)][string]$FilePath)
  if (-not (Test-Path -LiteralPath $FilePath)) { return $true }
  try {
    $fs = [System.IO.File]::Open($FilePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    $fs.Close()
    $fs.Dispose()
    return $true
  } catch {
    return $false
  }
}

# ---- 1. Resolve fixed output directory & validate built binary --------------
$exe = Join-Path $BuildDir "src\bt.exe"
if (-not (Test-Path -LiteralPath $exe)) {
  throw "Built executable not found at $exe (run 'meson compile -C $BuildDir' first)"
}
$exeFull = (Resolve-Path -LiteralPath $exe).Path

$outFull = [System.IO.Path]::GetFullPath($OutDir)
$outParent = Split-Path -Parent $outFull
if (-not (Test-Path -LiteralPath $outParent)) {
  New-Item -ItemType Directory -Force -Path $outParent | Out-Null
}
$stageDir = Join-Path $outParent ".staging"
$backupDir = Join-Path $outParent ".backup"

# ---- 2. Detect running processes inside output folder BEFORE writing --------
$locking = @(Get-ProcessesInDir -DirPath $outFull)
if ($locking.Count -gt 0) {
  Write-Host ("ERROR: Found {0} running process(es) locking '{1}':" -f $locking.Count, $outFull) -ForegroundColor Red
  foreach ($p in $locking) {
    Write-Host ("  PID {0,-7} {1}" -f $p.ProcessId, $p.ExecutablePath) -ForegroundColor Yellow
  }
  if (-not $KillRunning) {
    Write-Error ("Cannot overwrite '{0}' while processes above are running. Close them or re-run with -KillRunning." -f $outFull)
    exit 1
  }
  Write-Host "Stopping locking process(es) (-KillRunning)..."
  foreach ($p in $locking) {
    Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
  }
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    $still = @(Get-ProcessesInDir -DirPath $outFull)
    $lockedExe = $false
    foreach ($name in @("bt.exe", "bt-mcp.exe")) {
      $fp = Join-Path $outFull $name
      if (-not (Test-FileUnlocked -FilePath $fp)) { $lockedExe = $true }
    }
    if ($still.Count -eq 0 -and -not $lockedExe) { break }
    Start-Sleep -Milliseconds 150
  }
  $stillAfter = @(Get-ProcessesInDir -DirPath $outFull)
  if ($stillAfter.Count -gt 0 -or -not (Test-FileUnlocked -FilePath (Join-Path $outFull "bt.exe"))) {
    Write-Error ("Timed out waiting for file handles to release in '{0}'." -f $outFull)
    exit 1
  }
  Write-Host "All locking processes stopped and handles released."
}

# Also catch non-process external locks on bt.exe before staging.
if (-not (Test-FileUnlocked -FilePath (Join-Path $outFull "bt.exe"))) {
  Write-Error ("'{0}' is locked by another process. Close it before packaging." -f (Join-Path $outFull "bt.exe"))
  exit 1
}

# ---- 3. Stage full bundle into dist\.staging --------------------------------
foreach ($tmp in @($stageDir, $backupDir)) {
  if (Test-Path -LiteralPath $tmp) {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction Stop
  }
}

New-Item -ItemType Directory -Force -Path `
  $stageDir, `
  (Join-Path $stageDir "share\brainterm\language-specs"), `
  (Join-Path $stageDir "share\brainterm\styles"), `
  (Join-Path $stageDir "share\brainterm\fonts"), `
  (Join-Path $stageDir "share\glib-2.0\schemas"), `
  (Join-Path $stageDir "share\icons"), `
  (Join-Path $stageDir "lib\gdk-pixbuf-2.0") | Out-Null

Copy-FileStrict -Source $exeFull -Destination (Join-Path $stageDir "bt.exe")
$mcpExe = Join-Path $BuildDir "src\bt-mcp.exe"
if (Test-Path -LiteralPath $mcpExe) {
  Copy-FileStrict -Source $mcpExe -Destination (Join-Path $stageDir "bt-mcp.exe")
}
Copy-FileStrict -Source "data\language-specs\bt-markdown.lang" -Destination (Join-Path $stageDir "share\brainterm\language-specs\bt-markdown.lang")
foreach ($st in (Get-ChildItem -Path "data\styles" -Filter "bt-ink-*.xml")) {
  Copy-FileStrict -Source $st.FullName -Destination (Join-Path $stageDir ("share\brainterm\styles\" + $st.Name))
}
if (Test-Path -LiteralPath "data\fonts") {
  foreach ($f in (Get-ChildItem -Path "data\fonts" -File)) {
    Copy-FileStrict -Source $f.FullName -Destination (Join-Path $stageDir ("share\brainterm\fonts\" + $f.Name))
  }
}

# Recursively resolve UCRT64 DLL dependencies via objdump.
$objdump = Join-Path $MsysRoot "bin\objdump.exe"
if (-not (Test-Path -LiteralPath $objdump)) {
  throw "objdump.exe not found at $objdump"
}
$ucrtBin = Join-Path $MsysRoot "bin"
$queue = New-Object System.Collections.Generic.Queue[string]
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

$queue.Enqueue($exeFull)
$seen.Add("bt.exe") | Out-Null

$pixbufSrc = Join-Path $MsysRoot "lib\gdk-pixbuf-2.0"
if (Test-Path -LiteralPath $pixbufSrc) {
  Copy-FileStrict -Source $pixbufSrc -Destination (Join-Path $stageDir "lib\") -Recurse
  Get-ChildItem -Path (Join-Path $stageDir "lib\gdk-pixbuf-2.0") -Recurse -Filter "*.dll" | ForEach-Object {
    $queue.Enqueue($_.FullName)
  }
}

foreach ($helper in @("gspawn-win64-helper.exe", "gspawn-win64-helper-console.exe", "gdbus.exe")) {
  $hp = Join-Path $ucrtBin $helper
  if (Test-Path -LiteralPath $hp) {
    Copy-FileStrict -Source $hp -Destination (Join-Path $stageDir $helper)
    $queue.Enqueue($hp)
  }
}

while ($queue.Count -gt 0) {
  $cur = $queue.Dequeue()
  $lines = & $objdump -p $cur 2>$null | Select-String "DLL Name:"
  foreach ($line in $lines) {
    $dll = ($line.Line -split "DLL Name:")[1].Trim()
    if ($seen.Contains($dll)) { continue }
    $srcDll = Join-Path $ucrtBin $dll
    if (Test-Path -LiteralPath $srcDll) {
      $seen.Add($dll) | Out-Null
      Copy-FileStrict -Source $srcDll -Destination (Join-Path $stageDir $dll)
      $queue.Enqueue($srcDll)
    }
  }
}

# Schemas, GtkSourceView data, & icons
$schemas = Join-Path $MsysRoot "share\glib-2.0\schemas\gschemas.compiled"
if (Test-Path -LiteralPath $schemas) {
  Copy-FileStrict -Source $schemas -Destination (Join-Path $stageDir "share\glib-2.0\schemas\gschemas.compiled")
}
$gsvShare = Join-Path $MsysRoot "share\gtksourceview-5"
if (Test-Path -LiteralPath $gsvShare) {
  Copy-FileStrict -Source $gsvShare -Destination (Join-Path $stageDir "share\") -Recurse
}
foreach ($theme in @("hicolor", "Adwaita")) {
  $tpath = Join-Path $MsysRoot "share\icons\$theme"
  if (Test-Path -LiteralPath $tpath) {
    Copy-FileStrict -Source $tpath -Destination (Join-Path $stageDir "share\icons\") -Recurse
  }
}

# ---- 4. Atomic swap dist\.staging -> dist\brainterm -------------------------
$swapped = $false
$hadOld = Test-Path -LiteralPath $outFull
try {
  if ($hadOld) {
    Move-Item -LiteralPath $outFull -Destination $backupDir -ErrorAction Stop
  }
  Move-Item -LiteralPath $stageDir -Destination $outFull -ErrorAction Stop
  $swapped = $true
} catch {
  if (-not $swapped -and $hadOld -and (Test-Path -LiteralPath $backupDir) -and -not (Test-Path -LiteralPath $outFull)) {
    Move-Item -LiteralPath $backupDir -Destination $outFull -ErrorAction SilentlyContinue
  }
  throw ("Failed to swap staged bundle into '{0}': {1}" -f $outFull, $_.Exception.Message)
}
if (Test-Path -LiteralPath $backupDir) {
  Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction Stop
}

# ---- 5. Verify SHA-256 parity between build\src\bt.exe & dist\brainterm\bt.exe
$bundledExe = Join-Path $outFull "bt.exe"
$srcHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exeFull).Hash
$dstHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bundledExe).Hash
Write-Host ("SHA-256 build: {0}" -f $srcHash)
Write-Host ("SHA-256 dist : {0}" -f $dstHash)
if ($srcHash -ne $dstHash) {
  throw ("SHA-256 mismatch! build='{0}' vs dist='{1}'" -f $srcHash, $dstHash)
}

# ---- 6. Capture build stamp (isolated PATH) & optional §11.7 smoke test -----
$savedPath = $env:PATH
$savedXdg = $env:XDG_DATA_DIRS
$buildStamp = "(unknown)"
try {
  $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
  $env:XDG_DATA_DIRS = Join-Path $outFull "share"

  $prevEap = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  $verOut = & $bundledExe --version 2>&1
  $verCode = $LASTEXITCODE
  $ErrorActionPreference = $prevEap
  if ($verCode -ne 0) {
    $verText = ($verOut | ForEach-Object { "$_" }) -join [Environment]::NewLine
    throw ("Packaged bt.exe --version failed with exit code {0}: {1}" -f $verCode, $verText)
  }
  $buildStamp = (($verOut | ForEach-Object { "$_" }) -join [Environment]::NewLine).Trim()

  if ($SmokeTest) {
    $fixture = (Resolve-Path -LiteralPath "tests\fixtures\vault").Path
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $bundledExe --vault $fixture --smoke-test 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $text = ($out | ForEach-Object { "$_" }) -join [Environment]::NewLine
    Write-Host $text
    if ($code -ne 0) {
      throw "Packaging smoke test failed with exit code $code"
    }
    if ($text -notmatch "BT_SMOKE ok notes=2 lang=bt-markdown term=alive ui=adw-ink scheme=dark tabs=1 checkpoint=ok sessions=\d+ index=ok ask=search graph=nodes:2") {
      throw "Unexpected smoke test output"
    }
    if ($text -match "GtkSourceView-WARNING") {
      throw "GtkSourceView emitted a warning while loading bt-markdown.lang"
    }
  }
} finally {
  $env:PATH = $savedPath
  if ($null -eq $savedXdg) {
    Remove-Item Env:XDG_DATA_DIRS -ErrorAction SilentlyContinue
  } else {
    $env:XDG_DATA_DIRS = $savedXdg
  }
  Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "tests\fixtures\vault\.bt"
}

# ---- 7. Remove legacy dist\brainterm-dusk ONLY after verification passes ----
$legacyDir = Join-Path $outParent "brainterm-dusk"
if (($legacyDir -ne $outFull) -and (Test-Path -LiteralPath $legacyDir)) {
  $legacyProcs = @(Get-ProcessesInDir -DirPath $legacyDir)
  if ($legacyProcs.Count -eq 0) {
    Remove-Item -LiteralPath $legacyDir -Recurse -Force -ErrorAction Stop
    Write-Host ("Removed legacy folder: {0}" -f $legacyDir)
  } else {
    Write-Warning ("Legacy folder '{0}' has running processes; skipped removal." -f $legacyDir)
  }
}

# ---- 8. Final summary -------------------------------------------------------
$exeItem = Get-Item -LiteralPath $bundledExe
$totalBytes = (Get-ChildItem -LiteralPath $outFull -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ""
Write-Host "=== BrainTerm Package Summary ==="
Write-Host ("  Output exe   : {0}" -f $exeItem.FullName)
Write-Host ("  Exe size     : {0:N0} bytes ({1:N2} MB) | Bundle: {2:N2} MB" -f $exeItem.Length, ($exeItem.Length / 1MB), ($totalBytes / 1MB))
Write-Host ("  LastWriteTime: {0}" -f $exeItem.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
Write-Host ("  SHA-256 (12) : {0}" -f $dstHash.Substring(0, 12))
Write-Host ("  Build stamp  : {0}" -f $buildStamp)
