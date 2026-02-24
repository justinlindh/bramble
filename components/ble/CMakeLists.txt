if(CONFIG_BT_ENABLED)
  idf_component_register(
      SRCS "ble_server.c"
      INCLUDE_DIRS "include"
      REQUIRES bt rpc nvs_flash freertos
  )
else()
  idf_component_register(
      SRCS "ble_server_stub.c"
      INCLUDE_DIRS "include"
  )
endif()
