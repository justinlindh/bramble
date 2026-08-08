# ESP-IDF evaluates component CMakeLists twice: an early requirements pass
# runs BEFORE sdkconfig is loaded, so CONFIG_* is unset there. Requirements
# must therefore be unconditional (a conditional branch registers the wrong
# dependency set on the early pass); only the source selection may depend on
# config, because SRCS is resolved on the real configure pass.
# (IDF_TARGET is an exception: it is known on the early pass, so the linux
# simulator branch below is safe.)
if(CONFIG_BT_NIMBLE_ENABLED)
    set(ble_srcs "ble_server.c")
else()
    set(ble_srcs "ble_server_stub.c")
endif()

# The bt component does not exist on the POSIX/Linux simulator; NimBLE can
# never be enabled there, so the stub is always the selected source.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "ble_server_stub.c" "ble_pairing_policy.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "../rpc/include"
        REQUIRES nvs_keys nvs_flash freertos
        PRIV_REQUIRES rpc security main
    )
else()
    idf_component_register(
        SRCS ${ble_srcs} "ble_pairing_policy.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "../rpc/include"
        REQUIRES nvs_keys bt nvs_flash freertos
        PRIV_REQUIRES rpc security main
    )
endif()
