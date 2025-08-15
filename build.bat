@echo OFF

SET http_proxy_cmd=""
SET https_proxy_cmd=""
if /I "%1"=="proxy" (
    SET http_proxy_cmd= -e "http_proxy=http://%2"
    SET https_proxy_cmd= -e "https_proxy=https://%2"
)

echo proxy: %2
echo http_proxy_cmd: %http_proxy_cmd%
echo https_proxy_cmd: %https_proxy_cmd%

SET ROOT_PATH=%cd%
SET PATH=%PATH%;%cd%\tools

call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"

echo --- python38 ---
call rmdir /s /q python38
SET CMD=wget.exe%https_proxy_cmd% https://www.python.org/ftp/python/3.8.10/python-3.8.10-embed-win32.zip -O "%ROOT_PATH%\tools\python-3.8.10-embed-win32.zip"
echo %CMD%
call %CMD%
call 7z.exe x %ROOT_PATH%\tools\python-3.8.10-embed-win32.zip -o./tools/python38 -aoa -y

echo --- git ---
call rmdir /s /q git
SET CMD=wget.exe%https_proxy_cmd% https://github.com/git-for-windows/git/releases/download/v2.50.1.windows.1/PortableGit-2.50.1-64-bit.7z.exe -O "%ROOT_PATH%\tools\PortableGit-2.50.1-64-bit.7z.exe"
echo %CMD%
call %CMD%
call 7z.exe x %ROOT_PATH%\tools\PortableGit-2.50.1-64-bit.7z.exe -o./tools/git -aoa -y

SET PATH=%ROOT_PATH%\tools\python38;%ROOT_PATH%\tools\git\bin;%PATH%

call mkdir %ROOT_PATH%\Libraries

call mkdir %ROOT_PATH%\ThirdParty

echo prepare build
call tdesktop\Telegram\build\prepare\win.bat

cd %ROOT_PATH%\tdesktop\Telegram

echo generate visual studio projects
rem test
rem TELEGRAM_API_ID=3653963
rem TELEGRAM_API_HASH=53c860995094af360d4d4d821567a692
rem call configure.bat x64 outDirName=pc -D TDESKTOP_API_ID=3653963 -D TDESKTOP_API_HASH=53c860995094af360d4d4d821567a692

rem Telegram Web K
rem TELEGRAM_API_ID=1025907
rem TELEGRAM_API_HASH=452b0359b988148995f22ff0f4229750
rem call configure.bat x64 outDirName=web -D TDESKTOP_API_ID=1025907 -D TDESKTOP_API_HASH=452b0359b988148995f22ff0f4229750

rem Telegram pc
rem TELEGRAM_API_ID=611335
rem TELEGRAM_API_ID=d524b414d21f4d37f08684c1df41ac9c
call configure.bat x64 outDirName=pc -D TDESKTOP_API_ID=611335 -D TDESKTOP_API_HASH=d524b414d21f4d37f08684c1df41ac9c