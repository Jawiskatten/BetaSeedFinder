@echo off
if not exist config mkdir config >nul 2>nul
> config\gpu_backend.properties echo # backend=auto ^| amd ^| nvidia ^| legacy
>> config\gpu_backend.properties echo backend=auto
echo GPU backend set to AUTO.
pause
