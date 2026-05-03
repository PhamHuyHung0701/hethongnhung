/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Door lock — keypad 4x4 + OLED + relay + MQTT (hardcoded)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */
#include "string.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"
#include "wizchip_port.h"
#include "mqtt_client.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* Keypad rows  (OUTPUT): PA0, PA1, PA2, PA3  */
/* Keypad cols  (INPUT):  PA8, PA9, PB5, PA11 */
#define KP_ROW0   GPIO_PIN_0
#define KP_ROW1   GPIO_PIN_1
#define KP_ROW2   GPIO_PIN_2
#define KP_ROW3   GPIO_PIN_3
#define KP_COL0   GPIO_PIN_8
#define KP_COL1   GPIO_PIN_9
#define KP_COL2   GPIO_PIN_5      /* Changed from PA10 to PB5 */
#define KP_COL3   GPIO_PIN_11
#define KP_PORT   GPIOA
#define KP_COL2_PORT GPIOB        /* Separate port for COL2 */

/* Relay on PB11 — SET=locked, RESET=unlocked (active-LOW) */
#define RELAY_PORT      PB11_TOGGLE_GPIO_Port
#define RELAY_PIN       PB11_TOGGLE_Pin
#define RELAY_OPEN()    HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, GPIO_PIN_RESET)
#define RELAY_CLOSE()   HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, GPIO_PIN_SET)

/* Electric lock on PB4 — active-HIGH: SET=mở khoá, RESET=khoá */
#define ELOCK_PORT      LOCK_GPIO_Port
#define ELOCK_PIN       LOCK_Pin
#define ELOCK_OPEN()    HAL_GPIO_WritePin(ELOCK_PORT, ELOCK_PIN, GPIO_PIN_SET)
#define ELOCK_CLOSE()   HAL_GPIO_WritePin(ELOCK_PORT, ELOCK_PIN, GPIO_PIN_RESET)

/* Mở đồng thời relay + khoá điện */
#define DOOR_OPEN()     do { RELAY_OPEN();  ELOCK_OPEN();  } while(0)
#define DOOR_CLOSE()    do { RELAY_CLOSE(); ELOCK_CLOSE(); } while(0)

#define LOCK_OPEN_MS    3000U

/* Password */
#define PWD_MAX_LEN     16U

/* OLED log */
#define OLED_LINES      6U
#define OLED_LINE_LEN   18U

/*
 * MQTT topics (defined in mqtt_client.h):
 *   door/open     → mở cửa
 *   door/password → đổi mật khẩu
 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;

/* USER CODE BEGIN PV */

/* OLED state */
static char    oled_lines[OLED_LINES][OLED_LINE_LEN + 1U];
static uint8_t oled_ready = 0U;

/* Door lock state */
static char    s_password[PWD_MAX_LEN + 1U] = "123456";   /* default */
static char    s_input[PWD_MAX_LEN + 1U]    = {0};
static uint8_t s_input_len                  = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_SPI2_Init(void);

/* USER CODE BEGIN PFP */
static void MQTT_OnDoorOpen(void);
static void MQTT_OnPassword(const char *new_pwd);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ===========================================================================
 * OLED log helpers
 * ===========================================================================*/
static void OLED_LogRender(void)
{
    ssd1306_Fill(Black);
    for (uint8_t i = 0U; i < OLED_LINES; i++)
    {
        ssd1306_SetCursor(0, i * 10U);
        ssd1306_WriteString(oled_lines[i], Font_7x10, White);
    }
    ssd1306_UpdateScreen();
}

static void OLED_LogPushLine(const char *text)
{
    for (uint8_t i = 0U; i < (OLED_LINES - 1U); i++)
    {
        memcpy(oled_lines[i], oled_lines[i + 1U], sizeof(oled_lines[i]));
    }
    strncpy(oled_lines[OLED_LINES - 1U], text, OLED_LINE_LEN);
    oled_lines[OLED_LINES - 1U][OLED_LINE_LEN] = '\0';
}

