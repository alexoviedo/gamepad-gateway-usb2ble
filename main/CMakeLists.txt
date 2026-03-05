idf_component_register(
    SRCS "main.cpp"
         "usb_host_manager.cpp"
         "hid_device_manager.cpp"
         "hid_parser.cpp"
         "input_decoder.cpp"
         "input_elements.cpp"
         "ble_gamepad.cpp"
    INCLUDE_DIRS "."

    # Public deps (headers our public includes may expose)
    REQUIRES usb usb_host_hid esp_hid bt

    # Private deps (headers we include internally)
    PRIV_REQUIRES nvs_flash esp_driver_gpio
)

# NimBLE structs use designated initializers; keep warnings quiet on newer GCC.
set_source_files_properties(ble_gamepad.cpp PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers")
