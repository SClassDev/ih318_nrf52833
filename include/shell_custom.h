
#ifndef SHELL_CUST_BT_NUS_H_
#define SHELL_CUST_BT_NUS_H_

#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/services/nus.h>
#include <zephyr/sys/ring_buffer.h>

/***************** Custom Bluetooth NUS Shell Transport *****************
 * This header defines the interface for a custom Bluetooth NUS shell transport.
 * It includes necessary Zephyr headers and declares the initialization function
 * and UART interrupt handler for the custom transport.
 ***********************************************************************/
int cust_bt_nus_init(void);

void lpuart_irq_handler(const struct device *dev, void *context);

void send_alive_message(void);
#endif /* SHELL_CUST_BT_NUS_H_ */