static void OLED_LogMessage(const char *message)
{
    char    chunk[OLED_LINE_LEN + 1U];
    uint8_t len = 0U;

    if (message == NULL) { return; }

    while (*message != '\0')
    {
        if (*message == '\r' || *message == '\n')
        {
            if (len > 0U) { chunk[len] = '\0'; OLED_LogPushLine(chunk); len = 0U; }
            message++;
            continue;
        }
        chunk[len++] = *message++;
        if (len >= OLED_LINE_LEN) { chunk[len] = '\0'; OLED_LogPushLine(chunk); len = 0U; }
    }
    if (len > 0U) { chunk[len] = '\0'; OLED_LogPushLine(chunk); }
    if (oled_ready != 0U) { OLED_LogRender(); }
}

static void OLED_LogClear(void)
{
    memset(oled_lines, 0, sizeof(oled_lines));
    if (oled_ready != 0U) { OLED_LogRender(); }
}


/* ===========================================================================
 * Keypad 4x4
 *
 *        COL0   COL1   COL2   COL3
 *        PA8    PA9    PB5    PA11
 * ROW0  PA0  [ 1 ]  [ 2 ]  [ 3 ]  [ A ]
 * ROW1  PA1  [ 4 ]  [ 5 ]  [ 6 ]  [ B ]
 * ROW2  PA2  [ 7 ]  [ 8 ]  [ 9 ]  [ C ]
 * ROW3  PA3  [ * ]  [ 0 ]  [ # ]  [ D ]
 * ===========================================================================*/
static const uint16_t KP_ROWS[4] = { KP_ROW0, KP_ROW1, KP_ROW2, KP_ROW3 };
static const uint16_t KP_COLS[4] = { KP_COL0, KP_COL1, KP_COL2, KP_COL3 };
static const char     KP_MAP[4][4] = {
    { '1', '2', '3', 'A' },
    { '4', '5', '6', 'B' },
    { '7', '8', '9', 'C' },
    { '*', '0', '#', 'D' }
};

static void Keypad_Init(void)
{
    GPIO_InitTypeDef g = {0};

    /* Rows — output push-pull, idle HIGH */
    g.Pin   = KP_ROW0 | KP_ROW1 | KP_ROW2 | KP_ROW3;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(KP_PORT, &g);
    HAL_GPIO_WritePin(KP_PORT, KP_ROW0 | KP_ROW1 | KP_ROW2 | KP_ROW3, GPIO_PIN_SET);

    /* Cols PA8, PA9, PA11 — input pull-up */
    g.Pin  = KP_COL0 | KP_COL1 | KP_COL3;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KP_PORT, &g);

    /* Col2 PB5 — input pull-up (separate port) */
    g.Pin  = KP_COL2;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KP_COL2_PORT, &g);
}

static char Keypad_Scan(void)
{
    for (uint8_t r = 0U; r < 4U; r++)
    {
        HAL_GPIO_WritePin(KP_PORT, KP_ROWS[r], GPIO_PIN_RESET);
        HAL_Delay(1);
        for (uint8_t c = 0U; c < 4U; c++)
        {
            /* Select correct port for each column */
            GPIO_TypeDef *col_port = (c == 2U) ? KP_COL2_PORT : KP_PORT;

            if (HAL_GPIO_ReadPin(col_port, KP_COLS[c]) == GPIO_PIN_RESET)
            {
                HAL_Delay(20);  /* debounce */
                if (HAL_GPIO_ReadPin(col_port, KP_COLS[c]) == GPIO_PIN_RESET)
                {
                    /* wait for key release */
                    while (HAL_GPIO_ReadPin(col_port, KP_COLS[c]) == GPIO_PIN_RESET)
                    {
                        HAL_Delay(1);
                    }
                    HAL_GPIO_WritePin(KP_PORT, KP_ROWS[r], GPIO_PIN_SET);
                    return KP_MAP[r][c];
                }
            }
        }
        HAL_GPIO_WritePin(KP_PORT, KP_ROWS[r], GPIO_PIN_SET);
    }
    return 0;
}

/* ===========================================================================
 * Door lock logic
 * ===========================================================================*/
static void DoorLock_Init(void)
{
    DOOR_CLOSE();
    OLED_LogClear();
    OLED_LogMessage("== Door Lock ==");
    OLED_LogMessage("Enter password:");
}

