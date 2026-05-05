/**
 * @file   wizchip_port.c
 * @brief  Lớp port (HAL bridge) kết nối STM32 SPI với chip W5500.
 *
 * @detail File này đóng vai trò "glue layer" giữa HAL STM32 và thư viện WIZnet:
 *   - Cấu hình địa chỉ mạng tĩnh (IP, GW, SN, MAC, DNS).
 *   - Cung cấp các hàm callback SPI (Select/Unselect, Read/Write Byte, Burst).
 *   - W5500_Init(): khởi tạo toàn bộ chip W5500 theo trình tự chuẩn.
 *   - W5500_Diag(): hiển thị thông tin chẩn đoán lên OLED qua log callback.
 *
 * Cấu hình mặc định (chỉnh tại đây):
 *   IP  : 192.168.137.2
 *   GW  : 192.168.137.1
 *   SN  : 255.255.255.0
 *   MAC : AA:BB:CC:DD:EE:FF
 *   DNS : 8.8.8.8
 *   DHCP: Tắt (USE_DHCP = 0)
 *
 * Hardware mapping:
 *   SPI1  (hspi1)  — giao tiếp dữ liệu với W5500
 *   PA4           — W5500 CS (Chip Select, active LOW)
 *   PB0           — W5500 RST (Reset, active LOW)
 */

/*
 * wizchip_port.c
 */

#include "main.h"
#include "wizchip_port.h"
#include "wizchip_conf.h"
#include "string.h"

#define W5500_SPI hspi1
#define USE_DHCP  0


wiz_NetInfo netInfo = {
    .mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    .ip = {192, 168, 137, 2},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 137, 1},
    .dns = {8, 8, 8, 8},
#if USE_DHCP
	.dhcp = NETINFO_DHCP
#else
    .dhcp = NETINFO_STATIC
#endif
};

/*************************************************   NO Changes After This   ***************************************************************/

extern SPI_HandleTypeDef W5500_SPI;

static W5500_LogCallback w5500_log_callback = NULL;

static void W5500_Log(const char *message)
{
    if (w5500_log_callback != NULL) {
        w5500_log_callback(message);
    }
}

// SPI transmit/receive
/**
 * @brief  Kéo chân CS (PA4) xuống LOW để chọn chip W5500.
 * @detail Gọi trước mỗi chuỗi SPI transaction. Được đăng ký vào thư viện
 *         WIZnet qua reg_wizchip_cs_cbfunc().
 */
void W5500_Select(void)   {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); 
}

/**
 * @brief  Kéo chân CS (PA4) lên HIGH để bỏ chọn chip W5500.
 * @detail Gọi sau mỗi chuỗi SPI transaction. Được đăng ký vào thư viện
 *         WIZnet qua reg_wizchip_cs_cbfunc().
 */
void W5500_Unselect(void) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET); 
}

/**
 * @brief  Đọc 1 byte từ W5500 qua SPI (full-duplex).
 * @detail Gửi dummy byte 0xFF, nhận 1 byte dữ liệu từ W5500.
 *         Được đăng ký qua reg_wizchip_spi_cbfunc().
 * @return Byte dữ liệu nhận được từ W5500.
 */
uint8_t W5500_ReadByte(void)
{
    uint8_t rx;
    uint8_t tx = 0xFF;
    HAL_SPI_TransmitReceive(&W5500_SPI, &tx, &rx, 1, HAL_MAX_DELAY);
    return rx;
}

/**
 * @brief  Ghi 1 byte vào W5500 qua SPI (full-duplex).
 * @detail Gửi 1 byte, byte nhận về bị bỏ qua.
 *         Được đăng ký qua reg_wizchip_spi_cbfunc().
 * @param  byte  Byte dữ liệu cần ghi.
 */
void W5500_WriteByte(uint8_t byte)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&W5500_SPI, &byte, &rx, 1, HAL_MAX_DELAY);
}

/**
 * @brief  Đọc nhiều byte liên tiếp từ W5500 qua SPI (burst read).
 * @detail Sử dụng HAL_SPI_Receive() để đọc khối dữ liệu lớn hiệu quả hơn.
 *         Được đăng ký qua reg_wizchip_spiburst_cbfunc().
 * @param  pBuf  Con trỏ buffer nhận dữ liệu.
 * @param  len   Số byte cần đọc.
 */
