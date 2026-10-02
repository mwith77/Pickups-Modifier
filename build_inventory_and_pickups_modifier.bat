@echo off
rem Build inventory_and_pickups_modifier.dll.
rem Run from "x64 Native Tools Command Prompt for VS".

where cl.exe >nul 2>nul || (echo cl.exe not found - use the x64 Native Tools prompt & exit /b 1)
where rc.exe >nul 2>nul || (echo rc.exe not found & exit /b 1)

rem The version resource, so the DLL carries its author, product and version.
rc /nologo /fo inventory_and_pickups_modifier.res inventory_and_pickups_modifier.rc
if errorlevel 1 (echo RC FAILED & exit /b 1)

rem inventory_and_pickups_modifier.map turns an offset in a crash line back
rem into a function name.
cl /nologo /LD /O2 /EHsc /W4 /w14459 inventory_and_pickups_modifier.cpp ^
   inventory_and_pickups_modifier.res ^
   /link /OUT:inventory_and_pickups_modifier.dll ^
   /MAP:inventory_and_pickups_modifier.map
if errorlevel 1 (echo BUILD FAILED & exit /b 1)

del /q inventory_and_pickups_modifier.obj ^
       inventory_and_pickups_modifier.exp inventory_and_pickups_modifier.lib ^
       inventory_and_pickups_modifier.res 2>nul

echo.
echo Built inventory_and_pickups_modifier.dll
