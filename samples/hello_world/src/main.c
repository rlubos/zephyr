/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/ieee802154_mgmt.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/net/net_if.h>

#include <nrf_802154.h>

void main(void)
{
	usb_enable(NULL);

	uint16_t pan_id = 0x1234;
	uint16_t channel = 26;

	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(IEEE802154));
	if (!iface) {
		printk("No IEEE 802.15.4 interface found!\n");
		return;
	}

	net_if_flag_clear(iface, NET_IF_IPV6);

	net_mgmt(NET_REQUEST_IEEE802154_SET_PAN_ID, iface, &pan_id, sizeof(uint16_t));
	net_mgmt(NET_REQUEST_IEEE802154_SET_CHANNEL, iface, &channel, sizeof(uint16_t));

	net_if_up(iface);

	while(true) {
		printk("Hello World! %s\n", CONFIG_BOARD);
		k_msleep(1000);
	}
}
