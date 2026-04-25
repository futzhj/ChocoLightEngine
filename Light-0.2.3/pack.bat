@echo off
chcp 65001 >nul
title ChocoLight Pack Tool

:: ============================================================
:: ChocoLight 打包脚本
:: 用法: 双击运行，或命令行: pack.bat [脚本路径] [密钥]
:: ============================================================

set TEMPLATE=lightw.exe
set COMPILER=lightc.exe
set PACKER=pack.exe
set OUTPUT=game.exe

:: 默认脚本路径和密钥
set SCRIPT=lua\main.lua
set KEY=ChocoLight2026

:: 如果有命令行参数则使用
if not "%~1"=="" set SCRIPT=%~1
if not "%~2"=="" set KEY=%~2

:: 检查必要文件
if not exist "%TEMPLATE%" (
    echo [ERROR] 模板文件不存在: %TEMPLATE%
    pause & exit /b 1
)
if not exist "%PACKER%" (
    echo [ERROR] 打包工具不存在: %PACKER%
    pause & exit /b 1
)
if not exist "%SCRIPT%" (
    echo [ERROR] 脚本文件不存在: %SCRIPT%
    pause & exit /b 1
)

echo ========================================
echo   ChocoLight Pack Tool
echo ========================================
echo   模板:   %TEMPLATE%
echo   脚本:   %SCRIPT%
echo   输出:   %OUTPUT%
echo   密钥:   %KEY%
echo ========================================
echo.

%PACKER% %TEMPLATE% %SCRIPT% -o %OUTPUT% -c %COMPILER% -k "%KEY%"

if %errorlevel% equ 0 (
    echo.
    echo [OK] 打包完成: %OUTPUT%
) else (
    echo.
    echo [FAIL] 打包失败
)

echo.
pause
