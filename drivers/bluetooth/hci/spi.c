/* spi.c - SPI based Bluetooth driver */

#define DT_DRV_COMPAT zephyr_bt_hci_spi

/*
 * Copyright (c) 2017 Linaro Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth/hci_driver.h>

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_driver);

#define HCI_CMD			0x01
#define HCI_ACL			0x02
#define HCI_SCO			0x03
#define HCI_EVT			0x04

/* Special Values */
#define SPI_WRITE		0x0A
#define SPI_READ		0x0B
#define READY_NOW		0x02

#define EVT_BLUE_INITIALIZED	0x01

/* Offsets */
#define STATUS_HEADER_READY	0
#define STATUS_HEADER_TOREAD	3
#define STATUS_HEADER_TOWRITE	1

#define PACKET_TYPE		0
#define EVT_HEADER_TYPE		0
#define EVT_HEADER_EVENT	1
#define EVT_HEADER_SIZE		2
#define EVT_LE_META_SUBEVENT	3
#define EVT_VENDOR_CODE_LSB	3
#define EVT_VENDOR_CODE_MSB	4

#define CMD_OGF			1
#define CMD_OCF			2

/* Max SPI buffer length for transceive operations.
 *
 * Buffer size needs to be at least the size of the larger RX/TX buffer
 * required by the SPI slave, as the legacy spi_transceive requires both RX/TX
 * to be the same length. Size also needs to be compatible with the
 * slave device used (e.g. nRF5X max buffer length for SPIS is 255).
 */
#define SPI_MAX_MSG_LEN		255 /* As defined by X-NUCLEO-IDB04A1 BSP */

#define DATA_DELAY_US DT_INST_PROP(0, controller_data_delay_us)

/* Single byte header denoting the buffer type */
#define H4_HDR_SIZE 1

/* Maximum L2CAP MTU that can fit in a single packet */
#define MAX_MTU (SPI_MAX_MSG_LEN - H4_HDR_SIZE - BT_L2CAP_HDR_SIZE - BT_HCI_ACL_HDR_SIZE)

#if CONFIG_BT_L2CAP_TX_MTU > MAX_MTU
#warning CONFIG_BT_L2CAP_TX_MTU is too large and can result in packets that cannot \
	be transmitted across this HCI link
#endif /* CONFIG_BT_L2CAP_TX_MTU > MAX_MTU */

static uint8_t rxmsg[SPI_MAX_MSG_LEN];
static uint8_t txmsg[SPI_MAX_MSG_LEN];

static const struct gpio_dt_spec irq_gpio = GPIO_DT_SPEC_INST_GET(0, irq_gpios);
static const struct gpio_dt_spec rst_gpio = GPIO_DT_SPEC_INST_GET(0, reset_gpios);

static struct gpio_callback	gpio_cb;

static K_SEM_DEFINE(sem_initialised, 0, 1);
static K_SEM_DEFINE(sem_request, 0, 1);
static K_SEM_DEFINE(sem_busy, 1, 1);

static K_KERNEL_STACK_DEFINE(spi_rx_stack, CONFIG_BT_DRV_RX_STACK_SIZE);
static struct k_thread spi_rx_thread_data;

#if defined(CONFIG_BT_BLUENRG_ACI)
#define BLUENRG_ACI_WRITE_CONFIG_DATA       BT_OP(BT_OGF_VS, 0x000C)
#define BLUENRG_CONFIG_PUBADDR_OFFSET       0x00
#define BLUENRG_CONFIG_PUBADDR_LEN          0x06
#define BLUENRG_CONFIG_LL_ONLY_OFFSET       0x2C
#define BLUENRG_CONFIG_LL_ONLY_LEN          0x01

static int bt_spi_send_aci_config(uint8_t offset, const uint8_t *value, size_t value_len);
#endif /* CONFIG_BT_BLUENRG_ACI */

static const struct spi_dt_spec bus = SPI_DT_SPEC_INST_GET(
	0, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8), 0);

static struct spi_buf spi_tx_buf;
static struct spi_buf spi_rx_buf;
static const struct spi_buf_set spi_tx = {
	.buffers = &spi_tx_buf,
	.count = 1
};
static const struct spi_buf_set spi_rx = {
	.buffers = &spi_rx_buf,
	.count = 1
};