void W5500_ReadBurst(uint8_t* pBuf, uint16_t len)
{
    HAL_SPI_Receive(&W5500_SPI, pBuf, len, HAL_MAX_DELAY);
}

/**
 * @brief  Ghi nhiều byte liên tiếp vào W5500 qua SPI (burst write).
 * @detail Sử dụng HAL_SPI_Transmit() để ghi khối dữ liệu lớn hiệu quả hơn.
 *         Được đăng ký qua reg_wizchip_spiburst_cbfunc().
 * @param  pBuf  Con trỏ buffer chứa dữ liệu cần ghi.
 * @param  len   Số byte cần ghi.
 */
void W5500_WriteBurst(uint8_t* pBuf, uint16_t len)
{
    HAL_SPI_Transmit(&W5500_SPI, pBuf, len, HAL_MAX_DELAY);
}

/**
 * @brief  Đăng ký hàm callback để ghi log ra OLED.
 * @detail Hàm W5500_Log() nội bộ sẽ gọi callback này mỗi khi cần in thông báo.
 *         Thường truyền vào OLED_LogMessage từ main.c.
 * @param  callback  Con trỏ hàm kiểu void(*)(const char*).
 */
void W5500_SetLogCallback(W5500_LogCallback callback)
{
    w5500_log_callback = callback;
}

/**
 * @brief  Đọc version register của W5500.
 * @detail Giá trị đúng là 0x04. Dùng để xác nhận giao tiếp SPI hoạt động.
 * @return Version byte (0x04 nếu chip W5500 hợp lệ).
 */
uint8_t W5500_GetVersion(void)
{
    return getVERSIONR();
}


#if USE_DHCP
volatile bool ip_assigned = false;
#define DHCP_SOCKET   7  // last available socket

uint8_t DHCP_buffer[548];

void Callback_IPAssigned(void) {
    ip_assigned = true;
}

void Callback_IPConflict(void) {
    ip_assigned = false;
}
#endif

#define DNS_SOCKET	  6  // 2nd last socket
uint8_t DNS_buffer[512];

/**
 * @brief  Khởi tạo toàn bộ chip W5500 theo trình tự chuẩn.
 * @detail Thực hiện các bước:
 *   1. Deassert CS, đảm bảo bus sạch.
 *   2. Reset phần cứng: kéo PB0 LOW 100ms rồi HIGH, chờ 500ms ổn định.
 *   3. Đăng ký 4 callback SPI vào thư viện WIZnet.
 *   4. ctlwizchip(CW_INIT_WIZCHIP): khởi tạo chip, phân bổ 2KB/socket × 8.
 *   5. ctlnetwork(CN_SET_NETINFO): nạp địa chỉ mạng tĩnh vào chip.
 *   6. Test CS: kéo PA4 LOW 2s để kiểm tra LED (debug).
 *   7. Đọc version, so sánh với 0x04.
 *   8. Đọc trạng thái PHY link.
 * @return  0  thành công.
 *         -1  lỗi khởi tạo chip (ctlwizchip thất bại).
 *         -2  version không khớp (không phải W5500).
 *         -3  không đọc được PHY link.
 */
