# Header-only component holding tusb_config.h. It exists so that both TinyUSB
# itself and the code that includes tusb.h see the same configuration; TinyUSB
# looks for "tusb_config.h" on the include path and nowhere else.
idf_component_register(INCLUDE_DIRS "include")
