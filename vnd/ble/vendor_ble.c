/******************************************************************************
 *
 *  Copyright (C) 2003-2014 Broadcom Corporation
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

/*****************************************************************************
**
**  Name:          vendor_ble.c
**
**  Description:   This file contains vendor specific feature for BLE
**
******************************************************************************/
#include <string.h>
#include "bt_target.h"

#if (BLE_INCLUDED == TRUE && BLE_ANDROID_CONTROLLER_SCAN_FILTER == TRUE)
#include "bt_types.h"
#include "hcimsgs.h"
#include "btu.h"
#include "vendor_ble.h"
#include "vendor_hcidefs.h"
#include "gatt_int.h"

#define BTM_BLE_INVALID_COUNTER     0xff


static UINT8 btm_ble_cs_update_pf_counter(tBTM_BLE_SCAN_COND_OP action,
                                  UINT8 cond_type,
                                  tBLE_BD_ADDR   *p_bd_addr,
                                  UINT8           num_available);

#define BTM_BLE_SET_SCAN_PF_OPCODE(x, y) (((x)<<4)|y)
#define BTM_BLE_GET_SCAN_PF_SUBCODE(x)    ((x) >> 4)
#define BTM_BLE_GET_SCAN_PF_ACTION(x)    ((x) & 0x0f)

/* max number of filter available for different filter type, controller dependent number */
static const UINT8 btm_ble_cs_filter_max[BTM_BLE_PF_TYPE_MAX] =
{
    BTM_BLE_MAX_ADDR_FILTER,        /* address filter */
    1,                              /* no limit for service data change, always enable or disable */
    BTM_BLE_MAX_UUID_FILTER,        /* service UUID filter */
    BTM_BLE_MAX_UUID_FILTER,        /* solicitated UUID filter */
    BTM_BLE_PF_STR_COND_MAX,        /* local name filter */
    BTM_BLE_PF_STR_COND_MAX         /* manufacturer data filter */
};

/*** This needs to be moved to a VSC control block eventually per coding conventions ***/
#if VENDOR_DYNAMIC_MEMORY == FALSE
tBTM_BLE_VENDOR_CB  btm_ble_vendor_cb;
#endif

static const UINT8 op_code_to_cond_type[] =
{
    BTM_BLE_PF_TYPE_ALL,
    BTM_BLE_PF_ADDR_FILTER,
    BTM_BLE_PF_SRVC_UUID,
    BTM_BLE_PF_SRVC_SOL_UUID,
    BTM_BLE_PF_LOCAL_NAME,
    BTM_BLE_PF_MANU_DATA,
    BTM_BLE_PF_SRVC_DATA_PATTERN
};

static const BD_ADDR     na_bda= {0};

/*******************************************************************************
**
** Function         btm_ble_vendor_scan_pf_cmpl_cback
**
** Description      the BTM BLE customer feature VSC complete callback for ADV PF
**                  filtering
**
** Returns          pointer to the counter if found; NULL otherwise.
**
*******************************************************************************/
void btm_ble_vendor_scan_pf_cmpl_cback(tBTM_VSC_CMPL *p_params)
{
    UINT8  status;
    UINT8  *p = p_params->p_param_buf, op_subcode, action = 0xff;
    UINT16  evt_len = p_params->param_len;
    UINT8   num_avail = 0, cond_type = BTM_BLE_PF_TYPE_MAX;
    tBTM_BLE_PF_CMPL_CBACK  *p_cmpl_cback =   btm_ble_vendor_cb.p_scan_pf_cback;
    UINT8   op = BTM_BLE_GET_SCAN_PF_ACTION(btm_ble_vendor_cb.op_type);
    UINT8   subcode = BTM_BLE_GET_SCAN_PF_SUBCODE(btm_ble_vendor_cb.op_type);

    STREAM_TO_UINT8(status, p);

    evt_len--;

    if (evt_len < 1 )
    {
        BTM_TRACE_ERROR("can not interpret ADV PF filter setting callback. status = %d", status);
        return;
    }
    op_subcode   = *p ++;
        switch (op_subcode)
        {
        case BTM_BLE_META_PF_LOCAL_NAME:
        case BTM_BLE_META_PF_MANU_DATA:
        case BTM_BLE_META_PF_ADDR:
        case BTM_BLE_META_PF_UUID:
        case BTM_BLE_META_PF_SOL_UUID:
        case BTM_BLE_META_PF_FEAT_SEL:
        case BTM_BLE_META_PF_SRVC_DATA:
            cond_type = op_code_to_cond_type[op_subcode - BTM_BLE_META_PF_FEAT_SEL];
            if (status == HCI_SUCCESS)
            {
                action       = *p ++;
                if (op_subcode != BTM_BLE_META_PF_FEAT_SEL)
                {
                STREAM_TO_UINT8(num_avail, p);
                }

                if (memcmp(&btm_ble_vendor_cb.cur_filter_target.bda, &na_bda, BD_ADDR_LEN) == 0)
                    btm_ble_cs_update_pf_counter(action, cond_type, NULL, num_avail);
                else
                    btm_ble_cs_update_pf_counter(action, cond_type, &btm_ble_vendor_cb.cur_filter_target, num_avail);
            }
            break;

        case BTM_BLE_META_PF_ENABLE:
            cond_type = BTM_BLE_META_PF_ENABLE;
            BTM_TRACE_DEBUG("CS feature Enabled");
            break;

        default:
            BTM_TRACE_ERROR("unknow operation: %d", op_subcode);
            break;
        }

    /* send ADV PF opeartion complete */
    if (p_cmpl_cback && subcode == cond_type)
    {
        btm_ble_vendor_cb.p_scan_pf_cback = NULL;
        btm_ble_vendor_cb.op_type = 0;
        (* p_cmpl_cback)(op, subcode, status);
    }
}
/*******************************************************************************
**         adv payload filtering functions
*******************************************************************************/
/*******************************************************************************
**
** Function         btm_ble_find_addr_filter_counter
**
** Description      find the per bd address ADV payload filter counter by BD_ADDR.
**
** Returns          pointer to the counter if found; NULL otherwise.
**
*******************************************************************************/
tBTM_BLE_PF_COUNT   * btm_ble_find_addr_filter_counter(tBLE_BD_ADDR *p_le_bda)
{
    UINT8               i;
    tBTM_BLE_PF_COUNT   *p_addr_filter = &btm_ble_vendor_cb.addr_filter_count[1];

    if (p_le_bda == NULL)
        return &btm_ble_vendor_cb.addr_filter_count[0];

    for (i = 0; i < BTM_BLE_MAX_FILTER_COUNTER; i ++, p_addr_filter ++)
    {
        if (p_addr_filter->in_use &&
            memcmp(p_le_bda->bda, p_addr_filter->bd_addr, BD_ADDR_LEN) == 0)
        {
            return p_addr_filter;
        }
    }
    return NULL;
}

