@echo off
setlocal
cd /d "%~dp0"

where javac >nul 2>nul
if errorlevel 1 (
  echo JDK javac was not found. Install JDK 17 or newer.
  exit /b 1
)

if not exist bin mkdir bin
set "SOURCE_LIST=%TEMP%\betaseedfinder_java_sources_%RANDOM%.txt"
if exist "%SOURCE_LIST%" del "%SOURCE_LIST%"
for /r "src" %%F in (*.java) do echo "%%~fF">>"%SOURCE_LIST%"

echo Compiling BetaSeedFinder...
javac -encoding UTF-8 -d bin @"%SOURCE_LIST%"
set "RC=%ERRORLEVEL%"
del "%SOURCE_LIST%" >nul 2>nul
if not "%RC%"=="0" (
  echo.
  echo Compile failed.
  exit /b %RC%
)

echo Compile OK.
