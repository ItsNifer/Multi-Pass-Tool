# Nifer Multipass - OBS source plugin
#
# RECOMMENDED build path: use the official obs-plugintemplate
# (https://github.com/obsproject/obs-plugintemplate) - create a repo from
# the template, drop nifer-multipass-source.c into src/, copy
# ../shared/nifer_multipass_shared.h next to it (fix the #include path),
# and let the template's CMake presets / GitHub Actions build it.
#
# This standalone CMakeLists works if you have a libobs development
# package (obs-studio build tree) available to find_package.

cmake_minimum_required(VERSION 3.16)

project(nifer-multipass-source VERSION 2.0.0 LANGUAGES C)

if(NOT WIN32)
  message(FATAL_ERROR "This plugin is Windows-only (D3D11 shared textures).")
endif()

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED) # for record start/stop sync

add_library(nifer-multipass-source MODULE
  nifer-multipass-source.c
)

target_include_directories(nifer-multipass-source PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_link_libraries(nifer-multipass-source PRIVATE
  OBS::libobs
  OBS::obs-frontend-api
)

# Install into the OBS plugin folder layout:
#   obs-plugins/64bit/nifer-multipass-source.dll
set_target_properties(nifer-multipass-source PROPERTIES PREFIX "")
