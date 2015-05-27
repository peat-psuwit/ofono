/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2015 Ratchanan Srirattanamet
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gril.h"
#include "grilrequest.h"
#include "grilreply.h"

#include "drivers/rilmodem/radio-settings.h"
#include "drivers/rilmodem/rilutil.h"
#include "qcom_msim_modem.h"

struct qcom_msim_radio_data {
        //Keep entries below in sync with struct radio_data
	GRil *ril;
	struct ofono_modem *modem;
	gboolean fast_dormancy;
	gboolean pending_fd;
	//Keep entries above in sync with struct radio_data

	//Our custom entries
        int pending_pref;
        struct cb_data *pending_pref_cbd;
        int pending_pref_wait_amount;
};

#define MULTISIM_RS_LAST 2

static struct ofono_radio_settings *multisim_rs[MULTISIM_RS_LAST] = {};
static int multisim_rs_amount = 0;
static int pending_pref_slot = -1;

static void qcom_msim_set_rat_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct qcom_msim_radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: rat mode setting failed", __func__);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}

	pending_pref_slot = -1;
}

static void qcom_msim_set_2g_rat_mode_cb(struct ril_msg *message, gpointer user_data)
{
        struct ofono_radio_settings *rs = user_data;
        struct ofono_radio_settings *pending_pref_rs;
        struct qcom_msim_radio_data *pending_pref_rd;

        if (message->error == RIL_E_SUCCESS) {
                g_ril_print_response_no_args(rd->ril, message);
                radio_set_rat_mode(rs, OFONO_RADIO_ACCESS_MODE_GSM);
        }
        else {
                ofono_error("%s: rat mode setting to GSM failed, "
                                "cancel attempt for slot %d",
                                __func__, pending_pref_slot);
        }

        if (pending_pref_slot < 0) //The pending pref setting has been canceled
                return;

        pending_pref_rs = multisim_rs[pending_pref_slot];
        pending_pref_rd = ofono_radio_settings_get_data(pending_pref_rs);

        if (message->error == RIL_E_SUCCESS) {
                pending_pref_rd->pending_pref_wait_amount -= 1;
                if (pending_pref_rd->pending_pref_wait_amount == 0) {
                        struct parcel rilp;

                        g_ril_request_set_preferred_network_type(
                                        pending_pref_rd->ril,
                                        pref, &rilp);

                        if (g_ril_send(pending_pref_rd->ril,
                                        RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
                                        &rilp, qcom_msim_set_rat_cb,
                                        pending_pref_rd->pending_pref_cbd,
                                        g_free) == 0) {
                                ofono_error("%s: unable to set rat mode", __func__);
                                CALLBACK_WITH_FAILURE(
                                        pending_pref_rd->pending_pref_cbd->cb,
                                        pending_pref_rd->pending_pref_cbd->data);
                                g_free(pending_pref_rd->pending_pref_cbd);

                                pending_pref_slot = -1;
                        }
                }
        } else {
                CALLBACK_WITH_FAILURE(
                        pending_pref_rd->pending_pref_cbd->cb,
                        pending_pref_rd->pending_pref_cbd->data);
                g_free(pending_pref_rd->pending_pref_cbd);

                pending_pref_slot = -1;
        }

}