static void DoorLock_SetPassword(const char *new_pwd)
{
    size_t len;

    if (new_pwd == NULL) { return; }
    len = strlen(new_pwd);
    if (len == 0U || len > PWD_MAX_LEN)
    {
        OLED_LogMessage("Invalid new pwd");
        return;
    }

    strncpy(s_password, new_pwd, PWD_MAX_LEN);
    s_password[PWD_MAX_LEN] = '\0';

    memset(s_input, 0, sizeof(s_input));
    s_input_len = 0U;

    OLED_LogClear();
    OLED_LogMessage("Password changed");
    HAL_Delay(1000);
    OLED_LogClear();
    OLED_LogMessage("Enter password:");
}

static void DoorLock_ProcessKey(char key)
{
    char stars[PWD_MAX_LEN + 1U];

    if (key == 0) { return; }

    /* '#' — confirm / enter */
    if (key == '#')
    {
        s_input[s_input_len] = '\0';

        if (strcmp(s_input, s_password) == 0)
        {
            OLED_LogClear();
            OLED_LogMessage("** ACCESS OK **");
            OLED_LogMessage("Door opening...");
            DOOR_OPEN();
            HAL_Delay(LOCK_OPEN_MS);
            DOOR_CLOSE();
            OLED_LogClear();
            OLED_LogMessage("Door closed.");
            OLED_LogMessage("Enter password:");
        }
        else
        {
            OLED_LogClear();
            OLED_LogMessage("WRONG PASSWORD!");
            HAL_Delay(1500);
            OLED_LogClear();
            OLED_LogMessage("Enter password:");
        }

        memset(s_input, 0, sizeof(s_input));
        s_input_len = 0U;
        return;
    }

    /* '*' — clear input */
    if (key == '*')
    {
        memset(s_input, 0, sizeof(s_input));
        s_input_len = 0U;
        OLED_LogClear();
        OLED_LogMessage("Cleared.");
        OLED_LogMessage("Enter password:");
        return;
    }

    /* Ignore A, B, C, D */
    if (key < '0' || key > '9') { return; }

    /* Append digit and show asterisks */
    if (s_input_len < PWD_MAX_LEN)
    {
        s_input[s_input_len++] = key;
        for (uint8_t i = 0U; i < s_input_len; i++) { stars[i] = '*'; }
        stars[s_input_len] = '\0';

        OLED_LogClear();
        OLED_LogMessage("Enter password:");
        OLED_LogMessage(stars);
    }
}

/* ===========================================================================
 * MQTT callbacks — called from MQTT_Task() when a message arrives
 * ===========================================================================*/

/** Nhận "door/open" → mở khoá ngay */
static void MQTT_OnDoorOpen(void)
{
    OLED_LogClear();
    OLED_LogMessage("MQTT: Open door");
    DOOR_OPEN();
    HAL_Delay(LOCK_OPEN_MS);
    DOOR_CLOSE();
    OLED_LogClear();
    OLED_LogMessage("Door closed.");
    OLED_LogMessage("Enter password:");
}

/** Nhận "door/password" → đổi mật khẩu sang payload */
static void MQTT_OnPassword(const char *new_pwd)
{
    char line[OLED_LINE_LEN + 1U];

    OLED_LogClear();
    OLED_LogMessage("MQTT: Set pwd");
    line[0] = '\0';
    strncat(line, "New: ", OLED_LINE_LEN);
    strncat(line, new_pwd, OLED_LINE_LEN - 5U);
    OLED_LogMessage(line);
    HAL_Delay(2000);  /* Hiển thị 2s cho dễ đọc */

    DoorLock_SetPassword(new_pwd);
}

/**
 * Gọi cho MỌI message MQTT nhận được — hiển thị lên OLED.
 * Hỗ trợ 2 format:
 *   1. Text: "OPEN" hoặc "SET_PWD:111111"
 *   2. JSON: {"type":"DOOR","action":"OPEN"}
 */
