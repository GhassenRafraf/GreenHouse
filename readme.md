# Greenhouse Controller

This project implements a smart greenhouse controller on an **STM32F401RE Nucleo** board using **FreeRTOS**. It continuously reads sensor data via the ADC using DMA, processes these readings in real time, and then uses the data to drive control logic for actuators (e.g., a fan and a water pump). In addition, sensor readings are periodically output via UART for display and remote monitoring.

---

## Features

- **Sensor Acquisition via ADC with DMA**  
  - Reads four analog channels continuously:
    - **Channel 0 (PA0):** Temperature sensor (e.g., LM35; conversion: 10 mV/°C)
    - **Channel 1 (PA1):** Light sensor
    - **Channel 2 (PA2):** Soil moisture sensor
    - **Channel 3 (PA3):** Humidity sensor (analog output)
- **Real-Time Processing Using FreeRTOS**  
  - **SensorTask:** Waits for the ADC conversion complete signal (via DMA callback), converts raw ADC values into physical units, and updates the global sensor data structure.
  - **ControlTask:** Reads sensor data and applies control logic (e.g., turns on a fan if temperature > 30°C, starts a pump if soil moisture < 40%).
  - **DisplayTask:** Periodically sends sensor readings via UART (this can be adapted to update an LCD or OLED).
  - **CommunicationTask:** Periodically transmits formatted sensor data (e.g., to a remote PC or network module).
- **Efficient Operation:**  
  - Uses DMA to offload the CPU during ADC data transfers.
  - Synchronizes tasks using a binary semaphore (for ADC data ready) and a mutex (to protect shared sensor data).

---

## Hardware Requirements

- **STM32F401RE Nucleo Board**
- **Sensors:**  
  - Temperature sensor (e.g., LM35 connected to PA0)  
  - Light sensor (connected to PA1)  
  - Soil moisture sensor (connected to PA2)  
  - Humidity sensor (analog output on PA3)
- **Actuators:**  
  - Fan (controlled via GPIOB, Pin 0)  
  - Water Pump (controlled via GPIOB, Pin 1)
- **Communication:**  
  - UART (USART2 used for debugging/monitoring)
- **Optional:**  
  - LCD/OLED display (the project currently uses UART output)

---

## Software Requirements

- **STM32CubeMX** (for generating HAL initialization code)
- **STM32 HAL Libraries** (for the STM32F4 series)
- **FreeRTOS** 
- **Toolchain:** ARM GCC (or STM32CubeIDE)

---

## How It Works

1. **ADC with DMA:**  
   - The ADC is configured in scan mode with continuous conversion of 4 channels.
   - DMA transfers the conversion results into a global array (`adcBuffer`).
   - Once a full conversion sequence is complete, the HAL ADC callback (`HAL_ADC_ConvCpltCallback`) releases a semaphore to signal the **SensorTask**.

2. **SensorTask:**  
   - Waits for the ADC data ready semaphore.
   - Converts raw ADC values to voltages and then to physical sensor values using example conversion formulas.
   - Updates the global `sensorData` structure (protected by a mutex).

3. **ControlTask:**  
   - Reads the processed sensor data.
   - Applies simple control logic (e.g., if temperature > 30°C, turns on a fan; if soil moisture < 40%, activates the pump) by toggling GPIO pins.

4. **DisplayTask & CommunicationTask:**  
   - These tasks periodically fetch the latest sensor readings and send formatted data via UART.
   - The DisplayTask simulates updating an LCD display, while the CommunicationTask demonstrates how data might be sent for remote monitoring.

---

