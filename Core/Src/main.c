/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Full Optimized Paint/Color Wheel Program
  ******************************************************************************
  */
/* USER CODE END Header */
#include "main.h"
#include <stdio.h>

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
uint16_t x_pos = 95;
uint16_t y_pos = 10;

// 16-bit RGB565 Colors
uint16_t paint_colors[] = {
    0xF800, // Red
    0x07E0, // Green
    0x001F, // Blue
    0xFFE0, // Yellow
    0xF81F, // Magenta
    0x07FF  // Cyan
};

uint8_t color_index = 0;
uint8_t num_colors = sizeof(paint_colors) / sizeof(paint_colors[0]);
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_SPI1_Init(void);

/* USER CODE BEGIN PFP */
void LCD_Init(void);
void LCD_DrawBrush(uint16_t x, uint16_t y, uint8_t size);
void LCD_Fill(uint16_t color);
void LCD_WriteCommand(uint8_t cmd);
void LCD_WriteData(uint8_t data);
void LCD_Select(void);
void LCD_Unselect(void);
/* USER CODE END PFP */

int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_SPI1_Init();

  /* USER CODE BEGIN 2 */
  LCD_Init();
  LCD_Fill(0x0000); // Start with Black screen
  printf("System Ready. Press PC4 to Paint.\r\n");
  /* USER CODE END 2 */

  while (1)
  {
    // Check for button press on PC4 (Low = Pressed)
    if(HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_4) == GPIO_PIN_RESET)
    {
        // Draw an 8x8 block
        LCD_DrawBrush(x_pos, y_pos, 8);
        printf("Drawing Color: %d\r\n", color_index);

        // Delay to control color cycling speed
        HAL_Delay(150);
    }
    HAL_Delay(150);
  }
}

/* --- LCD Low Level Driver --- */

void LCD_Select(void) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
}

void LCD_Unselect(void) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
}

void LCD_WriteCommand(uint8_t cmd) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET); // DC Low = Command
    LCD_Select();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
    LCD_Unselect();
}

void LCD_WriteData(uint8_t data) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET); // DC High = Data
    LCD_Select();
    HAL_SPI_Transmit(&hspi1, &data, 1, HAL_MAX_DELAY);
    LCD_Unselect();
}

/* --- Optimized Graphics Functions --- */

void LCD_DrawBrush(uint16_t x, uint16_t y, uint8_t size) {
    uint16_t color = paint_colors[color_index];

    // Set X window
    LCD_WriteCommand(0x2A);
    LCD_WriteData(x >> 8); LCD_WriteData(x & 0xFF);
    LCD_WriteData((x + size - 1) >> 8); LCD_WriteData((x + size - 1) & 0xFF);

    // Set Y window
    LCD_WriteCommand(0x2B);
    LCD_WriteData(y >> 8); LCD_WriteData(y & 0xFF);
    LCD_WriteData((y + size - 1) >> 8); LCD_WriteData((y + size - 1) & 0xFF);

    // Write to RAM
    LCD_WriteCommand(0x2C);

    uint8_t color_bytes[] = { color >> 8, color & 0xFF };

    for(int i = 0; i < (size * size); i++) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET);
        LCD_Select();
        HAL_SPI_Transmit(&hspi1, color_bytes, 2, HAL_MAX_DELAY);
        LCD_Unselect();
    }

    color_index = (color_index + 1) % num_colors;
}

void LCD_Fill(uint16_t color) {
    LCD_WriteCommand(0x2A); // X
    LCD_WriteData(0x00); LCD_WriteData(0x00);
    LCD_WriteData(0x00); LCD_WriteData(0x7F);
    LCD_WriteCommand(0x2B); // Y
    LCD_WriteData(0x00); LCD_WriteData(0x00);
    LCD_WriteData(0x00); LCD_WriteData(0x9F);
    LCD_WriteCommand(0x2C);

    uint8_t data[] = { color >> 8, color & 0xFF };
    for (int i = 0; i < 128 * 160; i++) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET);
        LCD_Select();
        HAL_SPI_Transmit(&hspi1, data, 2, HAL_MAX_DELAY);
        LCD_Unselect();
    }
}

void LCD_Init(void) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET);
    HAL_Delay(50);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);
    HAL_Delay(150);

    LCD_WriteCommand(0x01); // Reset
    HAL_Delay(150);
    LCD_WriteCommand(0x11); // Wake
    HAL_Delay(200);
    LCD_WriteCommand(0x3A); // Color mode
    LCD_WriteData(0x05);    // 16-bit
    LCD_WriteCommand(0x29); // On
    HAL_Delay(100);
}

/* --- Peripheral Initialization --- */

static void MX_SPI1_Init(void)
{
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLED; // Corrected from DISABLE
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLED; // Corrected from DISABLE
  if (HAL_SPI_Init(&hspi1) != HAL_OK) {
    Error_Handler();
  }
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  HAL_UART_Init(&huart2);
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  // Button PC4
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  // LCD Pins PB13, PB14, PB15
  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

int _write(int file, char *ptr, int len) {
  HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, HAL_MAX_DELAY);
  return len;
}

void Error_Handler(void) {
  while (1) {}
}
