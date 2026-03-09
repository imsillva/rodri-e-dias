#include <MultiFunctionShield.h>
#include <Arduino_FreeRTOS.h>

void taskBlink1(void *pvParameters);
void taskBlink2(void *pvParameters);
void taskButton1(void *pvParameters);

freertos_copy_20260309152820.ino
3 KB
Yuki — 3:58 PM
~sewfjoiewfh
#include <MultiFunctionShield.h>
#include <Arduino_FreeRTOS.h>

void taskBlink1(void *pvParameters);
void taskBlink2(void *pvParameters);
void taskButton1(void *pvParameters);
void taskButton2(void *pvParameters);
void taskButton3(void *pvParameters);

volatile long interval1 = 500;
volatile long interval2 = 500;

volatile int incremento1 = 100;
volatile int incremento2 = 100;

MultiFunctionShield MFS;

void setup() {
  MFS.begin();

  pinMode(LED_1_PIN, OUTPUT);
  pinMode(LED_2_PIN, OUTPUT);
  pinMode(LED_3_PIN, OUTPUT);

  pinMode(BUTTON_1_PIN, INPUT);
  pinMode(BUTTON_2_PIN, INPUT);
  pinMode(BUTTON_3_PIN, INPUT);

  Serial.begin(9600);

  digitalWrite(LED_3_PIN, LOW);

  MFS.Display(interval1);

  xTaskCreate(taskBlink1, "Blink1", 128, NULL, 1, NULL);
  xTaskCreate(taskBlink2, "Blink2", 128, NULL, 1, NULL);
  xTaskCreate(taskButton1, "Button1", 128, NULL, 1, NULL);
  xTaskCreate(taskButton2, "Button2", 128, NULL, 1, NULL);
  xTaskCreate(taskButton3, "Button3", 128, NULL, 1, NULL);

  vTaskStartScheduler();
}

void taskBlink1(void *pvParameters)
{
  while (1)
  {
    long localInterval1 = interval1;

    digitalWrite(LED_1_PIN, HIGH);
    vTaskDelay(localInterval1 / portTICK_PERIOD_MS);

    digitalWrite(LED_1_PIN, LOW);
    vTaskDelay(localInterval1 / portTICK_PERIOD_MS);
  }
}

void taskBlink2(void *pvParameters)
{
  while (1)
  {
    long localInterval2 = interval2;

    digitalWrite(LED_2_PIN, HIGH);
    vTaskDelay(localInterval2 / portTICK_PERIOD_MS);

    digitalWrite(LED_2_PIN, LOW);
    vTaskDelay(localInterval2 / portTICK_PERIOD_MS);
  }
}

void taskButton1(void *pvParameters)
{
  int lastButtonState1 = digitalRead(BUTTON_1_PIN);

  while (1)
  {
    int currentButtonState1 = digitalRead(BUTTON_1_PIN);

    if (currentButtonState1 == LOW && lastButtonState1 == HIGH)
    {
      interval1 += incremento1;

      if (interval1 < 100) interval1 = 100;

      MFS.Display(interval1);

      Serial.print("LED 1 interval: ");
      Serial.println(interval1);

      vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    lastButtonState1 = currentButtonState1;
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void taskButton2(void *pvParameters)
{
... (55 lines left)

challengeBC1.ino
4 KB
﻿
Ze0n
ze0n5772
 
#include <MultiFunctionShield.h>
#include <Arduino_FreeRTOS.h>

void taskBlink1(void *pvParameters);
void taskBlink2(void *pvParameters);
void taskButton1(void *pvParameters);
void taskButton2(void *pvParameters);
void taskButton3(void *pvParameters);

volatile long interval1 = 500;
volatile long interval2 = 500;

volatile int incremento1 = 100;
volatile int incremento2 = 100;

MultiFunctionShield MFS;

void setup() {
  MFS.begin();

  pinMode(LED_1_PIN, OUTPUT);
  pinMode(LED_2_PIN, OUTPUT);
  pinMode(LED_3_PIN, OUTPUT);

  pinMode(BUTTON_1_PIN, INPUT);
  pinMode(BUTTON_2_PIN, INPUT);
  pinMode(BUTTON_3_PIN, INPUT);

  Serial.begin(9600);

  digitalWrite(LED_3_PIN, LOW);

  MFS.Display(interval1);

  xTaskCreate(taskBlink1, "Blink1", 128, NULL, 1, NULL);
  xTaskCreate(taskBlink2, "Blink2", 128, NULL, 1, NULL);
  xTaskCreate(taskButton1, "Button1", 128, NULL, 1, NULL);
  xTaskCreate(taskButton2, "Button2", 128, NULL, 1, NULL);
  xTaskCreate(taskButton3, "Button3", 128, NULL, 1, NULL);

  vTaskStartScheduler();
}

void taskBlink1(void *pvParameters)
{
  while (1)
  {
    long localInterval1 = interval1;

    digitalWrite(LED_1_PIN, HIGH);
    vTaskDelay(localInterval1 / portTICK_PERIOD_MS);

    digitalWrite(LED_1_PIN, LOW);
    vTaskDelay(localInterval1 / portTICK_PERIOD_MS);
  }
}

void taskBlink2(void *pvParameters)
{
  while (1)
  {
    long localInterval2 = interval2;

    digitalWrite(LED_2_PIN, HIGH);
    vTaskDelay(localInterval2 / portTICK_PERIOD_MS);

    digitalWrite(LED_2_PIN, LOW);
    vTaskDelay(localInterval2 / portTICK_PERIOD_MS);
  }
}

void taskButton1(void *pvParameters)
{
  int lastButtonState1 = digitalRead(BUTTON_1_PIN);

  while (1)
  {
    int currentButtonState1 = digitalRead(BUTTON_1_PIN);

    if (currentButtonState1 == LOW && lastButtonState1 == HIGH)
    {
      interval1 += incremento1;

      if (interval1 < 100) interval1 = 100;

      MFS.Display(interval1);

      Serial.print("LED 1 interval: ");
      Serial.println(interval1);

      vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    lastButtonState1 = currentButtonState1;
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void taskButton2(void *pvParameters)
{
  int lastButtonState2 = digitalRead(BUTTON_2_PIN);

  while (1)
  {
    int currentButtonState2 = digitalRead(BUTTON_2_PIN);

    if (currentButtonState2 == LOW && lastButtonState2 == HIGH)
    {
      interval2 += incremento2;

      if (interval2 < 100) interval2 = 100;

      MFS.Display(interval2);

      Serial.print("LED 2 interval: ");
      Serial.println(interval2);

      vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    lastButtonState2 = currentButtonState2;
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void taskButton3(void *pvParameters)
{
  int lastButtonState3 = digitalRead(BUTTON_3_PIN);

  while (1)
  {
    int currentButtonState3 = digitalRead(BUTTON_3_PIN);

    if (currentButtonState3 == LOW && lastButtonState3 == HIGH)
    {
      incremento1 = -incremento1;
      incremento2 = -incremento2;

      if (incremento1 > 0) {
        digitalWrite(LED_3_PIN, LOW);   // modo somar
        Serial.println("Modo atual: SOMAR 100");
      } else {
        digitalWrite(LED_3_PIN, HIGH);    // modo subtrair
        Serial.println("Modo atual: SUBTRAIR 100");
      }

      vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    lastButtonState3 = currentButtonState3;
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void loop() {}
