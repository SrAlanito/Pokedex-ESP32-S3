#!/usr/bin/env python3
"""Keep the ST7789 init from hanging on Arduino core 3.

Core 3's SPI clock divider leaves clkcnt_l and clkcnt_h illegal on the
ESP32-S3. The transfer-done bit then never clears, and tft.init() spins
with the backlight on and the panel black. After each transaction starts,
force a legal 40 MHz divider: clkcnt_l = clkcnt_n = 1, clkcnt_h = 0.
"""

from pathlib import Path

try:
    Import("env")  # noqa: F821
    ROOT = Path(env["PROJECT_DIR"])  # noqa: F821
    ENV_NAME = env["PIOENV"]  # noqa: F821
except Exception:
    ROOT = Path(__file__).resolve().parent.parent
    ENV_NAME = "esp32-s3-n16r8"

HEADER = (
    ROOT
    / ".pio"
    / "libdeps"
    / ENV_NAME
    / "TFT_eSPI"
    / "Processors"
    / "TFT_eSPI_ESP32_S3.h"
)

OLD = """\
  #if (TFT_SPI_MODE == SPI_MODE1) || (TFT_SPI_MODE == SPI_MODE2)
    #define SET_BUS_WRITE_MODE *_spi_user = SPI_USR_MOSI | SPI_CK_OUT_EDGE
    #define SET_BUS_READ_MODE  *_spi_user = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN | SPI_CK_OUT_EDGE
  #else
    #define SET_BUS_WRITE_MODE *_spi_user = SPI_USR_MOSI
    #define SET_BUS_READ_MODE  *_spi_user = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN
  #endif
"""

NEW = """\
  // Arduino core 3 programs clkcnt_l/clkcnt_h illegally, so the transfer bit never
  // clears and init() spins forever. 0x1001 is 40 MHz from the 80 MHz SPI clock:
  // clkcnt_l = clkcnt_n = 1, clkcnt_h = 0, pre-divider = 0.
  #define TFT_S3_CLOCK_REG ((volatile uint32_t*)(SPI_CLOCK_REG(SPI_PORT)))
  #if (TFT_SPI_MODE == SPI_MODE1) || (TFT_SPI_MODE == SPI_MODE2)
    #define SET_BUS_WRITE_MODE *_spi_user = SPI_USR_MOSI | SPI_CK_OUT_EDGE; *TFT_S3_CLOCK_REG = 0x1001
    #define SET_BUS_READ_MODE  *_spi_user = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN | SPI_CK_OUT_EDGE; *TFT_S3_CLOCK_REG = 0x1001
  #else
    #define SET_BUS_WRITE_MODE *_spi_user = SPI_USR_MOSI; *TFT_S3_CLOCK_REG = 0x1001
    #define SET_BUS_READ_MODE  *_spi_user = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN; *TFT_S3_CLOCK_REG = 0x1001
  #endif
"""


def main():
    if not HEADER.is_file():
        print("patch_tft: TFT_eSPI is not installed yet")
        return
    text = HEADER.read_text()
    if "TFT_S3_CLOCK_REG" in text:
        return
    if OLD not in text:
        raise SystemExit("patch_tft: TFT_eSPI S3 header layout changed; clock fix was not applied")
    HEADER.write_text(text.replace(OLD, NEW, 1))
    print("patch_tft: forced a legal SPI clock in", HEADER)


main()
