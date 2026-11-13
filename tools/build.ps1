# Builds build\WMVSlop.xex on Windows by running ..\build.sh in an MSYS2 bash.
#   .\build.ps1 -Bash C:\msys64\usr\bin\bash.exe -Xsdk C:\src\xsdk
#   .\build.ps1 -Bash ... -Xsdk ... -Base 0x91D80000   another load address, if it collides with a plugin in use
param([Parameter(Mandatory)][string]$Bash, [Parameter(Mandatory)][string]$Xsdk, [string]$Base)
function ToMsys([string]$path) {
    $full = (Resolve-Path -LiteralPath $path).Path
    '/' + $full.Substring(0, 1).ToLower() + $full.Substring(2).Replace('\', '/')
}
$set = "XSDK='$(ToMsys $Xsdk)' PYTHON=python "
if ($Base) {
    if ($Base -notmatch '^0x91[0-9A-Fa-f]{2}0000$') { throw "Base must look like 0x91E40000 (64 KB aligned, in 0x91000000-0x91FF0000)" }
    $set += "BASE=$Base "
}
$script = ToMsys (Join-Path (Split-Path $PSScriptRoot) 'build.sh')
& $Bash -lc "${set}'$script'"
