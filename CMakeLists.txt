cmake_minimum_required(VERSION 3.16...3.26)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(LOCAL_INSTALLATION "Copy to ~/.config/obs-studio/plugins after build" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)
add_subdirectory(deps)

find_package(libobs REQUIRED)
if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
else()
    set(MPV_LIBRARIES "${CMAKE_CURRENT_SOURCE_DIR}/deps/libmpv/libmpv.dll.a")
    set(MPV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/deps/libmpv/include")
    # Copy dependencies
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/libmpv/libmpv-2.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>)
    add_custom_command(
      TARGET ${PROJECT_NAME}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_BUILD_DIR}/obs-plugins/64bit"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/deps/libmpv/libmpv-2.dll"
              "${OBS_BUILD_DIR}/obs-plugins/64bit"
      COMMENT "Copy ${target} to obs-studio directory ${OBS_BUILD_DIR}"
      VERBATIM)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/deps/libmpv/libmpv-2.dll"
        CONFIGURATIONS RelWithDebInfo Debug Release
        DESTINATION obs-plugins/64bit)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/wgl.c src/wgl.h src/mpv-backend-d3d.c)
endif()
message(STATUS ${MPV_INCLUDE_DIRS})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs ${MPV_LIBRARIES} OBS::glad)
target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC "${MPV_INCLUDE_DIRS}")

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_qt(COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt::Core Qt::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME} PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
                                  -Wno-comma>)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON
               AUTOUIC ON
               AUTORCC ON)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/mpv-source.c src/mpv-source.h src/mpv-backend.c src/mpv-backend.h src/mpv-backend-opengl.c)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})


if (LOCAL_INSTALLATION)
    if (UNIX AND NOT APPLE)
        # Installs into home directory
        add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            # Copy to obs-studio dev environment for immediate testing
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "$ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}/bin/64bit"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "$ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}/data"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
                    "$ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}/bin/64bit"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${PROJECT_SOURCE_DIR}/data"
                    "$ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}/data"
        )
    endif()
endif()
