# 🔐 Luồng Hoạt Động Hệ Thống Door Lock

## 📋 Tổng Quan

Hệ thống khoá cửa thông minh dùng STM32F103C8 (Blue Pill) điều khiển:
- **Keypad 4x4** — nhập mật khẩu cục bộ
- **OLED SSD1306** — hiển thị trạng thái
- **W5500 Ethernet** — kết nối MQTT broker
- **Relay + Electric Lock** — điều khiển cơ cấu khoá

---

## 🧩 Sơ Đồ Khối Phần Cứng

```
┌─────────────────────────────────────────────────────────────────┐
│                      STM32F103C8 (Blue Pill)                    │
│                         (ARM Cortex-M3)                         │
│                                                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐         │
│  │   Keypad     │  │   W5500      │  │   OLED       │         │
│  │   Scanner    │  │   Ethernet   │  │  SSD1306     │         │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘         │
│         │                  │                  │                 │
│         ▼                  ▼                  ▼                 │
│    PA0-PA3 (rows)    SPI1 (MOSI/MISO)   SPI2 (MOSI/MISO)      │
│    PA8,PA9,PB5,PA11  PA4 (CS)            PB12 (CS)             │
│    (cols)            PB0 (RST)           PB14 (DC)             │
│                                          PA12 (RST)             │
│         │                  │                  │                 │
│         └────────┬─────────┴──────────────────┘                │
│                  ▼                                              │
│          ┌──────────────┐                                       │
│          │  Door Lock   │                                       │
│          │   Control    │                                       │
│          └──────┬───────┘                                       │
│                 │                                                │
│                 ▼                                                │
│         PB11 (Relay)                                            │
│         PB4  (Electric Lock)                                    │
└─────────────────────────────────────────────────────────────────┘
         │                                        │
         ▼                                        ▼
   ┌─────────┐                              ┌─────────┐
   │  Relay  │                              │E-Lock   │
   │ Module  │                              │12V DC   │
   └─────────┘                              └─────────┘
```

---

## 📊 Luồng Dữ Liệu Chính

### 1️⃣ **Luồng Nhập Mật Khẩu Từ Keypad**

```
┌───────────────┐
│  User nhấn    │
│  phím 4x4     │
└───────┬───────┘
        │
        ▼
┌─────────────────────────────────────┐
│ Keypad_Scan()                       │
│ - Quét 4 rows × 4 cols              │
│ - Debounce 20ms                     │
│ - Trả về ký tự: 0-9, *, #, A-D     │
└───────┬─────────────────────────────┘
        │ char key
        ▼
┌─────────────────────────────────────┐
│ DoorLock_ProcessKey(key)            │
│ Switch-case xử lý:                  │
│  ├─ 0-9: thêm vào s_input[]         │
│  ├─ '*' (CONFIRM): so sánh password│
│  ├─ '#' (CLEAR): xóa input         │
│  └─ A,B,C,D: bỏ qua                 │
└───────┬─────────────────────────────┘
        │
        ├──❌ SAI ────────────────────┐
        │                              │
        │  s_fail_count++              │
        │  ├─ < 5 lần: hiển thị tries  │
        │  └─ ≥ 5 lần: LOCK 30s        │
        │      └─ Đếm ngược trên OLED  │
        │                              │
        └──✅ ĐÚNG ───────────────────┤
                                       │
                                       ▼
                            ┌──────────────────┐
                            │ DOOR_OPEN()      │
                            │ - PB11 = LOW     │
                            │ - PB4  = HIGH    │
                            │ Delay 3 giây     │
                            │ DOOR_CLOSE()     │
                            └──────────────────┘
                                       │
                                       ▼
                            ┌──────────────────┐
                            │ Khoá vật lý mở   │
                            └──────────────────┘
```

**Chi tiết:**
- **Input**: Phím vật lý → GPIO PA0-PA3, PA8-PA11, PB5
- **Processing**: State machine trong `DoorLock_ProcessKey()`
- **Output**: GPIO PB11 (relay), PB4 (electric lock)
- **Feedback**: OLED hiển thị input + kết quả

---

### 2️⃣ **Luồng Điều Khiển Qua MQTT**

