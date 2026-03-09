#include <Arduino_FreeRTOS.h>
int var_global = 0;
void TaskCounter (void *pvParameters);
void setup() {
 Serial.begin(9600);
 xTaskCreate(TaskCounter, "Task1", 128, NULL, 1, NULL);
 xTaskCreate(TaskCounter, "Task2", 128, NULL, 1, NULL);
 vTaskStartScheduler();
}
void TaskCounter (void *pvParameters)
{
 char *taskName = pcTaskGetName(NULL);
 while(1)
 {
 int var_local = var_global;
 var_local++;
 vTaskDelay(random(50,500) / portTICK_PERIOD_MS);
 var_global = var_local;
 Serial.println(taskName + String(": ") + var_local);
 }
}
void loop() {

}