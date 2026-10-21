cmake_minimum_required(VERSION 3.20)
project(magpie_backend LANGUAGES C)

# Simulator-only knobs, set by pc/README.md recipes:
#   -DMAGPIE_SIM_TIME_SCALE=<n>     simulated seconds per real second
#   -DMAGPIE_SIM_STALE_AFTER_S=<n>  stop ageing at this age (0 = never)
set(MAGPIE_SIM_TIME_SCALE 1 CACHE STRING "Simulated seconds per real second")
set(MAGPIE_SIM_STALE_AFTER_S 0 CACHE STRING "Stop ageing at this age in seconds")
set(MAGPIE_SIM_MAX_SUBS 16 CACHE STRING "Subscriptions loaded from the fixtures")

if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    foreach(candidate IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp-gsp")
        get_filename_component(candidate "${candidate}" ABSOLUTE)
        if(EXISTS "${candidate}/tools/sim_bridge/CMakeLists.txt")
            set(ESP_GSP_COMPONENT_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR
        "Install espressif/esp-gsp first, or set ESP_GSP_COMPONENT_DIR")
endif()

add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)

# Only portable sources: no ESP-IDF, no BLE, no HTTP. The backend plays the
# sender from the generated payload fixtures, so the exact scene, the exact
# magpie_ui.c and the exact decoder that run on the device also run here.
gsp_add_backend(magpie_backend
    SOURCES
        ../main/magpie_ui.c
        ../main/magpie_ui_state.c
        ../main/magpie_format.c
        ../main/magpie_parse.c
        ../main/magpie_json.c
        platform_pc.c
    INCLUDE_DIRECTORIES ../main
    SCENES ../ui/main.json)

target_compile_definitions(magpie_backend PRIVATE
    MAGPIE_UI_DEBUG=1
    MAGPIE_SIM_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/../tests/host/fixtures/payloads"
    MAGPIE_SIM_TIME_SCALE=${MAGPIE_SIM_TIME_SCALE}
    MAGPIE_SIM_STALE_AFTER_S=${MAGPIE_SIM_STALE_AFTER_S}
    MAGPIE_SIM_MAX_SUBS=${MAGPIE_SIM_MAX_SUBS})