```
┌─────────────────────────────────────┐
│ MQTT Broker (Mosquitto)             │
│ 192.168.137.1:1883                  │
│ Topic: device/stm32-001/command     │
└───────┬─────────────────────────────┘
        │ Publish message
        │ {"type":"DOOR","action":"OPEN"}
        │ hoặc "OPEN" (text)
        ▼
┌─────────────────────────────────────┐
│ W5500 Ethernet Chip                 │
│ - IP: 192.168.137.2                 │
│ - Socket #0: TCP connection         │
│ - Nhận gói MQTT PUBLISH             │
└───────┬─────────────────────────────┘
        │ SPI1 transaction
        ▼
┌─────────────────────────────────────┐
│ MQTT_Task() - State Machine         │
│                                      │
│ OFFLINE ──5s──► WAIT_CONNACK        │
│    │              │                  │
│    └──────────────┴──► WAIT_SUBACK  │
│                            │         │
│                            ▼         │
│                        ACTIVE ◄──┐   │
│                          │        │   │
│                     recv PUBLISH │   │
│                          │        │   │
│                          └────────┘   │
└───────┬─────────────────────────────┘
        │ handle_publish()
        ▼
┌─────────────────────────────────────┐
│ json_str() - Parse JSON             │
│ Trích xuất:                         │
│  - type   = "DOOR"                  │
│  - action = "OPEN" / "SET_PWD"      │
│  - value  = "111111" (nếu có)       │
│                                      │
│ Hoặc nhận diện text command:        │
│  - "OPEN"                           │
│  - "SET_PWD:111111"                 │
└───────┬─────────────────────────────┘
        │
        ├── action = "OPEN" ────────────┐
        │                                │
        └── action = "SET_PWD" ─────┐   │
                                    │   │
                                    ▼   ▼
                        ┌──────────────────────┐
                        │ MQTT_OnPassword()    │
                        │ DoorLock_SetPassword │
                        └──────────────────────┘
                                    │
                                    ▼
                        ┌──────────────────────┐
                        │ MQTT_OnDoorOpen()    │
                        │ DOOR_OPEN() 3s       │
                        └──────────────────────┘
                                    │
                                    ▼
                        ┌──────────────────────┐
                        │ Khoá vật lý mở       │
                        └──────────────────────┘
```

**Chi tiết:**
- **Input**: Ethernet RJ45 → W5500 → SPI1 → STM32
- **Protocol Stack**: TCP/IP (W5500) → MQTT 3.1.1 (software)
- **Processing**: Non-blocking state machine `MQTT_Task()`
- **Output**: Callback `MQTT_OnDoorOpen()` hoặc `MQTT_OnPassword()`

---

### 3️⃣ **Luồng Hiển Thị OLED**

```
┌─────────────────────────────────────┐
│ Mọi event trong hệ thống:           │
│ - Keypad nhấn phím                  │
│ - MQTT nhận message                 │
│ - W5500 init/diag                   │
│ - Door lock state change            │
└───────┬─────────────────────────────┘
        │ Gọi OLED_LogMessage()
        ▼
┌─────────────────────────────────────┐
│ OLED_LogMessage(msg)                │
│ - Tự động ngắt dòng khi gặp \n      │
│ - Ngắt dòng khi đủ 18 ký tự         │
│ - Push từng chunk vào buffer        │
└───────┬─────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────┐
│ OLED_LogPushLine(text)              │
│ - Scroll buffer: dòng 0-4 dịch lên  │
│ - Ghi text mới vào dòng 5           │
└───────┬─────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────┐
│ OLED_LogRender()                    │
│ - ssd1306_Fill(Black)               │
│ - Vẽ 6 dòng text với Font_7x10      │
│ - ssd1306_UpdateScreen()            │
└───────┬─────────────────────────────┘
        │ SPI2 transaction
        ▼
┌─────────────────────────────────────┐
│ OLED SSD1306 (128x64 px)            │
│ - Hiển thị text scrolling           │
│ - 6 dòng × 18 ký tự                 │
└─────────────────────────────────────┘
```

**Chi tiết:**
- **Input**: String từ mọi module
- **Buffer**: `oled_lines[6][19]` — ring buffer
- **Output**: SPI2 → OLED controller
- **Render**: Toàn màn hình (không partial update)

