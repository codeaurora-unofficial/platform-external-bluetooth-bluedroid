/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains functions for the SMP L2Cap interface
 *
 ******************************************************************************/

#include "bt_target.h"

#if SMP_INCLUDED == TRUE

#include <string.h>
#include "btm_ble_api.h"
#include "l2c_api.h"

#include "smp_int.h"



static void smp_connect_cback (BD_ADDR bd_addr, BOOLEAN connected, UINT16 reason, tBT_TRANSPORT transport);
static void smp_data_ind (BD_ADDR bd_addr, BT_HDR *p_buf, tBT_TRANSPORT transport);

/*******************************************************************************
**
** Function         smp_l2cap_if_init
**
** Description      This function is called during the SMP task startup
**                  to register interface functions with L2CAP.
**
*******************************************************************************/
void smp_l2cap_if_init (void)
{
    tL2CAP_FIXED_CHNL_REG  fixed_reg;
    SMP_TRACE_EVENT ("SMDBG l2c smp_l2cap_if_init");
    fixed_reg.fixed_chnl_opts.mode         = L2CAP_FCR_BASIC_MODE;
    fixed_reg.fixed_chnl_opts.max_transmit = 0;
    fixed_reg.fixed_chnl_opts.rtrans_tout  = 0;
    fixed_reg.fixed_chnl_opts.mon_tout     = 0;
    fixed_reg.fixed_chnl_opts.mps          = 0;
    fixed_reg.fixed_chnl_opts.tx_win_sz    = 0;

    fixed_reg.pL2CA_FixedConn_Cb = smp_connect_cback;
    fixed_reg.pL2CA_FixedData_Cb = smp_data_ind;
    fixed_reg.pL2CA_FixedCong_Cb = NULL;    /* do not handle congestion on this channel */
    fixed_reg.default_idle_tout  = 60;      /* set 60 seconds timeout, 0xffff default idle timeout */

    /* Now, register with L2CAP */
    L2CA_RegisterFixedChannel (L2CAP_SMP_CID, &fixed_reg);
#if (defined BTM_LE_SECURE_CONN && BTM_LE_SECURE_CONN == TRUE)
    L2CA_RegisterFixedChannel (L2CAP_SMP_BREDR_CID, &fixed_reg);
#endif
}

/*******************************************************************************
**
** Function         smp_connect_cback
**
** Description      This callback function is called by L2CAP to indicate that
**                  SMP channel is
**                      connected (conn = TRUE)/disconnected (conn = FALSE).
**
*******************************************************************************/
static void smp_connect_cback (BD_ADDR bd_addr, BOOLEAN connected, UINT16 reason,
                                    tBT_TRANSPORT transport)
{
    tSMP_CB   *p_cb = &smp_cb;
    tSMP_INT_DATA   int_data;

    SMP_TRACE_EVENT ("SMDBG l2c smp_connect_cback ");

    if (transport == BT_TRANSPORT_BR_EDR) /*&& memcmp(bd_addr, p_cb->pairing_bda, BD_ADDR_LEN) == 0*/
    {
#if (defined BTM_LE_SECURE_CONN && BTM_LE_SECURE_CONN == TRUE)
        if(p_cb->state != SMP_ST_IDLE)
        {
            SMP_TRACE_EVENT ("SMDBG l2c smp_connect_cback SMP busy");
            return;
        }

        if (connected)
        {
            if(!p_cb->connect_initialized)
            {
                p_cb->connect_initialized = TRUE;
                /* initiating connection established */
                /*change role to send sec req instead of pair req*/
                p_cb->role = HCI_ROLE_MASTER;
                /*Copy the BDA for short pairing*/
                memcpy (p_cb->pairing_bda, bd_addr, BD_ADDR_LEN);
                p_cb->flags = SMP_PAIR_FLAGS_WE_STARTED_DD;
                p_cb->smp_bredr = TRUE;

                /* initialize local i/r key to be default keys */
                p_cb->loc_r_key = p_cb->loc_i_key =  SMP_SEC_DEFAULT_KEY;
                p_cb->loc_auth_req = p_cb->peer_auth_req = SMP_DEFAULT_AUTH_REQ;
                p_cb->cb_evt = SMP_IO_CAP_REQ_EVT;
                smp_sm_event(p_cb, SMP_L2CAP_CONN_EVT, NULL);
            }
        }
#endif
        SMP_TRACE_ERROR ("smp_connect_cback : BR-EDR transport");
        return;
    }

    if (memcmp(bd_addr, p_cb->pairing_bda, BD_ADDR_LEN) == 0)
    {
        SMP_TRACE_EVENT ("smp_connect_cback()  for pairing BDA: %08x%04x  Event: %s",
                        (bd_addr[0]<<24)+(bd_addr[1]<<16)+(bd_addr[2]<<8) + bd_addr[3],
                        (bd_addr[4]<<8)+bd_addr[5], (connected) ? "connected" : "disconnected");

        if (connected)
        {
            if(!p_cb->connect_initialized)
            {
                p_cb->connect_initialized = TRUE;
                /* initiating connection established */
                p_cb->role = L2CA_GetBleConnRole(bd_addr);

                /* initialize local i/r key to be default keys */
                p_cb->loc_r_key = p_cb->loc_i_key =  SMP_SEC_DEFAULT_KEY;
                p_cb->loc_auth_req = p_cb->peer_auth_req = SMP_DEFAULT_AUTH_REQ;
                p_cb->cb_evt = SMP_IO_CAP_REQ_EVT;
                smp_sm_event(p_cb, SMP_L2CAP_CONN_EVT, NULL);
            }
        }
        else
        {
            int_data.reason = reason;
            /* Disconnected while doing security */
            smp_sm_event(p_cb, SMP_L2CAP_DISCONN_EVT, &int_data);
        }
    }
}

