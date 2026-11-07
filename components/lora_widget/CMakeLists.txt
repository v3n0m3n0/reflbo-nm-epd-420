idf_component_register(
    SRCS
        "lora_widget.c"
        "meshcore_proto.c"
        "meshcore_crypto.c"
        "ed25519/fe.c"
        "ed25519/ge.c"
        "ed25519/sc.c"
        "ed25519/sha512.c"
        "ed25519/keypair.c"
        "ed25519/sign.c"
        "ed25519/verify.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "ed25519"
    PRIV_REQUIRES board esp_driver_gpio esp_driver_spi freertos esp_timer esp_hw_support log util
)
