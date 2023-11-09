add_subdirectory(glad)
if (WIN32)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libmpv")
        if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty_VS2013_v120_x86_x64_V9_small.7z")
            MESSAGE(STATUS "Downloading libmpv")
            file(DOWNLOAD https://kumisystems.dl.sourceforge.net/project/mpv-player-windows/libmpv/mpv-dev-x86_64-20231105-git-7480efa.7z
                "${CMAKE_CURRENT_SOURCE_DIR}/libmpv.7z" SHOW_PROGRESS
                EXPECTED_MD5 6b661a5cec5d50790d79b195d4028c4f)
        endif()
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libmpv")
        message(STATUS "Expanding libmpv")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${CMAKE_CURRENT_SOURCE_DIR}/libmpv.7z" )
        file(RENAME ${CMAKE_SOURCE_DIR}/libmpv-2.dll ${CMAKE_CURRENT_SOURCE_DIR}/libmpv/libmpv-2.dll)
        file(RENAME ${CMAKE_SOURCE_DIR}/libmpv.dll.a ${CMAKE_CURRENT_SOURCE_DIR}/libmpv/libmpv.dll.a)
        file(RENAME ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/libmpv/include)
    endif()

endif()