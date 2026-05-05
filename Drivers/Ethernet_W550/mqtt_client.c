/**
 * @file   mqtt_client.c
 * @brief  MQTT 3.1.1 client tối giản — W5500 socket, single topic subscribe.
 *
 * @detail Triển khai MQTT client theo state machine 4 trạng thái:
 *
 *   OFFLINE ──5s──► WAIT_CONNACK ──► WAIT_SUBACK ──► ACTIVE
 *      ▲                                                │
 *      └──────────── mất kết nối / timeout ────────────┘
 *
 *   - OFFLINE      : Chờ 5 giây rồi mở socket TCP và gửi CONNECT.
 *   - WAIT_CONNACK : Chờ gói CONNACK từ broker (timeout 5s).
 *   - WAIT_SUBACK  : Gửi SUBSCRIBE, chờ SUBACK (timeout 5s).
 *   - ACTIVE       : Nhận PUBLISH, gửi PINGREQ định kỳ (keep-alive).
 *
 * Tất cả xử lý không blocking — gọi MQTT_Task() trong main loop mỗi 10ms.
 *
 * Cấu hình (định nghĩa trong mqtt_client.h):
 *   MQTT_BROKER_IP    — IP broker (mảng 4 byte)
 *   MQTT_BROKER_PORT  — Port broker (1883)
 *   MQTT_CLIENT_ID    — Client ID
 *   MQTT_TOPIC_CMD    — Topic subscribe nhận lệnh
 *   MQTT_KEEPALIVE_S  — Thời gian keep-alive (giây)
 *   MQTT_SOCKET_NUM   — Socket W5500 dùng cho MQTT (0-7)
 *
 * Payload hỗ trợ 2 format:
 *   JSON : {"type":"DOOR","action":"OPEN"}
 *   TEXT : "OPEN" hoặc "SET_PWD:111111"
 */

/**
 * @file  mqtt_client.c
 * @brief MQTT 3.1.1 client — W5500 socket, JSON payload, single topic
 */

#include "mqtt_client.h"
#include "socket.h"
#include "wizchip_conf.h"
#include "W5500/w5500.h"
#include "string.h"
#include "main.h"

/* ---- MQTT packet type bytes -------------------------------------------- */
#define MQTTB_CONNECT    0x10U
#define MQTTB_CONNACK    0x20U
#define MQTTB_PUBLISH    0x30U
#define MQTTB_SUBSCRIBE  0x82U
#define MQTTB_SUBACK     0x90U
#define MQTTB_PINGREQ    0xC0U

/* ---- State machine -------------------------------------------------------- */
typedef enum {
    MQTT_ST_OFFLINE      = 0,
    MQTT_ST_WAIT_CONNACK = 1,
    MQTT_ST_WAIT_SUBACK  = 2,
    MQTT_ST_ACTIVE       = 3
} MQTT_State_t;

static MQTT_State_t s_state      = MQTT_ST_OFFLINE;
static uint32_t     s_timer      = 0U;
static uint32_t     s_ping_timer = 0U;

static MQTT_DoorOpenCb s_door_cb = NULL;
static MQTT_PasswordCb s_pwd_cb  = NULL;
static MQTT_MessageCb  s_msg_cb  = NULL;

static uint8_t s_rx[256];
static uint8_t s_tx[128];

/* =========================================================================
 * Minimal JSON string-field extractor
 *   Tìm  "key" : "value"  trong chuỗi json và copy value vào out[0..max-1].
 *   Trả về độ dài value, 0 nếu không tìm thấy.
 * ========================================================================= */
static uint8_t json_str(const char *json, const char *key,
                        char *out, uint8_t max)
{
    /* search pattern: "key" */
    char pat[36];
    uint8_t kl = (uint8_t)strlen(key);
    if (kl > 33U || max == 0U) { out[0] = '\0'; return 0U; }
    pat[0] = '"';
    memcpy(&pat[1], key, kl);
    pat[kl + 1U] = '"';
    pat[kl + 2U] = '\0';

    const char *p = strstr(json, pat);
    if (p == NULL) { out[0] = '\0'; return 0U; }

    p += kl + 2U;  /* skip "key" */
    /* Skip to the colon, then skip to the opening quote of the value */
    while (*p && *p != ':') p++;  /* find colon */
    if (*p != ':') { out[0] = '\0'; return 0U; }
    p++;  /* skip colon */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;  /* skip whitespace */
    if (*p != '"') { out[0] = '\0'; return 0U; }
    p++;  /* skip opening quote */

    uint8_t len = 0U;
    while (*p && *p != '"' && len < (max - 1U)) { out[len++] = *p++; }
    out[len] = '\0';
    return len;
}

