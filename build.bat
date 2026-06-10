echo >/dev/null # >nul & GOTO WINDOWS & rem ^

clang main.c -o main && ./main

exit 0
:WINDOWS
@ECHO off

cl.exe main.c /OUT:main.exe && main.exe
