/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define CS_Pin              GPIO_PIN_4      /* PA4  - W5500 SPI CS        */
#define CS_GPIO_Port        GPIOA
#define RESET_Pin           GPIO_PIN_12     /* PA12 - OLED /RST           */
#define RESET_GPIO_Port     GPIOA
#define PB11_TOGGLE_Pin     GPIO_PIN_11     /* PB11 - Relay               */
#define PB11_TOGGLE_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* OLED SPI (SPI2) control pins */
#define OLED_CS_Pin         GPIO_PIN_12     /* PB12 - OLED CS             */
#define OLED_CS_GPIO_Port   GPIOB
#define OLED_DC_Pin         GPIO_PIN_14     /* PB14 - OLED D/C            */
#define OLED_DC_GPIO_Port   GPIOB
#define OLED_Res_Pin        RESET_Pin       /* PA12 - OLED /RST (shared)  */
#define OLED_Res_GPIO_Port  RESET_GPIO_Port

#define OLED_RES_Pin        OLED_Res_Pin
#define OLED_RES_GPIO_Port  OLED_Res_GPIO_Port

/* Electric lock on PB4 — active-HIGH: SET = mở khoá, RESET = khoá */
#define LOCK_Pin            GPIO_PIN_4
#define LOCK_GPIO_Port      GPIOB

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