/*******************************************************************************
**
** Function         btm_ble_alloc_addr_filter_counter
**
** Description      allocate the per device adv payload filter counter.
**
** Returns          pointer to the counter if allocation succeed; NULL otherwise.
**
*******************************************************************************/
tBTM_BLE_PF_COUNT * btm_ble_alloc_addr_filter_counter(BD_ADDR bd_addr)
{
    UINT8               i;
    tBTM_BLE_PF_COUNT   *p_addr_filter = &btm_ble_vendor_cb.addr_filter_count[1];

    for (i = 0; i < BTM_BLE_MAX_FILTER_COUNTER; i ++, p_addr_filter ++)
    {
        if (memcmp(na_bda, p_addr_filter->bd_addr, BD_ADDR_LEN) == 0)
        {
            memcpy(p_addr_filter->bd_addr, bd_addr, BD_ADDR_LEN);
            p_addr_filter->in_use = TRUE;
            return p_addr_filter;
        }
    }
    return NULL;
}
/*******************************************************************************
**
** Function         btm_ble_dealloc_addr_filter_counter
**
** Description      de-allocate the per device adv payload filter counter.
**
** Returns          TRUE if deallocation succeed; FALSE otherwise.
**
*******************************************************************************/
BOOLEAN btm_ble_dealloc_addr_filter_counter(tBLE_BD_ADDR *p_bd_addr, UINT8 filter_type)
{
    UINT8               i;
    tBTM_BLE_PF_COUNT   *p_addr_filter = &btm_ble_vendor_cb.addr_filter_count[1];
    BOOLEAN             found = FALSE;

    if (filter_type == BTM_BLE_PF_TYPE_ALL && p_bd_addr == NULL)
        memset(&btm_ble_vendor_cb.addr_filter_count[0], 0, sizeof(tBTM_BLE_PF_COUNT));

    for (i = 0; i < BTM_BLE_MAX_FILTER_COUNTER; i ++, p_addr_filter ++)
    {
        if ((p_addr_filter->in_use) &&
            (p_bd_addr == NULL ||
             (p_bd_addr != NULL && memcmp(p_bd_addr->bda, p_addr_filter->bd_addr, BD_ADDR_LEN) == 0)))
        {
            found = TRUE;
            memset(p_addr_filter, 0, sizeof(tBTM_BLE_PF_COUNT));

            if (p_bd_addr != NULL) break;
        }
    }
    return found;
}
/*******************************************************************************
**
** Function         btm_ble_cs_update_pf_counter
**
** Description      this function is to update the adv data payload filter counter
**
** Returns          current number of the counter; BTM_BLE_INVALID_COUNTER if
**                  counter update failed.
**
*******************************************************************************/
UINT8 btm_ble_cs_update_pf_counter(tBTM_BLE_SCAN_COND_OP action,
                                  UINT8 cond_type,
                                  tBLE_BD_ADDR   *p_bd_addr,
                                  UINT8           num_available)
{
    tBTM_BLE_PF_COUNT   *p_addr_filter = NULL;
    UINT8               *p_counter = NULL;
    UINT32              *p_feat_mask = NULL;


    if (cond_type > BTM_BLE_PF_TYPE_ALL)
    {
        BTM_TRACE_ERROR("unknown PF filter condition type %d", cond_type);
        return BTM_BLE_INVALID_COUNTER;
    }
    /* for these three types of filter, always generic */
    if (cond_type == BTM_BLE_PF_ADDR_FILTER ||
        cond_type == BTM_BLE_PF_MANU_DATA ||
        cond_type == BTM_BLE_PF_LOCAL_NAME ||
        cond_type == BTM_BLE_PF_SRVC_DATA_PATTERN)
        p_bd_addr = NULL;

    if ((p_addr_filter = btm_ble_find_addr_filter_counter(p_bd_addr)) == NULL &&
        action == BTM_BLE_SCAN_COND_ADD)
    {
        p_addr_filter = btm_ble_alloc_addr_filter_counter(p_bd_addr->bda);
    }

    if (p_addr_filter != NULL)
    {
        /* all filter just cleared */
        if ((cond_type == BTM_BLE_PF_TYPE_ALL && action == BTM_BLE_SCAN_COND_CLEAR) ||
            /* or bd address filter been deleted */
            (cond_type == BTM_BLE_PF_ADDR_FILTER &&
             (action == BTM_BLE_SCAN_COND_DELETE || action == BTM_BLE_SCAN_COND_CLEAR)))
        {
            btm_ble_dealloc_addr_filter_counter(p_bd_addr, cond_type);
        }
        /* if not feature selection, update new addition/reduction of the filter counter */
        else if (cond_type != BTM_BLE_PF_TYPE_ALL)
        {
        p_counter = p_addr_filter->pf_counter;
        p_feat_mask = &p_addr_filter->feat_mask;

        p_counter[cond_type] = btm_ble_cs_filter_max[cond_type] - num_available;

        BTM_TRACE_DEBUG("current filter counter number = %d", p_counter[cond_type]);

        /* update corresponding feature mask */
        if (p_counter[cond_type] > 0)
            *p_feat_mask |= (BTM_BLE_PF_BIT_TO_MASK(cond_type));
        else
            *p_feat_mask &= ~(BTM_BLE_PF_BIT_TO_MASK(cond_type));

        return p_counter[cond_type];
    }
    }
    else
    {
        BTM_TRACE_ERROR("no matching filter counter found");
    }
    /* no matching filter located and updated */
    return BTM_BLE_INVALID_COUNTER;
}
/*******************************************************************************
**
** Function         btm_ble_update_pf_manu_data
**
** Description      this function update(add,delete or clear) the adv manufacturer
**                  data filtering condition.
**
**
** Returns          BTM_SUCCESS if sucessful,
**                  BTM_ILLEGAL_VALUE if paramter is not valid.
**
*******************************************************************************/
tBTM_STATUS btm_ble_update_pf_manu_data(tBTM_BLE_SCAN_COND_OP action,
                                        tBTM_BLE_PF_COND_PARAM *p_data,
                                        tBTM_BLE_PF_COND_TYPE cond_type)
{
    tBTM_BLE_PF_MANU_COND *p_manu_data = (p_data == NULL) ? NULL : &p_data->manu_data;
    tBTM_BLE_PF_SRVC_PATTERN_COND *p_srvc_data = (p_data == NULL) ? NULL : &p_data->srvc_data;
    UINT8       param[BTM_BLE_PF_STR_LEN_MAX + BTM_BLE_PF_STR_LEN_MAX + BTM_BLE_META_HDR_LENGTH],
                *p = param,
                len = BTM_BLE_META_HDR_LENGTH;
    tBTM_STATUS st = BTM_ILLEGAL_VALUE;

    memset(param, 0, BTM_BLE_PF_STR_LEN_MAX + BTM_BLE_META_HDR_LENGTH);

    if (cond_type == BTM_BLE_PF_SRVC_DATA_PATTERN)
    {
        UINT8_TO_STREAM(p, BTM_BLE_META_PF_SRVC_DATA);
    }
    else
    {
        UINT8_TO_STREAM(p, BTM_BLE_META_PF_MANU_DATA);
    }
    UINT8_TO_STREAM(p, action);

    /* Filter index */
    UINT8_TO_STREAM(p, 0);

    if (action == BTM_BLE_SCAN_COND_ADD ||
        action == BTM_BLE_SCAN_COND_DELETE)
    {
        if (p_manu_data == NULL)
            return st;
        if (cond_type == BTM_BLE_PF_SRVC_DATA_PATTERN)
        {
            if (p_srvc_data->data_len > (BTM_BLE_PF_STR_LEN_MAX - 2))
                p_srvc_data->data_len = (BTM_BLE_PF_STR_LEN_MAX - 2);

            UINT16_TO_STREAM(p, p_srvc_data->uuid);
            ARRAY_TO_STREAM(p, p_srvc_data->p_pattern, p_srvc_data->data_len);
            len += (p_srvc_data->data_len + 2);
        }
        else
        {
            if (p_manu_data->data_len > (BTM_BLE_PF_STR_LEN_MAX - 2))
                p_manu_data->data_len = (BTM_BLE_PF_STR_LEN_MAX - 2);

            UINT16_TO_STREAM(p, p_manu_data->company_id);
            ARRAY_TO_STREAM(p, p_manu_data->p_pattern, p_manu_data->data_len);
            len += (p_manu_data->data_len + 2);

            if (p_manu_data->company_id_mask != 0)
            {
                UINT16_TO_STREAM (p, p_manu_data->company_id_mask);
            }
            else
            {
                memset(p, 0xff, 2);
                p += 2;
            }
            if (p_manu_data->p_pattern_mask != NULL)
            {
                ARRAY_TO_STREAM(p, p_manu_data->p_pattern_mask, p_manu_data->data_len);
            }
            else
                memset(p, 0xff, p_manu_data->data_len);
            len += (p_manu_data->data_len + 2);
        }
    }

    /* send manufacturer*/
    if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                              len,
                              param,
                              btm_ble_vendor_scan_pf_cmpl_cback)) != BTM_NO_RESOURCES)
    {
        memset(&btm_ble_vendor_cb.cur_filter_target, 0, sizeof(tBLE_BD_ADDR));
    }
    else
    {
        BTM_TRACE_ERROR("manufacturer data PF filter update failed");
    }

    return st;
}
/*******************************************************************************
**
** Function         btm_ble_update_pf_local_name
**
** Description      this function update(add,delete or clear) the adv lcoal name
**                  filtering condition.
**
**
** Returns          BTM_SUCCESS if sucessful,
**                  BTM_ILLEGAL_VALUE if paramter is not valid.
**
*******************************************************************************/
tBTM_STATUS btm_ble_update_pf_local_name(tBTM_BLE_SCAN_COND_OP action,
                                         tBTM_BLE_PF_COND_PARAM *p_cond)
{
    tBTM_BLE_PF_LOCAL_NAME_COND *p_local_name = (p_cond == NULL) ? NULL : &p_cond->local_name;
    UINT8       param[BTM_BLE_PF_STR_LEN_MAX + BTM_BLE_META_HDR_LENGTH],
                *p = param,
                len = BTM_BLE_META_HDR_LENGTH;
    tBTM_STATUS st = BTM_ILLEGAL_VALUE;

    memset(param, 0, BTM_BLE_PF_STR_LEN_MAX + BTM_BLE_META_HDR_LENGTH);

    UINT8_TO_STREAM(p, BTM_BLE_META_PF_LOCAL_NAME);
    UINT8_TO_STREAM(p, action);

    /* Filter index */
    UINT8_TO_STREAM(p, 0);

    if (action == BTM_BLE_SCAN_COND_ADD ||
        action == BTM_BLE_SCAN_COND_DELETE)
    {
        if (p_local_name == NULL)
            return st;

        if (p_local_name->data_len > BTM_BLE_PF_STR_LEN_MAX)
            p_local_name->data_len = BTM_BLE_PF_STR_LEN_MAX;

        ARRAY_TO_STREAM(p, p_local_name->p_data, p_local_name->data_len);
        len += p_local_name->data_len;
    }
    /* send local name filter */
    if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                              len,
                              param,
                              btm_ble_vendor_scan_pf_cmpl_cback))
            != BTM_NO_RESOURCES)
    {
        memset(&btm_ble_vendor_cb.cur_filter_target, 0, sizeof(tBLE_BD_ADDR));
    }
    else
    {
        BTM_TRACE_ERROR("Local Name PF filter update failed");
    }

    return st;
}
/*******************************************************************************
**
** Function         btm_ble_update_addr_filter
**
** Description      this function update(add,delete or clear) the address filter of adv.
**
**
** Returns          BTM_SUCCESS if sucessful,
**                  BTM_ILLEGAL_VALUE if paramter is not valid.
**
*******************************************************************************/
tBTM_STATUS btm_ble_update_addr_filter(tBTM_BLE_SCAN_COND_OP action,
                                       tBTM_BLE_PF_COND_PARAM *p_cond)
{
    UINT8       param[BTM_BLE_META_ADDR_LEN + BTM_BLE_META_HDR_LENGTH],
                * p= param;
    tBTM_STATUS st = BTM_ILLEGAL_VALUE;
    tBLE_BD_ADDR *p_addr = (p_cond == NULL) ? NULL : &p_cond->target_addr;

    memset(param, 0, BTM_BLE_META_ADDR_LEN + BTM_BLE_META_HDR_LENGTH);

    UINT8_TO_STREAM(p, BTM_BLE_META_PF_ADDR);
    UINT8_TO_STREAM(p, action);

    /* Filter index */
    UINT8_TO_STREAM(p, 0);

    if (action == BTM_BLE_SCAN_COND_ADD ||
        action == BTM_BLE_SCAN_COND_DELETE)
    {
        if (p_addr == NULL)
            return st;

        BDADDR_TO_STREAM(p, p_addr->bda);
        UINT8_TO_STREAM(p, p_addr->type);
    }
    /* send address filter */
    if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                              (UINT8)(BTM_BLE_META_HDR_LENGTH + BTM_BLE_META_ADDR_LEN),
                              param,
                              btm_ble_vendor_scan_pf_cmpl_cback)) != BTM_NO_RESOURCES)
    {
        memset(&btm_ble_vendor_cb.cur_filter_target, 0, sizeof(tBLE_BD_ADDR));
    }
    else
    {
        BTM_TRACE_ERROR("Broadcaster Address Filter Update failed");
    }
    return st;
}
/*******************************************************************************
**
** Function         btm_ble_update_uuid_filter
**
** Description      this function update(add,delete or clear) service UUID filter.
**
**
** Returns          BTM_SUCCESS if sucessful,
**                  BTM_ILLEGAL_VALUE if paramter is not valid.
**
*******************************************************************************/
tBTM_STATUS btm_ble_update_uuid_filter(tBTM_BLE_SCAN_COND_OP action,
                                       tBTM_BLE_PF_COND_TYPE filter_type,
                                       tBTM_BLE_PF_COND_PARAM *p_cond)
{
    UINT8       param[BTM_BLE_META_UUID_LEN + BTM_BLE_META_HDR_LENGTH],
                * p= param,
                len = BTM_BLE_META_HDR_LENGTH;
    tBTM_STATUS st = BTM_ILLEGAL_VALUE;
    tBTM_BLE_PF_UUID_COND *p_uuid_cond;
    UINT8           evt_type;

    memset(param, 0, BTM_BLE_META_UUID_LEN + BTM_BLE_META_HDR_LENGTH);

    if (filter_type == BTM_BLE_PF_SRVC_UUID)
    {
        evt_type = BTM_BLE_META_PF_UUID;
        p_uuid_cond = p_cond ? &p_cond->srvc_uuid : NULL;
    }
    else
    {
        evt_type = BTM_BLE_META_PF_SOL_UUID;
        p_uuid_cond = p_cond ? &p_cond->solicitate_uuid : NULL;
    }

    if (p_uuid_cond == NULL && action != BTM_BLE_SCAN_COND_CLEAR)
    {
        BTM_TRACE_ERROR("Illegal param for add/delete UUID filter");
        return st;
    }

    /* need to add address fitler first, if adding per bda UUID filter without address filter */
    if (action == BTM_BLE_SCAN_COND_ADD &&
        p_uuid_cond->p_target_addr &&
        btm_ble_find_addr_filter_counter(p_uuid_cond->p_target_addr) == NULL)
    {
        UINT8_TO_STREAM(p, BTM_BLE_META_PF_ADDR);
        UINT8_TO_STREAM(p, action);

        /* Filter index */
        UINT8_TO_STREAM(p, 0);

        BDADDR_TO_STREAM(p, p_uuid_cond->p_target_addr->bda);
        UINT8_TO_STREAM(p, p_uuid_cond->p_target_addr->type);

        /* send address filter */
        if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                                  (UINT8)(BTM_BLE_META_HDR_LENGTH + BTM_BLE_META_ADDR_LEN),
                                  param,
                                  btm_ble_vendor_scan_pf_cmpl_cback)) == BTM_NO_RESOURCES)
        {
            BTM_TRACE_ERROR("Update Address filter into controller failed.");
            return st;
        }
    }

    p= param;
    UINT8_TO_STREAM(p, evt_type);
    UINT8_TO_STREAM(p, action);

    /* Filter index */
    UINT8_TO_STREAM(p, 0);

    if (action == BTM_BLE_SCAN_COND_ADD ||
        action == BTM_BLE_SCAN_COND_DELETE)
    {
        if (p_uuid_cond->uuid.len == LEN_UUID_16)
        {
            UINT16_TO_STREAM(p, p_uuid_cond->uuid.uu.uuid16);
            len += LEN_UUID_16;
        }
        else if (p_uuid_cond->uuid.len == LEN_UUID_32)/*4 bytes */
        {
            UINT32_TO_STREAM(p, p_uuid_cond->uuid.uu.uuid32);
            len += LEN_UUID_32;
        }
        else if (p_uuid_cond->uuid.len == LEN_UUID_128)
        {
            ARRAY_TO_STREAM (p, p_uuid_cond->uuid.uu.uuid128, LEN_UUID_128);
            len += LEN_UUID_128;
        }
        else
        {
            BTM_TRACE_ERROR("illegal UUID length: %d", p_uuid_cond->uuid.len);
            return BTM_ILLEGAL_VALUE;
        }
#if !(defined VENDOR_ADV_PCF_LEGACY && VENDOR_ADV_PCF_LEGACY == TRUE)
        if (p_uuid_cond->p_uuid_mask != NULL)
        {
            if (p_uuid_cond->uuid.len == LEN_UUID_16)
            {
                UINT16_TO_STREAM(p, p_uuid_cond->p_uuid_mask->uuid16_mask);
                len += LEN_UUID_16;
            }
            else if (p_uuid_cond->uuid.len == LEN_UUID_32)/*4 bytes */
            {
                UINT32_TO_STREAM(p, p_uuid_cond->p_uuid_mask->uuid32_mask);
                len += LEN_UUID_32;
            }
            else if (p_uuid_cond->uuid.len == LEN_UUID_128)
            {
                ARRAY_TO_STREAM (p, p_uuid_cond->p_uuid_mask->uuid128_mask, LEN_UUID_128);
                len += LEN_UUID_128;
            }
        }
        else
        {
            memset(p, 0xff, p_uuid_cond->uuid.len);
            len += p_uuid_cond->uuid.len;
        }
#endif
    }

    /* send UUID filter update */
    if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                              len,
                              param,
                              btm_ble_vendor_scan_pf_cmpl_cback)) != BTM_NO_RESOURCES)
    {
        if (p_uuid_cond && p_uuid_cond->p_target_addr)
            memcpy(&btm_ble_vendor_cb.cur_filter_target, p_uuid_cond->p_target_addr, sizeof(tBLE_BD_ADDR));
        else
            memset(&btm_ble_vendor_cb.cur_filter_target, 0, sizeof(tBLE_BD_ADDR));
    }
    else
    {
        BTM_TRACE_ERROR("UUID filter udpating failed");
    }

    return st;
}
/*******************************************************************************
**
** Function         btm_ble_update_srvc_data_change
**
** Description      this function update(add/remove) service data change filter.
**
**
** Returns          BTM_SUCCESS if sucessful,
**                  BTM_ILLEGAL_VALUE if paramter is not valid.
**
*******************************************************************************/
tBTM_STATUS btm_ble_update_srvc_data_change(tBTM_BLE_SCAN_COND_OP action,
                                       tBTM_BLE_PF_COND_PARAM *p_cond)
{
    tBTM_STATUS st = BTM_ILLEGAL_VALUE;
    tBLE_BD_ADDR   *p_bd_addr = p_cond ? &p_cond->target_addr : NULL;
    UINT8           num_avail = (action == BTM_BLE_SCAN_COND_ADD) ? 0 : 1;

    if (btm_ble_cs_update_pf_counter (action, BTM_BLE_PF_SRVC_DATA, p_bd_addr, num_avail)
                    != BTM_BLE_INVALID_COUNTER)
        st = BTM_SUCCESS;

    return st;
}

