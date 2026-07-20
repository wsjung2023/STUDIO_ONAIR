param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$activity = Join-Path $RepositoryRoot 'android/src/com/studioonair/creatorstudio/CreatorStudioActivity.java'
$bridge = Join-Path $RepositoryRoot 'src/app/android/AndroidDeviceProfile.cpp'
$main = Join-Path $RepositoryRoot 'src/main.cpp'

$checks = @(
    @{ Path = $activity; Pattern = 'deviceMemoryClassMiB'; Name = 'Android memory-class probe' },
    @{ Path = $activity; Pattern = 'isLowRamDevice'; Name = 'Android low-RAM probe' },
    @{ Path = $activity; Pattern = 'isPowerSaveMode'; Name = 'Android power-save probe' },
    @{ Path = $activity; Pattern = 'getCurrentThermalStatus'; Name = 'Android thermal probe' },
    @{ Path = $bridge; Pattern = 'MobilePerformancePolicy::create'; Name = 'shared deterministic budget policy' },
    @{ Path = $main; Pattern = 'setResourceConstraints'; Name = 'export budget wiring' },
    @{ Path = $main; Pattern = 'applicationStateChanged'; Name = 'foreground export boundary' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path) -or
        -not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing Android performance budget boundary: $($check.Name)"
    }
}
