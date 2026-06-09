/**
 * net_task.h — M1 Ethernet bring-up task.
 *
 * Waits for link + DHCP, logs the acquired IP over the USART1 console, resolves
 * the stock-API host via DNS, and proves a raw TCP connection to it. This is the
 * networking foundation that the mbedTLS HTTPS client (M2) will build on.
 */
#ifndef APP_NET_TASK_H
#define APP_NET_TASK_H

#include "cmsis_os.h"

/* FreeRTOS (CMSIS-RTOS v1) thread entry. Create with osThreadCreate(). */
void StartNetTask(void const *argument);

#endif /* APP_NET_TASK_H */
