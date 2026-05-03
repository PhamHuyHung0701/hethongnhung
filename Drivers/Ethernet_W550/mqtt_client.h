/**
 * @file  mqtt_client.h
 * @brief Minimal MQTT 3.1.1 client for STM32 + W5500
 *
 * Topics (Mosquitto broker):
 *   door/open     – bất kỳ payload nào  → mở cửa ngay
 *   door/password – payload = mật khẩu mới → đổi mật khẩu
 *
 * Cấu hình nhanh:
 *   1. Đổi MQTT_BROKER_IP  cho đúng IP máy chạy Mosquitto
 *   2. Đổi MQTT_DEVICE_IP  nếu muốn IP khác cho STM32
 *   3. Build & flash
 *
 * Test từ PC (mosquitto_pub):
 *   mosquitto_pub -h 192.168.1.X -t door/open     -m "1"
 *   mosquitto_pub -h 192.168.1.X -t door/password -m "newpass"
 */

#ifndef MQTT_CLIENT_H_
#define MQTT_CLIENT_H_

#include <stdint.h>

/* ================================================================
 * Network configuration — chỉnh theo mạng của bạn
 * ================================================================ */

/** IP của PC/server chạy Mosquitto broker */
#define MQTT_BROKER_IP      {192, 168, 137, 1}

/** Port MQTT tiêu chuẩn (Mosquitto mặc định) */
#define MQTT_BROKER_PORT    1883U

/** Keep-alive interval (giây) */
#define MQTT_KEEPALIVE_S    60U

/** Client ID nhận diện thiết bị trên broker */
#define MQTT_CLIENT_ID      "STM32_DoorLock"

/** W5500 socket số dùng cho MQTT (0–7) */
#define MQTT_SOCKET_NUM     0U

/* ================================================================
 * Topics
 * ================================================================ */
#define MQTT_TOPIC_OPEN     "door/open"      /**< Nhận → mở cửa     */
#define MQTT_TOPIC_PASSWORD "door/password"  /**< Nhận → đổi mật khẩu */

#define MQTT_PAYLOAD_MAX    32U

/* ================================================================
 * Callbacks
 * ================================================================ */
typedef void (*MQTT_DoorOpenCb)(void);
typedef void (*MQTT_PasswordCb)(const char *new_pwd);

/* ================================================================
 * API
 * ================================================================ */

/**
 * @brief Khởi tạo MQTT module, đăng ký callbacks.
 *        Gọi 1 lần sau W5500_Init().
 */
void MQTT_Init(MQTT_DoorOpenCb door_cb, MQTT_PasswordCb pwd_cb);

/**
 * @brief Polling task — gọi trong vòng lặp main mỗi ~10 ms.
 *        Tự động kết nối lại nếu mất kết nối.
 */
void MQTT_Task(void);

#endif /* MQTT_CLIENT_H_ */


