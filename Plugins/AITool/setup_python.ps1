<#
.SYNOPSIS
    Prepare embedded Python environment for AITool plugin packaging.
    Downloads Python 3.11 embeddable, installs pip, installs dependencies,
    copies NKSR package.

.USAGE
    .\setup_python.ps1
    .\setup_python.ps1 -NKSRPackagePath "D:\path\to\NKSR\package"
    .\setup_python.ps1 -SkipTorch   # if torch is already in the env
#>
param(
    [string]$NKSRPackagePath = "D:\MyWork\UnrealProject\AITest\ConvertToSurface\NKSR\package",
    [switch]$SkipTorch,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$PluginDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PythonDir = Join-Path $PluginDir "ThirdParty\Python"
$PythonExe = Join-Path $PythonDir "python.exe"
$PythonZipUrl = "https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip"
$GetPipUrl = "https://bootstrap.pypa.io/get-pip.py"

if ((Test-Path $PythonExe) -and -not $Force) {
    Write-Host "[INFO] Embedded Python already exists at $PythonDir" -ForegroundColor Yellow
    Write-Host "       Use -Force to re-create." -ForegroundColor Yellow
    exit 0
}

# --- Step 1: Download and extract Python embeddable ---
Write-Host "`n[1/5] Downloading Python 3.11.9 embeddable..." -ForegroundColor Cyan
$zipPath = Join-Path $env:TEMP "python-3.11.9-embed-amd64.zip"
if (-not (Test-Path $zipPath)) {
    Invoke-WebRequest -Uri $PythonZipUrl -OutFile $zipPath
}

if (Test-Path $PythonDir) { Remove-Item $PythonDir -Recurse -Force }
New-Item $PythonDir -ItemType Directory -Force | Out-Null
Expand-Archive $zipPath -DestinationPath $PythonDir -Force
Write-Host "       Extracted to $PythonDir" -ForegroundColor Green

# --- Step 2: Enable site-packages (modify ._pth file) ---
Write-Host "`n[2/5] Enabling site-packages..." -ForegroundColor Cyan
$pthFile = Get-ChildItem $PythonDir -Filter "python311._pth"
if ($pthFile) {
    $content = Get-Content $pthFile.FullName
    $newContent = $content | ForEach-Object {
        if ($_ -match "^#\s*import site") { "import site" } else { $_ }
    }
    Set-Content $pthFile.FullName $newContent
    Write-Host "       Uncommented 'import site' in $($pthFile.Name)" -ForegroundColor Green
}

# --- Step 3: Install pip ---
Write-Host "`n[3/5] Installing pip..." -ForegroundColor Cyan
$getPipPath = Join-Path $env:TEMP "get-pip.py"
if (-not (Test-Path $getPipPath)) {
    Invoke-WebRequest -Uri $GetPipUrl -OutFile $getPipPath
}
& $PythonExe $getPipPath --no-warn-script-location 2>&1 | Write-Host
Write-Host "       pip installed." -ForegroundColor Green

# --- Step 4: Install dependencies ---
Write-Host "`n[4/5] Installing Python dependencies..." -ForegroundColor Cyan

if (-not $SkipTorch) {
    Write-Host "       Installing PyTorch (this may take a while)..."
    & $PythonExe -m pip install torch==2.11.0 --index-url https://download.pytorch.org/whl/cu128 --no-warn-script-location 2>&1 | Write-Host
}

$packages = @("numpy", "trimesh", "plyfile", "scipy")
foreach ($pkg in $packages) {
    Write-Host "       Installing $pkg..."
    & $PythonExe -m pip install $pkg --no-warn-script-location 2>&1 | Write-Host
}

Write-Host "       All pip packages installed." -ForegroundColor Green

# --- Step 5: Copy NKSR package ---
Write-Host "`n[5/5] Copying NKSR package..." -ForegroundColor Cyan
$nksrSrc = Join-Path $NKSRPackagePath "nksr"
$sitePackages = Join-Path $PythonDir "Lib\site-packages"
if (-not (Test-Path $sitePackages)) {
    $sitePackages = Join-Path $PythonDir "site-packages"
    if (-not (Test-Path $sitePackages)) {
        New-Item $sitePackages -ItemType Directory -Force | Out-Null
    }
}
$nksrDst = Join-Path $sitePackages "nksr"

if (Test-Path $nksrSrc) {
    if (Test-Path $nksrDst) { Remove-Item $nksrDst -Recurse -Force }
    Copy-Item $nksrSrc $nksrDst -Recurse
    Write-Host "       Copied nksr package to $nksrDst" -ForegroundColor Green
} else {
    Write-Host "       [WARN] NKSR package not found at $nksrSrc" -ForegroundColor Yellow
    Write-Host "       You need to manually copy the nksr package." -ForegroundColor Yellow
}

# --- Summary ---
$totalSize = (Get-ChildItem $PythonDir -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1GB
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Embedded Python setup complete!" -ForegroundColor Green
Write-Host "  Location: $PythonDir" -ForegroundColor White
Write-Host "  Size:     $([math]::Round($totalSize, 2)) GB" -ForegroundColor White
Write-Host "  Python:   $PythonExe" -ForegroundColor White
Write-Host "========================================`n" -ForegroundColor Cyan
