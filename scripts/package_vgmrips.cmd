@echo off
setlocal

where py >nul 2>nul
if not errorlevel 1 (
    py -3 -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
    if not errorlevel 1 (
        py -3 "%~dp0package_vgmrips.py" %*
        exit /b %errorlevel%
    )
)

where python >nul 2>nul
if not errorlevel 1 (
    python -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
    if not errorlevel 1 (
        python "%~dp0package_vgmrips.py" %*
        exit /b %errorlevel%
    )
)

echo Python 3.10 or newer is required to run package_vgmrips. 1>&2
echo No third-party Python modules are required. 1>&2
exit /b 1
