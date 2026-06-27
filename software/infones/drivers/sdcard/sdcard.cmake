if (NOT TARGET sdcard)
    add_library(sdcard INTERFACE)

    pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/spi.pio)

    target_sources(sdcard INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/sdcard.c
            ${CMAKE_CURRENT_LIST_DIR}/pio_spi.c
    )

    target_link_libraries(sdcard INTERFACE fatfs pico_stdlib hardware_clocks hardware_spi hardware_pio)

    # sdcard.c dispatches FatFs drive 1 to the flashfs backend when
    # FLASHFS_ENABLED is set (currently only GAMEPI20). The link picks up
    # flashfs.h via flashfs's INTERFACE include dir; if flashfs isn't built
    # for this target, the dispatch is elided by #ifdef.
    if (TARGET flashfs)
        target_link_libraries(sdcard INTERFACE flashfs)
    endif()
    target_include_directories(sdcard INTERFACE ${CMAKE_CURRENT_LIST_DIR})
endif()
