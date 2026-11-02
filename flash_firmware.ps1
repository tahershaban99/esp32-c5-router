param (
    [string]$Port = "COM3",
    [int]$Baud = 921600
)

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

Write-Host "Flashing firmware to $Port at $Baud baud..."
python E:\esp\v6.1\esp-idf\tools\idf.py -B build -p $Port -b $Baud flash monitor

