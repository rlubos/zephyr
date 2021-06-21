/*
 * Copyright (c) 2018 Texas Instruments, Incorporated
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief General WiFi Definitions
 */

#ifndef ZEPHYR_INCLUDE_NET_WIFI_H_
#define ZEPHYR_INCLUDE_NET_WIFI_H_

#include <net/ethernet.h>

enum wifi_security_type {
	WIFI_SECURITY_TYPE_NONE = 0,
	WIFI_SECURITY_TYPE_PSK,
};

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PSK_MAX_LEN 64

#define WIFI_CHANNEL_MAX 14
#define WIFI_CHANNEL_ANY 255

struct wifi_api {
	struct ethernet_api eth_api;

	/* Any WiFi specific functionality can be added here. */
};

#endif /* ZEPHYR_INCLUDE_NET_WIFI_H_ */