int W5500_Init(void)
{
    uint8_t memsize[2][8] = {{2,2,2,2,2,2,2,2},{2,2,2,2,2,2,2,2}};
    uint8_t version;
    uint8_t link = PHY_LINK_OFF;

    /***** Deassert CS before reset  *****/
    W5500_Unselect();
    HAL_Delay(10);

    /***** Reset Sequence  *****/
    W5500_Log("W5500 reset");
    // W5500_RST_LOW (PB0)
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    HAL_Delay(100);
    // W5500_RST_HIGH (PB0)
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_Delay(500);

    /***** Register callbacks  *****/
    W5500_Log("Bind SPI1 cb");
    reg_wizchip_cs_cbfunc(W5500_Select, W5500_Unselect);
    reg_wizchip_spi_cbfunc(W5500_ReadByte, W5500_WriteByte);
    reg_wizchip_spiburst_cbfunc(W5500_ReadBurst, W5500_WriteBurst);

    /***** Initialize the chip  *****/
    W5500_Log("Init wizchip");
    if (ctlwizchip(CW_INIT_WIZCHIP, (void*)memsize) == -1){
        W5500_Log("ERR: init chip");
        return -1;
    }
    W5500_Log("Init ok");

    /***** Set static network info (IP, GW, SN, MAC, DNS) *****/
    W5500_Log("Set net info");
    ctlnetwork(CN_SET_NETINFO, (void*)&netInfo);

    /***** check communication by reading Version  *****/
    W5500_Log("Test CS (2s low)");
    /* Kéo CS xuống LOW trong 2s để test LED theo yêu cầu */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); 
    HAL_Delay(2000); // Bạn sẽ thấy LED tắt (nếu dương LED nối vào PA4, âm nối GND) hoặc sáng (nếu dương nối VCC, âm nối PA4) trong 2s
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET); 
    HAL_Delay(100);

    version = W5500_GetVersion();
    W5500_Log((version == 0x04U) ? "Ver: 0x04 OK" : "Ver: unexpected");
    if (version != 0x04U){
        W5500_Log("ERR: bad ver");
        return -2;
    }

    /*****  Check PHY link once for status log  *****/
    if (ctlwizchip(CW_GET_PHYLINK, &link) == -1){
        W5500_Log("ERR: phy read");
        return -3;
    }
    W5500_Log((link == PHY_LINK_ON) ? "Link: UP" : "Link: DOWN");

    return 0;
}

/* -------------------------------------------------------------------------
 * W5500_Diag — chuyển 1 byte uint8 thành chuỗi thập phân
 * ------------------------------------------------------------------------- */
static void u8_to_dec(char *out, uint8_t v)
{
    uint8_t i = 0;
    if (v >= 100U) { out[i++] = (char)('0' + v / 100U); v = (uint8_t)(v % 100U); }
    if (v >= 10U || i > 0U) { out[i++] = (char)('0' + v / 10U); v = (uint8_t)(v % 10U); }
    out[i++] = (char)('0' + v);
    out[i]   = '\0';
}

/* Ghép "prefix" + IP[0].IP[1].IP[2].IP[3] vào buf[0..max-1] */
static void fmt_ip(char *buf, uint8_t max, const char *prefix, const uint8_t *ip)
{
    char tmp[4];
    uint8_t p = 0;
    /* copy prefix */
    while (*prefix && p < max - 1U) { buf[p++] = *prefix++; }
    /* append each octet */
    for (uint8_t o = 0; o < 4U; o++) {
        u8_to_dec(tmp, ip[o]);
        for (uint8_t k = 0; tmp[k] && p < max - 1U; k++) { buf[p++] = tmp[k]; }
        if (o < 3U && p < max - 1U) { buf[p++] = '.'; }
    }
    buf[p] = '\0';
}

/**
 * @brief  Hiển thị thông tin chẩn đoán W5500 lên OLED qua log callback.
 * @detail In ra 4 dòng thông tin:
 *   - "W5500 v:04 OK" hoặc "W5500 v:XX ERR" (version).
 *   - "PHY: UP" hoặc "PHY: DOWN" (trạng thái kết nối vật lý).
 *   - "IP:192.168.x.x" (địa chỉ IP đã cấu hình).
 *   - "GW:192.168.x.x" (default gateway).
 *         Thường gọi sau W5500_Init() để xác nhận kết nối.
 */
void W5500_Diag(void)
{
    char     line[24];
    uint8_t  ver  = W5500_GetVersion();
    uint8_t  link = PHY_LINK_OFF;

    /* ---- Version ---- */
    if (ver == 0x04U) {
        W5500_Log("W5500 v:04 OK");
    } else {
        line[0] = '\0';
        /* manually build "W5500 v:XX ERR" */
        memcpy(line, "W5500 v:", 8);
        u8_to_dec(line + 8, ver);
        memcpy(line + 8 + strlen(line + 8), " ERR", 5);
        W5500_Log(line);
    }

    /* ---- PHY link ---- */
    if (ctlwizchip(CW_GET_PHYLINK, &link) == 0) {
        W5500_Log((link == PHY_LINK_ON) ? "PHY: UP" : "PHY: DOWN");
    }

    /* ---- Network info (dùng struct netInfo đã cấu hình) ---- */
    fmt_ip(line, sizeof(line), "IP:", netInfo.ip);
    W5500_Log(line);

    fmt_ip(line, sizeof(line), "GW:", netInfo.gw);
    W5500_Log(line);
}

