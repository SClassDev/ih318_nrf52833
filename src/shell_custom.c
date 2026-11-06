/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <shell_custom.h>
#include <stdarg.h>
#include <stdio.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include "main.h"

LOG_MODULE_REGISTER(shell_custom);

#define UUID_STRING_LENGTH 36
extern uint8_t uuidBytes[16];
extern adv_mfg_data_type adv_mfg_data;


#define QUEUE_SIZE 128
struct data_item_type {
    uint32_t length;
	uint8_t data[QUEUE_SIZE];
} quePutData;

K_MSGQ_DEFINE (uart_queue, sizeof(struct data_item_type), 4, 4);

struct uart_rx_item {
	uint32_t length;
	uint8_t data[CONFIG_NRF_SW_LPUART_MAX_PACKET_SIZE + 1];
};

K_MSGQ_DEFINE(uart_rx_queue, sizeof(struct uart_rx_item), 4, 4);

extern const struct device *lpuart;
static bool bt_is_enabled = false;

#define MODEM_UART_RSP_TIMEOUT K_MSEC(2000)

K_SEM_DEFINE(modem_rsp_sem, 0, 1);
K_MUTEX_DEFINE(modem_rsp_mutex);

static struct {
	volatile bool pending;
	uint8_t *buf;
	size_t size;
	int len;
} modem_rsp;

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

/*
 * Formats fmt/... into buf, sends buf over lpuart and blocks until the
 * nrf91 answers. The answer is written back into buf (null-terminated).
 * Returns the response length or a negative error code.
 * Must not be called from the system workqueue (lpuart_rx_work_handler).
 */
int nrf_modem_uart_msg (void *buf, size_t len, const char *fmt, ...)
{
	va_list args;
	int tx_len;
	int ret;

	if ((NULL == buf) || (0 == len) || (NULL == fmt)) {
		return -EINVAL;
	}

	va_start(args, fmt);
	tx_len = vsnprintf(buf, len, fmt, args);
	va_end(args);

	if (tx_len < 0) {
		return tx_len;
	}
	if (((size_t)tx_len >= len) || (tx_len > sizeof(quePutData.data))) {
		return -ENOMEM;
	}

	k_mutex_lock(&modem_rsp_mutex, K_FOREVER);

	k_sem_reset(&modem_rsp_sem);
	modem_rsp.buf = buf;
	modem_rsp.size = len;
	modem_rsp.len = 0;
	modem_rsp.pending = true;

	memcpy (quePutData.data, buf, tx_len);
	quePutData.length = tx_len;
	if (0 != k_msgq_put (&uart_queue, &quePutData, K_NO_WAIT)) {
		modem_rsp.pending = false;
		k_mutex_unlock(&modem_rsp_mutex);
		return -ENOBUFS;
	}
	uart_irq_tx_enable(lpuart);

	if (0 == k_sem_take(&modem_rsp_sem, MODEM_UART_RSP_TIMEOUT)) {
		ret = modem_rsp.len;
	} else {
		modem_rsp.pending = false;
		ret = -ETIMEDOUT;
	}

	k_mutex_unlock(&modem_rsp_mutex);

	return ret;
}

void set_bt_enabled (void)
{
	bt_is_enabled = true;
}

void set_bt_disabled (void)
{
	bt_is_enabled = false;
}

static void bt_nus_rx_callback(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	*((char *)data + len) = '\0';

	if (0 == strncmp ("lte ", data, 4))
	{
		/* Handling LTE command */
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
			shell_execute_cmd(NULL, data + 4);
		}
	}
}


static void bt_nus_tx_callback(struct bt_conn *conn)
{
	
}

static void send_enabled_callback(enum bt_nus_send_status status)
{
	
}

int cust_bt_nus_init(void)
{
	struct bt_nus_cb callbacks = {
		.received = bt_nus_rx_callback,
		.sent = bt_nus_tx_callback,
		.send_enabled = send_enabled_callback
	};

	return bt_nus_init(&callbacks);
}

static void process_lpuart_rx(const struct uart_rx_item *item)
{
	if (0 == strncmp ("ble ", item->data, 4))
	{
		/* Shell command for BLE */
		shell_execute_cmd (NULL, (item->data + 4));
	}
	else
	{
		if (true == modem_rsp.pending)
		{
			/* Response for nrf_modem_uart_msg(): hand over to waiting thread */
			size_t cpy = MIN((size_t)item->length, modem_rsp.size - 1);
			memcpy (modem_rsp.buf, item->data, cpy);
			modem_rsp.buf[cpy] = '\0';
			modem_rsp.len = cpy;
			modem_rsp.pending = false;
			k_sem_give(&modem_rsp_sem);
			return;
		}

		if (true == bt_is_enabled)
		{
			/* Message for BLE central */
			bt_nus_send (NULL, item->data, item->length);
		}
		else
		{
			/* Message for LTE or other non-BLE commands */
			if (0 == strncmp ("+uuid: ", item->data, 7))
			{
				uuid_str_to_bytes(item->data + 7, uuidBytes);
				/* Copy the UUID bytes to the advertising manufacturer data */
				memcpy (adv_mfg_data.uuid_bytes, uuidBytes, sizeof(uuidBytes));
			}
		}
	}
}

void lpuart_rx_work_handler(struct k_work *work)
{
	struct uart_rx_item item;

	ARG_UNUSED(work);

	while (k_msgq_get(&uart_rx_queue, &item, K_NO_WAIT) == 0) {
		process_lpuart_rx(&item);
	}
}

K_WORK_DEFINE(lpuart_rx_work, lpuart_rx_work_handler);

void lpuart_irq_handler(const struct device *dev, void *context)
{
	ARG_UNUSED(context);

	if (!uart_irq_update(dev)) {
		return;
	}

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
	while (uart_irq_rx_ready(dev)) {
		struct uart_rx_item item;
		int len = uart_fifo_read(dev, item.data, sizeof(item.data) - 1);

		if (len <= 0) {
			LOG_ERR("Failed to read LPUART RX FIFO (err: %d)", len);
			break;
		}

		item.data[len] = '\0';
		item.length = len;
		if (k_msgq_put(&uart_rx_queue, &item, K_NO_WAIT) != 0) {
			LOG_ERR("LPUART RX queue full; dropped %d bytes", len);
		} else {
			k_work_submit(&lpuart_rx_work);
		}
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
