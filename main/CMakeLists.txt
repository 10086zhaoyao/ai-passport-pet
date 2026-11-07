idf_component_register(
    SRCS "main.c"
         "pet_app.c"
         "pet_logic.c"
         "pet_store.c"
         "profile_store.c"
         "wifi_portal.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash esp_event esp_wifi esp_netif esp_http_server
             esp_http_client lwip
)
