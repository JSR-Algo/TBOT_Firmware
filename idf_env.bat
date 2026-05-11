@echo off
REM ESP-IDF v5.5.4 EIM env setup, then run whatever was passed as args.
REM Usage:  idf_env.bat <any command>
REM   e.g.  idf_env.bat idf.py build
REM         idf_env.bat python scripts/release.py freenove-esp32s3-display-2.8-lcd

set "IDF_PATH=C:\esp\v5.5.4\esp-idf"
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.4\venv"
set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011"
set "OPENOCD_SCRIPTS=C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts"
set "IDF_COMPONENT_LOCAL_STORAGE_URL=file://C:\Espressif\tools"
set "ESP_IDF_VERSION=5.5"

set "PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Espressif\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin;C:\Espressif\tools\riscv32-esp-elf-gdb\16.3_20250913\riscv32-esp-elf-gdb\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\xtensa-esp-elf\bin;C:\Espressif\tools\python\v5.5.4\venv\Scripts;%PATH%"

set "PYTHONUNBUFFERED=1"

cd /d "%~dp0"

%*
