#ifndef DRIVERS_RS_485_H_
#define DRIVERS_RS_485_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

__subsystem struct rs_485_driver_api {
	int (*transmit)(const struct device *dev, const uint8_t *data, size_t len);
	int (*callback_register)(const struct device *dev,
				 uart_irq_callback_user_data_t callback, void *user_data);
};

__syscall int rs485_transmit(const struct device *dev, const uint8_t *data, size_t len);

static inline int z_impl_rs485_transmit(const struct device *dev, const uint8_t *data,
					size_t len)
{
	__ASSERT_NO_MSG(dev != NULL && dev->api != NULL);
	const struct rs_485_driver_api *api = (const struct rs_485_driver_api *)dev->api;

	return api->transmit(dev, data, len);
}

/* Not a syscall: function pointers can't be safely passed from user mode */
static inline int rs485_callback_register(const struct device *dev,
					  uart_irq_callback_user_data_t callback,
					  void *user_data)
{
	__ASSERT_NO_MSG(dev != NULL && dev->api != NULL);
	const struct rs_485_driver_api *api = (const struct rs_485_driver_api *)dev->api;

	return api->callback_register(dev, callback, user_data);
}

#ifdef __cplusplus
}
#endif

#include <zephyr/syscalls/rs_485.h>

#endif /* DRIVERS_RS_485_H_ */