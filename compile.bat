@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
rc wavec.rc
cl wavec.cpp wavec.res user32.lib gdi32.lib comdlg32.lib comctl32.lib /Fe:wavec.exe /O2