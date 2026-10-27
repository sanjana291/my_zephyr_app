
/*
 * Copyright 2026 Calixto Systems Pvt Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || \
    !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "ADC io-channels are missing from devicetree"
#endif

#define ADC_SPEC(node_id, prop, idx) \
    ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channel[] = {
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, ADC_SPEC)
};

int main(void)
{
    int ret;
    uint16_t raw_value;

    struct adc_sequence sequence = {
        .buffer = &raw_value,
        .buffer_size = sizeof(raw_value),
    };

    printk("\nADC Test Application Started\n");

    if (!adc_is_ready_dt(&adc_channel[0])) {
        printk("ERROR: ADC controller not ready\n");
        return 0;
    }

    ret = adc_channel_setup_dt(&adc_channel[0]);
    if (ret < 0) {
        printk("ERROR: ADC channel setup failed: %d\n", ret);
        return 0;
    }

    while (1) {
        int32_t millivolts;

        ret = adc_sequence_init_dt(&adc_channel[0], &sequence);
        if (ret < 0) {
            printk("ADC sequence setup failed: %d\n", ret);
            k_sleep(K_SECONDS(1));
            continue;
        }

        ret = adc_read(adc_channel[0].dev, &sequence);
        if (ret < 0) {
            printk("ADC read failed: %d\n", ret);
        } else {
            printk("Raw ADC value: %u", raw_value);

            millivolts = raw_value;
            ret = adc_raw_to_millivolts_dt(
                &adc_channel[0], &millivolts);

            if (ret == 0) {
                printk(" | Voltage: %d mV\n", (int)millivolts);
            } else {
                printk(" | Voltage conversion unavailable\n");
            }
        }

        k_sleep(K_SECONDS(1));
    }

    return 0;
}