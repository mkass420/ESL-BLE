/**
 * Copyright 2022 Evgeniy Morozov
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE
 */

#include "estc_service.h"

#include "app_error.h"
#include "ble_gap.h"
#include "ble_types.h"
#include "nrf_log.h"

#include "ble.h"
#include "ble_gatts.h"
#include "ble_srv_common.h"
#include "sdk_errors.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* some handy macros */
#define ESTC_RETURN_IF_ERROR(ret_code)          \
    if(ret_code != NRF_SUCCESS) return ret_code

#define ESTC_BLE_GAP_CONN_SEC_MODE_SET_OPEN_OR_NOT_ACCESS(ptr, val) \
    if(val)                                                         \
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(ptr);                        \
    else                                                            \
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(ptr)

static ble_uuid_t m_char_uuids[] = {
    [ESTC_GATT_CHAR_DEFAULT] = {ESTC_GATT_CHAR_1_UUID_VALUE, BLE_UUID_TYPE_UNKNOWN},
    [ESTC_GATT_CHAR_NOTIFY] = {ESTC_GATT_CHAR_NOTIFY_UUID_VALUE, BLE_UUID_TYPE_UNKNOWN},
    [ESTC_GATT_CHAR_INDICATE] = {ESTC_GATT_CHAR_INDICATE_UUID_VALUE, BLE_UUID_TYPE_UNKNOWN}
};
STATIC_ASSERT(sizeof(m_char_uuids) / sizeof(m_char_uuids[0]) == ESTC_GATT_CHAR_COUNT, "ESTC_GATT_CHAR_COUNT should be equal to number of characteristic uuids inside m_char_uuids[]");

static ret_code_t estc_ble_add_characteristic(ble_estc_service_t* service,
                                              estc_gatt_chars_t   char_idx,
                                              bool                read_prop,
                                              bool                write_prop,
                                              bool                notify_prop,
                                              bool                indicate_prop,
                                              size_t              value_size,
                                              void*               p_init_value,
                                              size_t              max_value_size);

static ret_code_t estc_ble_add_all_characteristics(ble_estc_service_t* service);
static bool       estc_ble_hvx_error_is_expected(ret_code_t error_code);

ret_code_t estc_ble_service_init(ble_estc_service_t* service) {
    ret_code_t error_code = NRF_SUCCESS;

    service->connection_handle = BLE_CONN_HANDLE_INVALID;
    service->uuid_type         = BLE_UUID_TYPE_UNKNOWN;
    service->notification_enabled = false;
    service->indication_enabled   = false;
    service->indication_in_flight = false;

    ble_uuid128_t base_uuid    = {ESTC_BASE_UUID};
    ble_uuid_t    service_uuid = {ESTC_SERVICE_UUID, BLE_UUID_TYPE_UNKNOWN};

    // TODO: 3. Add service UUIDs to the BLE stack table using `sd_ble_uuid_vs_add`
    error_code = sd_ble_uuid_vs_add(&base_uuid, &service_uuid.type);
    ESTC_RETURN_IF_ERROR(error_code);

    service->uuid_type = service_uuid.type;

    for(size_t i = 0; i < ESTC_GATT_CHAR_COUNT; ++i) {
        m_char_uuids[i].type = service_uuid.type;
    }

    // TODO: 4. Add service to the BLE stack using `sd_ble_gatts_service_add`
    error_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &service_uuid, &service->service_handle);
    ESTC_RETURN_IF_ERROR(error_code);

    NRF_LOG_DEBUG("%s:%d | Service UUID: 0x%04x", __FUNCTION__, __LINE__, service_uuid.uuid);
    NRF_LOG_DEBUG("%s:%d | Service UUID type: 0x%02x", __FUNCTION__, __LINE__, service_uuid.type);
    NRF_LOG_DEBUG("%s:%d | Service handle: 0x%04x", __FUNCTION__, __LINE__, service->service_handle);

    return estc_ble_add_all_characteristics(service);
}

