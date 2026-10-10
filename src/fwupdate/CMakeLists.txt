# Firmware update from the SD card (RP2350 A/B partitions)
add_library(pm_fwupdate INTERFACE)

target_include_directories(pm_fwupdate INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_sources(pm_fwupdate INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/pm_fwupdate.c
)

target_link_libraries(pm_fwupdate INTERFACE
    boot_uf2_headers
    hardware_flash
    pico_bootrom
    pico_multicore
)
