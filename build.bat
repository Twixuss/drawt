@echo off

set cFlags=/nologo /link

set srcdir=..\src\

set runtime=/MD

if "%1" equ "r" (
    set cFlags=/O2 /GL /D"BUILD_DEBUG=0" %cFlags% /LTCG
) else (
    set runtime=%runtime%d
    set cFlags=/Od /D"BUILD_DEBUG=1" %cFlags%
)

set deps=user32.lib
set cFlags=/Z7 /FC /Oi /EHa /fp:fast /std:c++latest %runtime% %cFlags% /incremental:no %deps%

set cFlags=/favor:INTEL64 %cFlags%

set bindir=bintool\
if not exist %bindir% mkdir %bindir%
pushd %bindir%

cl %srcdir%bmp_printer.cpp %cFlags% %lFlags% /out:bmp_printer.exe
if %errorlevel% neq 0 goto fail

call bmp_printer.exe ..\ui\atlas.bmp ..\src\atlas.h --transparent-closest

popd

set bindir=bin\
if not exist %bindir% mkdir %bindir%
pushd %bindir%

cl %srcdir%main.cpp %cFlags% %lFlags% /out:drawt.exe
if %errorlevel% neq 0 goto fail

popd

echo [32mCompilation succeeded[0m
goto end
:fail
echo [31mCompilation failed[0m
popd
:end
