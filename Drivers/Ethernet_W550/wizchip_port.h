/*
 * wizchip_port.h
 *
 *  Created on: Oct 27, 2025
 *      Author: arunrawat
 */

#ifndef INC_WIZCHIP_PORT_H_
#define INC_WIZCHIP_PORT_H_

#include <stdint.h>

typedef void (*W5500_LogCallback)(const char *message);

void W5500_SetLogCallback(W5500_LogCallback callback);
int  W5500_Init(void);
uint8_t W5500_GetVersion(void);

/**
 * @brief Hiển thị thông tin chẩn đoán lên OLED (qua log callback).
 *        Gọi sau W5500_Init() thành công.
 *        Hiện: chip version, PHY link, IP, Gateway.
 */
void W5500_Diag(void);

#endif /* INC_WIZCHIP_PORT_H_ */
