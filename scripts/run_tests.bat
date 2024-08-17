@echo off
setlocal enabledelayedexpansion

if "%~1"=="" (
    echo Usage: %0 input_path
    exit /b 1
)

set "input=%~1"
set "output=%~2"

if not exist "%input%\" (
    echo Directory %input% does not exist.
    exit /b 1
)

if not exist "%output%\" (
    echo Directory %output% does not exist.
    exit /b 1
)

set "txtoutput=%output%\txt"
if not exist %txtoutput% mkdir "%txtoutput%"

echo Executing conversions on %input%
..\x64\Release\pdbconv.exe --test --input="%input%" --output="%output%"

for %%f in ("%input%\*.pdb") do (
    for %%g in ("%output%\%%~nf*") do (
		set "textFile=%txtoutput%\%%~ng.txt"
        echo Processing %%g
        "C:\Program Files\Microsoft Visual Studio\2022\Community\DIA SDK\Samples\DIA2Dump\x64\Release\Dia2Dump.exe" "%%g" >"!textFile!"
		if NOT "!prevTextFile!"=="" (
            fc /b "!prevTextFile!" "!textFile!" >nul
            if NOT errorlevel == 0 (
                echo Error: !textFile! differs from !prevTextFile!
                exit /b 1
            ) else (
				echo OK: !textFile! vs !prevTextFile!
				del "!prevTextFile!"
			)
        )

        set "prevTextFile=!textFile!"
    )
)

endlocal