static ret_code_t estc_ble_add_characteristic(ble_estc_service_t* service,
                                              estc_gatt_chars_t   char_idx,
                                              bool                read_prop,
                                              bool                write_prop,
                                              bool                notify_prop,
                                              bool                indicate_prop,
                                              size_t              value_size,
                                              void*               p_init_value,
                                              size_t              max_value_size) {
    if(char_idx >= ESTC_GATT_CHAR_COUNT) return NRF_ERROR_INVALID_PARAM;
    if(value_size > max_value_size) return NRF_ERROR_INVALID_LENGTH;

    ret_code_t error_code = NRF_SUCCESS;

    // TODO: 6.1. Add custom characteristic UUID using `sd_ble_uuid_vs_add`, same as in step 4
    ble_uuid_t char_uuid = m_char_uuids[char_idx];

    // TODO: 6.5. Configure Characteristic metadata (enable read and write)
    ble_gatts_char_md_t char_md = {0};
    char_md.char_props.read     = read_prop;
    char_md.char_props.write    = write_prop;
    char_md.char_props.notify   = notify_prop;
    char_md.char_props.indicate = indicate_prop;

    char user_desc[28] = {0};
    snprintf(user_desc, sizeof(user_desc), "My cool descriptor number %d", char_idx);

    ble_gatts_attr_md_t user_desc_md = {0};
    user_desc_md.vloc                = BLE_GATTS_VLOC_STACK;

    /* Descriptors should always be read-only, otherwise it is very stupid */
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&user_desc_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&user_desc_md.write_perm);

    char_md.p_user_desc_md          = &user_desc_md;
    char_md.p_char_user_desc        = (uint8_t*)user_desc;
    char_md.char_user_desc_size     = sizeof(user_desc);
    char_md.char_user_desc_max_size = sizeof(user_desc);

    ble_gatts_attr_md_t cccd_md = {0};
    if(notify_prop || indicate_prop) {
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
        cccd_md.vloc       = BLE_GATTS_VLOC_STACK;
        char_md.p_cccd_md  = &cccd_md;
    }

    // Configures attribute metadata. For now we only specify that the attribute will be stored in the softdevice
    ble_gatts_attr_md_t attr_md = {0};
    attr_md.vloc                = BLE_GATTS_VLOC_STACK;

    // TODO: 6.6. Set read/write security levels to our attribute metadata using `BLE_GAP_CONN_SEC_MODE_SET_OPEN`
    ESTC_BLE_GAP_CONN_SEC_MODE_SET_OPEN_OR_NOT_ACCESS(&attr_md.read_perm, read_prop);
    ESTC_BLE_GAP_CONN_SEC_MODE_SET_OPEN_OR_NOT_ACCESS(&attr_md.write_perm, write_prop);

    // TODO: 6.2. Configure the characteristic value attribute (set the UUID and metadata)
    ble_gatts_attr_t attr_char_value = {0};
    attr_char_value.p_uuid           = &char_uuid;
    attr_char_value.p_attr_md        = &attr_md;

    // TODO: 6.7. Set characteristic length in number of bytes in attr_char_value structure
    attr_char_value.init_len = value_size;
    attr_char_value.max_len  = max_value_size;

    if(p_init_value != NULL) {
        attr_char_value.p_value = (uint8_t*)p_init_value;
    }

    // TODO: 6.4. Add new characteristic to the service using `sd_ble_gatts_characteristic_add`
    //
    error_code = sd_ble_gatts_characteristic_add(service->service_handle,
                                                 &char_md,
                                                 &attr_char_value,
                                                 &service->characteristic_handles[char_idx]);
    ESTC_RETURN_IF_ERROR(error_code);

    return NRF_SUCCESS;
}

static ret_code_t estc_ble_add_all_characteristics(ble_estc_service_t* service) {
    ret_code_t error_code = NRF_SUCCESS;

    int16_t default_val   = 123;
    uint32_t notify_val = 0;
    uint32_t indicate_val = 0;

    error_code = estc_ble_add_characteristic(service, ESTC_GATT_CHAR_DEFAULT, true, true, false, false, sizeof(default_val), &default_val, sizeof(default_val));
    ESTC_RETURN_IF_ERROR(error_code);

    error_code = estc_ble_add_characteristic(service, ESTC_GATT_CHAR_NOTIFY, true, false, true, false, sizeof(notify_val), &notify_val, sizeof(notify_val));
    ESTC_RETURN_IF_ERROR(error_code);

    error_code = estc_ble_add_characteristic(service, ESTC_GATT_CHAR_INDICATE, true, false, false, true, sizeof(indicate_val), &indicate_val, sizeof(indicate_val));
    ESTC_RETURN_IF_ERROR(error_code);

    return NRF_SUCCESS;
}

