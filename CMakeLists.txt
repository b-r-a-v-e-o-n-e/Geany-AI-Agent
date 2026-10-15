# geany-ai-qt — Claude AI assistant plugin for Geany with a Qt6 user interface
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(geany-ai-qt VERSION 0.4.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GEANY REQUIRED IMPORTED_TARGET geany)
pkg_check_modules(GTK3  REQUIRED IMPORTED_TARGET gtk+-3.0)

# Where Geany looks for system-wide plugins: <libdir>/geany
pkg_get_variable(GEANY_LIBDIR geany libdir)
set(GEANY_PLUGIN_DIR "${GEANY_LIBDIR}/geany" CACHE PATH "Geany plugin install directory")

set(PLUGIN_SOURCES
    src/plugin.cpp
    src/qtgtk.cpp
    src/i18n.cpp
    src/settings.cpp
    src/tools.cpp
    src/mcp.cpp
    src/api.cpp
    src/diff.cpp
    src/dialogs.cpp
)

add_library(geany-ai-qt MODULE ${PLUGIN_SOURCES})
set_target_properties(geany-ai-qt PROPERTIES PREFIX "" OUTPUT_NAME "geany-ai-qt")
target_compile_definitions(geany-ai-qt PRIVATE QT_NO_KEYWORDS GEANY_AI_VERSION="${PROJECT_VERSION}")
target_compile_options(geany-ai-qt PRIVATE -Wall -Wextra)
target_link_libraries(geany-ai-qt PRIVATE Qt6::Widgets Qt6::Network PkgConfig::GEANY PkgConfig::GTK3)
# Never unload: Qt keeps the QApplication alive for the lifetime of Geany.
target_link_options(geany-ai-qt PRIVATE -Wl,-z,nodelete)

install(TARGETS geany-ai-qt LIBRARY DESTINATION "${GEANY_PLUGIN_DIR}")

# Pure-logic unit tests (no Geany / GTK needed): cmake -DBUILD_TESTS=ON
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests tests/unit-tests.cpp src/diff.cpp src/tools.cpp src/mcp.cpp src/api.cpp src/settings.cpp src/i18n.cpp)
    target_include_directories(unit-tests PRIVATE src)
    target_compile_definitions(unit-tests PRIVATE QT_NO_KEYWORDS GEANY_AI_VERSION="${PROJECT_VERSION}")
    target_link_libraries(unit-tests PRIVATE Qt6::Core Qt6::Network PkgConfig::GEANY)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()
