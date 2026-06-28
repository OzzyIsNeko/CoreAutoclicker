$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Invoke-Checked {
    param([scriptblock]$Command)
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE"
    }
}

$clangCl = "C:\Program Files\LLVM\bin\clang-cl.exe"
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$version = "1.0.0"
$exeName = "CoreAutoclicker.exe"
$releaseName = "CoreAutoclicker-$version-win-x64"

if ((Test-Path $clangCl) -and (Test-Path $vcvars)) {
    $ccompile = "`"$vcvars`" >nul && `"$clangCl`" /nologo /TC /O2 /Oi /clang:-Os /GS- /Gy /Gw /D_WIN32_WINNT=0x0601 /DNDEBUG"
    $clink = "/link /subsystem:windows /entry:WinMainCRTStartup /nodefaultlib kernel32.lib user32.lib gdi32.lib comctl32.lib coreautoclicker.res /opt:ref /opt:icf /incremental:no /Brepro"
    Remove-Item -LiteralPath .\CoreAutoclicker.exe,.\coreautoclicker.exe -Force -ErrorAction SilentlyContinue
    Invoke-Checked { cmd /d /s /c "`"$vcvars`" >nul && rc /nologo /fo coreautoclicker.res coreautoclicker.rc" }
    Invoke-Checked { cmd /d /s /c "$ccompile clicker.c $clink /out:$exeName" }
    Remove-Item -LiteralPath .\coreautoclicker.res -ErrorAction SilentlyContinue

    $distRoot = Join-Path (Get-Location) "dist"
    $releaseDir = Join-Path $distRoot $releaseName
    $zipPath = "$releaseDir.zip"
    Remove-Item -LiteralPath $releaseDir,$zipPath -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $releaseDir | Out-Null
    Copy-Item -LiteralPath ".\$exeName",.\README.md,.\RELEASE_NOTES.md -Destination $releaseDir
    Compress-Archive -Path (Join-Path $releaseDir "*") -DestinationPath $zipPath -Force

    Write-Host "Built no-CRT C release: $zipPath"
    exit 0
}

throw "clang-cl and the Visual Studio x64 build environment are required for the no-CRT C build."