/*******************************************************************************
**
** Function         btm_ble_clear_scan_pf_filter
**
** Description      clear all adv payload filter by de-select all the adv pf feature bits
**
**
** Returns          BTM_SUCCESS if sucessful,
**                  BTM_ILLEGAL_VALUE if paramter is not valid.
**
*******************************************************************************/
tBTM_STATUS btm_ble_clear_scan_pf_filter(tBTM_BLE_SCAN_COND_OP action,
                                       tBTM_BLE_PF_COND_PARAM *p_cond)
{
    tBLE_BD_ADDR *p_target = (p_cond == NULL)? NULL : &p_cond->target_addr;
    tBTM_BLE_PF_COUNT *p_bda_filter;
    tBTM_STATUS     st = BTM_WRONG_MODE;
    UINT8           param[20], *p;

    if (action != BTM_BLE_SCAN_COND_CLEAR)
    {
        BTM_TRACE_ERROR("unable to perform action:%d for generic adv filter type", action);
        return BTM_ILLEGAL_VALUE;
    }

    p = param;
    memset(param, 0, 20);

    p_bda_filter = btm_ble_find_addr_filter_counter(p_target);

    if (p_bda_filter == NULL ||
        /* not a generic filter, and feature selection is empty */
        (p_target != NULL && p_bda_filter && p_bda_filter->feat_mask == 0))
    {
        BTM_TRACE_ERROR("Error: Can not clear filter, No PF filter has been configured!");
        return st;
    }

    /* clear the general filter entry */
    if (p_target == NULL)
    {
        /* clear manufactuer data filter */
        btm_ble_update_pf_manu_data(BTM_BLE_SCAN_COND_CLEAR, NULL, BTM_BLE_PF_MANU_DATA);
        /* clear local name filter */
        btm_ble_update_pf_local_name(BTM_BLE_SCAN_COND_CLEAR, NULL);
        /* update the counter  for service data */
        btm_ble_update_srvc_data_change(BTM_BLE_SCAN_COND_CLEAR, NULL);
        /* clear UUID filter */
        btm_ble_update_uuid_filter(BTM_BLE_SCAN_COND_CLEAR, BTM_BLE_PF_SRVC_UUID, NULL);
        btm_ble_update_uuid_filter(BTM_BLE_SCAN_COND_CLEAR, BTM_BLE_META_PF_SOL_UUID, NULL);
        /* clear service data filter */
        btm_ble_update_pf_manu_data(BTM_BLE_SCAN_COND_CLEAR, NULL, BTM_BLE_PF_MANU_DATA);
    }

    /* select feature based on control block settings */
    UINT8_TO_STREAM(p, BTM_BLE_META_PF_FEAT_SEL);
    UINT8_TO_STREAM(p, BTM_BLE_SCAN_COND_CLEAR);

    /* Filter index */
    UINT8_TO_STREAM(p, 0);

    /* set PCF selection */
    UINT32_TO_STREAM(p, BTM_BLE_PF_SELECT_NONE);
    /* set logic condition as OR as default */
    UINT8_TO_STREAM(p, BTM_BLE_PF_LOGIC_OR);

    if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                               (UINT8)(BTM_BLE_META_HDR_LENGTH + BTM_BLE_PF_FEAT_SEL_LEN),
                                param,
                                btm_ble_vendor_scan_pf_cmpl_cback))
            != BTM_NO_RESOURCES)
    {
        if (p_bda_filter)
            p_bda_filter->feat_mask = BTM_BLE_PF_SELECT_NONE;

        if (p_target)
            memcpy(&btm_ble_vendor_cb.cur_filter_target, p_target, sizeof(tBLE_BD_ADDR));
        else
            memset(&btm_ble_vendor_cb.cur_filter_target, 0, sizeof(tBLE_BD_ADDR));
    }

    return st;
}
/*******************************************************************************
**
** Function         BTM_BleEnableFilterCondition
**
** Description      This function is called to enable the adv data payload filter
**                  condition.
**
** Parameters       p_target: enabble the filter condition on a target device; if NULL
**                            enable the generic scan condition.
**                  enable: enable or disable the filter condition
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS BTM_BleEnableFilterCondition(BOOLEAN enable, tBLE_BD_ADDR *p_target,
                                         tBTM_BLE_PF_CMPL_CBACK *p_cmpl_cback)
{
    UINT8           param[20], *p;
    tBTM_STATUS     st = BTM_WRONG_MODE;
    tBTM_BLE_PF_COUNT *p_bda_filter;

    p = param;
    memset(param, 0, 20);

    if (btm_ble_vendor_cb.p_scan_pf_cback)
    {
        BTM_TRACE_ERROR("ADV PF Filter activity busy");
        return BTM_BUSY;
    }

    if (enable)
    {
        p_bda_filter = btm_ble_find_addr_filter_counter(p_target);

        if (p_bda_filter == NULL ||
            (p_bda_filter && p_bda_filter->feat_mask == BTM_BLE_PF_SELECT_NONE))
        {
            BTM_TRACE_ERROR("No PF filter has been configured!");
            return st;
        }

        /* select feature based on control block settings */
        UINT8_TO_STREAM(p, BTM_BLE_META_PF_FEAT_SEL);
        UINT8_TO_STREAM(p, BTM_BLE_SCAN_COND_ADD);

        /* Filter index */
        UINT8_TO_STREAM(p, 0);

        /* set PCF selection */
        UINT32_TO_STREAM(p, p_bda_filter->feat_mask);
        /* set logic condition as OR as default */
        UINT8_TO_STREAM(p, BTM_BLE_PF_LOGIC_OR);

        if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                                   (UINT8)(BTM_BLE_META_HDR_LENGTH + BTM_BLE_PF_FEAT_SEL_LEN),
                                    param,
                                    btm_ble_vendor_scan_pf_cmpl_cback))
               == BTM_NO_RESOURCES)
        {
            return st;
        }

        /* enable the content filter in controller */
        p = param;
        UINT8_TO_STREAM(p, BTM_BLE_META_PF_ENABLE);
        /* enable adv data payload filtering */
        UINT8_TO_STREAM(p, enable);
    }
    else
    {
        UINT8_TO_STREAM(p, BTM_BLE_META_PF_ENABLE);
        /* disable adv data payload filtering */
        UINT8_TO_STREAM(p, enable);
    }

    if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_PCF_VSC,
                               BTM_BLE_PCF_ENABLE_LEN,
                               param,
                               btm_ble_vendor_scan_pf_cmpl_cback))
         == BTM_CMD_STARTED)
    {
        btm_ble_vendor_cb.op_type =  BTM_BLE_SET_SCAN_PF_OPCODE(BTM_BLE_META_PF_ENABLE, BTM_BLE_PF_ENABLE);
        btm_ble_vendor_cb.p_scan_pf_cback = p_cmpl_cback;
    }

    return st;
}
/*******************************************************************************
**
** Function         BTM_BleCfgFilterCondition
**
** Description      This function is called to configure the adv data payload filter
**                  condition.
**
** Parameters       action: to read/write/clear
**                  cond_type: filter condition type.
**                  p_cond: filter condition paramter
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS BTM_BleCfgFilterCondition(tBTM_BLE_SCAN_COND_OP action,
                                      tBTM_BLE_PF_COND_TYPE cond_type,
                                      tBTM_BLE_PF_COND_PARAM *p_cond,
                                      tBTM_BLE_PF_CMPL_CBACK *p_cmpl_cback)
{
    tBTM_STATUS     st = BTM_ILLEGAL_VALUE;

    if (btm_ble_vendor_cb.p_scan_pf_cback != NULL)
        return BTM_BUSY;

    switch (cond_type)
    {
    case BTM_BLE_PF_SRVC_DATA_PATTERN:
    /* write manufacture data filter */
    case BTM_BLE_PF_MANU_DATA:
        st = btm_ble_update_pf_manu_data(action, p_cond, cond_type);
        break;

    /* write local name filter */
    case BTM_BLE_PF_LOCAL_NAME:
        st = btm_ble_update_pf_local_name(action, p_cond);
        break;

    /* filter on advertiser address */
    case BTM_BLE_PF_ADDR_FILTER:
        st = btm_ble_update_addr_filter(action, p_cond);
        break;

    /* filter on service/solicitated UUID */
    case BTM_BLE_PF_SRVC_UUID:
    case BTM_BLE_PF_SRVC_SOL_UUID:
        st = btm_ble_update_uuid_filter(action, cond_type, p_cond);
        break;

    case BTM_BLE_PF_SRVC_DATA:
        st = btm_ble_update_srvc_data_change(action, p_cond);
        break;

    case BTM_BLE_PF_TYPE_ALL: /* only used to clear filter */
        st = btm_ble_clear_scan_pf_filter(action, p_cond);
        break;

    default:
        BTM_TRACE_WARNING("condition type [%d] not supported currently.", cond_type);
        break;
    }
    if (st == BTM_CMD_STARTED
        /* no vsc needed for service data change */
        && cond_type != BTM_BLE_PF_SRVC_DATA)
    {
        btm_ble_vendor_cb.op_type        = BTM_BLE_SET_SCAN_PF_OPCODE(cond_type, BTM_BLE_PF_CONFIG);
        btm_ble_vendor_cb.p_scan_pf_cback = p_cmpl_cback;
    }

    return st;
}

