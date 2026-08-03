idf_component_register(
    SRCS "main.c" "rtc.c" "maquina.c" "hal_real.c" "hal_sim.c" "ultra.c" "sd.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
)
