idf_component_register(
    SRCS "main.c" "rtc.c" "maquina.c" "hal_real.c" "hal_sim.c" "ultra.c" "ads.c" "sd.c" "max.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
)
