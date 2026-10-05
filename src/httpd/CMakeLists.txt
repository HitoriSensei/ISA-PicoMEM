# Minimal HTTP server for the SD card (lwIP raw API, shares the CYW43 with the NE2000)
add_library(pm_httpd INTERFACE)

target_include_directories(pm_httpd INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_sources(pm_httpd INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/pm_netif.c
    ${CMAKE_CURRENT_LIST_DIR}/pm_httpd.c
)

target_link_libraries(pm_httpd INTERFACE
    pico_lwip
    pico_lwip_arch
)
