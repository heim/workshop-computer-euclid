# Euclid4 card for Workshop Computer
# Forutsetter Pico SDK installert og PICO_SDK_PATH satt,
# og ComputerCard.h liggende i denne mappen (kopier fra
# Demonstrations+HelloWorlds/PicoSDK/ComputerCard/ i repoet).

cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(euclid C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# main.cpp + USB-descriptorer (rå TinyUSB CDC)
add_executable(euclid
    main.cpp
    usb_descriptors.c
)

# tusb_config.h ligger i denne mappen
target_include_directories(euclid PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(euclid
    pico_stdlib
    pico_multicore        # USB-stacken kjøres på core1
    tinyusb_device        # rå TinyUSB CDC (device-modus)
    pico_unique_id        # serienummer fra flash-unik-ID
    hardware_adc
    hardware_dma
    hardware_pwm
    hardware_spi
    hardware_i2c
    hardware_flash
)

# Vi bruker rå TinyUSB på core1 og driver stdio ikke over USB.
# (Slå av stdio helt over både USB og UART for å unngå at noe annet
#  griper USB-controlleren.)
pico_enable_stdio_usb(euclid 0)
pico_enable_stdio_uart(euclid 0)

# Generer .uf2 for flashing via BOOTSEL
pico_add_extra_outputs(euclid)
