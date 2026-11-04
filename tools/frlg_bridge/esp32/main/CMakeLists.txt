# The bridge component: the state machine is platform-free (bridge.c);
# the network and the entry are the board's.
idf_component_register(SRCS "main.c" "bridge.c" "wifi.c"
                       INCLUDE_DIRS ".")