/*******************************************************************************
**         Resolve Address Using IRK List functions
*******************************************************************************/

#if BLE_PRIVACY_SPT == TRUE
/* Forward declaration */
tBTM_STATUS BTM_BleEnableIRKFeature(BOOLEAN enable);

/*******************************************************************************
**
** Function         btm_ble_vendor_enq_irk_pending
**
** Description      add target address into IRK pending operation queue
**
** Parameters       target_bda: target device address
**                  add_entry: TRUE for add entry, FALSE for remove entry
**
** Returns          void
**
*******************************************************************************/
void btm_ble_vendor_enq_irk_pending(BD_ADDR target_bda, BD_ADDR psuedo_bda, UINT8 to_add)
{
    tBTM_BLE_IRK_Q          *p_q = &btm_ble_vendor_cb.irk_pend_q;

    memcpy(p_q->irk_q[p_q->q_next], target_bda, BD_ADDR_LEN);
    memcpy(p_q->irk_q_random_pseudo[p_q->q_next], psuedo_bda, BD_ADDR_LEN);
    p_q->irk_q_action[p_q->q_next] = to_add;

    p_q->q_next ++;
    p_q->q_next %= BTM_CS_IRK_LIST_MAX;

    return ;
}
/*******************************************************************************
**
** Function         btm_ble_vendor_find_irk_pending_entry
**
** Description      check to see if the action is in pending list
**
** Parameters       TRUE: action pending;
**                  FALSE: new action
**
** Returns          void
**
*******************************************************************************/
BOOLEAN btm_ble_vendor_find_irk_pending_entry(BD_ADDR psuedo_addr, UINT8 action)
{
    tBTM_BLE_IRK_Q          *p_q = &btm_ble_vendor_cb.irk_pend_q;
    UINT8   i;

    for (i = p_q->q_pending; i != p_q->q_next; )
    {
        if (memcmp(p_q->irk_q_random_pseudo[i], psuedo_addr, BD_ADDR_LEN) == 0 &&
            action == p_q->irk_q_action[i])
            return TRUE;

        i ++;
        i %= BTM_CS_IRK_LIST_MAX;
    }
    return FALSE;
}
/*******************************************************************************
**
** Function         btm_ble_vendor_deq_irk_pending
**
** Description      add target address into IRK pending operation queue
**
** Parameters       target_bda: target device address
**                  add_entry: TRUE for add entry, FALSE for remove entry
**
** Returns          void
**
*******************************************************************************/
BOOLEAN btm_ble_vendor_deq_irk_pending(BD_ADDR target_bda, BD_ADDR psuedo_addr)
{
    tBTM_BLE_IRK_Q          *p_q = &btm_ble_vendor_cb.irk_pend_q;

    if (p_q->q_next != p_q->q_pending)
    {
        memcpy(target_bda, p_q->irk_q[p_q->q_pending], BD_ADDR_LEN);
        memcpy(psuedo_addr, p_q->irk_q_random_pseudo[p_q->q_pending], BD_ADDR_LEN);

        p_q->q_pending ++;
        p_q->q_pending %= BTM_CS_IRK_LIST_MAX;

        return TRUE;
    }

    return FALSE;

}
/*******************************************************************************
**
** Function         btm_ble_vendor_find_irk_entry
**
** Description      find IRK entry in local host IRK list by static address
**
** Returns          IRK list entry pointer
**
*******************************************************************************/
tBTM_BLE_IRK_ENTRY * btm_ble_vendor_find_irk_entry(BD_ADDR target_bda)
{
    tBTM_BLE_IRK_ENTRY  *p_irk_entry = &btm_ble_vendor_cb.irk_list[0];
    UINT8   i;

    for (i = 0; i < BTM_CS_IRK_LIST_MAX; i ++, p_irk_entry++)
    {
        if (p_irk_entry->in_use && memcmp(p_irk_entry->bd_addr, target_bda, BD_ADDR_LEN) == 0)
        {
            return p_irk_entry ;
        }
    }
    return NULL;
}
/*******************************************************************************
**
** Function         btm_ble_vendor_find_irk_entry_by_psuedo_addr
**
** Description      find IRK entry in local host IRK list by psuedo address
**
** Returns          IRK list entry pointer
**
*******************************************************************************/
tBTM_BLE_IRK_ENTRY * btm_ble_vendor_find_irk_entry_by_psuedo_addr (BD_ADDR psuedo_bda)
{
    tBTM_BLE_IRK_ENTRY  *p_irk_entry = &btm_ble_vendor_cb.irk_list[0];
    UINT8   i;

    for (i = 0; i < BTM_CS_IRK_LIST_MAX; i ++, p_irk_entry++)
    {
        if (p_irk_entry->in_use && memcmp(p_irk_entry->psuedo_bda, psuedo_bda, BD_ADDR_LEN) == 0)
        {
            return p_irk_entry ;
        }
    }
    return NULL;
}
/*******************************************************************************
**
** Function         btm_ble_vendor_alloc_irk_entry
**
** Description      allocate IRK entry in local host IRK list
**
** Returns          IRK list index
**
*******************************************************************************/
UINT8 btm_ble_vendor_alloc_irk_entry(BD_ADDR target_bda, BD_ADDR pseudo_bda)
{
    tBTM_BLE_IRK_ENTRY  *p_irk_entry = &btm_ble_vendor_cb.irk_list[0];
    UINT8   i;

    for (i = 0; i < BTM_CS_IRK_LIST_MAX; i ++, p_irk_entry++)
    {
        if (!p_irk_entry->in_use)
        {
            memcpy(p_irk_entry->bd_addr, target_bda, BD_ADDR_LEN);
            memcpy(p_irk_entry->psuedo_bda, pseudo_bda, BD_ADDR_LEN);

            p_irk_entry->index = i;
            p_irk_entry->in_use = TRUE;

            return i;
        }
    }
    return BTM_CS_IRK_LIST_INVALID;
}