static void qcom_msim_set_rat_mode(struct ofono_radio_settings *rs,
			enum ofono_radio_access_mode mode,
			ofono_radio_settings_rat_mode_set_cb_t cb,
			void *data)
{
	struct qcom_msim_radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct parcel rilp;
	int pref = PREF_NET_TYPE_GSM_WCDMA;

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_GSM:
		pref = PREF_NET_TYPE_GSM_ONLY;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		pref = PREF_NET_TYPE_GSM_WCDMA;
		break;

        if (multisim_rs_amount == 1) {
                g_ril_request_set_preferred_network_type(rd->ril, pref, &rilp);

                if (g_ril_send(rd->ril, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
                                        &rilp, qcom_msim_set_rat_cb(), cbd,
                                        g_free) == 0) {
                        ofono_error("%s: unable to set rat mode", __func__);
                        g_free(cbd);
                        CALLBACK_WITH_FAILURE(cb, data);
                }
        }
	else if (pref == PREF_NET_TYPE_GSM_WCDMA) {
                int i;
                for (i = 0; i < MULTISIM_RS_LAST; i++) {
                        if (multisim_rs[i] == rs) {
                                struct qcom_msim_radio_data *rd = ofono_radio_settings_get_data(rs);
                                rd->pending_pref = pref;
                                rd->pending_pref_cbd = cbd;
                                rd->pending_pref_wait_amount = multisim_rs_amount - 1;

                                pending_pref_slot = i;
                        }
                        else if (multisim_rs[i] != NULL) {
                                struct qcom_msim_radio_data *rd = ofono_radio_settings_get_data(multisim_rs[i]);
                                g_ril_request_set_preferred_network_type(
                                                rd->ril, PREF_NET_TYPE_GSM_ONLY,
                                                &rilp);

                                if (g_ril_send(rd->ril, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
                                                &rilp,
                                                qcom_msim_set_2g_rat_mode_cb,
                                                multisim_rs[i], NULL) == 0) {
                                        ofono_error("%s: unable to set rat mode", __func__);
                                        g_free(cbd);
                                        CALLBACK_WITH_FAILURE(cb, data);

                                        pending_pref_slot = -1;
                                        break;
                                }
                        }
                }
	}
}

static ofono_bool_t query_modem_rats_cb(gpointer user_data)
{
	ofono_bool_t modem_rats[OFONO_RADIO_ACCESS_MODE_LAST] = { FALSE };
	struct cb_data *cbd = user_data;
	ofono_radio_settings_modem_rats_query_cb_t cb = cbd->cb;
	struct ofono_radio_settings *rs = cbd->user;
	struct qcom_msim_radio_data *rd = ofono_radio_settings_get_data(rs);

	modem_rats[OFONO_RADIO_ACCESS_MODE_GSM] = TRUE;
	modem_rats[OFONO_RADIO_ACCESS_MODE_UMTS] = TRUE;

        /* I don't have multi-sim device with LTE, so I don't know what I
         * should do. Just hide it for now.
         */
	/* if (ofono_modem_get_boolean(rd->modem, MODEM_PROP_LTE_CAPABLE))
		modem_rats[OFONO_RADIO_ACCESS_MODE_LTE] = TRUE; */

	CALLBACK_WITH_SUCCESS(cb, modem_rats, cbd->data);

	g_free(cbd);

	return FALSE;
}

static void qcom_msim_query_modem_rats(struct ofono_radio_settings *rs,
				ofono_radio_settings_modem_rats_query_cb_t cb,
				void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data, rs);

	g_idle_add(query_modem_rats_cb, cbd);
}

static int qcom_msim_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user)
{
	struct ril_radio_settings_driver_data* rs_init_data = user;
	struct qcom_msim_radio_data *rsd = g_try_new0(struct qcom_msim_radio_data, 1);
	int slot_id;

	if (rsd == NULL) {
		ofono_error("%s: cannot allocate memory", __func__);
		return -ENOMEM;
	}

	rsd->ril = g_ril_clone(rs_init_data->gril);
	rsd->modem = rs_init_data->modem;

	ofono_radio_settings_set_data(rs, rsd);

	ril_set_fast_dormancy(rs, FALSE, ril_delayed_register, rs);

	slot_id = ofono_modem_get_integer(rsd->modem, "Slot");
	multisim_rs[slot_id] = rs;
	multisim_rs_amount += 1;

	return 0;
}

void qcom_msim_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct qcom_msim_radio_data *rd = ofono_radio_settings_get_data(rs);
	int slot_id = ofono_modem_get_integer(rd->modem, "Slot");

        multisim_rs[slot_id] = NULL;
	multisim_rs_amount -= 1;

	ofono_radio_settings_set_data(rs, NULL);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= QCOMMSIMMODEM,
	.probe			= qcom_msim_radio_settings_probe,
	.remove			= ril_radio_settings_remove,
	.query_rat_mode		= ril_query_rat_mode,
	.set_rat_mode		= ril_set_rat_mode,
	.query_fast_dormancy	= ril_query_fast_dormancy,
	.set_fast_dormancy	= ril_set_fast_dormancy,
	.query_modem_rats	= qcom_msim_query_modem_rats
};

void qcom_msim_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void qcom_msim_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