---

## 🔄 State Machine Chính

### **MQTT Connection State**

```
┌─────────────────────────────────────────────────────────┐
│                   MQTT State Machine                    │
└─────────────────────────────────────────────────────────┘

  ┌─────────────┐
  │   OFFLINE   │ ◄────────────────────────┐
  └──────┬──────┘                          │
         │ 5 giây delay                    │
         │ socket() + connect()            │
         │ send CONNECT                    │ timeout/
         ▼                                 │ error
  ┌─────────────┐                          │
  │WAIT_CONNACK │ ─────────────────────────┤
  └──────┬──────┘   timeout 5s             │
         │ recv CONNACK (0x20)             │
         │ return code = 0x00              │
         │ send SUBSCRIBE                  │
         ▼                                 │
  ┌─────────────┐                          │
  │WAIT_SUBACK  │ ─────────────────────────┤
  └──────┬──────┘   timeout 5s             │
         │ recv SUBACK (0x90)              │
         ▼                                 │
  ┌─────────────┐                          │
  │   ACTIVE    │ ─────────────────────────┘
  └──────┬──────┘   link lost
         │
         │ ┌─ recv PUBLISH → handle_publish()
         │ │
         │ └─ Mỗi KEEPALIVE/2 giây → send PINGREQ
         │
         └─ Loop...
```

### **Door Lock Input State**

```
┌─────────────────────────────────────────────────────────┐
│              Door Lock Input Processing                 │
└─────────────────────────────────────────────────────────┘

  s_input[] = ""
  s_fail_count = 0

  User nhấn phím
       │
       ├─ 0-9 ──► s_input += key
       │          Display: "******"
       │
       ├─ '#' ──► s_input = ""
       │          Display: "Cleared"
       │
       └─ '*' ──► Compare s_input vs s_password
                      │
                      ├─ MATCH ──────────────┐
                      │                      │
                      │  s_fail_count = 0    │
                      │  DOOR_OPEN() 3s      │
                      │  Display: "ACCESS OK"│
                      │                      │
                      └─ NO MATCH ───────────┤
                                             │
                         s_fail_count++      │
                              │              │
                         ┌────┴────┐         │
                         │ < 5     │ ≥ 5     │
                         ▼         ▼         │
                    "Tries    LOCKOUT       │
                    left: N"  30 giây       │
                              đếm ngược     │
                              reset fail    │
                                             │
                         ┌───────────────────┘
                         ▼
                    s_input = ""
                    Quay về chờ input
```

---

## 🔌 Mapping GPIO

### **Input Pins**

| Pin | Chức năng | Mode | Pull | Ghi chú |
|-----|-----------|------|------|---------|
| PA8 | Keypad COL0 | INPUT | PULLUP | Phím 1,4,7,* |
| PA9 | Keypad COL1 | INPUT | PULLUP | Phím 2,5,8,0 |
| PB5 | Keypad COL2 | INPUT | PULLUP | Phím 3,6,9,# |
| PA11 | Keypad COL3 | INPUT | PULLUP | Phím A,B,C,D |

### **Output Pins**

| Pin | Chức năng | Mode | Init State | Active State |
|-----|-----------|------|------------|--------------|
| PA0 | Keypad ROW0 | OUTPUT_PP | HIGH | LOW (scan) |
| PA1 | Keypad ROW1 | OUTPUT_PP | HIGH | LOW (scan) |
| PA2 | Keypad ROW2 | OUTPUT_PP | HIGH | LOW (scan) |
| PA3 | Keypad ROW3 | OUTPUT_PP | HIGH | LOW (scan) |
| PB11 | Relay | OUTPUT_PP | HIGH (locked) | LOW (open) |
| PB4 | Electric Lock | OUTPUT_PP | LOW (locked) | HIGH (open) |

### **SPI1 - W5500**

| Pin | Chức năng | Mode |
|-----|-----------|------|
| PA5 | SPI1_SCK | AF_PP |
| PA6 | SPI1_MISO | INPUT_FLOATING |
| PA7 | SPI1_MOSI | AF_PP |
| PA4 | W5500 CS | OUTPUT_PP |
| PB0 | W5500 RST | OUTPUT_PP |

