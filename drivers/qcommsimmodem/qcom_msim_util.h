/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Canonical Ltd.
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

#define MULTISIM_RS_LAST 2

struct qcom_msim_pending_pref_setting {
	struct ofono_radio_settings *rs;
	int pref;
	int pending_gsm_pref_remaining;
	struct cb_data *cbd;
};

struct qcom_msim_set_2g_rat {
	struct ofono_radio_settings *rs;
	struct qcom_msim_pending_pref_setting *pps;
};
