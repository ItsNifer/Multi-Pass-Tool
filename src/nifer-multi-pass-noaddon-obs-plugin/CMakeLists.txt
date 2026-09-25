# Nifer Multipass Grid - OBS source plugin (no addon version)
#
# Build through the official obs-plugintemplate, the same way as the addon
# version. See LOCAL_VS_BUILD.txt. Configure with -DENABLE_FRONTEND_API=ON.

cmake_minimum_required(VERSION 3.16)

project(nifer-multipass-grid-source VERSION 2.0.0 LANGUAGES C)

if(NOT WIN32)
  message(FATAL_ERROR "This plugin is Windows-only.")
endif()

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

add_library(nifer-multipass-grid-source MODULE
  nifer-multipass-grid-source.c
)

target_link_libraries(nifer-multipass-grid-source PRIVATE
  OBS::libobs
  OBS::obs-frontend-api
)

set_target_properties(nifer-multipass-grid-source PROPERTIES PREFIX "")
