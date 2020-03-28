@echo off
cd rynx/generate
"./sharpmake/Sharpmake.Application.exe" /sources(@"../../generate/main.sharpmake.cs")
cd ..
@if %errorlevel% neq 0 goto :error

@goto :exit
:error
@echo "========================= ERROR ========================="
@pause

:exit

@echo Solution generation completed with SUCCESS