static inline int bt_spi_transceive(void *tx, uint32_t tx_len,
				    void *rx, uint32_t rx_len)
{
	spi_tx_buf.buf = tx;
	spi_tx_buf.len = (size_t)tx_len;
	spi_rx_buf.buf = rx;
	spi_rx_buf.len = (size_t)rx_len;
	return spi_transceive_dt(&bus, &spi_tx, &spi_rx);
}

static inline uint16_t bt_spi_get_cmd(uint8_t *msg)
{
	return (msg[CMD_OCF] << 8) | msg[CMD_OGF];
}

static inline uint16_t bt_spi_get_evt(uint8_t *msg)
{
	return (msg[EVT_VENDOR_CODE_MSB] << 8) | msg[EVT_VENDOR_CODE_LSB];
}

static void bt_spi_isr(const struct device *unused1,
		       struct gpio_callback *unused2,
		       uint32_t unused3)
{
	LOG_DBG("");

	k_sem_give(&sem_request);
}

static bool bt_spi_handle_vendor_evt(uint8_t *msg)
{
	bool handled = false;

	switch (bt_spi_get_evt(msg)) {
	case EVT_BLUE_INITIALIZED: {
		k_sem_give(&sem_initialised);
#if defined(CONFIG_BT_BLUENRG_ACI)
		/* force BlueNRG to be on controller mode */
		uint8_t data = 1;

		bt_spi_send_aci_config(BLUENRG_CONFIG_LL_ONLY_OFFSET, &data, 1);
#endif
		handled = true;
	}
	default:
		break;
	}
	return handled;
}

#define IS_IRQ_HIGH gpio_pin_get_dt(&irq_gpio)

#if DT_HAS_COMPAT_STATUS_OKAY(st_hci_spi_v1)
/* Define a limit when reading IRQ high */
/* It can be required to be increased for */
/* some particular cases. */
#define IRQ_HIGH_MAX_READ 3
/* On BlueNRG-MS, host is expected to read */
/* as long as IRQ pin is high */
#define READ_CONDITION IS_IRQ_HIGH
/* We cannot retry write data without reading again the header */
#define WRITE_DATA_CONDITION(...) true

static void assert_cs(void)
{
	gpio_pin_set_dt(&bus.config.cs.gpio, 0);
	gpio_pin_set_dt(&bus.config.cs.gpio, 1);
}

static void release_cs(bool data_transaction)
{
	ARG_UNUSED(data_transaction);
	gpio_pin_set_dt(&bus.config.cs.gpio, 0);
}

static int bt_spi_get_header(uint8_t op, uint16_t *size)
{
	uint8_t header_master[5] = {op, 0, 0, 0, 0};
	uint8_t header_slave[5];
	uint8_t size_offset, attempts;
	int ret;

	if (op == SPI_READ) {
		if (!IS_IRQ_HIGH) {
			*size = 0;
			return 0;
		}
		size_offset = STATUS_HEADER_TOREAD;
	} else if (op == SPI_WRITE) {
		size_offset = STATUS_HEADER_TOWRITE;
	} else {
		return -EINVAL;
	}
	attempts = IRQ_HIGH_MAX_READ;
	do {
		if (op == SPI_READ) {
			/* Keep checking that IRQ is still high, if we need to read */
			if (!IS_IRQ_HIGH) {
				*size = 0;
				return 0;
			}
		}
		assert_cs();
		ret = bt_spi_transceive(header_master, 5, header_slave, 5);
		if (ret) {
			/* SPI transaction failed */
			break;
		}

		*size = (header_slave[STATUS_HEADER_READY] == READY_NOW) ?
				header_slave[size_offset] : 0;
		attempts--;
	} while ((*size == 0) && attempts);

	return ret;
}

#elif DT_HAS_COMPAT_STATUS_OKAY(st_hci_spi_v2)

#define READ_CONDITION false
/* We cannot retry writing data without reading the header again */
#define WRITE_DATA_CONDITION(...) true

static void assert_cs(uint16_t delay)
{
	gpio_pin_set_dt(&bus.config.cs.gpio, 0);
	if (delay) {
		k_sleep(K_USEC(delay));
	}
	gpio_pin_set_dt(&bus.config.cs.gpio, 1);
	gpio_pin_interrupt_configure_dt(&irq_gpio, GPIO_INT_DISABLE);
}

