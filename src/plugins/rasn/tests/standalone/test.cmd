@ECHO OFF
SETLOCAL EnableExtensions

SET "build_type=%~2"
IF "%build_type%" EQU "" SET "build_type=Debug"
SET "bin=.\%build_type%\rasn.unit_tests.exe"
IF NOT EXIST "%bin%" SET "bin=.\rasn.unit_tests.exe"

ECHO %bin% --gtest_filter=rasn_*.*:codepilot_*.*
CALL "%bin%" --gtest_filter=rasn_*.*:codepilot_*.*
EXIT /B %ERRORLEVEL%
