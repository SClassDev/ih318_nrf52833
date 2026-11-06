/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Nordic UART Bridge Service (NUS) sample
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/shell/shell.h>
#include <bluetooth/services/nus.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_ficr.h> // Header für den Zugriff auf die nRF-Hardware-Register
#include "main.h"

// #include <shell/shell_bt_nus.h>
#include <stdio.h>
#include <inttypes.h>

#include "shell_custom.h"
#include <zephyr/drivers/uart.h>

LOG_MODULE_REGISTER(app);

#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN	        (sizeof(DEVICE_NAME) - 1)



/* Bit definitions of hardware features */
#define HW_PulseCapacitor                    0x00000001      /* If device has C300 */
#define HW_Bluetooth                         0x00000002      /* If device has bluetooth interface */
#define HW_BME280                            0x00000004      /* If device has Bosch BME280 sensor */
#define HW_ExternalFlash                     0x00000008      /* If device has external flash */
#define HW_eSim                              0x00000010      /* If device has embedded SIM card */
#define DEVICE_TypeIH318 0x00010000      /* ih318 HW board revision 1 */
#define DEVICE_HardwareFeatures HW_BME280 | HW_ExternalFlash
#define HW_Version_1 0x00000100      /* Version 1 */
#define DEVICE_DESCRIPTOR DEVICE_TypeIH318 | DEVICE_HardwareFeatures | HW_Version_1


adv_mfg_data_type adv_mfg_data = { COMPANY_ID_CODE, DEVICE_DESCRIPTOR, {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF} };
	
/** @brief LPUART device instance */
const struct device *lpuart = DEVICE_DT_GET(DT_NODELABEL(lpuart)); 

static struct bt_conn *current_conn;
static struct k_work adv_work;
static struct k_work_delayable nus_send_work;
static bt_addr_le_t nus_peer_addr;

uint8_t uuidBytes[16];
uint16_t adcBytes[2];
uint8_t powerSource;	

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, (unsigned char *)&adv_mfg_data, sizeof(adv_mfg_data)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/* Command shell registration */
static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
        ARG_UNUSED(argc);
        ARG_UNUSED(argv);

		bt_nus_send(NULL, "+version BLE 1.8.3\r\n", strlen("+version BLE 1.8.3\r\n"));

        return 0;
}
SHELL_CMD_REGISTER(version, NULL, "Show kernel version", cmd_version);

static int cmd_uuid(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	char uuid_string[46];

	snprintk(uuid_string, sizeof(uuid_string),
		 "+uuid: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\r\n",
		 uuidBytes[0], uuidBytes[1], uuidBytes[2], uuidBytes[3],
		 uuidBytes[4], uuidBytes[5], uuidBytes[6], uuidBytes[7],
		 uuidBytes[8], uuidBytes[9], uuidBytes[10], uuidBytes[11],
		 uuidBytes[12], uuidBytes[13], uuidBytes[14], uuidBytes[15]);
	bt_nus_send(NULL, uuid_string, strlen(uuid_string));

	return 0;
}
SHELL_CMD_REGISTER(uuid, NULL, "Show device UUID", cmd_uuid);

/* 1. Funktion zur Berechnung des Passkeys aus der Device ID */
uint32_t generate_passkey_from_device_id(void) {
    
	uint32_t calculated_passkey = 0;
	
	// Die nRF52 Device ID ist aufgeteilt in zwei 32-Bit Register
    uint32_t dev_id_0 = nrf_ficr_deviceid_get(NRF_FICR, 0);
    uint32_t dev_id_1 = nrf_ficr_deviceid_get(NRF_FICR, 1);

    // Verknüpfung der beiden IDs (z.B. per XOR)
    uint32_t combined_id = dev_id_0 ^ dev_id_1;

    // Ein BLE-Passkey MUSS exakt zwischen 000000 und 999999 liegen.
    // Modulo 1.000.000 erzwingt genau diesen 6-stelligen Bereich.
    calculated_passkey = combined_id % 1000000;

    printk("Generierter Hardware-Passkey: %06u\n", calculated_passkey);

	return (calculated_passkey);
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	printk("Advertising successfully started\n");
}

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void nus_send_work_handler(struct k_work *work)
{
	static const char message[] = "SEED\r\n";
	struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &nus_peer_addr);
	int err;

	if (!conn) {
		return;
	}

	err = bt_nus_send(conn, message, sizeof(message) - 1);
	bt_conn_unref(conn);

	if (err == -EINVAL) {
		k_work_reschedule(&nus_send_work, K_MSEC(250));
	} else if (err) {
		LOG_ERR("NUS send failed (err %d)", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	LOG_INF("Connected");
	current_conn = bt_conn_ref(conn);
	bt_addr_le_copy(&nus_peer_addr, bt_conn_get_dst(conn));

	set_bt_enabled ();
	k_work_reschedule(&nus_send_work, K_SECONDS(1));

	/*
	int security_err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (security_err) {
    	LOG_ERR("Security request failed: %d", security_err);
	}
    */
	// shell_bt_nus_enable(conn);
	// bt_nus_send(conn, "SEED", strlen("SEED"));
	
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));
	k_work_cancel_delayable(&nus_send_work);

	// shell_bt_nus_disable();
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
}

