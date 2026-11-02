$paths = @(
    "E:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "E:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
    "E:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "E:\Espressif\tools\cmake\4.0.3\bin",
    "E:\Espressif\tools\ninja\1.12.1",
    "E:\Espressif\tools\python\v6.1\venv\Scripts",
    "E:\esp\v6.1\esp-idf\tools"
)
$env:PATH = ($paths -join ";") + ";" + $env:PATH
$env:IDF_PATH = "E:\esp\v6.1\esp-idf"
$env:ESP_ROM_ELF_DIR = "E:\Espressif\tools\esp-rom-elfs\20241011"

if (!(Test-Path "build\build.ninja")) {
    Write-Host "Configuring CMake project with Ninja for ESP32-C5..."
    cmake -G Ninja -DIDF_TARGET=esp32c5 -DPYTHON="E:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -DPYTHON_DEPS_CHECKED=1 -S . -B build
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Project configuration failed."
        exit $LASTEXITCODE
    }
}

Write-Host "Compiling project with Ninja..."
ninja -C build
if ($LASTEXITCODE -eq 0) {
    if (Test-Path "build\esp32_napt_router.bin") {
        if (!(Test-Path "bin")) { New-Item -ItemType Directory -Path "bin" -Force | Out-Null }
        Copy-Item -Path "build\esp32_napt_router.bin" -Destination "bin\esp32_napt_router.bin" -Force
        Copy-Item -Path "build\bootloader\bootloader.bin" -Destination "bin\bootloader.bin" -Force
        Copy-Item -Path "build\partition_table\partition-table.bin" -Destination "bin\partition-table.bin" -Force
        if (Test-Path "build\ota_data_initial.bin") {
            Copy-Item -Path "build\ota_data_initial.bin" -Destination "bin\ota_data_initial.bin" -Force
        }
    }
    Write-Host "`n[SUCCESS] Build completed successfully!"
    exit 0
} else {
    Write-Host "`n[ERROR] Build failed."
    exit $LASTEXITCODE
}
