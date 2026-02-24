if(CONFIG_BT_NIMBLE_ENABLED)
  idf_component_register(
      SRCS "ble_server.c"
      INCLUDE_DIRS "include"
      PRIV_INCLUDE_DIRS "../rpc/include"
      REQUIRES bt nvs_flash freertos
      PRIV_REQUIRES rpc
  )
else()
  idf_component_register(
      SRCS "ble_server_stub.c"
      INCLUDE_DIRS "include"
  )
endif()