### **SPI2 - OLED**

| Pin | Chức năng | Mode |
|-----|-----------|------|
| PB13 | SPI2_SCK | AF_PP |
| PB14 | SPI2_MISO | INPUT_FLOATING |
| PB15 | SPI2_MOSI | AF_PP |
| PB12 | OLED CS | OUTPUT_PP |
| PB14 | OLED DC | OUTPUT_PP |
| PA12 | OLED RST | OUTPUT_PP |

---

## ⏱️ Timing Diagram

### **Keypad Scan Cycle**

```
Total: ~17ms per full scan (4 rows × ~4ms/row)

ROW0 ──┐     ┌──────────────────────────
       └─────┘
       ◄─1ms─►◄─── 20ms debounce (if key pressed)

COL0   ────────┐                    ┌────
               └────────────────────┘
                    ◄─ detected ─►
```

### **MQTT Keep-Alive**

```
KEEPALIVE_S = 60 giây
Ping interval = 60 / 2 = 30 giây

Time: 0s        30s       60s       90s
      │         │         │         │
      CONNECT   PINGREQ   PINGREQ   PINGREQ
      │         │         │         │
      └─ OK ────┴─ OK ────┴─ OK ────┴─ OK ...
```

### **Main Loop**

```
while(1) {
    Keypad_Scan()     // ~1-20ms (nếu có phím nhấn)
    DoorLock_ProcessKey()  // < 1ms
    MQTT_Task()       // < 5ms (no blocking)
    HAL_Delay(10)     // 10ms fixed delay
}
// Total loop time: ~11-35ms (depends on key press)
```

---

## 📦 Module Dependencies

```
main.c
  ├─► ssd1306.h         (OLED driver)
  ├─► wizchip_port.h    (W5500 HAL bridge)
  └─► mqtt_client.h     (MQTT protocol)

wizchip_port.c
  ├─► wizchip_conf.h    (WIZnet library)
  ├─► socket.h          (TCP/IP stack)
  └─► HAL SPI1

mqtt_client.c
  ├─► socket.h          (W5500 socket API)
  ├─► wizchip_conf.h
  └─► wizchip_port.h    (không trực tiếp, qua socket)

ssd1306.c
  └─► HAL SPI2
```

---

## 🔐 Security Flow

### **Password Validation**

```
Input: "111111*"
       │
       ├─ Parse: s_input = "111111"
       │
       ▼
   strcmp(s_input, s_password)
       │
       ├─ EQUAL ──► Access Granted
       │              └─► Door opens 3s
       │
       └─ NOT EQUAL ──► s_fail_count++
                          │
                          ├─ fail < 5 ──► Show "Tries left: N"
                          │
                          └─ fail ≥ 5 ──► LOCKOUT 30s
                                           └─► Countdown on OLED
                                               └─► Reset fail_count = 0
```

### **MQTT Command Validation**

```
Nhận payload: {"type":"DOOR","action":"OPEN"}
       │
       ▼
   json_str() parse
       │
       ├─ type = "DOOR" ? ──✅──► Continue
       │                    ❌──► Ignore
       │
       ├─ action = "OPEN" ? ──✅──► MQTT_OnDoorOpen()
       │                      ❌──► Check other actions
       │
       └─ action = "SET_PWD" + value present?
              └─► MQTT_OnPassword(value)

⚠️ Lưu ý: Không có authentication MQTT!
         Bất kỳ ai publish vào topic đều điều khiển được.
```

---

## 📝 Data Structures

### **OLED Buffer**

```c
static char oled_lines[6][19];  // 6 dòng × 18 ký tự + null
static uint8_t oled_ready = 0;  // flag init hoàn tất
```

### **Password State**

```c
static char s_password[17] = "111111";  // mật khẩu đúng
static char s_input[17];                // input từ user
static uint8_t s_input_len = 0;         // độ dài hiện tại
static uint8_t s_fail_count = 0;        // số lần sai (max 5)
```

### **MQTT Buffers**