static void release_cs(bool data_transaction)
{
	/* Consume possible event signals */
	while (k_sem_take(&sem_request, K_NO_WAIT) == 0) {
	}
	if (data_transaction) {
		/* Wait for IRQ to become low only when data phase has been performed */
		while (IS_IRQ_HIGH) {
		}
	}
	gpio_pin_interrupt_configure_dt(&irq_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_pin_set_dt(&bus.config.cs.gpio, 0);
}

static int bt_spi_get_header(uint8_t op, uint16_t *size)
{
	uint8_t header_master[5] = {op, 0, 0, 0, 0};
	uint8_t header_slave[5];
	uint16_t cs_delay;
	uint8_t size_offset;
	int ret;

	if (op == SPI_READ) {
		if (!IS_IRQ_HIGH) {
			*size = 0;
			return 0;
		}
		cs_delay = 0;
		size_offset = STATUS_HEADER_TOREAD;
	} else if (op == SPI_WRITE) {
		/* To make sure we have a minimum delay from previous release cs */
		cs_delay = 100;
		size_offset = STATUS_HEADER_TOWRITE;
	} else {
		return -EINVAL;
	}

	assert_cs(cs_delay);
	/* Wait up to a maximum time of 100 ms */
	if (!WAIT_FOR(IS_IRQ_HIGH, 100000, k_usleep(100))) {
		LOG_ERR("IRQ pin did not raise");
		return -EIO;
	}

	ret = bt_spi_transceive(header_master, 5, header_slave, 5);
	*size = header_slave[size_offset] | (header_slave[size_offset + 1] << 8);
	return ret;
}
/* Other Boards */
#else

#define release_cs(...)
#define READ_CONDITION false
#define WRITE_DATA_CONDITION(ret, rx_first) (rx_first != 0U || ret)

static int bt_spi_get_header(uint8_t op, uint16_t *size)
{
	uint8_t header_master[5] = {op, 0, 0, 0, 0};
	uint8_t header_slave[5];
	bool reading = (op == SPI_READ);
	bool loop_cond;
	uint8_t size_offset;
	int ret;

	if (!(op == SPI_READ || op == SPI_WRITE)) {
		return -EINVAL;
	}
	if (reading) {
		size_offset = STATUS_HEADER_TOREAD;
	}

	do {
		ret = bt_spi_transceive(header_master, 5, header_slave, 5);
		if (ret) {
			break;
		}
		if (reading) {
			/* When reading, keep looping if read buffer is not valid */
			loop_cond = ((header_slave[STATUS_HEADER_TOREAD] == 0U) ||
					(header_slave[STATUS_HEADER_TOREAD] == 0xFF));
		} else {
			/* When writing, keep looping if all bytes are zero */
			loop_cond = ((header_slave[1] | header_slave[2] | header_slave[3] |
					header_slave[4]) == 0U);
		}
	} while ((header_slave[STATUS_HEADER_READY] != READY_NOW) || loop_cond);

	*size = (reading ? header_slave[size_offset] : SPI_MAX_MSG_LEN);

	return ret;
}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_hci_spi_v1) */

#if defined(CONFIG_BT_BLUENRG_ACI)
static int bt_spi_send_aci_config(uint8_t offset, const uint8_t *value, size_t value_len)
{
	struct net_buf *buf;
	uint8_t *cmd_data;
	size_t data_len = 2 + value_len;

	buf = bt_hci_cmd_create(BLUENRG_ACI_WRITE_CONFIG_DATA, data_len);
	if (!buf) {
		return -ENOBUFS;
	}

	cmd_data = net_buf_add(buf, data_len);
	cmd_data[0] = offset;
	cmd_data[1] = value_len;
	memcpy(&cmd_data[2], value, value_len);

	return bt_hci_cmd_send(BLUENRG_ACI_WRITE_CONFIG_DATA, buf);
}

static int bt_spi_bluenrg_setup(const struct bt_hci_setup_params *params)
{
	int ret;
	const bt_addr_t *addr = &params->public_addr;

	if (!bt_addr_eq(addr, BT_ADDR_NONE) && !bt_addr_eq(addr, BT_ADDR_ANY)) {
		ret = bt_spi_send_aci_config(
			BLUENRG_CONFIG_PUBADDR_OFFSET,
			addr->val, sizeof(addr->val));

		if (ret != 0) {
			LOG_ERR("Failed to set BlueNRG public address (%d)", ret);
			return ret;
		}
	}

	return 0;
}
#endif /* CONFIG_BT_BLUENRG_ACI */

static struct net_buf *bt_spi_rx_buf_construct(uint8_t *msg)
{
	bool discardable = false;
	k_timeout_t timeout = K_FOREVER;
	struct bt_hci_acl_hdr acl_hdr;
	struct net_buf *buf;
	int len;

