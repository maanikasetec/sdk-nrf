/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#if defined(CONFIG_MEMFAULT)
#include <memfault/metrics/metrics.h>
#include <memfault/ports/zephyr/http.h>
#include <memfault/core/data_packetizer.h>
#include <memfault/core/trace_event.h>
#include <memfault/ports/watchdog.h>
#endif

#define MODULE debug_module

#if defined(CONFIG_WATCHDOG_APPLICATION)
#include "watchdog_app.h"
#endif /* CONFIG_WATCHDOG_APPLICATION */
#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/cloud_module_event.h"
#include "events/data_module_event.h"
#include "events/sensor_module_event.h"
#include "events/util_module_event.h"
#include "events/gnss_module_event.h"
#include "events/modem_module_event.h"
#include "events/ui_module_event.h"
#include "events/debug_module_event.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_DEBUG_MODULE_LOG_LEVEL);

struct debug_msg_data {
	union {
		struct cloud_module_event cloud;
		struct util_module_event util;
		struct ui_module_event ui;
		struct sensor_module_event sensor;
		struct data_module_event data;
		struct app_module_event app;
		struct gnss_module_event gnss;
		struct modem_module_event modem;
	} module;
};

/* Forward declarations. */
static void message_handler(struct debug_msg_data *msg);

#if defined(CONFIG_MEMFAULT)
static K_SEM_DEFINE(mflt_internal_send_sem, 0, 1);

static void memfault_internal_send(void)
{
	while (true) {
		k_sem_take(&mflt_internal_send_sem, K_FOREVER);

		/* Because memfault_zephyr_port_post_data() can block for a long time we cannot
		 * call it from the system workqueue.
		 */
		memfault_zephyr_port_post_data();
	}
}

K_THREAD_DEFINE(mflt_send_thread, CONFIG_DEBUG_MODULE_MEMFAULT_THREAD_STACK_SIZE,
		memfault_internal_send, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

#endif /* if defined(CONFIG_MEMFAULT) */

static struct module_data self = {
	.name = "debug",
	.msg_q = NULL,
	.supports_shutdown = false,
};

/* Handlers */
static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_modem_module_event(aeh)) {
		struct modem_module_event *event = cast_modem_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.modem = *event
		};

		message_handler(&debug_msg);
	}

	if (is_cloud_module_event(aeh)) {
		struct cloud_module_event *event = cast_cloud_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.cloud = *event
		};

		message_handler(&debug_msg);
	}

	if (is_gnss_module_event(aeh)) {
		struct gnss_module_event *event = cast_gnss_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.gnss = *event
		};

		message_handler(&debug_msg);
	}

	if (is_sensor_module_event(aeh)) {
		struct sensor_module_event *event =
				cast_sensor_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.sensor = *event
		};

		message_handler(&debug_msg);
	}

	if (is_ui_module_event(aeh)) {
		struct ui_module_event *event = cast_ui_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.ui = *event
		};

		message_handler(&debug_msg);
	}

	if (is_app_module_event(aeh)) {
		struct app_module_event *event = cast_app_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.app = *event
		};

		message_handler(&debug_msg);
	}

	if (is_data_module_event(aeh)) {
		struct data_module_event *event = cast_data_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.data = *event
		};

		message_handler(&debug_msg);
	}

	if (is_util_module_event(aeh)) {
		struct util_module_event *event = cast_util_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.util = *event
		};

		message_handler(&debug_msg);
	}

	return false;
}

#if defined(CONFIG_MEMFAULT)
#if defined(CONFIG_WATCHDOG_APPLICATION)
static void watchdog_handler(const struct watchdog_evt *evt)
{
	int err;

	switch (evt->type) {
	case WATCHDOG_EVT_START:
		LOG_DBG("WATCHDOG_EVT_START");
		err = memfault_software_watchdog_enable();
		if (err) {
			LOG_ERR("memfault_software_watchdog_enable, error: %d", err);
		}
		break;
	case WATCHDOG_EVT_FEED:
		LOG_DBG("WATCHDOG_EVT_FEED");
		err = memfault_software_watchdog_feed();
		if (err) {
			LOG_ERR("memfault_software_watchdog_feed, error: %d", err);
		}
		break;
	case WATCHDOG_EVT_TIMEOUT_INSTALLED:
		LOG_DBG("WATCHDOG_EVT_TIMEOUT_INSTALLED");
		/* Set the software watchdog timeout slightly shorter than the actual watchdog
		 * timeout. This is to catch a timeout in advance so that Memfault can save
		 * coredump data before a reboot occurs.
		 */
		__ASSERT(evt->timeout > CONFIG_DEBUG_MODULE_MEMFAULT_WATCHDOG_DELTA_MS,
			 "Installed watchdog timeout is too small");

		err = memfault_software_watchdog_update_timeout(
						evt->timeout -
						CONFIG_DEBUG_MODULE_MEMFAULT_WATCHDOG_DELTA_MS);
		if (err) {
			LOG_ERR("memfault_software_watchdog_update_timeout, error: %d", err);
		}
		break;
	default:
		break;
	}
}
#endif /* defined(CONFIG_WATCHDOG_APPLICATION) */

/**
 * @brief Send Memfault data. Memfault data should always be sent over the internal HTTP transport
 *	  upon a connection to LTE. This is to not be dependent on a cloud connection for critical
 *	  data such as coredumps. For regular metric updates the transport can be selected by
 *	  setting CONFIG_DEBUG_MODULE_MEMFAULT_USE_EXTERNAL_TRANSPORT.
 *
 * @param[in] lte_connected Flag set when the debug module is notified that an LTE connection
 *			    has been established.
 */