static void MQTT_OnMessage(const char *type, const char *action, const char *value)
{
    char line[OLED_LINE_LEN + 1U];

    OLED_LogClear();
    OLED_LogMessage("--- MQTT msg ---");

    /* Nếu type rỗng → có thể là text format, không phải JSON */
    if (type[0] == '\0' && action[0] == '\0') {
        /* Hiển thị message nhận được */
        OLED_LogMessage("Text mode:");
        if (value[0] != '\0') {
            OLED_LogMessage(value);
        }

        HAL_Delay(1000);  /* Giảm delay để phản hồi nhanh hơn */

        /* Parse text commands */
        if (strcmp(value, "OPEN") == 0) {
            OLED_LogMessage("-> Opening door");
            HAL_Delay(1000);
            MQTT_OnDoorOpen();
            return;
        }

        /* SET_PWD:password format */
        if (strncmp(value, "SET_PWD:", 8) == 0) {
            const char *pwd = value + 8;  /* skip "SET_PWD:" */
            if (pwd[0] != '\0') {
                OLED_LogMessage("-> Changing pwd");
                HAL_Delay(500);
                MQTT_OnPassword(pwd);
                return;
            }
        }

        /* Không nhận dạng được */
        OLED_LogMessage("Unknown cmd!");
        OLED_LogMessage("Use: OPEN");
        OLED_LogMessage("Or: SET_PWD:pass");
        HAL_Delay(3000);  /* Hiển thị lỗi 3s */
        OLED_LogClear();
        OLED_LogMessage("Enter password:");
        return;
    }

    /* JSON format - hiển thị parsed values */
    line[0] = '\0';
    strncat(line, "T:", OLED_LINE_LEN);
    strncat(line, type, OLED_LINE_LEN - 2U);
    OLED_LogMessage(line);

    line[0] = '\0';
    strncat(line, "A:", OLED_LINE_LEN);
    strncat(line, action, OLED_LINE_LEN - 2U);
    OLED_LogMessage(line);

    if (value[0] != '\0') {
        line[0] = '\0';
        strncat(line, "V:", OLED_LINE_LEN);
        strncat(line, value, OLED_LINE_LEN - 2U);
        OLED_LogMessage(line);
    }

    HAL_Delay(1500);

    /* Xử lý JSON commands */
    if (strcmp(type, "DOOR") == 0) {
        if (strcmp(action, "OPEN") == 0) {
            MQTT_OnDoorOpen();
        } else if (strcmp(action, "SET_PWD") == 0 && value[0] != '\0') {
            MQTT_OnPassword(value);
        }
    }
}

/* USER CODE END 0 */

/* ===========================================================================
 * Application entry point
 * ===========================================================================*/
int main(void)
{
    /* USER CODE BEGIN 1 */
    /* USER CODE END 1 */

    HAL_Init();

    /* USER CODE BEGIN Init */
    /* USER CODE END Init */

    SystemClock_Config();

    /* USER CODE BEGIN SysInit */
    /* USER CODE END SysInit */
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_SPI2_Init();
    /* USER CODE BEGIN 2 */

    /* OLED */
    ssd1306_Init();
    oled_ready = 1U;
    OLED_LogClear();
    OLED_LogMessage("System starting");

    /* W5500 */
    W5500_SetLogCallback(OLED_LogMessage);
    if (W5500_Init() != 0) {
        OLED_LogMessage("W5500 FAIL");
        while (1) { HAL_Delay(100); }  /* dừng — không chuyển màn */
    } else {
        OLED_LogMessage("W5500 OK");
        W5500_Diag();
        HAL_Delay(3000);   /* hiển thị 3s rồi tiếp tục */
    }

    /* Keypad */
    Keypad_Init();

    /* Door lock */
    DoorLock_Init();

    /* MQTT — kết nối tới Mosquitto broker, đăng ký 2 topics:
     *   door/open     → mở cửa
     *   door/password → đổi mật khẩu */
    MQTT_Init(MQTT_OnDoorOpen, MQTT_OnPassword, MQTT_OnMessage);

    /* USER CODE END 2 */

    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* USER CODE END WHILE */

        /* USER CODE BEGIN 3 */
        char key = Keypad_Scan();
        DoorLock_ProcessKey(key);
        MQTT_Task();          /* MQTT polling — tự kết nối lại nếu mất */
        HAL_Delay(10);
    }
    /* USER CODE END 3 */
}