	switch (msg[PACKET_TYPE]) {
	case HCI_EVT:
		switch (msg[EVT_HEADER_EVENT]) {
		case BT_HCI_EVT_VENDOR:
			/* Run event through interface handler */
			if (bt_spi_handle_vendor_evt(msg)) {
				return NULL;
			}
			/* Event has not yet been handled */
			__fallthrough;
		default:
			if (msg[EVT_HEADER_EVENT] == BT_HCI_EVT_LE_META_EVENT &&
			    (msg[EVT_LE_META_SUBEVENT] == BT_HCI_EVT_LE_ADVERTISING_REPORT)) {
				discardable = true;
				timeout = K_NO_WAIT;
			}
			buf = bt_buf_get_evt(msg[EVT_HEADER_EVENT],
					     discardable, timeout);
			if (!buf) {
				LOG_DBG("Discard adv report due to insufficient buf");
				return NULL;
			}
		}

		len = sizeof(struct bt_hci_evt_hdr) + msg[EVT_HEADER_SIZE];
		if (len > net_buf_tailroom(buf)) {
			LOG_ERR("Event too long: %d", len);
			net_buf_unref(buf);
			return NULL;
		}
		net_buf_add_mem(buf, &msg[1], len);
		break;
	case HCI_ACL:
		buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);
		memcpy(&acl_hdr, &msg[1], sizeof(acl_hdr));
		len = sizeof(acl_hdr) + sys_le16_to_cpu(acl_hdr.len);
		if (len > net_buf_tailroom(buf)) {
			LOG_ERR("ACL too long: %d", len);
			net_buf_unref(buf);
			return NULL;
		}
		net_buf_add_mem(buf, &msg[1], len);
		break;
	default:
		LOG_ERR("Unknown BT buf type %d", msg[0]);
		return NULL;
	}

	return buf;
}

static void bt_spi_rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct net_buf *buf;
	uint16_t size = 0U;
	int ret;

	(void)memset(&txmsg, 0xFF, SPI_MAX_MSG_LEN);
	while (true) {

		/* Wait for interrupt pin to be active */
		k_sem_take(&sem_request, K_FOREVER);

		LOG_DBG("");

		do {
			/* Wait for SPI bus to be available */
			k_sem_take(&sem_busy, K_FOREVER);
			ret = bt_spi_get_header(SPI_READ, &size);

			/* Delay here is rounded up to next tick */
			k_sleep(K_USEC(DATA_DELAY_US));
			/* Read data */
			if (ret == 0 && size != 0) {
				do {
					ret = bt_spi_transceive(&txmsg, size,
								&rxmsg, size);
					if (rxmsg[0] == 0U) {
						/* Consider increasing controller-data-delay-us
						 * if this message is extremely common.
						 */
						LOG_DBG("Controller not ready for SPI transaction "
							"of %d bytes", size);
					}
				} while (rxmsg[0] == 0U && ret == 0);
			}

			release_cs(size > 0);

			k_sem_give(&sem_busy);

			if (ret || size == 0) {
				if (ret) {
					LOG_ERR("Error %d", ret);
				}
				continue;
			}

			LOG_HEXDUMP_DBG(rxmsg, size, "SPI RX");

			/* Construct net_buf from SPI data */
			buf = bt_spi_rx_buf_construct(rxmsg);
			if (buf) {
				/* Handle the received HCI data */
				bt_recv(buf);
			}
		} while (READ_CONDITION);
	}
}

