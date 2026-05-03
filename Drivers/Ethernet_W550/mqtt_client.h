/**
 * @file  mqtt_client.h
 * @brief Minimal MQTT 3.1.1 client for STM32 + W5500
 *
 * Topic duy nhất: device/stm32-001/command
 * Payload JSON:   {"type":"DOOR","action":"OPEN"}
 *                 {"type":"DOOR","action":"SET_PWD","value":"newpass"}
 *                 {"type":"LED", "action":"ON"}   ← hiển thị lên OLED
 *
 * Test từ PC:
 *   mosquitto_pub -h 192.168.137.1 -t device/stm32-001/command \
 *                 -m "{\"type\":\"DOOR\",\"action\":\"OPEN\"}"
 */

#ifndef MQTT_CLIENT_H_
#define MQTT_CLIENT_H_

#include <stdint.h>

/* ================================================================
 * Broker — chỉnh IP nếu cần
 * ================================================================ */
#define MQTT_BROKER_IP      {192, 168, 137, 1}
#define MQTT_BROKER_PORT    1883U
#define MQTT_KEEPALIVE_S    60U
#define MQTT_CLIENT_ID      "stm32-001"
#define MQTT_SOCKET_NUM     0U

/* ================================================================
 * Topic & JSON constants
 * ================================================================ */
#define MQTT_TOPIC_CMD      "device/stm32-001/command"

/* JSON type/action values mà STM32 xử lý */
#define MQTT_TYPE_DOOR      "DOOR"
#define MQTT_ACT_OPEN       "OPEN"       /* {"type":"DOOR","action":"OPEN"}             */
#define MQTT_ACT_SET_PWD    "SET_PWD"    /* {"type":"DOOR","action":"SET_PWD","value":…}*/

#define MQTT_PAYLOAD_MAX    64U

/* ================================================================
 * Callbacks
 * ================================================================ */
/** Gọi khi type=DOOR, action=OPEN → mở cửa */
typedef void (*MQTT_DoorOpenCb)(void);

/** Gọi khi type=DOOR, action=SET_PWD → đổi mật khẩu */
typedef void (*MQTT_PasswordCb)(const char *new_pwd);

/**
 * Gọi cho MỌI message nhận được — dùng để hiển thị OLED
 * @param type    giá trị field "type"   trong JSON
 * @param action  giá trị field "action" trong JSON
 * @param value   giá trị field "value"  trong JSON (hoặc "" nếu không có)
 */
typedef void (*MQTT_MessageCb)(const char *type, const char *action, const char *value);

/* ================================================================
 * API
 * ================================================================ */
void MQTT_Init(MQTT_DoorOpenCb door_cb,
               MQTT_PasswordCb pwd_cb,
               MQTT_MessageCb  msg_cb);

void MQTT_Task(void);  /* gọi trong main loop mỗi ~10 ms */

#endif /* MQTT_CLIENT_H_ */


