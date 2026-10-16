idf_component_register(SRCS "app_main.c"
                            "auto_gen.c"
                            "automation.c"
                            "button.c"
                            "encoder.c"
                            "lcd_i2c.c"
                            "pulse_gen.c"
                            "run.c"
                            "screen.c"
                            "settings.c"
                            "history.c"
                            "ringlog.c"
                            "state_log.c"
                            "tally.c"
                            "ui.c"
                            "wallclock.c"
                            "web_server.c"
                            "wifi_ap.c"
                    INCLUDE_DIRS ".")

# Página do modo Automação: web/index.html + cabeçalho HTML, comprimida com gzip e embutida
idf_build_get_property(python PYTHON)
set(page_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${page_gz}"
    COMMAND "${python}" "${CMAKE_CURRENT_SOURCE_DIR}/web/pack.py" "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" "${page_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" "${CMAKE_CURRENT_SOURCE_DIR}/web/pack.py"
    VERBATIM)
add_custom_target(web_page DEPENDS "${page_gz}")
add_dependencies(${COMPONENT_LIB} web_page)
target_add_binary_data(${COMPONENT_LIB} "${page_gz}" BINARY DEPENDS web_page)
