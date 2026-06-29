@echo off
REM Kill any running llama-server, then restart with Phi-3.5-mini (deterministic flags).
REM Called by test_roundtrip.c via system() between encode and decode phases.

set MODEL=E:\Development\doublespeak-c\models\Phi-3.5-mini-instruct-Q4_K_M.gguf
set LLAMA=E:\Development\doublespeak-c\meteor_stego\out\build\x64-debug\bin\llama-server.exe
set LOGFILE=E:\Development\doublespeak-c\meteor_stego\out\build\x64-debug\llama_server.log

echo [restart] killing llama-server.exe...
taskkill /F /IM llama-server.exe >nul 2>&1
timeout /T 3 /NOBREAK >nul

echo [restart] starting fresh llama-server with Phi-3.5-mini...
start /B "" "%LLAMA%" ^
    --model          "%MODEL%"   ^
    --port           8080         ^
    --host           127.0.0.1    ^
    --threads        1            ^
    --seed           42           ^
    --temp           0.0          ^
    --ctx-size       2048         ^
    --no-mmap                     ^
    --no-cont-batching            ^
    --log-disable                 ^
    >> "%LOGFILE%" 2>&1

echo [restart] llama-server relaunched.
exit /b 0
