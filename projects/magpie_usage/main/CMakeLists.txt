# Split so the portable half stays ESP-IDF free and can be compiled by the
# host tests (tests/host/run_tests.py) and the PC simulator backend.
#
# The BLE central is only compiled when Bluetooth is enabled, so a Wi-Fi-only
# build (CONFIG_BT_ENABLED=n) still links and the transport arbiter simply has
# no BLE source to choose.
set(magpie_sources
    "main.c"
    "board_display.c"
    "magpie_config.c"
    "magpie_store.c"
    "magpie_ui.c"
    "magpie_ui_state.c"
    "magpie_pull.c"
    "magpie_parse.c"
    "magpie_json.c"
    "magpie_format.c"
    "magpie_wifi.c")
if(CONFIG_BT_ENABLED)
    list(APPEND magpie_sources "magpie_ble.c")
endif()

idf_component_register(
    SRCS ${magpie_sources}
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
        esp_timer bt esp_event esp_netif esp_wifi esp_http_client
    PRIV_REQUIRES esp-gsp esp_display_present esp_lcd)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")