static bool estc_ble_hvx_error_is_expected(ret_code_t error_code) {
    switch(error_code) {
        case BLE_ERROR_INVALID_CONN_HANDLE:
        case BLE_ERROR_GATTS_SYS_ATTR_MISSING:
        case NRF_ERROR_BUSY:
        case NRF_ERROR_INVALID_STATE:
        case NRF_ERROR_RESOURCES:
            return true;

        default:
            return false;
    }
}

ret_code_t estc_update_characteristic_value(ble_estc_service_t* service, size_t char_idx, void* p_value, size_t value_len) {
    if(service == NULL)                  return NRF_ERROR_INVALID_PARAM;
    if(char_idx >= ESTC_GATT_CHAR_COUNT) return NRF_ERROR_INVALID_PARAM;
    if(p_value == NULL)                  return NRF_ERROR_INVALID_PARAM;
    if(value_len == 0)                   return NRF_ERROR_INVALID_LENGTH;
    if(value_len > UINT16_MAX)           return NRF_ERROR_INVALID_LENGTH;

    ble_gatts_value_t gatt_value = {
        .len     = (uint16_t)value_len,
        .offset  = 0,
        .p_value = (uint8_t*)p_value
    };

    ret_code_t error_code = sd_ble_gatts_value_set(service->connection_handle, service->characteristic_handles[char_idx].value_handle, &gatt_value);
    ESTC_RETURN_IF_ERROR(error_code);

    return error_code;
}

ret_code_t estc_read_characteristic_value(ble_estc_service_t* service, size_t char_idx, void* p_value, size_t* p_value_len) {
    if(service == NULL)                  return NRF_ERROR_INVALID_PARAM;
    if(char_idx >= ESTC_GATT_CHAR_COUNT) return NRF_ERROR_INVALID_PARAM;
    if(p_value == NULL)                  return NRF_ERROR_INVALID_PARAM;
    if(p_value_len == NULL)              return NRF_ERROR_INVALID_PARAM;
    if(*p_value_len == 0)                return NRF_ERROR_INVALID_LENGTH;
    if(*p_value_len > UINT16_MAX)        return NRF_ERROR_INVALID_LENGTH;

    ble_gatts_value_t gatt_value = {
        .len     = (uint16_t)*p_value_len,
        .offset  = 0,
        .p_value = (uint8_t*)p_value
    };

    ret_code_t error_code = sd_ble_gatts_value_get(service->connection_handle, service->characteristic_handles[char_idx].value_handle, &gatt_value);
    ESTC_RETURN_IF_ERROR(error_code);

    *p_value_len = gatt_value.len;

    return error_code;
}

