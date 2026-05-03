/*
 * wizchip_port.c
 */

#include "main.h"
#include "wizchip_port.h"
#include "wizchip_conf.h"

#define W5500_SPI hspi1
#define USE_DHCP  0


wiz_NetInfo netInfo = {
    .mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    .ip = {192, 168, 1, 10},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 1, 1},
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
void W5500_Select(void)   { 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); 
}
void W5500_Unselect(void) { 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET); 
}

uint8_t W5500_ReadByte(void)
{
    uint8_t rx;
    uint8_t tx = 0xFF;
    HAL_SPI_TransmitReceive(&W5500_SPI, &tx, &rx, 1, HAL_MAX_DELAY);
    return rx;
}

void W5500_WriteByte(uint8_t byte)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&W5500_SPI, &byte, &rx, 1, HAL_MAX_DELAY);
}

void W5500_ReadBurst(uint8_t* pBuf, uint16_t len)
{
    HAL_SPI_Receive(&W5500_SPI, pBuf, len, HAL_MAX_DELAY);
}

void W5500_WriteBurst(uint8_t* pBuf, uint16_t len)
{
    HAL_SPI_Transmit(&W5500_SPI, pBuf, len, HAL_MAX_DELAY);
}

void W5500_SetLogCallback(W5500_LogCallback callback)
{
    w5500_log_callback = callback;
}

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
