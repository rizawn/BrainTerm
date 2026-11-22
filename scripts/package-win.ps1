# package-win.ps1 — Bundles bt.exe with UCRT64 GTK4/GtkSourceView runtime DLLs
# and runs the §11.7 packaging smoke test with an isolated PATH.
param(
  [string]$BuildDir = "build",
  [string]$OutDir = "dist\brainterm",
  [string]$MsysRoot = "C:\msys64\ucrt64",
  [switch]$SmokeTest
)

$ErrorActionPreference = "Stop"

$exe = Join-Path $BuildDir "src\bt.exe"
if (-not (Test-Path $exe)) {
  throw "Built executable not found at $exe"
}

if (Test-Path $OutDir) {
  Remove-Item -Recurse -Force $OutDir
}
New-Item -ItemType Directory -Force -Path `
  $OutDir, `
  (Join-Path $OutDir "share\brainterm\language-specs"), `
  (Join-Path $OutDir "share\brainterm\styles"), `
  (Join-Path $OutDir "share\glib-2.0\schemas"), `
  (Join-Path $OutDir "share\icons"), `
  (Join-Path $OutDir "lib\gdk-pixbuf-2.0") | Out-Null

Copy-Item $exe (Join-Path $OutDir "bt.exe")
$mcpExe = Join-Path $BuildDir "src\bt-mcp.exe"
if (Test-Path $mcpExe) {
  Copy-Item $mcpExe (Join-Path $OutDir "bt-mcp.exe")
}
Copy-Item "data\language-specs\bt-markdown.lang" (Join-Path $OutDir "share\brainterm\language-specs\")
Copy-Item "data\styles\bt-ink-*.xml" (Join-Path $OutDir "share\brainterm\styles\")

# Recursively resolve DLL dependencies from UCRT64 using objdump.
$objdump = Join-Path $MsysRoot "bin\objdump.exe"
$ucrtBin = Join-Path $MsysRoot "bin"
$queue = New-Object System.Collections.Generic.Queue[string]
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

$queue.Enqueue((Resolve-Path $exe).Path)
$seen.Add("bt.exe") | Out-Null

# Include gdk-pixbuf loaders and gspawn helper.
$pixbufSrc = Join-Path $MsysRoot "lib\gdk-pixbuf-2.0"
if (Test-Path $pixbufSrc) {
  Copy-Item -Recurse -Force $pixbufSrc (Join-Path $OutDir "lib\")
  Get-ChildItem -Recurse -Filter "*.dll" (Join-Path $OutDir "lib\gdk-pixbuf-2.0") | ForEach-Object {
    $queue.Enqueue($_.FullName)
  }
}

foreach ($helper in @("gspawn-win64-helper.exe", "gspawn-win64-helper-console.exe", "gdbus.exe")) {
  $hp = Join-Path $ucrtBin $helper
  if (Test-Path $hp) {
    Copy-Item $hp $OutDir
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
    if (Test-Path $srcDll) {
      $seen.Add($dll) | Out-Null
      Copy-Item $srcDll (Join-Path $OutDir $dll)
      $queue.Enqueue($srcDll)
    }
  }
}

# Schemas, GtkSourceView schemas/styles, & icons (Adwaita/hicolor minimal)
$schemas = Join-Path $MsysRoot "share\glib-2.0\schemas\gschemas.compiled"
if (Test-Path $schemas) {
  Copy-Item $schemas (Join-Path $OutDir "share\glib-2.0\schemas\")
}
$gsvShare = Join-Path $MsysRoot "share\gtksourceview-5"
if (Test-Path $gsvShare) {
  Copy-Item -Recurse -Force $gsvShare (Join-Path $OutDir "share\")
}
foreach ($theme in @("hicolor", "Adwaita")) {
  $tpath = Join-Path $MsysRoot "share\icons\$theme"
  if (Test-Path $tpath) {
    Copy-Item -Recurse -Force $tpath (Join-Path $OutDir "share\icons\")
  }
}

$totalBytes = (Get-ChildItem -Recurse -File $OutDir | Measure-Object -Property Length -Sum).Sum
$binBytes = (Get-Item (Join-Path $OutDir "bt.exe")).Length
Write-Host ("Packaged {0}: bt.exe={1:N2} MB, bundle={2:N2} MB" -f $OutDir, ($binBytes / 1MB), ($totalBytes / 1MB))

if ($SmokeTest) {
  $savedPath = $env:PATH
  try {
    # Isolate PATH to Windows system dirs only so missing UCRT64 DLLs fail loudly.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:XDG_DATA_DIRS = (Resolve-Path (Join-Path $OutDir "share")).Path
    $fixture = (Resolve-Path "tests\fixtures\vault").Path
    $bundledExe = (Resolve-Path (Join-Path $OutDir "bt.exe")).Path
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $bundledExe --vault $fixture --smoke-test 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $text = ($out | Out-String)
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
  } finally {
    $env:PATH = $savedPath
  }
}