/*******************************************************************************
**
** Function         btm_ble_vendor_update_irk_list
**
** Description      update IRK entry in local host IRK list
**
** Returns          void
**
*******************************************************************************/
void btm_ble_vendor_update_irk_list(BD_ADDR target_bda, BD_ADDR pseudo_bda, BOOLEAN add)
{
    tBTM_BLE_IRK_ENTRY   *p_irk_entry = btm_ble_vendor_find_irk_entry(target_bda);
    UINT8       i;

    if (add)
    {
        if (p_irk_entry == NULL)
        {
            if ((i = btm_ble_vendor_alloc_irk_entry(target_bda, pseudo_bda)) == BTM_CS_IRK_LIST_INVALID)
            {
                BTM_TRACE_ERROR("max IRK capacity reached");
            }
        }
        else
        {
            BTM_TRACE_WARNING(" IRK already in queue");
        }
    }
    else
    {
        if (p_irk_entry != NULL)
        {
            memset(p_irk_entry, 0, sizeof(tBTM_BLE_IRK_ENTRY));
        }
        else
        {
            BTM_TRACE_ERROR("No IRK exist in list, can not remove");
        }
    }
    return ;
}
/*******************************************************************************
**
** Function         btm_ble_vendor_irk_vsc_op_cmpl
**
** Description      IRK operation VSC complete handler
**
** Parameters
**
** Returns          void
**
*******************************************************************************/
void btm_ble_vendor_irk_vsc_op_cmpl (tBTM_VSC_CMPL *p_params)
{
    UINT8  status;
    UINT8  *p = p_params->p_param_buf, op_subcode;
    UINT16  evt_len = p_params->param_len;
    UINT8   i;
    tBTM_BLE_VENDOR_CB  *p_cb = &btm_ble_vendor_cb;
    BD_ADDR         target_bda, pseudo_bda, rra;


    STREAM_TO_UINT8(status, p);

    evt_len--;

    /*if (evt_len < 2 )
    {
        BTM_TRACE_ERROR("can not interpret IRK  VSC cmpl callback");
        return;
    }*/
    op_subcode   = *p ++;
    BTM_TRACE_DEBUG("btm_ble_vendor_irk_vsc_op_cmpl op_subcode = %d", op_subcode);
    if (evt_len < 2 )
    {
        BTM_TRACE_ERROR("can not interpret IRK  VSC cmpl callback");
        return;
    }


    if (op_subcode == BTM_BLE_META_CLEAR_IRK_LIST)
    {
        if (status == HCI_SUCCESS)
        {
            STREAM_TO_UINT8(p_cb->irk_avail_size, p);
            p_cb->irk_list_size = 0;

            BTM_TRACE_DEBUG("p_cb->irk_list_size = %d", p_cb->irk_avail_size);

            for (i = 0; i < BTM_CS_IRK_LIST_MAX; i ++)
                memset(&p_cb->irk_list[i], 0, sizeof(tBTM_BLE_IRK_ENTRY));
        }
    }
    else if (op_subcode == BTM_BLE_META_ADD_IRK_ENTRY)
    {
        if (!btm_ble_vendor_deq_irk_pending(target_bda, pseudo_bda))
        {
            BTM_TRACE_ERROR("no pending IRK operation");
            return;
        }

        if (status == HCI_SUCCESS)
        {
            STREAM_TO_UINT8(p_cb->irk_avail_size, p);
            btm_ble_vendor_update_irk_list(target_bda, pseudo_bda, TRUE);
        }
        else if (status == 0x07) /* BT_ERROR_CODE_MEMORY_CAPACITY_EXCEEDED  */
        {
            p_cb->irk_avail_size = 0;
            BTM_TRACE_ERROR("IRK Full ");
        }
        else
        {
            /* give the credit back if invalid parameter failed the operation */
            p_cb->irk_list_size ++;
        }
    }
    else if (op_subcode == BTM_BLE_META_REMOVE_IRK_ENTRY)
    {
        if (!btm_ble_vendor_deq_irk_pending(target_bda, pseudo_bda))
        {
            BTM_TRACE_ERROR("no pending IRK operation");
            return;
        }
        if (status == HCI_SUCCESS)
        {
            STREAM_TO_UINT8(p_cb->irk_avail_size, p);
            btm_ble_vendor_update_irk_list(target_bda, pseudo_bda, FALSE);
        }
        else
        {
            /* give the credit back if invalid parameter failed the operation */
            if (p_cb->irk_avail_size > 0)
                p_cb->irk_list_size --;
        }

    }
    else if (op_subcode == BTM_BLE_META_READ_IRK_ENTRY)
    {
        if (status == HCI_SUCCESS)
        {
            //STREAM_TO_UINT8(index, p);
            p += (1 + 16 + 1); /* skip index, IRK value, address type */
            STREAM_TO_BDADDR(target_bda, p);
            STREAM_TO_BDADDR(rra, p);

#if (defined BLE_VND_INCLUDED && BLE_VND_INCLUDED == TRUE)
            btm_ble_refresh_rra(target_bda, rra);
#endif
        }
    }

}
/*******************************************************************************
**
** Function         btm_ble_remove_irk_entry
**
** Description      This function to remove an IRK entry from the list
**
** Parameters       ble_addr_type: address type
**                  ble_addr: LE adddress
**
** Returns          status
**
*******************************************************************************/
tBTM_STATUS btm_ble_remove_irk_entry(tBTM_SEC_DEV_REC *p_dev_rec)
{
    UINT8           param[20], *p;
    tBTM_STATUS     st;
    tBTM_BLE_VENDOR_CB  *p_cb = &btm_ble_vendor_cb;

    p = param;
    memset(param, 0, 20);

    UINT8_TO_STREAM(p, BTM_BLE_META_REMOVE_IRK_ENTRY);
    UINT8_TO_STREAM(p, p_dev_rec->ble.static_addr_type);
    BDADDR_TO_STREAM(p, p_dev_rec->ble.static_addr);

    if ((st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_RPA_VSC,
                                    BTM_BLE_META_REMOVE_IRK_LEN,
                                    param,
                                    btm_ble_vendor_irk_vsc_op_cmpl))
        != BTM_NO_RESOURCES)
    {
        btm_ble_vendor_enq_irk_pending(p_dev_rec->ble.static_addr, p_dev_rec->bd_addr, FALSE);
        p_cb->irk_list_size --;
    }

    return st;

}
/*******************************************************************************
**
** Function         btm_ble_vendor_clear_irk_list
**
** Description      This function clears the IRK entry list
**
** Parameters       None.
**
** Returns          status
**
*******************************************************************************/
tBTM_STATUS btm_ble_vendor_clear_irk_list(void)
{
    UINT8           param[20], *p;
    tBTM_STATUS     st;

    p = param;
    memset(param, 0, 20);

    UINT8_TO_STREAM(p, BTM_BLE_META_CLEAR_IRK_LIST);

    st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_RPA_VSC,
                                    BTM_BLE_META_CLEAR_IRK_LEN,
                                    param,
                                    btm_ble_vendor_irk_vsc_op_cmpl);

    return st;

}
/*******************************************************************************
**
** Function         btm_ble_read_irk_entry
**
** Description      This function read an IRK entry by index
**
** Parameters       entry index.
**
** Returns          status
**
*******************************************************************************/
tBTM_STATUS btm_ble_read_irk_entry(BD_ADDR target_bda)
{
    UINT8           param[20], *p;
    tBTM_STATUS     st = BTM_UNKNOWN_ADDR;
    tBTM_BLE_IRK_ENTRY *p_entry = btm_ble_vendor_find_irk_entry(target_bda);

    if (p_entry == NULL)
        return st;

    p = param;
    memset(param, 0, 20);

    UINT8_TO_STREAM(p, BTM_BLE_META_READ_IRK_ENTRY);
    UINT8_TO_STREAM(p, p_entry->index);

    st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_RPA_VSC,
                                    BTM_BLE_META_READ_IRK_LEN,
                                    param,
                                    btm_ble_vendor_irk_vsc_op_cmpl);

    return st;

}


