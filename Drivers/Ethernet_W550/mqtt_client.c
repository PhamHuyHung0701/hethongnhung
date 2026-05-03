/**
 * @file  mqtt_client.c
 * @brief Minimal MQTT 3.1.1 client — W5500 socket backend
 *
 *  State machine:
 *   OFFLINE  ──connect_W5x00()──►  WAIT_CONNACK
 *   WAIT_CONNACK  ──CONNACK OK──►  WAIT_SUBACK
 *   WAIT_SUBACK   ──SUBACK OK──►  ACTIVE
 *   ACTIVE   ──link down/timeout──►  OFFLINE  (auto-reconnect)
 */

#include "mqtt_client.h"
#include "socket.h"
#include "wizchip_conf.h"
#include "W5500/w5500.h"
#include "string.h"
#include "main.h"          /* HAL_GetTick() */

/* ----------------------------------------------------------------
 * MQTT 3.1.1 fixed-header first bytes
 * ---------------------------------------------------------------- */
#define MQTTB_CONNECT     0x10U
#define MQTTB_CONNACK     0x20U
#define MQTTB_PUBLISH     0x30U   /* 0x3x – lower nibble = flags  */
#define MQTTB_SUBSCRIBE   0x82U
#define MQTTB_SUBACK      0x90U
#define MQTTB_PINGREQ     0xC0U
#define MQTTB_PINGRESP    0xD0U

/* ----------------------------------------------------------------
 * State machine
 * ---------------------------------------------------------------- */
typedef enum {
    MQTT_ST_OFFLINE       = 0,  /* not connected, waiting retry   */
    MQTT_ST_WAIT_CONNACK  = 1,  /* TCP up, CONNECT sent           */
    MQTT_ST_WAIT_SUBACK   = 2,  /* CONNACK received, SUB sent     */
    MQTT_ST_ACTIVE        = 3   /* fully connected & subscribed   */
} MQTT_State_t;

static MQTT_State_t s_state       = MQTT_ST_OFFLINE;
static uint32_t     s_timer       = 0U;   /* reconnect / ack timeout  */
static uint32_t     s_ping_timer  = 0U;   /* keep-alive timer         */

static MQTT_DoorOpenCb s_door_cb  = NULL;
static MQTT_PasswordCb s_pwd_cb   = NULL;

/* RX/TX scratch buffers */
static uint8_t s_rx[256];
static uint8_t s_tx[128];

/* ----------------------------------------------------------------
 * Helpers: write 2-byte big-endian string into buffer
 * ---------------------------------------------------------------- */
static uint16_t put_str(uint8_t *buf, uint16_t pos, const char *str, uint8_t len)
{
    buf[pos++] = 0;
    buf[pos++] = len;
    memcpy(&buf[pos], str, len);
    return pos + (uint16_t)len;
}

/* ----------------------------------------------------------------
 * Build MQTT CONNECT packet → s_tx, return length
 * ---------------------------------------------------------------- */
static uint16_t build_connect(void)
{
    const char *id  = MQTT_CLIENT_ID;
    uint8_t     id_len = (uint8_t)strlen(id);
    uint8_t     rem    = 10U + 2U + id_len;   /* variable hdr + payload */
    uint16_t    p = 0;

    s_tx[p++] = MQTTB_CONNECT;
    s_tx[p++] = rem;
    /* Protocol Name "MQTT" */
    s_tx[p++] = 0x00; s_tx[p++] = 0x04;
    s_tx[p++] = 'M'; s_tx[p++] = 'Q'; s_tx[p++] = 'T'; s_tx[p++] = 'T';
    /* Protocol Level 3.1.1 */
    s_tx[p++] = 0x04;
    /* Connect Flags: Clean Session */
    s_tx[p++] = 0x02;
    /* Keep-Alive */
    s_tx[p++] = (uint8_t)(MQTT_KEEPALIVE_S >> 8);
    s_tx[p++] = (uint8_t)(MQTT_KEEPALIVE_S);
    /* Client ID */
    p = put_str(s_tx, p, id, id_len);
    return p;
}

/* ----------------------------------------------------------------
 * Build MQTT SUBSCRIBE for both topics → s_tx, return length
 * ---------------------------------------------------------------- */
