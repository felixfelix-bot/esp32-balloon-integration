set(COMPONENT_REQUIRES
    RadioLib esp_timer driver bmp280 power_manager telemetry
    antenna_switch sky66112 gps cli bench
)

if(CONFIG_ENABLE_MESH)
    list(APPEND COMPONENT_REQUIRES pipeline erasure frag mesh_adapter)
endif()

if(CONFIG_ENABLE_TDMA)
    list(APPEND COMPONENT_REQUIRES tdma)
endif()

if(CONFIG_ENABLE_NOSTR_STORE)
    list(APPEND COMPONENT_REQUIRES nostr_store)
endif()

if(CONFIG_ENABLE_MESHCORE)
    list(APPEND COMPONENT_REQUIRES meshcore)
    message(STATUS "MeshCore enabled, adding meshcore to REQUIRES")
endif()

idf_component_register(
    SRCS "app_main.cpp" "bench_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES ${COMPONENT_REQUIRES}
    PRIV_REQUIRES meshcore
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RADIOLIB_DEBUG_SPI=1
    RADIOLIB_DEBUG_BASIC=1
)

# `ID? fw=<sha7>` (spec §2.1): the bench console reports the revision it was
# built from, so a captured ID line is traceable to a commit.
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BENCH_FW_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BENCH_FW_SHA)
    set(BENCH_FW_SHA "nogit")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_FW_SHA="${BENCH_FW_SHA}")
