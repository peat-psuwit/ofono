/*
 *
 *  oFono - Open Source Telephony - RIL-based devices: Qualcomm multi-sim modems
 *
 *  Copyright (C) 2015 Ratchanan Srirattanamet.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#define OFONO_API_SUBJECT_TO_CHANGE

#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "ofono.h"

#include <grilreply.h>
#include <grilrequest.h>
#include <grilunsol.h>

#include "drivers/rilmodem/vendor.h"
#include "drivers/rilmodem/rilmodem.h"
#include "ril.h"

#define MAX_SIM_STATUS_RETRIES 15

/* this gives 30s for rild to initialize */
#define RILD_MAX_CONNECT_RETRIES 5
#define RILD_CONNECT_RETRY_TIME_S 5

char* RILD_CMD_SOCKET[2]={"/dev/socket/rild", "/dev/socket/rild1"};
char* GRIL_HEX_PREFIX[2]={"Device 0: ", "Device 1: "};

//Keep in sync with ril.c
struct ril_data {
	GRil *ril;
	enum ofono_ril_vendor vendor;
	int sim_status_retries;
	ofono_bool_t connected;
	ofono_bool_t ofono_online;
	int radio_state;
	struct ofono_sim *sim;
	struct ofono_radio_settings *radio_settings;
	int rild_connect_retries;
};

static void qcom_msim_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void qcom_msim_radio_state_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	int radio_state = g_ril_unsol_parse_radio_state_changed(rd->ril,
								message);

	if (radio_state != rd->radio_state) {

		ofono_info("%s: state: %s rd->ofono_online: %d",
				__func__,
				ril_radio_state_to_string(radio_state),
				rd->ofono_online);

		rd->radio_state = radio_state;

		switch (radio_state) {
		case RADIO_STATE_ON:

			if (rd->radio_settings == NULL) {
				struct ril_radio_settings_driver_data rs_data =
							{ rd->ril, modem };
				rd->radio_settings =
					ofono_radio_settings_create(modem,
						rd->vendor, RILMODEM, &rs_data);
			}

			break;

		case RADIO_STATE_UNAVAILABLE:
		case RADIO_STATE_OFF:

			/*
			 * If radio powers off asychronously, then
			 * assert, and let upstart re-start the stack.
			 */
			if (rd->ofono_online) {
				ofono_error("%s: radio self-powered off!",
						__func__);
				g_assert(FALSE);
			}
			break;
		default:
			/* Malformed parcel; no radio state == broken rild */
			g_assert(FALSE);
		}
	}
}

static void qcom_msim_connected(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_info("[%d,UNSOL]< %s", g_ril_get_slot(rd->ril),
		g_ril_unsol_request_to_string(rd->ril, message->req));

	/* TODO: need a disconnect function to restart things! */
	rd->connected = TRUE;

	DBG("calling set_powered(TRUE)");

	ofono_modem_set_powered(modem, TRUE);
}

static int qcom_msim_create_gril(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	int slot_id = ofono_modem_get_integer(modem, "Slot");

	ofono_info("Using %s as socket for slot %d.", RILD_CMD_SOCKET[slot_id], slot_id);
	rd->ril = g_ril_new(RILD_CMD_SOCKET[slot_id], OFONO_RIL_VENDOR_AOSP);

	/* NOTE: Since AT modems open a tty, and then call
	 * g_at_chat_new(), they're able to return -EIO if
	 * the first fails, and -ENOMEM if the second fails.
	 * in our case, we already return -EIO if the ril_new
	 * fails.  If this is important, we can create a ril_socket
	 * abstraction... ( probaby not a bad idea ).
	 */

	if (rd->ril == NULL) {
		ofono_error("g_ril_new() failed to create modem!");
		return -EIO;
	}
	g_ril_set_slot(rd->ril, slot_id);

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(rd->ril, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(rd->ril, qcom_msim_debug, GRIL_HEX_PREFIX[slot_id]);

	g_ril_register(rd->ril, RIL_UNSOL_RIL_CONNECTED,
			qcom_msim_connected, modem);

	g_ril_register(rd->ril, RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
			qcom_msim_radio_state_changed, modem);

	return 0;
}

static gboolean qcom_msim_reconnect_rild(gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_info("Trying to reconnect to rild...");

	if (rd->rild_connect_retries++ < RILD_MAX_CONNECT_RETRIES) {
		if (qcom_msim_create_gril(modem) < 0)
			return TRUE;
	} else {
		ofono_error("Exiting, can't connect to rild.");
		exit(0);
	}

	return FALSE;
}

static int qcom_msim_enable(struct ofono_modem *modem)
{
	int ret;

	DBG("");

	ret = qcom_msim_create_gril(modem);
	if (ret < 0)
		g_timeout_add_seconds(RILD_CONNECT_RETRY_TIME_S,
					qcom_msim_reconnect_rild, modem);

	return -EINPROGRESS;
}

static int qcom_msim_probe(struct ofono_modem *modem)
{
	return ril_create(modem, OFONO_RIL_VENDOR_QCOM_MSIM);
}

static struct ofono_modem_driver qcom_msim_driver = {
	.name = "qcom_msim",
	.probe = qcom_msim_probe,
	.remove = ril_remove,
	.enable = qcom_msim_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

/*
 * This plugin is a device plugin for Qualcomm's multi-sim device that use
 * RIL interface. The plugin 'rildev' is used to determine which RIL plugin
 * should be loaded based upon an environment variable.
 */
static int qcom_msim_init(void)
{
	int retval = ofono_modem_driver_register(&qcom_msim_driver);

	if (retval)
		DBG("ofono_modem_driver_register returned: %d", retval);

	return retval;
}

static void qcom_msim_exit(void)
{
	DBG("");
	ofono_modem_driver_unregister(&qcom_msim_driver);
}

OFONO_PLUGIN_DEFINE(qcom_msim, "Modem driver for Qualcomm's multi-sim device",
	VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT, qcom_msim_init, qcom_msim_exit)
