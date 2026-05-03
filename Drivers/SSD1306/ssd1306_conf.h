#ifndef __SSD1306_CONF_H__
#define __SSD1306_CONF_H__

#include "main.h"

#define STM32F1

#define SSD1306_USE_SPI

#define SSD1306_SPI_PORT        hspi2
#define SSD1306_CS_Port         OLED_CS_GPIO_Port
#define SSD1306_CS_Pin          OLED_CS_Pin
#define SSD1306_DC_Port         OLED_DC_GPIO_Port
#define SSD1306_DC_Pin          OLED_DC_Pin
#define SSD1306_Reset_Port      OLED_Res_GPIO_Port
#define SSD1306_Reset_Pin       OLED_Res_Pin

#define SSD1306_WIDTH           128
#define SSD1306_HEIGHT          64

#define SSD1306_INCLUDE_FONT_7x10
#define SSD1306_INCLUDE_FONT_11x18

#endif /* __SSD1306_CONF_H__ */