/*******************************************************************************
**
** Function         btm_ble_vendor_enable_irk_list_known_dev
**
** Description      This function add all known device with random address into
**                  IRK list.
**
** Parameters       enable: enable IRK list with known device, or disable it
**
** Returns          status
**
*******************************************************************************/
void btm_ble_vendor_irk_list_known_dev(BOOLEAN enable)
{
    UINT8               i;
    UINT8               count = 0;
    tBTM_SEC_DEV_REC    *p_dev_rec = &btm_cb.sec_dev_rec[0];

    /* add all known device with random address into IRK list */
    for (i = 0; i < BTM_SEC_MAX_DEVICE_RECORDS; i ++, p_dev_rec ++)
    {
        if (p_dev_rec->sec_flags & BTM_SEC_IN_USE)
        {
            if (btm_ble_vendor_irk_list_load_dev(p_dev_rec))
                count ++;
        }
    }

    if ((count > 0 && enable) || !enable)
        BTM_BleEnableIRKFeature(enable);

    return ;
}
/*******************************************************************************
**
** Function         btm_ble_vendor_irk_list_load_dev
**
** Description      This function add a device which is using RPA into white list
**
** Parameters
**
** Returns          status
**
*******************************************************************************/
BOOLEAN btm_ble_vendor_irk_list_load_dev(tBTM_SEC_DEV_REC *p_dev_rec)
{
    UINT8           param[40], *p;
    tBTM_BLE_VENDOR_CB  *p_cb = &btm_ble_vendor_cb;
    BOOLEAN         rt = FALSE;
    tBTM_BLE_IRK_ENTRY  *p_irk_entry = NULL;
    BTM_TRACE_DEBUG ("btm_ble_vendor_irk_list_load_dev:max_irk_size=%d", p_cb->irk_avail_size);
    memset(param, 0, 40);

    if (p_dev_rec != NULL && /* RPA is being used and PID is known */
        (p_dev_rec->ble.key_type & BTM_LE_KEY_PID) != 0)
    {

        if ((p_irk_entry = btm_ble_vendor_find_irk_entry_by_psuedo_addr(p_dev_rec->bd_addr)) == NULL &&
            btm_ble_vendor_find_irk_pending_entry(p_dev_rec->bd_addr, TRUE) == FALSE)
        {

            if (p_cb->irk_avail_size > 0)
            {
                p = param;

                UINT8_TO_STREAM(p, BTM_BLE_META_ADD_IRK_ENTRY);
                ARRAY_TO_STREAM(p, p_dev_rec->ble.keys.irk, BT_OCTET16_LEN);
                UINT8_TO_STREAM(p, p_dev_rec->ble.static_addr_type);
                BDADDR_TO_STREAM(p,p_dev_rec->ble.static_addr);

                if (BTM_VendorSpecificCommand (HCI_VENDOR_BLE_RPA_VSC,
                                                BTM_BLE_META_ADD_IRK_LEN,
                                                param,
                                                btm_ble_vendor_irk_vsc_op_cmpl)
                       != BTM_NO_RESOURCES)
                {
                    btm_ble_vendor_enq_irk_pending(p_dev_rec->ble.static_addr, p_dev_rec->bd_addr, TRUE);
                    p_cb->irk_list_size ++;
                    rt = TRUE;
                }
            }
        }
        else
        {
            BTM_TRACE_ERROR("Device already in IRK list");
            rt = TRUE;
        }
    }
    else
    {
        BTM_TRACE_DEBUG("Device not a RPA enabled device");
    }
    return rt;
}
/*******************************************************************************
**
** Function         btm_ble_vendor_irk_list_remove_dev
**
** Description      This function remove the device from IRK list
**
** Parameters
**
** Returns          status
**
*******************************************************************************/
void btm_ble_vendor_irk_list_remove_dev(tBTM_SEC_DEV_REC *p_dev_rec)
{
    tBTM_BLE_VENDOR_CB  *p_cs_cb = &btm_ble_vendor_cb;
    tBTM_BLE_IRK_ENTRY *p_irk_entry;

    if ((p_irk_entry = btm_ble_vendor_find_irk_entry_by_psuedo_addr(p_dev_rec->bd_addr)) != NULL &&
        btm_ble_vendor_find_irk_pending_entry(p_dev_rec->bd_addr, FALSE) == FALSE)
    {
        btm_ble_remove_irk_entry(p_dev_rec);
    }
    else
    {
        BTM_TRACE_ERROR("Device not in IRK list");
    }

    if (p_cs_cb->irk_list_size == 0)
        BTM_BleEnableIRKFeature(FALSE);
}
/*******************************************************************************
**
** Function         btm_ble_vendor_disable_irk_list
**
** Description      disable LE resolve address feature
**
** Parameters
**
** Returns          status
**
*******************************************************************************/
void btm_ble_vendor_disable_irk_list(void)
{
    BTM_BleEnableIRKFeature(FALSE);

}

