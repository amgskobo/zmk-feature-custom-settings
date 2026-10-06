cmake_minimum_required(VERSION 3.20.0)
list(APPEND DTS_ROOT "${ZMK_ROOT}/app")
list(APPEND SYSCALL_INCLUDE_DIRS "${ZMK_ROOT}/app/include")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(custom_settings_boundary)
if(NOT CUSTOM_SETTINGS_SOURCE_DIR)
  set(CUSTOM_SETTINGS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
endif()
if(NOT EXISTS "${ZMK_ROOT}/app/src/event_manager.c")
  message(FATAL_ERROR "ZMK_ROOT must select the resolved ZMK checkout")
endif()
target_sources(app PRIVATE main.c "${CUSTOM_SETTINGS_SOURCE_DIR}/custom_settings.c"
  "${ZMK_ROOT}/app/src/event_manager.c")
target_include_directories(app PRIVATE ../../include "${ZMK_ROOT}/app/include")
target_compile_definitions(app PRIVATE CONFIG_ZMK_LOG_LEVEL=0
  CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE=64
  CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE=128
  CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN=32
  CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN=48
  CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS=2 CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE=32)
if(SETTINGS_BOUNDARY_FEATURES)
  target_sources(app PRIVATE "${CUSTOM_SETTINGS_SOURCE_DIR}/custom_settings_array.c"
    "${CUSTOM_SETTINGS_SOURCE_DIR}/custom_settings_pool.c"
    "${CUSTOM_SETTINGS_SOURCE_DIR}/custom_settings_keyspace.c"
    "${CUSTOM_SETTINGS_SOURCE_DIR}/custom_settings_rpc_convert.c")
  target_compile_definitions(app PRIVATE CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY=1
    CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES=1 CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=1
    CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS=1 CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE=16)
endif()
zephyr_linker_sources(ROM_SECTIONS ../../include/linker/zmk-custom-settings-rom.ld)
zephyr_linker_sources(DATA_SECTIONS ../../include/linker/zmk-custom-settings.ld)
zephyr_linker_sources(RODATA "${ZMK_ROOT}/app/include/linker/zmk-events.ld")
target_link_options(zephyr_interface INTERFACE -Wl,--wrap=settings_save_one
  -Wl,--wrap=settings_delete -Wl,--wrap=settings_load_subtree)
