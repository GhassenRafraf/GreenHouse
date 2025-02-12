/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Defines -------------------------------------------------------------------*/
#define ADC_CHANNELS    4
#define ADC_MAX_VALUE   4095.0f
#define VREF            3.3f

/* Global Variables ----------------------------------------------------------*/
ADC_HandleTypeDef       hadc1;
DMA_HandleTypeDef       hdma_adc1;
UART_HandleTypeDef      huart2;

uint32_t adcBuffer[ADC_CHANNELS] = {0};

/* FreeRTOS synchronization objects */
SemaphoreHandle_t  xADCSemaphore;   // Binary semaphore: signaled when ADC conversion complete
SemaphoreHandle_t  xSensorMutex;      // Mutex to protect access to sensorData

typedef struct {
    float temperature;   // in °C
    float light;         // arbitrary units (e.g. lux)
    float soilMoisture;  // percentage (%)
    float humidity;      // percentage (%)
} SensorData_t;

SensorData_t sensorData;

/* Function Prototypes -------------------------------------------------------*/
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_DMA_Init(void);
void MX_ADC1_Init(void);
void MX_UART2_Init(void);

void SensorTask(void *pvParameters);
void ControlTask(void *pvParameters);
void DisplayTask(void *pvParameters);
void CommunicationTask(void *pvParameters);

void Error_Handler(void);

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if(hadc->Instance == ADC1)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(xADCSemaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* Main ----------------------------------------------------------------------*/
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_ADC1_Init();
    MX_UART2_Init();

    if (HAL_ADC_Start_DMA(&hadc1, adcBuffer, ADC_CHANNELS) != HAL_OK)
    {
        Error_Handler();
    }

    xADCSemaphore = xSemaphoreCreateBinary();
    if (xADCSemaphore == NULL) { Error_Handler(); }

    xSensorMutex = xSemaphoreCreateMutex();
    if (xSensorMutex == NULL) { Error_Handler(); }

    xTaskCreate(SensorTask, "SensorTask", 256, NULL, 3, NULL);
    xTaskCreate(ControlTask, "ControlTask", 256, NULL, 2, NULL);
    xTaskCreate(DisplayTask, "DisplayTask", 256, NULL, 1, NULL);
    xTaskCreate(CommunicationTask, "CommTask", 256, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1) {}
}

/* SensorTask:
   Waits for the ADC conversion complete semaphore and then reads the raw ADC values
   from the DMA buffer. It converts these raw values to physical units and updates
   the global sensorData structure (protected by a mutex). */
void SensorTask(void *pvParameters)
{
    SensorData_t localData;
    float voltage0, voltage1, voltage2, voltage3;
    (void) pvParameters;

    for (;;)
    {
        /* Wait indefinitely until a new conversion sequence is complete */
        if (xSemaphoreTake(xADCSemaphore, portMAX_DELAY) == pdTRUE)
        {
            voltage0 = ((float)adcBuffer[0] * VREF) / ADC_MAX_VALUE;
            voltage1 = ((float)adcBuffer[1] * VREF) / ADC_MAX_VALUE;
            voltage2 = ((float)adcBuffer[2] * VREF) / ADC_MAX_VALUE;
            voltage3 = ((float)adcBuffer[3] * VREF) / ADC_MAX_VALUE;

            /* Example conversion formulas:
               - Temperature sensor (e.g., LM35: 10 mV/°C): */
            localData.temperature = voltage0 * 100.0f;

            /* - Light sensor: assume lux is proportional to voltage (scale factor chosen by calibration) */
            localData.light = voltage1 * 200.0f;

            /* - Soil moisture sensor: convert voltage to percentage (0V => 0%, VREF => 100%) */
            localData.soilMoisture = (voltage2 / VREF) * 100.0f;

            /* - Humidity sensor: assume linear mapping from 0.8V = 0% to 3.9V = 100% */
            if (voltage3 < 0.8f) voltage3 = 0.8f;
            if (voltage3 > 3.9f) voltage3 = 3.9f;
            localData.humidity = ((voltage3 - 0.8f) / (3.9f - 0.8f)) * 100.0f;

            /* Update global sensor data (protected by a mutex) */
            if (xSemaphoreTake(xSensorMutex, portMAX_DELAY) == pdTRUE)
            {
                sensorData = localData;
                xSemaphoreGive(xSensorMutex);
            }
        }
    }
}

/* ControlTask:
   Reads the processed sensor data and implements control logic.
   For example, if temperature > 30°C, it activates a fan (GPIOB Pin 0);
   if soil moisture < 40%, it activates a water pump (GPIOB Pin 1). */
