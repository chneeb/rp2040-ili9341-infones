# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
#set(PICO_SDK_PATH "../../../pico-sdk")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(infoNES C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)

add_executable(infoNES InfoNES.cpp K6502.cpp InfoNES_Mapper.cpp InfoNES_pAPU.cpp tar.cpp audio.c menu.cpp RomLister.cpp FrensHelpers.cpp main.cpp )

pico_set_program_name(infoNES "infoNES")
pico_set_program_version(infoNES "0.1")

pico_enable_stdio_uart(infoNES 1)
pico_enable_stdio_usb(infoNES 1)

# Add the standard library to the build
target_link_libraries(infoNES pico_stdlib)

# ── Hardware target selection ────────────────────────────────────────────────
# Set HARDWARE_TARGET to one of:
#   ORIGINAL_RP2040   — RP2040 + ILI9341 320×240, spi0 LCD / spi1 SD (separate buses)
#   PICO_RESTOUCH     — RP2350 + ST7789  320×240, spi1 shared by LCD, SD, touch (current default)
#   WAVESHARE_LCD13   — RP2350 + ST7789  240×240, spi1 LCD only, GPIO buttons+joystick
set(HARDWARE_TARGET "PICO_RESTOUCH" CACHE STRING
    "Hardware target: ORIGINAL_RP2040 | PICO_RESTOUCH | WAVESHARE_LCD13")

if(HARDWARE_TARGET STREQUAL "ORIGINAL_RP2040")
    # RP2040 + ILI9341 — LCD on spi0, SD on spi1 (separate buses, no touch, no I2C controller)
    set(LCD_CONTROLLER "ILI9341")
    set(LCD_SPI  "spi0")
    set(LCD_DC   "20")
    set(LCD_CS   "17")
    set(LCD_CLK  "18")
    set(LCD_MOSI "19")
    set(LCD_RST  "21")
    set(LCD_BL   "22")
    set(LCD_MISO "-1")
    set(TOUCH_CS "-1")
    set(SD_SPI   "spi1")
    set(SD_CS    "13")
    set(SD_SCK   "10")
    set(SD_MOSI  "11")
    set(SD_MISO  "12")
    set(CPU_FREQ_KHZ "252000")
    set(DISPLAY_WIDTH "320")
    set(DISPLAY_HEIGHT "240")
    set(NES_FIRST_SCANLINE "4")
    set(NES_LAST_SCANLINE "235")
    set(CONTROLLER_TYPE "NONE")

elseif(HARDWARE_TARGET STREQUAL "PICO_RESTOUCH")
    # RP2350 + ST7789 — spi1 shared by LCD, SD card, and XPT2046 touch controller
    set(LCD_CONTROLLER "ST7789")
    set(LCD_SPI  "spi1")
    set(LCD_DC   "8")
    set(LCD_CS   "9")
    set(LCD_CLK  "10")
    set(LCD_MOSI "11")
    set(LCD_RST  "15")
    set(LCD_BL   "13")
    set(LCD_MISO "-1")
    set(TOUCH_CS "16")
    set(SD_SPI   "spi1")
    set(SD_CS    "22")
    set(SD_SCK   "10")
    set(SD_MOSI  "11")
    set(SD_MISO  "12")
    set(CPU_FREQ_KHZ "300000")
    set(DISPLAY_WIDTH "320")
    set(DISPLAY_HEIGHT "240")
    set(NES_FIRST_SCANLINE "4")
    set(NES_LAST_SCANLINE "235")
    set(CONTROLLER_TYPE "NUNCHUCK")
    set(NUNCHUCK_I2C_BUS "i2c1")
    set(NUNCHUCK_SDA "26")
    set(NUNCHUCK_SCL "27")

elseif(HARDWARE_TARGET STREQUAL "WAVESHARE_LCD13")
    # RP2350 + ST7789 240x240 — spi1 LCD only (no SD slot, no touch), GPIO buttons + joystick
    set(LCD_CONTROLLER "ST7789")
    set(LCD_SPI  "spi1")
    set(LCD_DC   "8")
    set(LCD_CS   "9")
    set(LCD_CLK  "10")
    set(LCD_MOSI "11")
    set(LCD_RST  "12")
    set(LCD_BL   "13")
    set(LCD_MISO "-1")
    set(TOUCH_CS "-1")
    # No SD card slot; dummy SD pins so the driver compiles (initSDCard is never called)
    set(SD_SPI   "spi1")
    set(SD_CS    "-1")
    set(SD_SCK   "10")
    set(SD_MOSI  "11")
    set(SD_MISO  "12")
    set(CPU_FREQ_KHZ "300000")
    set(DISPLAY_WIDTH "240")
    set(DISPLAY_HEIGHT "240")
    set(NES_FIRST_SCANLINE "0")
    set(NES_LAST_SCANLINE "239")
    set(CONTROLLER_TYPE "GPIO_BUTTONS")
    set(BTN_A "15")
    set(BTN_B "17")
    set(BTN_X "19")
    set(BTN_Y "21")
    set(JOY_UP    "2")
    set(JOY_DOWN  "18")
    set(JOY_LEFT  "16")
    set(JOY_RIGHT "20")
    set(JOY_CTR   "3")

