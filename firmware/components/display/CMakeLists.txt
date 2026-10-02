# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the portable status/button model and opt-in Waveshare ST7789 LVGL screen.
idf_component_register(SRCS "display.c" "display_model.c" INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_lvgl_port lvgl
    PRIV_REQUIRES driver esp_driver_gpio esp_driver_spi esp_timer)