void ControlTask(void *pvParameters)
{
    SensorData_t localData;
    (void) pvParameters;

    for (;;)
    {
        if (xSemaphoreTake(xSensorMutex, portMAX_DELAY) == pdTRUE)
        {
            localData = sensorData;
            xSemaphoreGive(xSensorMutex);
        }

        if (localData.temperature > 30.0f)
        {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);   // Activate fan
        }
        else
        {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); // Deactivate fan
        }

        if (localData.soilMoisture < 40.0f)
        {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);   // Activate pump
        }
        else
        {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET); // Deactivate pump
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* DisplayTask:
   Periodically (every 1 second) displays sensor readings via UART2.
   In a real product, this might update an LCD or OLED display. */
void DisplayTask(void *pvParameters)
{
    SensorData_t localData;
    char buf[128];
    (void) pvParameters;

    for (;;)
    {
        if (xSemaphoreTake(xSensorMutex, portMAX_DELAY) == pdTRUE)
        {
            localData = sensorData;
            xSemaphoreGive(xSensorMutex);
        }

        snprintf(buf, sizeof(buf),
                 "Temp: %.1f C, Light: %.1f, Soil: %.1f%%, Hum: %.1f%%\r\n",
                 localData.temperature, localData.light,
                 localData.soilMoisture, localData.humidity);
        HAL_UART_Transmit(&huart2, (uint8_t*)buf, strlen(buf), HAL_MAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* CommunicationTask:
   Transmits sensor data (e.g. to a remote PC or over network) every 2 seconds.
   Here we simply send a formatted string over UART2. */
void CommunicationTask(void *pvParameters)
{
    SensorData_t localData;
    char buf[128];
    (void) pvParameters;

    for (;;)
    {
        if (xSemaphoreTake(xSensorMutex, portMAX_DELAY) == pdTRUE)
        {
            localData = sensorData;
            xSemaphoreGive(xSensorMutex);
        }

        snprintf(buf, sizeof(buf),
                 "DATA:T=%.1f,H=%.1f,L=%.1f,M=%.1f\r\n",
                 localData.temperature, localData.humidity,
                 localData.light, localData.soilMoisture);
        HAL_UART_Transmit(&huart2, (uint8_t*)buf, strlen(buf), HAL_MAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* Peripheral Initialization Functions -------------------------------------*/

/* System Clock Configuration */
void SystemClock_Config(void)
{
    /* This is a basic clock configuration.
       In practice, use CubeMX or your own configuration as needed. */
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM            = 16;
    RCC_OscInitStruct.PLL.PLLN            = 336;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ            = 7;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}

/* GPIO Initialization */
void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* Enable clocks for GPIO ports */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Configure GPIO pins for actuators (fan and pump on GPIOB) */
    GPIO_InitStruct.Pin   = GPIO_PIN_0 | GPIO_PIN_1;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* (Other GPIO configuration as required can be added here) */
}

/* DMA Initialization */
void MX_DMA_Init(void)
{
    /* Enable DMA2 clock (ADC1 uses DMA2 on STM32F4) */
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* Configure DMA for ADC1 */
    hdma_adc1.Instance                 = DMA2_Stream0;
    hdma_adc1.Init.Channel             = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_adc1.Init.Mode                = DMA_CIRCULAR;
    hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adc1.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK)
    {
        Error_Handler();
    }

    /* Link DMA handle to ADC handle */
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    /* Configure NVIC for DMA */
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}

/* ADC1 Initialization with DMA */
void MX_ADC1_Init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = ENABLE;      // Multiple channels
    hadc1.Init.ContinuousConvMode    = ENABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion       = ADC_CHANNELS;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SEQ_CONV;
    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    /* Configure channel 0: Temperature sensor (e.g., connected to PA0) */
    sConfig.Channel      = ADC_CHANNEL_0;
    sConfig.Rank         = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_56CYCLES;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        Error_Handler();
    }

    /* Configure channel 1: Light sensor (e.g., PA1) */
    sConfig.Channel = ADC_CHANNEL_1;
    sConfig.Rank    = 2;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        Error_Handler();
    }

    /* Configure channel 2: Soil moisture sensor (e.g., PA2) */
    sConfig.Channel = ADC_CHANNEL_2;
    sConfig.Rank    = 3;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        Error_Handler();
    }

    /* Configure channel 3: Humidity sensor (e.g., PA3) */
    sConfig.Channel = ADC_CHANNEL_3;
    sConfig.Rank    = 4;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        Error_Handler();
    }
}

/* UART2 Initialization (for debugging/communication) */
void MX_UART2_Init(void)
{
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 115200;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK)
    {
        Error_Handler();
    }
}

/* Error Handler */
void Error_Handler(void)
{
    /* You can add your own error handling here */
    while(1) { }
}

/* DMA IRQ Handler */
void DMA2_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}
