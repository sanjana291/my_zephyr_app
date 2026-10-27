#define DT_DRV_COMPAT calixto_rs_485

#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "drivers/rs_485.h"

LOG_MODULE_REGISTER(rs_485_calixto, CONFIG_RS_485_LOG_LEVEL);

struct rs_485_config {
	const struct device *uart_dev;
	struct gpio_dt_spec gpio;
	uint32_t post_tx_delay_us;
};

struct rs_485_data {
	struct k_mutex lock;
};

static int rs_485_transmit(const struct device *dev, const uint8_t *data, size_t len)
{
	const struct rs_485_config *cfg = dev->config;
	struct rs_485_data *drv = dev->data;
	int ret;

	if (data == NULL && len > 0) {
		return -EINVAL;
	}

	k_mutex_lock(&drv->lock, K_FOREVER);

	LOG_DBG("Transmitting %zu bytes over RS-485", len);

	ret = gpio_pin_set_dt(&cfg->gpio, 1);
	if (ret < 0) {
		goto out;
	}

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(cfg->uart_dev, data[i]);
	}

	/* Let the last byte finish shifting out before releasing the bus */
	if (cfg->post_tx_delay_us > 0) {
		k_busy_wait(cfg->post_tx_delay_us);
	}

	ret = gpio_pin_set_dt(&cfg->gpio, 0);
out:
	k_mutex_unlock(&drv->lock);
	return ret;
}

static int rs_485_callback_register(const struct device *dev,
				    uart_irq_callback_user_data_t callback, void *user_data)
{
	const struct rs_485_config *cfg = dev->config;
	int ret;

	LOG_DBG("Registering callback for RS-485 events");

	ret = uart_irq_callback_user_data_set(cfg->uart_dev, callback, user_data);
	if (ret < 0) {
		return ret;
	}

	uart_irq_rx_enable(cfg->uart_dev);

	return 0;
}

static int rs_485_init(const struct device *dev)
{
	const struct rs_485_config *cfg = dev->config;
	struct rs_485_data *data = dev->data;
	int ret;

	LOG_DBG("Initializing RS-485 driver");

	k_mutex_init(&data->lock);

	if (!device_is_ready(cfg->uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->gpio)) {
		LOG_ERR("GPIO device not ready");
		return -ENODEV;
	}

	/* Start in receive mode */
	ret = gpio_pin_configure_dt(&cfg->gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure DE/RE pin (%d)", ret);
		return ret;
	}

	return 0;
}

static const struct rs_485_driver_api rs485_api = {
	.transmit = rs_485_transmit,
	.callback_register = rs_485_callback_register,
};

#define RS_485_DEVICE(inst)                                                                    \
	static struct rs_485_data rs_485_data_##inst;                                          \
	static const struct rs_485_config rs_485_config_##inst = {                             \
		.uart_dev = DEVICE_DT_GET(DT_INST_BUS(inst)),                                  \
		.gpio = GPIO_DT_SPEC_INST_GET(inst, rs_485_gpios),                             \
		.post_tx_delay_us = DT_INST_PROP(inst, post_tx_delay_us),                      \
	};                                                                                     \
	DEVICE_DT_INST_DEFINE(inst, rs_485_init, NULL, &rs_485_data_##inst,                    \
			      &rs_485_config_##inst, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
			      &rs485_api);

DT_INST_FOREACH_STATUS_OKAY(RS_485_DEVICE)