else()
    message(FATAL_ERROR "Unknown HARDWARE_TARGET '${HARDWARE_TARGET}'. "
            "Choose: ORIGINAL_RP2040, PICO_RESTOUCH, WAVESHARE_LCD13")
endif()

message("Hardware target     : ${HARDWARE_TARGET}")
message("LCD controller      : ${LCD_CONTROLLER}  SPI=${LCD_SPI}")
message("LCD pins DC/CS/CLK/MOSI/RST/BL : ${LCD_DC}/${LCD_CS}/${LCD_CLK}/${LCD_MOSI}/${LCD_RST}/${LCD_BL}")
message("Display             : ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}  NES lines ${NES_FIRST_SCANLINE}-${NES_LAST_SCANLINE}")
message("SD card SPI/CS      : ${SD_SPI} / ${SD_CS}")
message("Touch CS            : ${TOUCH_CS}")
message("CPU freq (kHz)      : ${CPU_FREQ_KHZ}")
message("Controller          : ${CONTROLLER_TYPE}")

target_compile_definitions(infoNES PRIVATE
    HARDWARE_TARGET_${HARDWARE_TARGET}
    SDCARD_SPI_BUS=${SD_SPI}
    SDCARD_PIN_SPI0_CS=${SD_CS}
    SDCARD_PIN_SPI0_SCK=${SD_SCK}
    SDCARD_PIN_SPI0_MOSI=${SD_MOSI}
    SDCARD_PIN_SPI0_MISO=${SD_MISO}
    DISPLAY_SPI_PORT=${LCD_SPI}
    DISPLAY_PIN_DC=${LCD_DC}
    DISPLAY_PIN_CS=${LCD_CS}
    DISPLAY_PIN_CLK=${LCD_CLK}
    DISPLAY_PIN_MOSI=${LCD_MOSI}
    DISPLAY_PIN_RST=${LCD_RST}
    DISPLAY_PIN_BL=${LCD_BL}
    DISPLAY_PIN_MISO=${LCD_MISO}
    TOUCH_PIN_CS=${TOUCH_CS}
    CPU_FREQ_KHZ=${CPU_FREQ_KHZ}
    DISPLAY_WIDTH=${DISPLAY_WIDTH}
    DISPLAY_HEIGHT=${DISPLAY_HEIGHT}
    NES_FIRST_SCANLINE=${NES_FIRST_SCANLINE}
    NES_LAST_SCANLINE=${NES_LAST_SCANLINE}
)

if(LCD_CONTROLLER STREQUAL "ILI9341")
    add_compile_definitions(ILI9341)
elseif(LCD_CONTROLLER STREQUAL "ST7789")
    add_compile_definitions(ST7789)
    add_compile_definitions(DISPLAY_INVERT)
endif()

if(HARDWARE_TARGET STREQUAL "PICO_RESTOUCH")
    # SD card, LCD, and touch controller share spi1 — requires shared-bus workarounds
    add_compile_definitions(SHARED_SPI_BUS)
    add_compile_definitions(OVERCLOCK_VREG)
elseif(HARDWARE_TARGET STREQUAL "WAVESHARE_LCD13")
    add_compile_definitions(OVERCLOCK_VREG)
endif()

if(CONTROLLER_TYPE STREQUAL "NUNCHUCK")
    target_compile_definitions(infoNES PRIVATE
        CONTROLLER_NUNCHUCK
        NUNCHUCK_I2C_BUS=${NUNCHUCK_I2C_BUS}
        NUNCHUCK_SDA=${NUNCHUCK_SDA}
        NUNCHUCK_SCL=${NUNCHUCK_SCL}
    )
elseif(CONTROLLER_TYPE STREQUAL "GPIO_BUTTONS")
    target_compile_definitions(infoNES PRIVATE
        CONTROLLER_GPIO_BUTTONS
        BTN_A=${BTN_A}
        BTN_B=${BTN_B}
        BTN_X=${BTN_X}
        BTN_Y=${BTN_Y}
        JOY_UP=${JOY_UP}
        JOY_DOWN=${JOY_DOWN}
        JOY_LEFT=${JOY_LEFT}
        JOY_RIGHT=${JOY_RIGHT}
        JOY_CTR=${JOY_CTR}
    )
endif()

# Add any user requested libraries
target_link_libraries(infoNES
        pico_stdlib
	pico_multicore
        hardware_spi
        hardware_dma
        hardware_pio
	hardware_i2c
	hardware_interp
    	hardware_timer
    	hardware_clocks
    	hardware_pwm
    	hardware_flash
    sdcard
    fatfs        
        )

pico_add_extra_outputs(infoNES)

