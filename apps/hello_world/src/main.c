/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <zephyr/kernel.h>

int main(void)
{
	printf("Hello World! %s\n", CONFIG_BOARD_TARGET);
	uint8_t counter = 0;
	while(1){
		printf("%d\r\n", counter++);
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