/*******************************************************************************
**
** Function         smp_data_ind
**
** Description      This function is called when data is received from L2CAP on
**                  SMP channel.
**
**
** Returns          void
**
*******************************************************************************/
static void smp_data_ind (BD_ADDR bd_addr, BT_HDR *p_buf, tBT_TRANSPORT transport)
{
    tSMP_CB *p_cb = &smp_cb;
    UINT8   *p = (UINT8 *)(p_buf + 1) + p_buf->offset;
    UINT8   cmd ;
    SMP_TRACE_EVENT ("SMDBG l2c smp_data_ind, transport = %d", transport);

    SMP_TRACE_EVENT ("Got smp_data_ind");

    STREAM_TO_UINT8(cmd, p);

    /* sanity check */
    if ((SMP_OPCODE_MAX <= cmd) || (cmd == 0))
    {
        SMP_TRACE_WARNING( "Ignore received command with RESERVED code 0x%02x", cmd);
        GKI_freebuf (p_buf);
        return;
    }

    /* reject the pairing request if there is an on-going SMP pairing */
    if (SMP_OPCODE_PAIRING_REQ == cmd || SMP_OPCODE_SEC_REQ == cmd)
    {
        if (p_cb->state == SMP_ST_IDLE)
        {
            if(transport == BT_TRANSPORT_LE)
            {
                p_cb->role = L2CA_GetBleConnRole(bd_addr);
#if (defined BTM_LE_SECURE_CONN && BTM_LE_SECURE_CONN == TRUE)
                p_cb->smp_bredr = FALSE;
#endif
            }
#if (defined BTM_LE_SECURE_CONN && BTM_LE_SECURE_CONN == TRUE)
            else
            {
                p_cb->role = (cmd == SMP_OPCODE_PAIRING_REQ) ? \
                              HCI_ROLE_SLAVE : HCI_ROLE_MASTER;
                p_cb->smp_bredr = TRUE;
            }
#endif
            memcpy(&p_cb->pairing_bda[0], bd_addr, BD_ADDR_LEN);
        }
        else if (memcmp(&bd_addr[0], p_cb->pairing_bda, BD_ADDR_LEN))
        {
            GKI_freebuf (p_buf);
            smp_reject_unexp_pair_req(bd_addr);
            return;
        }
        /* else, out of state pairing request/security request received, passed into SM */
    }

    if (memcmp(&bd_addr[0], p_cb->pairing_bda, BD_ADDR_LEN) == 0)
    {
        if (p_cb->state != SMP_ST_RELEASE_DELAY)
        {
            btu_stop_timer (&p_cb->rsp_timer_ent);
            btu_start_timer (&p_cb->rsp_timer_ent, BTU_TTYPE_SMP_PAIRING_CMD,
                             SMP_WAIT_FOR_RSP_TOUT);
        }
        p_cb->rcvd_cmd_code = cmd;
        p_cb->rcvd_cmd_len = (UINT8) p_buf->len;
        smp_sm_event(p_cb, cmd, p);
    }

    GKI_freebuf (p_buf);
}
#endif