```c
static uint8_t s_rx[256];  // Buffer nhận MQTT
static uint8_t s_tx[128];  // Buffer gửi MQTT
static MQTT_State_t s_state = OFFLINE;
static uint32_t s_timer = 0;
static uint32_t s_ping_timer = 0;
```

### **Network Config**

```c
wiz_NetInfo netInfo = {
    .mac  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    .ip   = {192, 168, 137, 2},
    .sn   = {255, 255, 255, 0},
    .gw   = {192, 168, 137, 1},
    .dns  = {8, 8, 8, 8},
    .dhcp = NETINFO_STATIC
};
```

---

## 🚀 Boot Sequence

```
1. HAL_Init()
2. SystemClock_Config() ──► 72 MHz (PLL from 8 MHz HSE)
3. MX_GPIO_Init()
   ├─ Enable AFIO clock
   ├─ Remap JTAG (giải phóng PB4)
   └─ Init all GPIO pins
4. MX_SPI1_Init() ──► W5500
5. MX_SPI2_Init() ──► OLED
6. ssd1306_Init() ──► OLED ready
7. W5500_Init()
   ├─ Reset W5500 (PB0 toggle)
   ├─ Register SPI callbacks
   ├─ ctlwizchip(INIT)
   ├─ ctlnetwork(SET_NETINFO)
   ├─ Verify version = 0x04
   └─ Check PHY link
8. W5500_Diag() ──► Display IP/GW on OLED (3s)
9. Keypad_Init()
10. DoorLock_Init()
    ├─ DOOR_CLOSE()
    ├─ Self-test: xung PB4/PB11 1s
    └─ Display "Enter password"
11. MQTT_Init(callbacks)
12. while(1) main loop
```

---

## 🧪 Test Points

### **Hardware Test**

1. **LED PA4 Test**: Khi boot, PA4 kéo LOW 2s → LED sáng/tắt
2. **Self-Test**: PB4=HIGH + PB11=LOW trong 1s → relay click
3. **Keypad**: Nhấn phím `1` → OLED hiển thị `1`
4. **OLED**: Boot hiển thị "System starting"

### **Software Test**

1. **Password**: Nhập `111111*` → cửa mở
2. **Fail Lockout**: Nhập sai 5 lần → khoá 30s
3. **MQTT Text**: Gửi `"OPEN"` → cửa mở
4. **MQTT JSON**: Gửi `{"type":"DOOR","action":"OPEN"}` → cửa mở
5. **Change PWD**: Gửi `SET_PWD:999999` → mật khẩu đổi

### **Network Test**

```bash
# Ping STM32
ping 192.168.137.2

# MQTT publish (text format)
mosquitto_pub -h 192.168.137.1 -t device/stm32-001/command -m "OPEN"

# MQTT publish (JSON format)
mosquitto_pub -h 192.168.137.1 -t device/stm32-001/command \
  -m '{"type":"DOOR","action":"OPEN"}'
```

---

## 📌 Kết Luận

### **Input Sources (3 nguồn)**

1. **Keypad 4x4** → GPIO scan → Local password authentication
2. **MQTT/Ethernet** → W5500 SPI → Remote command
3. **OLED callback** → Mọi module gọi `OLED_LogMessage()`

### **Processing (State Machines)**

1. **Keypad FSM**: Idle → Input → Verify → Open/Deny → Idle
2. **MQTT FSM**: Offline → Connect → Subscribe → Active → (loop)
3. **Lockout FSM**: Normal → Fail(1-4) → Locked(30s) → Normal

### **Output Targets (3 đích)**

1. **Physical Lock** → PB11 (relay) + PB4 (electric lock)
2. **OLED Display** → SPI2 → Visual feedback
3. **Network** → W5500 → MQTT PINGREQ (keep-alive)

### **Critical Paths**

1. **Fastest**: Keypad → Door (< 50ms nếu đúng password)
2. **Network**: MQTT → Door (< 100ms sau khi nhận PUBLISH)
3. **Slowest**: Self-test boot sequence (~4 giây)

---

**🔗 Xem thêm:**
- `main.c` — Entry point + door logic
- `mqtt_client.c` — MQTT state machine
- `wizchip_port.c` — W5500 hardware abstraction
- `HUONG_DAN_SUA_LOI.md` — Troubleshooting guide

