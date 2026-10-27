zephyr_library()
zephyr_library_sources_ifdef(CONFIG_RS_485 rs_485_calixto.c)
