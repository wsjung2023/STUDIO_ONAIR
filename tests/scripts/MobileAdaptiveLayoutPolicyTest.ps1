param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$main = Join-Path $RepositoryRoot 'qml/Main.qml'
$homePage = Join-Path $RepositoryRoot 'qml/HomePage.qml'
$studio = Join-Path $RepositoryRoot 'qml/StudioPage.qml'
$editor = Join-Path $RepositoryRoot 'qml/EditorPage.qml'

$checks = @(
    @{ Path = $main; Pattern = 'readonly property bool compact'; Name = 'compact application header' },
    @{ Path = $homePage; Pattern = 'columns: root\.compact \? 1 : 4'; Name = 'stacked mobile project actions' },
    @{ Path = $studio; Pattern = 'objectName: "studioCompactTabs"'; Name = 'touch studio workspace navigation' },
    @{ Path = $studio; Pattern = 'compactSection === "capture"'; Name = 'mobile capture pane' },
    @{ Path = $editor; Pattern = 'objectName: "editorCompactTabs"'; Name = 'touch editor workspace navigation' },
    @{ Path = $editor; Pattern = 'compactSection === "inspector"'; Name = 'mobile editor inspector pane' },
    @{ Path = $editor; Pattern = 'Layout\.preferredHeight: root\.compact \? 180 : 270'; Name = 'mobile timeline budget' }
)

foreach ($check in $checks) {
    if (-not (Select-String -LiteralPath $check.Path -Pattern $check.Pattern -Quiet)) {
        throw "missing adaptive mobile layout boundary: $($check.Name)"
    }
}