static uint16_t build_subscribe(void)
{
    const char *t1  = MQTT_TOPIC_OPEN;
    const char *t2  = MQTT_TOPIC_PASSWORD;
    uint8_t     l1  = (uint8_t)strlen(t1);
    uint8_t     l2  = (uint8_t)strlen(t2);
    /* rem = pkt_id(2) + [len(2)+topic+qos(1)] x2 */
    uint8_t     rem = 2U + (uint8_t)(2U + l1 + 1U) + (uint8_t)(2U + l2 + 1U);
    uint16_t    p   = 0;

    s_tx[p++] = MQTTB_SUBSCRIBE;
    s_tx[p++] = rem;
    s_tx[p++] = 0x00; s_tx[p++] = 0x01;   /* Packet ID = 1 */
    p = put_str(s_tx, p, t1, l1);
    s_tx[p++] = 0x00;                      /* QoS 0 */
    p = put_str(s_tx, p, t2, l2);
    s_tx[p++] = 0x00;                      /* QoS 0 */
    return p;
}

/* ----------------------------------------------------------------
 * Dispatch a PUBLISH packet (already in s_rx[0..len-1])
 * ---------------------------------------------------------------- */
static void handle_publish(uint16_t len)
{
    if (len < 4U) { return; }

    /* s_rx[0] = 0x3x, s_rx[1] = remaining length (single-byte ≤127) */
    uint8_t  rem       = s_rx[1];
    uint16_t topic_len = ((uint16_t)s_rx[2] << 8) | s_rx[3];

    if ((uint16_t)(2U + rem) > len)    { return; }
    if (4U + topic_len > len)          { return; }

    /* Copy topic with null terminator */
    char topic[32];
    uint8_t tl = (topic_len < 31U) ? (uint8_t)topic_len : 31U;
    memcpy(topic, &s_rx[4], tl);
    topic[tl] = '\0';

    /* Payload follows immediately after topic (QoS 0 – no packet-id) */
    uint16_t pay_offset = 4U + topic_len;
    uint8_t  pay_len    = (uint8_t)((uint16_t)rem - 2U - topic_len);
    if (pay_len > MQTT_PAYLOAD_MAX) { pay_len = MQTT_PAYLOAD_MAX; }

    char payload[MQTT_PAYLOAD_MAX + 1U];
    if (pay_len > 0U && (pay_offset + pay_len) <= len) {
        memcpy(payload, &s_rx[pay_offset], pay_len);
    }
    payload[pay_len] = '\0';

    /* Invoke the right callback */
    if (strcmp(topic, MQTT_TOPIC_OPEN) == 0) {
        if (s_door_cb != NULL) { s_door_cb(); }
    } else if (strcmp(topic, MQTT_TOPIC_PASSWORD) == 0 && pay_len > 0U) {
        if (s_pwd_cb  != NULL) { s_pwd_cb(payload); }
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

void MQTT_Init(MQTT_DoorOpenCb door_cb, MQTT_PasswordCb pwd_cb)
{
    s_door_cb = door_cb;
    s_pwd_cb  = pwd_cb;
    s_state   = MQTT_ST_OFFLINE;
    s_timer   = 0U;
}

void MQTT_Task(void)
{
    uint32_t now        = HAL_GetTick();
    uint8_t  sock_st    = 0U;
    uint16_t avail      = 0U;
    int32_t  rlen;
    uint8_t  broker_ip[] = MQTT_BROKER_IP;

    switch (s_state)
    {
        /* --------------------------------------------------------- */
        case MQTT_ST_OFFLINE:
            /* Retry every 5 seconds */
            if ((now - s_timer) < 5000U) { break; }
            s_timer = now;

            close(MQTT_SOCKET_NUM);

            /* Open TCP socket */
            if (socket(MQTT_SOCKET_NUM, Sn_MR_TCP, 0U, 0U) != (int8_t)MQTT_SOCKET_NUM) {
                break;  /* try again next cycle */
            }

            /* Connect to broker (blocking; W5500 built-in ~1.6 s timeout) */
            if (connect_W5x00(MQTT_SOCKET_NUM, broker_ip, MQTT_BROKER_PORT) != SOCK_OK) {
                close(MQTT_SOCKET_NUM);
                break;
            }

            /* TCP established → send MQTT CONNECT */
            {
                uint16_t plen = build_connect();
                send(MQTT_SOCKET_NUM, s_tx, plen);
            }
            s_state = MQTT_ST_WAIT_CONNACK;
            s_timer = now;
            break;

        /* --------------------------------------------------------- */
        case MQTT_ST_WAIT_CONNACK:
            /* Check TCP still up */
            getsockopt(MQTT_SOCKET_NUM, SO_STATUS, &sock_st);
            if (sock_st != SOCK_ESTABLISHED) {
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE; s_timer = now; break;
            }
            /* 5-second ack timeout */
            if ((now - s_timer) > 5000U) {
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE; s_timer = now; break;
            }

            getsockopt(MQTT_SOCKET_NUM, SO_RECVBUF, &avail);
            if (avail < 4U) { break; }

            rlen = recv(MQTT_SOCKET_NUM, s_rx, sizeof(s_rx));
            if (rlen >= 4 && s_rx[0] == MQTTB_CONNACK && s_rx[3] == 0x00U) {
                /* Broker accepted → subscribe */
                uint16_t plen = build_subscribe();
                send(MQTT_SOCKET_NUM, s_tx, plen);
                s_state = MQTT_ST_WAIT_SUBACK;
                s_timer = now;
            } else if (rlen > 0) {
                /* CONNACK refused or wrong packet */
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE; s_timer = now;
            }
            break;

        /* --------------------------------------------------------- */
        case MQTT_ST_WAIT_SUBACK:
            getsockopt(MQTT_SOCKET_NUM, SO_STATUS, &sock_st);
            if (sock_st != SOCK_ESTABLISHED) {
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE; s_timer = now; break;
            }
            if ((now - s_timer) > 5000U) {
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE; s_timer = now; break;
            }

            getsockopt(MQTT_SOCKET_NUM, SO_RECVBUF, &avail);
            if (avail < 2U) { break; }

            rlen = recv(MQTT_SOCKET_NUM, s_rx, sizeof(s_rx));
            if (rlen >= 2 && s_rx[0] == MQTTB_SUBACK) {
                /* Subscribed and ready */
                s_state      = MQTT_ST_ACTIVE;
                s_ping_timer = now;
            } else if (rlen > 0) {
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE; s_timer = now;
            }
            break;

        /* --------------------------------------------------------- */
        case MQTT_ST_ACTIVE:
            getsockopt(MQTT_SOCKET_NUM, SO_STATUS, &sock_st);
            if (sock_st != SOCK_ESTABLISHED) {
                close(MQTT_SOCKET_NUM);
                s_state = MQTT_ST_OFFLINE; s_timer = now; break;
            }

            /* PINGREQ every keepalive/2 to keep session alive */
            if ((now - s_ping_timer) >= (uint32_t)(MQTT_KEEPALIVE_S * 500UL)) {
                uint8_t ping[2] = { MQTTB_PINGREQ, 0x00U };
                send(MQTT_SOCKET_NUM, ping, 2U);
                s_ping_timer = now;
            }

            /* Poll for incoming messages */
            getsockopt(MQTT_SOCKET_NUM, SO_RECVBUF, &avail);
            if (avail > 0U) {
                if (avail > (uint16_t)sizeof(s_rx)) { avail = (uint16_t)sizeof(s_rx); }
                rlen = recv(MQTT_SOCKET_NUM, s_rx, avail);
                if (rlen > 0) {
                    /* Handle each packet in the buffer (simplified: one per recv) */
                    uint8_t ptype = s_rx[0] & 0xF0U;
                    if (ptype == MQTTB_PUBLISH) {
                        handle_publish((uint16_t)rlen);
                    }
                    /* PINGRESP (0xD0) – no action needed */
                } else if (rlen < 0) {
                    close(MQTT_SOCKET_NUM);
                    s_state = MQTT_ST_OFFLINE; s_timer = now;
                }
            }
            break;

        default:
            s_state = MQTT_ST_OFFLINE;
            break;
    }
}