ret_code_t estc_send_characteristic_value(ble_estc_service_t* service, estc_gatt_chars_t char_idx, void* p_value, size_t value_len) {
    if(service == NULL)                  return NRF_ERROR_INVALID_PARAM;
    if(char_idx >= ESTC_GATT_CHAR_COUNT) return NRF_ERROR_INVALID_PARAM;
    if(p_value == NULL)                  return NRF_ERROR_INVALID_PARAM;
    if(value_len == 0)                   return NRF_ERROR_INVALID_LENGTH;
    if(value_len > UINT16_MAX)           return NRF_ERROR_INVALID_LENGTH;

    ret_code_t error_code = estc_update_characteristic_value(service, char_idx, p_value, value_len);
    if(error_code != NRF_SUCCESS) {
        if(estc_ble_hvx_error_is_expected(error_code)) return NRF_SUCCESS;
        return error_code;
    }

    if(service->connection_handle == BLE_CONN_HANDLE_INVALID) return NRF_SUCCESS;

    uint8_t hvx_type = 0;
    if(char_idx == ESTC_GATT_CHAR_NOTIFY) {
        if(!service->notification_enabled) return NRF_SUCCESS;
        hvx_type = BLE_GATT_HVX_NOTIFICATION;
    } else if(char_idx == ESTC_GATT_CHAR_INDICATE) {
        if(!service->indication_enabled)  return NRF_SUCCESS;
        if(service->indication_in_flight) return NRF_SUCCESS;
        hvx_type = BLE_GATT_HVX_INDICATION;
    } else {
        return NRF_ERROR_INVALID_PARAM;
    }

    uint16_t hvx_len = (uint16_t)value_len;
    ble_gatts_hvx_params_t hvx_params = {
        .handle = service->characteristic_handles[char_idx].value_handle,
        .type   = hvx_type,
        .offset = 0,
        .p_len  = &hvx_len,
        .p_data = (uint8_t*)p_value
    };

    error_code = sd_ble_gatts_hvx(service->connection_handle, &hvx_params);
    if(error_code == NRF_SUCCESS) {
        if((size_t)hvx_len != value_len) return NRF_ERROR_DATA_SIZE;
        if(char_idx == ESTC_GATT_CHAR_INDICATE) service->indication_in_flight = true;
        return NRF_SUCCESS;
    }

    if(error_code == NRF_ERROR_BUSY && char_idx == ESTC_GATT_CHAR_INDICATE) {
        service->indication_in_flight = true;
    }

    if(estc_ble_hvx_error_is_expected(error_code)) return NRF_SUCCESS;

    return error_code;
}

void estc_ble_service_on_ble_event(const ble_evt_t* ble_evt, void* ctx) {
    if(ble_evt == NULL) return;
    if(ctx == NULL)     return;

    ble_estc_service_t* service = (ble_estc_service_t*)ctx;

    switch(ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            service->connection_handle = ble_evt->evt.gap_evt.conn_handle;
            service->notification_enabled = false;
            service->indication_enabled   = false;
            service->indication_in_flight = false;
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            service->connection_handle = BLE_CONN_HANDLE_INVALID;
            service->notification_enabled = false;
            service->indication_enabled   = false;
            service->indication_in_flight = false;
            break;

        case BLE_GATTS_EVT_WRITE:
            if(ble_evt->evt.gatts_evt.params.write.len >= sizeof(uint16_t) &&
               ble_evt->evt.gatts_evt.params.write.handle == service->characteristic_handles[ESTC_GATT_CHAR_NOTIFY].cccd_handle) {
                service->notification_enabled = ble_srv_is_notification_enabled(ble_evt->evt.gatts_evt.params.write.data);
                NRF_LOG_DEBUG("ESTC notification enabled: %u", (unsigned int)service->notification_enabled);
            } else if(ble_evt->evt.gatts_evt.params.write.len >= sizeof(uint16_t) &&
                      ble_evt->evt.gatts_evt.params.write.handle == service->characteristic_handles[ESTC_GATT_CHAR_INDICATE].cccd_handle) {
                service->indication_enabled = ble_srv_is_indication_enabled(ble_evt->evt.gatts_evt.params.write.data);
                if(!service->indication_enabled) service->indication_in_flight = false;
                NRF_LOG_DEBUG("ESTC indication enabled: %u", (unsigned int)service->indication_enabled);
            } else {
                for(size_t i = 0; i < ESTC_GATT_CHAR_COUNT; ++i) {
                    if(ble_evt->evt.gatts_evt.params.write.handle == service->characteristic_handles[i].value_handle) {
                        NRF_LOG_DEBUG("ESTC characteristic %u written", (unsigned int)i);
                        break;
                    }
                }
            }
            break;

        case BLE_GATTS_EVT_HVC:
            if(ble_evt->evt.gatts_evt.params.hvc.handle == service->characteristic_handles[ESTC_GATT_CHAR_INDICATE].value_handle) {
                service->indication_in_flight = false;
            }
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING: {
            ret_code_t error_code = sd_ble_gatts_sys_attr_set(service->connection_handle, NULL, 0, 0);
            APP_ERROR_CHECK(error_code);
            break;
        }

        default:
            break;
    }
}
