# ESP-IDF evaluates component CMakeLists twice: an early requirements pass
# runs BEFORE sdkconfig is loaded, so CONFIG_* is unset there. Requirements
# must therefore be unconditional (a conditional branch registers the wrong
# dependency set on the early pass); only the source selection may depend on
# config, because SRCS is resolved on the real configure pass.
if(CONFIG_BT_NIMBLE_ENABLED)
    set(ble_srcs "ble_server.c")
else()
    set(ble_srcs "ble_server_stub.c")
endif()

idf_component_register(
    SRCS ${ble_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../rpc/include"
    REQUIRES nvs_keys bt nvs_flash freertos
    PRIV_REQUIRES rpc security main
)