/* ===========================================================================
 * Peripheral initialisation (CubeMX generated)
 * ===========================================================================*/
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType  = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState        = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue  = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState        = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState    = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource   = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL      = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { Error_Handler(); }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) { Error_Handler(); }
}

static void MX_SPI1_Init(void)
{
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) { Error_Handler(); }
}

static void MX_SPI2_Init(void)
{
    hspi2.Instance               = SPI2;
    hspi2.Init.Mode              = SPI_MODE_MASTER;
    hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi2.Init.NSS               = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi2) != HAL_OK) { Error_Handler(); }
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* ---- Default output levels ----------------------------------------- */
    /* PA4  = W5500 CS   → HIGH (deasserted)
       PA12 = OLED /RST  → LOW  (hold in reset; ssd1306_Init() releases it) */
    HAL_GPIO_WritePin(GPIOA, CS_Pin,    GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, RESET_Pin, GPIO_PIN_RESET);

    /* PB0  = W5500 RST  → HIGH (not in reset) */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);

    /* PB11 = Relay      → SET  = locked (active-LOW relay)
       PB12 = OLED CS    → HIGH (deasserted)
       PB14 = OLED D/C   → LOW  (command mode default) */
    HAL_GPIO_WritePin(GPIOB, PB11_TOGGLE_Pin,  GPIO_PIN_SET);   /* relay locked */
    HAL_GPIO_WritePin(GPIOB, OLED_CS_Pin,       GPIO_PIN_SET);   /* OLED CS high */
    HAL_GPIO_WritePin(GPIOB, OLED_DC_Pin,       GPIO_PIN_RESET); /* OLED D/C low */

    /* ---- PA4  W5500 CS — output PP high-speed --------------------------- */
    GPIO_InitStruct.Pin   = CS_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* ---- PA12 OLED /RST — output PP low-speed --------------------------- */
    GPIO_InitStruct.Pin   = RESET_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* ---- PB0 W5500 RST — output PP high-speed --------------------------- */
    GPIO_InitStruct.Pin   = GPIO_PIN_0;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* ---- PB11 Relay — output PP low-speed ------------------------------- */
    GPIO_InitStruct.Pin   = PB11_TOGGLE_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* ---- PB4 Electric lock — output PP low-speed, idle LOW (khoá) ------- */
    GPIO_InitStruct.Pin   = LOCK_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LOCK_GPIO_Port, &GPIO_InitStruct);
    HAL_GPIO_WritePin(LOCK_GPIO_Port, LOCK_Pin, GPIO_PIN_RESET); /* khoá mặc định */

    /* ---- PB12 OLED CS, PB14 OLED D/C — output PP low-speed ------------- */
    GPIO_InitStruct.Pin   = OLED_CS_Pin | OLED_DC_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* ---- Keypad rows PA0-PA3 — output PP, idle HIGH --------------------- */
    GPIO_InitStruct.Pin   = KP_ROW0 | KP_ROW1 | KP_ROW2 | KP_ROW3;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(KP_PORT, &GPIO_InitStruct);
    HAL_GPIO_WritePin(KP_PORT, KP_ROW0 | KP_ROW1 | KP_ROW2 | KP_ROW3, GPIO_PIN_SET);

    /* ---- Keypad cols PA8, PA9, PA11 — input pull-up --------------------- */
    GPIO_InitStruct.Pin  = KP_COL0 | KP_COL1 | KP_COL3;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KP_PORT, &GPIO_InitStruct);

    /* ---- Keypad col2 PB5 — input pull-up -------------------------------- */
    GPIO_InitStruct.Pin  = KP_COL2;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KP_COL2_PORT, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

void Error_Handler(void)
{
    /* USER CODE BEGIN Error_Handler_Debug */
    if (oled_ready != 0U) { OLED_LogMessage("!! FATAL ERROR"); }
    __disable_irq();
    /* Intentional infinite loop — halt CPU on fatal error */
    while (1) { /* spin */ }
    /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif