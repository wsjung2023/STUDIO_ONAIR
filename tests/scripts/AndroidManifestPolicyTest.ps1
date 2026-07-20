param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'

$manifestPath = Join-Path $RepositoryRoot 'android/AndroidManifest.xml'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Android manifest is missing: $manifestPath"
}

[xml]$manifest = Get-Content -Raw -LiteralPath $manifestPath
$android = 'http://schemas.android.com/apk/res/android'
$permissions = @($manifest.manifest.'uses-permission' | ForEach-Object {
    $_.GetAttribute('name', $android)
})

$required = @(
    'android.permission.CAMERA',
    'android.permission.RECORD_AUDIO',
    'android.permission.FOREGROUND_SERVICE',
    'android.permission.FOREGROUND_SERVICE_MEDIA_PROJECTION',
    'android.permission.POST_NOTIFICATIONS'
)

foreach ($permission in $required) {
    if ($permissions -notcontains $permission) {
        throw "Required Android permission is missing: $permission"
    }
}

if ($manifest.manifest.application.GetAttribute('label', $android) -ne 'Creator Studio') {
    throw 'Android application label must be Creator Studio'
}