static int bt_spi_send(struct net_buf *buf)
{
	uint16_t size;
	uint8_t rx_first[1];
	int ret;

	LOG_DBG("");

	/* Buffer needs an additional byte for type */
	if (buf->len >= SPI_MAX_MSG_LEN) {
		LOG_ERR("Message too long (%d)", buf->len);
		return -EINVAL;
	}

	/* Wait for SPI bus to be available */
	k_sem_take(&sem_busy, K_FOREVER);

	switch (bt_buf_get_type(buf)) {
	case BT_BUF_ACL_OUT:
		net_buf_push_u8(buf, HCI_ACL);
		break;
	case BT_BUF_CMD:
		net_buf_push_u8(buf, HCI_CMD);
		break;
	default:
		LOG_ERR("Unsupported type");
		k_sem_give(&sem_busy);
		return -EINVAL;
	}

	ret = bt_spi_get_header(SPI_WRITE, &size);
	size = MIN(buf->len, size);

	if (size < buf->len) {
		LOG_WRN("Unable to write full data, skipping");
		size = 0;
		ret = -ECANCELED;
	}

	if (!ret) {
		/* Delay here is rounded up to next tick */
		k_sleep(K_USEC(DATA_DELAY_US));
		/* Transmit the message */
		while (true) {
			ret = bt_spi_transceive(buf->data, size,
						rx_first, 1);
			if (WRITE_DATA_CONDITION(ret, rx_first[0])) {
				break;
			}
			/* Consider increasing controller-data-delay-us
			 * if this message is extremely common.
			 */
			LOG_DBG("Controller not ready for SPI transaction of %d bytes", size);
		}
	}

	release_cs(size > 0);

	k_sem_give(&sem_busy);

	if (ret) {
		LOG_ERR("Error %d", ret);
		goto out;
	}

	LOG_HEXDUMP_DBG(buf->data, buf->len, "SPI TX");

#if (DT_HAS_COMPAT_STATUS_OKAY(st_hci_spi_v1) || DT_HAS_COMPAT_STATUS_OKAY(st_hci_spi_v2))
	/*
	 * Since a RESET has been requested, the chip will now restart.
	 * Unfortunately the BlueNRG will reply with "reset received" but
	 * since it does not send back a NOP, we have no way to tell when the
	 * RESET has actually taken place.  Instead, we use the vendor command
	 * EVT_BLUE_INITIALIZED as an indication that it is safe to proceed.
	 */
	if (bt_spi_get_cmd(buf->data) == BT_HCI_OP_RESET) {
		k_sem_take(&sem_initialised, K_FOREVER);
	}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_hci_spi_v1) || DT_HAS_COMPAT_STATUS_OKAY(st_hci_spi_v2) */
out:
	net_buf_unref(buf);

	return ret;
}

static int bt_spi_open(void)
{
	int err;

	/* Configure RST pin and hold BLE in Reset */
	err = gpio_pin_configure_dt(&rst_gpio, GPIO_OUTPUT_ACTIVE);
	if (err) {
		return err;
	}

	/* Configure IRQ pin and the IRQ call-back/handler */
	err = gpio_pin_configure_dt(&irq_gpio, GPIO_INPUT);
	if (err) {
		return err;
	}

	gpio_init_callback(&gpio_cb, bt_spi_isr, BIT(irq_gpio.pin));
	err = gpio_add_callback(irq_gpio.port, &gpio_cb);
	if (err) {
		return err;
	}

	/* Enable the interrupt line */
	err = gpio_pin_interrupt_configure_dt(&irq_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		return err;
	}

	/* Take BLE out of reset */
	k_sleep(K_MSEC(DT_INST_PROP_OR(0, reset_assert_duration_ms, 0)));
	gpio_pin_set_dt(&rst_gpio, 0);

	/* Start RX thread */
	k_thread_create(&spi_rx_thread_data, spi_rx_stack,
			K_KERNEL_STACK_SIZEOF(spi_rx_stack),
			bt_spi_rx_thread, NULL, NULL, NULL,
			K_PRIO_COOP(CONFIG_BT_DRIVER_RX_HIGH_PRIO),
			0, K_NO_WAIT);

	/* Device will let us know when it's ready */
	k_sem_take(&sem_initialised, K_FOREVER);

	return 0;
}

static const struct bt_hci_driver drv = {
	.name		= DEVICE_DT_NAME(DT_DRV_INST(0)),
	.bus		= BT_HCI_DRIVER_BUS_SPI,
#if defined(CONFIG_BT_BLUENRG_ACI)
	.quirks		= BT_QUIRK_NO_RESET,
	.setup          = bt_spi_bluenrg_setup,
#endif /* CONFIG_BT_BLUENRG_ACI */
	.open		= bt_spi_open,
	.send		= bt_spi_send,
};

static int bt_spi_init(void)
{

	if (!spi_is_ready_dt(&bus)) {
		LOG_ERR("SPI device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&irq_gpio)) {
		LOG_ERR("IRQ GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&rst_gpio)) {
		LOG_ERR("Reset GPIO device not ready");
		return -ENODEV;
	}

	bt_hci_driver_register(&drv);


	LOG_DBG("BT SPI initialized");

	return 0;
}

SYS_INIT(bt_spi_init, POST_KERNEL, CONFIG_BT_SPI_INIT_PRIORITY);