/* =========================================================================
 * Packet builders
 * ========================================================================= */
static uint16_t put_str(uint8_t *buf, uint16_t pos, const char *s, uint8_t l)
{
    buf[pos++] = 0U; buf[pos++] = l;
    memcpy(&buf[pos], s, l);
    return pos + (uint16_t)l;
}

static uint16_t build_connect(void)
{
    const char *id  = MQTT_CLIENT_ID;
    uint8_t     il  = (uint8_t)strlen(id);
    uint16_t    p   = 0U;
    s_tx[p++] = MQTTB_CONNECT;
    s_tx[p++] = 10U + 2U + il;
    s_tx[p++] = 0U; s_tx[p++] = 4U;
    s_tx[p++] = 'M'; s_tx[p++] = 'Q'; s_tx[p++] = 'T'; s_tx[p++] = 'T';
    s_tx[p++] = 0x04U;                      /* protocol level 3.1.1  */
    s_tx[p++] = 0x02U;                      /* clean session         */
    s_tx[p++] = (uint8_t)(MQTT_KEEPALIVE_S >> 8);
    s_tx[p++] = (uint8_t)(MQTT_KEEPALIVE_S);
    p = put_str(s_tx, p, id, il);
    return p;
}

static uint16_t build_subscribe(void)
{
    const char *t  = MQTT_TOPIC_CMD;
    uint8_t     tl = (uint8_t)strlen(t);
    uint16_t    p  = 0U;
    s_tx[p++] = MQTTB_SUBSCRIBE;
    s_tx[p++] = 2U + 2U + tl + 1U;         /* pkt_id + len + topic + QoS */
    s_tx[p++] = 0U; s_tx[p++] = 1U;        /* packet ID = 1              */
    p = put_str(s_tx, p, t, tl);
    s_tx[p++] = 0U;                         /* QoS 0                      */
    return p;
}

/* =========================================================================
 * PUBLISH dispatcher — parse JSON, call callbacks
 * ========================================================================= */