static void send_memfault_data(bool lte_connected)
{
	bool data_available = memfault_packetizer_data_available();

	if (lte_connected && data_available) {
		k_sem_give(&mflt_internal_send_sem);
		return;
	}

#if defined(CONFIG_DEBUG_MODULE_MEMFAULT_USE_EXTERNAL_TRANSPORT)
	if (data_available) {
		uint8_t data[CONFIG_DEBUG_MODULE_MEMFAULT_CHUNK_SIZE_MAX];
		size_t len = sizeof(data);

		while (memfault_packetizer_get_chunk(data, &len)) {
			struct debug_module_event *debug_module_event = new_debug_module_event();

			debug_module_event->type = DEBUG_EVT_MEMFAULT_DATA_READY;
			debug_module_event->data.memfault.len = len;

			BUILD_ASSERT(sizeof(debug_module_event->data.memfault.buf) >= sizeof(data));
			memcpy(debug_module_event->data.memfault.buf, data,
			       sizeof(debug_module_event->data.memfault.buf));

			APP_EVENT_SUBMIT(debug_module_event);
		}
	}

	return;
#else
	if (data_available) {
		/* Offload sending of Memfault data to a dedicated thread. */
		k_sem_give(&mflt_internal_send_sem);
	}
#endif /* if defined(CONFIG_DEBUG_MODULE_MEMFAULT_USE_EXTERNAL_TRANSPORT) */
}

static void add_gnss_metrics(uint8_t satellites, uint32_t search_time,
			    enum gnss_module_event_type event)
{
	int err;

	switch (event) {
	case GNSS_EVT_DATA_READY:
		err = memfault_metrics_heartbeat_set_unsigned(
						MEMFAULT_METRICS_KEY(GnssTimeToFix),
						search_time);
		if (err) {
			LOG_ERR("Failed updating GnssTimeToFix metric, error: %d", err);
		}
		break;
	case GNSS_EVT_TIMEOUT:
		err = memfault_metrics_heartbeat_set_unsigned(
						MEMFAULT_METRICS_KEY(GnssTimeoutSearchTime),
						search_time);
		if (err) {
			LOG_ERR("Failed updating GnssTimeoutSearchTime metric, error: %d", err);
		}
		break;
	default:
		LOG_ERR("Unknown GNSS module event type");
		return;
	}

	err = memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(GnssSatellitesTracked),
						      satellites);
	if (err) {
		LOG_ERR("Failed updating GnssSatellitesTracked metric, error: %d", err);
	}

	memfault_metrics_heartbeat_debug_trigger();
}

static void memfault_handle_event(struct debug_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		/* Register callback for watchdog events. Used to attach Memfault software
		 * watchdog.
		 */
#if defined(CONFIG_WATCHDOG_APPLICATION)
		watchdog_register_handler(watchdog_handler);
#endif
	}

	/* Send Memfault data at the same time application data is sent to save overhead
	 * compared to having Memfault SDK trigger regular updates independently. All data
	 * should preferably be sent within the same LTE RRC connected window.
	 */
	if ((IS_EVENT(msg, data, DATA_EVT_DATA_SEND)) ||
	    (IS_EVENT(msg, data, DATA_EVT_DATA_SEND_BATCH))) {
		send_memfault_data(false);
		return;
	}

	/* When the device connects to LTE, the debug module will check if there is available
	 * Memfault data and send it before the application connects to cloud. If Memfault data
	 * has been stored prior to receiving an LTE connected event, there is the likelihood
	 * that the data is a coredump from a previous crash. This data must be prioritized and
	 * should not depend on a connection to cloud. Due to a limitation of maximum 1 TLS
	 * handshake occur at the time, this will cause the first cloud connection attempt to fail.
	 */
	if (IS_EVENT(msg, modem, MODEM_EVT_LTE_CONNECTED)) {
		send_memfault_data(true);
		return;
	}

	if ((IS_EVENT(msg, gnss, GNSS_EVT_TIMEOUT)) ||
	    (IS_EVENT(msg, gnss, GNSS_EVT_DATA_READY))) {
		add_gnss_metrics(msg->module.gnss.data.gnss.satellites_tracked,
				msg->module.gnss.data.gnss.search_time,
				msg->module.gnss.type);
		return;
	}
}
#endif /* defined(CONFIG_MEMFAULT) */

static void message_handler(struct debug_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		int err = module_start(&self);

		if (err) {
			LOG_ERR("Failed starting module, error: %d", err);
			SEND_ERROR(debug, DEBUG_EVT_ERROR, err);
		}

		/* Notify the rest of the application that it is connected to network
		 * when building for QEMU x86.
		 */
		if (IS_ENABLED(CONFIG_BOARD_QEMU_X86)) {
			{ SEND_EVENT(debug, DEBUG_EVT_QEMU_X86_INITIALIZED); }
			SEND_EVENT(debug, DEBUG_EVT_QEMU_X86_NETWORK_CONNECTED);
		}
	}

#if defined(CONFIG_MEMFAULT)
	memfault_handle_event(msg);
#endif
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, app_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, modem_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, cloud_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, gnss_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, ui_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, sensor_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, data_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, util_module_event);