static void recycled_cb(void)
{
	printk("Connection object available from previous conn. Disconnect is complete!\n");
	advertising_start();
}

static char *log_addr(struct bt_conn *conn)
{
	static char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	return addr;
}

static void __attribute__((unused)) security_changed(struct bt_conn *conn,
						     bt_security_t level,
						     enum bt_security_err err)
{
	char *addr = log_addr(conn);

	if (!err) {
		LOG_INF("Security changed: %s level %u", addr, level);
	} else {
		LOG_INF("Security failed: %s level %u err %d %s", addr, level, err,
			bt_security_err_to_str(err));
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
	.recycled     = recycled_cb,
	COND_CODE_1(CONFIG_BT_SMP,
		(.security_changed = security_changed), ())
};

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	LOG_INF("Passkey for %s: %06u", log_addr(conn), passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	LOG_INF("Pairing cancelled: %s", log_addr(conn));
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("Pairing completed: %s, bonded: %d", log_addr(conn), bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_INF("Pairing failed conn: %s, reason %d %s", log_addr(conn), reason,
		bt_security_err_to_str(reason));
}

/* Define fixed passkey for authentication, Later from uuid */
#define FIXED_PASSKEY 123456U

static uint32_t auth_passkey(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	return FIXED_PASSKEY;
    // return generate_passkey_from_device_id();
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	// .app_passkey = auth_passkey,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};

static int setup_lpuart(const struct device *dev)
{
	int err;

	if (!device_is_ready(dev)) {
		LOG_ERR("LPUART device %s is not ready", dev->name);
		return -ENODEV;
	}

	err = uart_irq_callback_user_data_set(dev, lpuart_irq_handler, NULL);
	if (err) {
		LOG_ERR("Failed to register LPUART IRQ callback (err: %d)", err);
		return err;
	}

	uart_irq_rx_enable(dev);
	LOG_INF("LPUART IRQ callback registered on %s", dev->name);
	return 0;
}

 
int main(void)
{
	int err;
	char modem_response[64];
	

	/* Wait for 2 seconds before talking to the LTE modem */
	k_sleep(K_MSEC(2000));

	printk("Starting Bluetooth NUS shell transport sample\n");

	/* Initialize Bluetooth enabled flag */
	set_bt_disabled ();

	/* Setup LPUART for communication with LTE */
	err = setup_lpuart(lpuart);
	if (err) {
		return err;
	}
	
	/* Read the UUID from LTE before advertising. */
	err = nrf_modem_uart_msg(modem_response, sizeof(modem_response), "uuid");
	if (err < 0) {
		LOG_ERR("Failed to read UUID from LTE (err: %d)", err);
	} else if ((strncmp(modem_response, "+uuid: ", 7) != 0) ||
		   (uuid_str_to_bytes(modem_response + 7, uuidBytes) != 0)) {
		LOG_ERR("Invalid UUID response from LTE");
	} else {
		memcpy(adv_mfg_data.uuid_bytes, uuidBytes, sizeof(uuidBytes));
	}

	if (IS_ENABLED(CONFIG_BT_SMP)) {
		err = bt_conn_auth_cb_register(&conn_auth_callbacks);
		if (err) {
			printk("Failed to register authorization callbacks.\n");
			return 0;
		}

		err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
		if (err) {
			printk("Failed to register authorization info callbacks.\n");
			return 0;
		}
	}

	/* Must be initialized before enabling Bluetooth */
	err = cust_bt_nus_init(); 
	if (err) {
		LOG_ERR("Failed to initialize BT NUS shell (err: %d)", err);
		return 0;
	}

	/* Enable Bluetooth */
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("BLE enable failed (err: %d)", err);
		return 0;
	}

	/*
	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	if (err) {
		LOG_ERR("Failed to clear Bluetooth pairing information (err: %d)", err);
	} else {
		LOG_INF("Bluetooth pairing information cleared");
	}
	*/
	
	k_work_init(&adv_work, adv_work_handler);
	k_work_init_delayable(&nus_send_work, nus_send_work_handler);
	advertising_start();

	/* TEST 
	while (1)
	{
		// send_alive_message();
		// k_sleep(K_MSEC(3000));
		// bt_nus_send(NULL, "SEED\r\n", strlen("SEED\r\n"));
		// k_sleep(K_SECONDS(3));
	}
	*/
	k_sleep(K_FOREVER);
	return 0;
}