static void handle_publish(uint16_t pkt_len)
{
    if (pkt_len < 4U) { return; }

    /* --- Extract topic --------------------------------------------------- */
    uint8_t  rem       = s_rx[1];
    uint16_t topic_len = ((uint16_t)s_rx[2] << 8) | s_rx[3];
    if ((uint16_t)(2U + rem) > pkt_len)    { return; }
    if (4U + topic_len > pkt_len)          { return; }

    /* --- Extract payload (QoS 0: immediately after topic) ---------------- */
    uint16_t pay_off = 4U + topic_len;
    uint16_t pay_len = (uint16_t)rem - 2U - topic_len;
    if (pay_len > MQTT_PAYLOAD_MAX) { pay_len = MQTT_PAYLOAD_MAX; }

    char payload[MQTT_PAYLOAD_MAX + 1U];
    memset(payload, 0, sizeof(payload));  /* Initialize to zeros */
    if (pay_len > 0U && (pay_off + pay_len) <= pkt_len) {
        memcpy(payload, &s_rx[pay_off], pay_len);
        payload[pay_len] = '\0';
    } else {
        /* Invalid payload - skip processing */
        return;
    }

    /* --- Parse JSON fields ------------------------------------------------ */
    char type[16]  = {0};
    char action[16] = {0};
    char value[MQTT_PAYLOAD_MAX + 1U] = {0};

    json_str(payload, "type",   type,   sizeof(type));
    json_str(payload, "action", action, sizeof(action));
    json_str(payload, "value",  value,  sizeof(value));

    /* If JSON parsing failed (no type/action), treat payload as raw text */
    if (type[0] == '\0' && action[0] == '\0') {
        /* Copy raw payload to value for text command processing */
        strncpy(value, payload, MQTT_PAYLOAD_MAX);
        value[MQTT_PAYLOAD_MAX] = '\0';
    }

    /* --- Message display callback (hiển thị payload thô + parsed fields) -- */
    if (s_msg_cb != NULL) { s_msg_cb(type, action, value); }
    /* DEBUG: also log raw payload via door callback if available */
    // Uncomment below to see raw payload on OLED (use MQTT_OnMessage for both)

    /* --- Door-control actions -------------------------------------------- */
    if (strcmp(type, MQTT_TYPE_DOOR) == 0) {
        if (strcmp(action, MQTT_ACT_OPEN) == 0) {
            if (s_door_cb != NULL) { s_door_cb(); }
        } else if (strcmp(action, MQTT_ACT_SET_PWD) == 0 && value[0] != '\0') {
            if (s_pwd_cb  != NULL) { s_pwd_cb(value); }
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */
/**
 * @brief  Khởi tạo MQTT client, đăng ký 3 callback xử lý sự kiện.
 * @detail Lưu các callback vào biến static, đặt trạng thái về OFFLINE.
 *         Phải gọi trước MQTT_Task() trong main loop.
 * @param  door_cb  Callback khi nhận lệnh mở cửa (type=DOOR, action=OPEN).
 * @param  pwd_cb   Callback khi nhận lệnh đổi mật khẩu (action=SET_PWD).
 * @param  msg_cb   Callback hiển thị MỌI message MQTT lên OLED.
 */
void MQTT_Init(MQTT_DoorOpenCb door_cb,
               MQTT_PasswordCb pwd_cb,
               MQTT_MessageCb  msg_cb)
{
    s_door_cb = door_cb;
    s_pwd_cb  = pwd_cb;
    s_msg_cb  = msg_cb;
    s_state   = MQTT_ST_OFFLINE;
    s_timer   = 0U;
}

/**
 * @brief  Vòng lặp xử lý MQTT — gọi liên tục trong main loop (mỗi ~10ms).
 * @detail State machine không blocking:
 *
 *   OFFLINE:
 *     - Chờ 5 giây, mở socket TCP (Sn_MR_TCP).
 *     - Kết nối tới broker_ip:MQTT_BROKER_PORT.
 *     - Gửi gói CONNECT, chuyển sang WAIT_CONNACK.
 *
 *   WAIT_CONNACK:
 *     - Kiểm tra socket còn ESTABLISHED và chưa timeout 5s.
 *     - Đợi ít nhất 4 byte trong buffer recv.
 *     - Nhận CONNACK (0x20), kiểm tra return code = 0x00.
 *     - Gửi SUBSCRIBE, chuyển sang WAIT_SUBACK.
 *
 *   WAIT_SUBACK:
 *     - Kiểm tra socket còn ESTABLISHED và chưa timeout 5s.
 *     - Nhận SUBACK (0x90), chuyển sang ACTIVE.
 *
 *   ACTIVE:
 *     - Kiểm tra socket còn ESTABLISHED.
 *     - Gửi PINGREQ (0xC0) mỗi MQTT_KEEPALIVE_S/2 giây.
 *     - Nhận gói dữ liệu, nếu là PUBLISH (0x3x) → gọi handle_publish().
 *     - Nếu recv lỗi → quay về OFFLINE với auto-reconnect.
 */
void MQTT_Task(void)
{
    uint32_t now         = HAL_GetTick();
    uint8_t  sock_st     = 0U;
    uint16_t avail       = 0U;
    int32_t  rlen;
    uint8_t  broker_ip[] = MQTT_BROKER_IP;

    switch (s_state)
{
    /* ============================================================
     * MQTT_ST_OFFLINE
     * Trạng thái mất kết nối / chưa kết nối MQTT broker
     * Nhiệm vụ:
     * - Cứ mỗi 5 giây thử kết nối lại broker
     * - Mở socket TCP
     * - Connect tới MQTT broker
     * - Gửi MQTT CONNECT packet
     * ============================================================ */
    case MQTT_ST_OFFLINE:

        // Nếu chưa đủ 5 giây từ lần thử trước thì không làm gì
        if ((now - s_timer) < 5000U) {
            break;
        }

        // Cập nhật thời điểm bắt đầu thử reconnect
        s_timer = now;

        // Đóng socket cũ nếu còn tồn tại
        close(MQTT_SOCKET_NUM);

        // Tạo socket TCP mới cho MQTT
        // Nếu tạo socket thất bại thì thoát, lần sau thử lại
        if (socket(MQTT_SOCKET_NUM, Sn_MR_TCP, 0U, 0U) != (int8_t)MQTT_SOCKET_NUM) {
            break;
        }

        // Kết nối TCP tới broker MQTT
        // Nếu connect thất bại thì đóng socket và quay lại OFFLINE
        if (connect_W5x00(MQTT_SOCKET_NUM, broker_ip, MQTT_BROKER_PORT) != SOCK_OK) {
            close(MQTT_SOCKET_NUM);
            break;
        }

        // Build gói MQTT CONNECT rồi gửi lên broker
        {
            uint16_t pl = build_connect();
            send(MQTT_SOCKET_NUM, s_tx, pl);
        }

        // Sau khi gửi CONNECT, chuyển sang trạng thái chờ CONNACK
        s_state = MQTT_ST_WAIT_CONNACK;

        // Lưu lại thời điểm bắt đầu chờ CONNACK để timeout
        s_timer = now;
        break;


    /* ============================================================
     * MQTT_ST_WAIT_CONNACK
     * Trạng thái chờ broker phản hồi CONNACK
     * CONNACK nghĩa là broker đã chấp nhận MQTT CONNECT
     * ============================================================ */
    case MQTT_ST_WAIT_CONNACK:

        // Kiểm tra socket TCP còn ESTABLISHED không
        getsockopt(MQTT_SOCKET_NUM, SO_STATUS, &sock_st);

        // Nếu socket mất kết nối hoặc chờ quá 5 giây thì reset về OFFLINE
        if (sock_st != SOCK_ESTABLISHED || (now - s_timer) > 5000U) {
            close(MQTT_SOCKET_NUM);
            s_state = MQTT_ST_OFFLINE;
            s_timer = now;
            break;
        }

        // Kiểm tra có bao nhiêu byte dữ liệu đang chờ nhận
        getsockopt(MQTT_SOCKET_NUM, SO_RECVBUF, &avail);

        // Gói CONNACK tối thiểu 4 byte, chưa đủ thì chờ tiếp
        if (avail < 4U) {
            break;
        }

        // Đọc dữ liệu từ socket
        rlen = recv(MQTT_SOCKET_NUM, s_rx, sizeof(s_rx));

        // Kiểm tra đúng gói CONNACK và return code = 0x00
        // s_rx[0] == MQTTB_CONNACK: đúng loại packet CONNACK
        // s_rx[3] == 0x00: broker chấp nhận kết nối
        if (rlen >= 4 && s_rx[0] == MQTTB_CONNACK && s_rx[3] == 0x00U) {

            // Sau khi connect thành công, build gói SUBSCRIBE
            uint16_t pl = build_subscribe();

            // Gửi yêu cầu subscribe topic command
            send(MQTT_SOCKET_NUM, s_tx, pl);

            // Chuyển sang trạng thái chờ SUBACK
            s_state = MQTT_ST_WAIT_SUBACK;
            s_timer = now;
        }
        else if (rlen > 0) {
            // Có dữ liệu nhưng không phải CONNACK hợp lệ
            // Coi như lỗi MQTT, đóng socket và reconnect lại
            close(MQTT_SOCKET_NUM);
            s_state = MQTT_ST_OFFLINE;
            s_timer = now;
        }
        break;


    /* ============================================================
     * MQTT_ST_WAIT_SUBACK
     * Trạng thái chờ broker phản hồi SUBACK
     * SUBACK nghĩa là broker đã xác nhận subscribe topic thành công
     * ============================================================ */
    case MQTT_ST_WAIT_SUBACK:

        // Kiểm tra socket còn kết nối không
        getsockopt(MQTT_SOCKET_NUM, SO_STATUS, &sock_st);

        // Nếu mất TCP hoặc quá 5 giây chưa có SUBACK thì reconnect
        if (sock_st != SOCK_ESTABLISHED || (now - s_timer) > 5000U) {
            close(MQTT_SOCKET_NUM);
            s_state = MQTT_ST_OFFLINE;
            s_timer = now;
            break;
        }

        // Kiểm tra buffer nhận có dữ liệu chưa
        getsockopt(MQTT_SOCKET_NUM, SO_RECVBUF, &avail);

        // SUBACK tối thiểu cần 2 byte
        if (avail < 2U) {
            break;
        }

        // Nhận dữ liệu từ socket
        rlen = recv(MQTT_SOCKET_NUM, s_rx, sizeof(s_rx));

        // Nếu đúng là packet SUBACK
        if (rlen >= 2 && s_rx[0] == MQTTB_SUBACK) {

            // Subscribe thành công, chuyển sang trạng thái hoạt động chính
            s_state = MQTT_ST_ACTIVE;

            // Khởi tạo timer để gửi PINGREQ định kỳ
            s_ping_timer = now;
        }
        else if (rlen > 0) {
            // Có dữ liệu nhưng không phải SUBACK hợp lệ
            // Reset kết nối
            close(MQTT_SOCKET_NUM);
            s_state = MQTT_ST_OFFLINE;
            s_timer = now;
        }
        break;


    /* ============================================================
     * MQTT_ST_ACTIVE
     * Trạng thái MQTT đã kết nối và subscribe thành công
     * Nhiệm vụ:
     * - Kiểm tra socket còn sống
     * - Gửi PINGREQ giữ kết nối
     * - Nhận message từ broker
     * - Nếu là PUBLISH thì xử lý command
     * ============================================================ */
    case MQTT_ST_ACTIVE:

        // Kiểm tra TCP socket còn ESTABLISHED không
        getsockopt(MQTT_SOCKET_NUM, SO_STATUS, &sock_st);

        // Nếu socket mất kết nối thì quay lại OFFLINE để reconnect
        if (sock_st != SOCK_ESTABLISHED) {
            close(MQTT_SOCKET_NUM);
            s_state = MQTT_ST_OFFLINE;
            s_timer = now;
            break;
        }

        // MQTT keep-alive:
        // Gửi PINGREQ định kỳ để broker biết client vẫn còn sống
        // Ở đây gửi sau một nửa thời gian keepalive
        if ((now - s_ping_timer) >= (uint32_t)(MQTT_KEEPALIVE_S * 500UL)) {
            uint8_t ping[2] = { MQTTB_PINGREQ, 0x00U };

            // Gửi packet PINGREQ gồm 2 byte
            send(MQTT_SOCKET_NUM, ping, 2U);

            // Cập nhật thời điểm gửi ping gần nhất
            s_ping_timer = now;
        }

        // Kiểm tra có dữ liệu MQTT từ broker gửi xuống không
        getsockopt(MQTT_SOCKET_NUM, SO_RECVBUF, &avail);

        if (avail > 0U) {

            // Nếu dữ liệu nhiều hơn buffer thì chỉ đọc tối đa bằng size buffer
            if (avail > (uint16_t)sizeof(s_rx)) {
                avail = (uint16_t)sizeof(s_rx);
            }

            // Nhận dữ liệu từ socket
            rlen = recv(MQTT_SOCKET_NUM, s_rx, avail);

            // Nếu nhận được packet PUBLISH
            // MQTT PUBLISH có high nibble là 0x30
            if (rlen > 0 && (s_rx[0] & 0xF0U) == MQTTB_PUBLISH) {

                // Xử lý nội dung message MQTT
                // Ví dụ: OPEN, SET_PWD:111111
                handle_publish((uint16_t)rlen);
            }
            else if (rlen < 0) {
                // Lỗi khi recv thì đóng socket và reconnect
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE;
                s_timer = now;
            }
        }
        break;


    /* ============================================================
     * Trường hợp state bị sai giá trị
     * Reset về OFFLINE cho an toàn
     * ============================================================ */
    default:
        s_state = MQTT_ST_OFFLINE;
        break;
    }
}

