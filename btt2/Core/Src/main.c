/* USER CODE BEGIN Header */
/**
  * Hoàn thiện: Ngắt ngoài EXTI (PB0) + Queue (FromISR) + UART (Task)
  * Xung nhịp: HSI 8MHz
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stdio.h"
#include "string.h"

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;
osThreadId defaultTaskHandle;

/* USER CODE BEGIN PV */
QueueHandle_t qButtonPress;
uint32_t press_count = 0; // Biến đếm số lần nhấn nút
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
void StartDefaultTask(void const * argument);

/* USER CODE BEGIN PFP */
void Task_UART_Tx(void *argument);
/* USER CODE END PFP */

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART1_UART_Init();

  /* USER CODE BEGIN 2 */
  // 1. Khởi tạo Queue chứa 10 phần tử kiểu uint32_t
  qButtonPress = xQueueCreate(10, sizeof(uint32_t));

  // 2. Khởi tạo Task truyền UART
  xTaskCreate(Task_UART_Tx, "UartTxTask", 256, NULL, osPriorityNormal, NULL);
  /* USER CODE END 2 */

  // Mặc định sinh ra Default Task (ta có thể kệ nó)
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  osKernelStart();

  while (1)
  {
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART1 Initialization Function
  */
static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING; // Ngắt cạnh xuống
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0); // Ưu tiên 5 (Hợp lệ cho FreeRTOS)
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
}

/* USER CODE BEGIN 4 */

// --- 1. HÀM PHỤC VỤ NGẮT NGOÀI (ISR) ---
// Chú ý: Hàm này được gọi tự động khi PB0 chuyển từ 1 -> 0 (Bấm nút)
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if(GPIO_Pin == GPIO_PIN_0) {
        // Biến kiểm tra xem có cần chuyển ngữ cảnh (Context Switch) ngay lập tức hay không
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        press_count++; // Tăng biến đếm

        // Gửi dữ liệu vào Queue từ trong ngắt (BẮT BUỘC DÙNG HÀM ...FromISR)
        xQueueSendFromISR(qButtonPress, &press_count, &xHigherPriorityTaskWoken);

        // Nếu Task nhận dữ liệu có ưu tiên cao hơn Task đang chạy trước khi xảy ra ngắt
        // thì gọi hàm này để nhảy thẳng sang Task nhận ngay khi thoát ngắt.
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// --- 2. TASK NHẬN DỮ LIỆU TỪ QUEUE VÀ GỬI UART ---
void Task_UART_Tx(void *argument) {
    uint32_t rx_count;
    char uart_buf[50]; // Buffer chứa chuỗi ký tự

    for(;;) {
        // Task sẽ nằm im ở trạng thái Blocked (không tốn CPU) cho đến khi có dữ liệu chui vào Queue
        if (xQueueReceive(qButtonPress, &rx_count, portMAX_DELAY) == pdPASS) {
            
            // Định dạng chuỗi
            sprintf(uart_buf, "So lan nhan nut: %lu\r\n", (unsigned long)rx_count);
            
            // Gửi dữ liệu qua UART1
            HAL_UART_Transmit(&huart1, (uint8_t*)uart_buf, strlen(uart_buf), 100);
        }
    }
}
/* USER CODE END 4 */

void StartDefaultTask(void const * argument)
{
  for(;;)
  {
    osDelay(1);
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