/*******************************************************************************
**
** Function         BTM_BleEnableIRKFeature
**
** Description      This function is called to enable or disable the RRA
**                  offloading feature.
**
** Parameters       enable: enable or disable the RRA offloading feature
**
** Returns          BTM_SUCCESS if successful
**
*******************************************************************************/
tBTM_STATUS BTM_BleEnableIRKFeature(BOOLEAN enable)
{
    UINT8           param[20], *p;
    tBTM_STATUS     st = BTM_WRONG_MODE;
    tBTM_BLE_PF_COUNT *p_bda_filter;

    p = param;
    memset(param, 0, 20);

    /* select feature based on control block settings */
    UINT8_TO_STREAM(p, BTM_BLE_META_IRK_ENABLE);
    UINT8_TO_STREAM(p, enable ? 0x01 : 0x00);

    st = BTM_VendorSpecificCommand (HCI_VENDOR_BLE_RPA_VSC, BTM_BLE_IRK_ENABLE_LEN,
                               param, btm_ble_vendor_irk_vsc_op_cmpl);

    return st;
}

#endif


/*******************************************************************************
**
** Function         btm_ble_vendor_init
**
** Description      Initialize customer specific feature information in host stack
**
** Parameters
**
** Returns          status
**
*******************************************************************************/
void btm_ble_vendor_init(void)
{
    //memset(&btm_ble_vendor_cb, 0, sizeof(tBTM_BLE_VENDOR_CB));

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return;
}

#endif

