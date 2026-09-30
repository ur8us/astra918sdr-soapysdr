param(
    [Parameter(Mandatory = $true)][ValidateSet("0.7", "0.8")][string]$Abi,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workDirectory = Join-Path $env:RUNNER_TEMP "astra918-soapy-$Abi"
if (Test-Path $workDirectory) {
    Remove-Item -Recurse -Force $workDirectory
}
New-Item -ItemType Directory -Force -Path $workDirectory | Out-Null

$tag = if ($Abi -eq "0.7") { "soapy-sdr-0.7.1" } else { "soapy-sdr-0.8.1" }
$source = Join-Path $workDirectory "SoapySDR"
$sdkPrefix = Join-Path $workDirectory "sdk"
$modulePrefix = Join-Path $workDirectory "module-install"
git clone --quiet --depth 1 --branch $tag https://github.com/pothosware/SoapySDR.git $source
if ($LASTEXITCODE -ne 0) { throw "Could not clone SoapySDR $tag" }

cmake -S $source -B (Join-Path $workDirectory "soapy-build") `
    -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_INSTALL_PREFIX=$sdkPrefix" `
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
    -DENABLE_LIBRARY=ON -DENABLE_APPS=OFF -DENABLE_TESTS=OFF -DENABLE_DOCS=OFF `
    -DENABLE_PYTHON=OFF -DENABLE_PYTHON3=OFF
if ($LASTEXITCODE -ne 0) { throw "SoapySDR $Abi configure failed" }
cmake --build (Join-Path $workDirectory "soapy-build") --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw "SoapySDR $Abi build failed" }
cmake --install (Join-Path $workDirectory "soapy-build") --config Release
if ($LASTEXITCODE -ne 0) { throw "SoapySDR $Abi install failed" }

$soapyConfig = Get-ChildItem -Path $sdkPrefix -Recurse -Filter SoapySDRConfig.cmake |
    Select-Object -First 1
if (-not $soapyConfig) { throw "SoapySDR $Abi CMake package was not installed" }

$driverBuild = Join-Path $workDirectory "driver-build"
$vcpkgRoot = if ($env:VCPKG_INSTALLATION_ROOT) {
    $env:VCPKG_INSTALLATION_ROOT
} elseif ($env:VCPKG_ROOT) {
    $env:VCPKG_ROOT
} else {
    throw "The vcpkg installation root is unavailable"
}
cmake -S $repoRoot -B $driverBuild `
    -G "Visual Studio 17 2022" -A x64 `
    "-DSoapySDR_DIR=$($soapyConfig.DirectoryName)" `
    "-DCMAKE_INSTALL_PREFIX=$modulePrefix" `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { throw "Astra918 module ABI $Abi configure failed" }
cmake --build $driverBuild --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Astra918 module ABI $Abi build failed" }
cmake --install $driverBuild --config Release
if ($LASTEXITCODE -ne 0) { throw "Astra918 module ABI $Abi install failed" }

$module = Get-ChildItem -Path $modulePrefix -Recurse -Filter *SoapyAstra918Support*.dll |
    Select-Object -First 1
if (-not $module) { throw "SoapySDR ABI $Abi DLL was not installed" }
$abiDirectory = Join-Path $OutputDirectory "modules$Abi"
New-Item -ItemType Directory -Force -Path $abiDirectory | Out-Null
Copy-Item $module.FullName $abiDirectory
Write-Host "Built SoapySDR ABI $Abi module for Windows x64: $($module.FullName)"
