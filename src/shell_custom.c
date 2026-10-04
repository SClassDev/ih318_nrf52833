/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <shell_custom.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/util.h>
#include "main.h"


#define UUID_STRING_LENGTH 36
extern uint8_t uuidBytes[16];
extern adv_mfg_data_type adv_mfg_data;


#define QUEUE_SIZE 128
struct data_item_type {
    uint32_t length;
	uint8_t data[QUEUE_SIZE];
} quePutData, queGetData;

K_MSGQ_DEFINE (uart_queue, sizeof(struct data_item_type), 4, 4);

extern const struct device *lpuart;
static bool bt_is_enabled = false;

int uuid_str_to_bytes(const char *uuid_str, uint8_t *uuid_bytes) {
    // sscanf parst den String anhand des UUID-Formats und ignoriert die Bindestriche.
    // %2hhx liest exakt 2 Hex-Zeichen und speichert sie als ein Byte (uint8_t).
    int parsed = sscanf(uuid_str,
        "%2hhx%2hhx%2hhx%2hhx-%2hhx%2hhx-%2hhx%2hhx-%2hhx%2hhx-%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
        &uuid_bytes[0],  &uuid_bytes[1],  &uuid_bytes[2],  &uuid_bytes[3],
        &uuid_bytes[4],  &uuid_bytes[5],
        &uuid_bytes[6],  &uuid_bytes[7],
        &uuid_bytes[8],  &uuid_bytes[9],
        &uuid_bytes[10], &uuid_bytes[11], &uuid_bytes[12], &uuid_bytes[13], &uuid_bytes[14], &uuid_bytes[15]
    );

    // Wenn erfolgreich geladen, wurden alle 16 Bytes geparst.
    return (parsed == 16) ? 0 : -1;
}

void read_uuid (void)
{
	memcpy (quePutData.data, "uuid", 4);
	quePutData.length = 4;
	k_msgq_put (&uart_queue, &quePutData, K_NO_WAIT);
	uart_irq_tx_enable(lpuart);
}

void set_bt_enabled (void)
{
	bt_is_enabled = true;
}
void set_bt_disabled (void)
{
	bt_is_enabled = false;
}

static void rx_callback(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	if (0 == strncmp ("lte ", data, 4))
	{
		/* This is a nrf91 command. Write to FIFO and send over lpuart */
		memcpy (quePutData.data, data + 4,  len - 4);
		quePutData.length = len - 4;
		k_msgq_put (&uart_queue, &quePutData, K_NO_WAIT);
		uart_irq_tx_enable(lpuart);
	}
	else
	{
		if (0 == strncmp ("ble ", data, 4))
		{
			/* This a nrf52 command. Execute as shell */
			// pipe = pipe_ble;
			shell_execute_cmd(NULL, (data + 4));
		}
	}
}


static void tx_callback(struct bt_conn *conn)
{
	
}

static void send_enabled_callback(enum bt_nus_send_status status)
{
	
}

int cust_bt_nus_init(void)
{
	struct bt_nus_cb callbacks = {
		.received = rx_callback,
		.sent = tx_callback,
		.send_enabled = send_enabled_callback
	};

	return bt_nus_init(&callbacks);
}

/* Work handler function for lpuart RX */
void lpuart_rx_work_handler(struct k_work *work) {

	/* Read received data from UART FIFO */
	int len = uart_fifo_read(lpuart, queGetData.data, sizeof(queGetData.data) - 1);
	if (len < 0) {
		return;
	}

	/* Null-terminate the received data */
	queGetData.data[len] = '\0';
	queGetData.length = len;

	if (0 == strncmp ("ble ", queGetData.data, 4))
	{
		/* Shell command for BLE */
		shell_execute_cmd (NULL, (queGetData.data + 4));
	}
	else
	{
		if (true == bt_is_enabled)
		{
			/* Message for BLE central */
	    	bt_nus_send (NULL, queGetData.data, queGetData.length);
		}
		else
		{
			/* Message for LTE or other non-BLE commands */
			if (0 == strncmp ("+uuid: ", queGetData.data, 7))
			{
				uuid_str_to_bytes(queGetData.data + 7, uuidBytes);
				/* Copy the UUID bytes to the advertising manufacturer data */
				memcpy (adv_mfg_data.uuid_bytes, uuidBytes, sizeof(uuidBytes));
			}
		}
	}
}

/* Register lpuart RX work handler */
K_WORK_DEFINE(lpuart_rx_work, lpuart_rx_work_handler);

void lpuart_irq_handler(const struct device *dev, void *context)
{
	/* Tx handler. Sending data over lpuart */
	if (uart_irq_tx_ready(dev)) {
		k_msgq_get (&uart_queue, &quePutData, K_NO_WAIT);
		uart_fifo_fill(dev, quePutData.data, quePutData.length);
		if (0 <	k_msgq_num_used_get (&uart_queue))
		{
			uart_irq_tx_enable(dev);		
		}
		else
		{
			uart_irq_tx_disable(dev);		
		}
	}

	/* Rx handler. Receiving data from lpuart */
	if (uart_irq_rx_ready(dev)) {
		/* Schedule work to read received data and process it in the work handler */
		k_work_submit(&lpuart_rx_work);
		/* Re-enable RX interrupt to receive next data */
		uart_irq_rx_enable (lpuart);	
	}
}

void send_alive_message(void)
{
	sprintf (quePutData.data, "version");
	quePutData.length = strlen(quePutData.data);
	k_msgq_put (&uart_queue, &quePutData, K_NO_WAIT);	
	uart_irq_tx_enable(lpuart);
	
	bt_nus_send(NULL, quePutData.data, quePutData.length);
}
