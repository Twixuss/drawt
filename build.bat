@echo off

set srcdir=..\src\

set runtime=/MD
set deps=user32.lib
set cFlags=/Z7 /FC /Oi /EHa /fp:fast /std:c++latest /nologo %runtime% /link /incremental:no %deps%
set cFlags=/favor:INTEL64 %cFlags%

set releaseFlags=/O2 /Ob3 /GL /D"BUILD_DEBUG=0" %cFlags% /LTCG
set debugFlags=/Od /D"BUILD_DEBUG=1" %cFlags%
set fastestFlags=/O2 /D"NDEBUG" /Z7 /FC /Oi /Ob3 /EHa /fp:fast /nologo %runtime% /link

if "%1" equ "r" (
    set cFlags=%releaseFlags%
) else (
    set cFlags=%debugFlags%
)

set bindir=bin\
if not exist %bindir% mkdir %bindir%
pushd %bindir%

::cl %srcdir%stb.c /c %fastestFlags%
::if %errorlevel% neq 0 goto fail

::cl %srcdir%image2cpp.cpp /std:c++latest %fastestFlags% /incremental:no /out:image2cpp.exe stb.obj
::if %errorlevel% neq 0 goto fail

::image2cpp.exe ..\ui\atlas.png ..\src\atlas.h --flip-y --transparent-adjacent
::if %errorlevel% neq 0 goto fail

::rc /r /nologo /fo resource.res %srcdir%resource.rc
::if %errorlevel% neq 0 goto fail

cl %srcdir%os_windows.cpp /c %cFlags%
if %errorlevel% neq 0 goto fail

cl %srcdir%r_d3d11.cpp /c %cFlags%
if %errorlevel% neq 0 goto fail

::cl %srcdir%r_gl.cpp /c %cFlags%
::if %errorlevel% neq 0 goto fail

cl %srcdir%main.cpp %cFlags% /out:drawt.exe stb.obj resource.res os_windows.obj r_d3d11.obj
if %errorlevel% neq 0 goto fail

popd

echo [32mCompilation succeeded[0m
goto end
:fail
echo [31mCompilation failed[0m
popd
:end
