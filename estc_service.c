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
#include "ble_srv_common.h"
#include "nrf_log.h"

#include <stddef.h>
#include <string.h>

#define ESTC_RETURN_IF_ERROR(err_code)  \
    do {                                \
        if((err_code) != NRF_SUCCESS) { \
            return (err_code);          \
        }                               \
    } while(0)

static ble_uuid_t m_char_uuids[ESTC_LED_CHAR_COUNT] =
    {
        [ESTC_LED_CHAR_STATE]      = {     ESTC_LED_STATE_UUID, BLE_UUID_TYPE_UNKNOWN},
        [ESTC_LED_CHAR_HUE]        = {       ESTC_LED_HUE_UUID, BLE_UUID_TYPE_UNKNOWN},
        [ESTC_LED_CHAR_SATURATION] = {ESTC_LED_SATURATION_UUID, BLE_UUID_TYPE_UNKNOWN},
        [ESTC_LED_CHAR_VALUE]      = {     ESTC_LED_VALUE_UUID, BLE_UUID_TYPE_UNKNOWN},
};

STATIC_ASSERT((sizeof(m_char_uuids) / sizeof(m_char_uuids[0])) == ESTC_LED_CHAR_COUNT,
              "Characteristic table size mismatch");

static bool estc_hvx_error_is_non_fatal(ret_code_t err_code) {
    switch(err_code) {
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

static ret_code_t estc_ble_add_characteristic(ble_estc_service_t* p_service,
                                              estc_led_char_t     characteristic,
                                              void const*         p_initial_value,
                                              uint16_t            value_len) {
    ble_uuid_t const char_uuid = m_char_uuids[characteristic];

    ble_gatts_char_md_t char_md;
    memset(&char_md, 0, sizeof(char_md));

    char_md.char_props.read   = 1U;
    char_md.char_props.write  = 1U;
    char_md.char_props.notify = 1U;

    ble_gatts_attr_md_t cccd_md;
    memset(&cccd_md, 0, sizeof(cccd_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
    cccd_md.vloc      = BLE_GATTS_VLOC_STACK;
    char_md.p_cccd_md = &cccd_md;

    ble_gatts_attr_md_t attr_md;
    memset(&attr_md, 0, sizeof(attr_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
    attr_md.vloc = BLE_GATTS_VLOC_STACK;

    ble_gatts_attr_t attr_value;
    memset(&attr_value, 0, sizeof(attr_value));
    attr_value.p_uuid    = &char_uuid;
    attr_value.p_attr_md = &attr_md;
    attr_value.init_len  = value_len;
    attr_value.max_len   = value_len;
    attr_value.p_value   = (uint8_t*)p_initial_value;

    return sd_ble_gatts_characteristic_add(p_service->service_handle,
                                           &char_md,
                                           &attr_value,
                                           &p_service->characteristic_handles[characteristic]);
}

static ret_code_t estc_ble_update_value(ble_estc_service_t* p_service,
                                        estc_led_char_t     characteristic,
                                        uint8_t const*      p_data,
                                        uint16_t            value_len) {
    ble_gatts_value_t gatt_value =
        {
            .len     = value_len,
            .offset  = 0,
            .p_value = (uint8_t*)p_data,
        };

    return sd_ble_gatts_value_set(BLE_CONN_HANDLE_INVALID,
                                  p_service->characteristic_handles[characteristic].value_handle,
                                  &gatt_value);
}

static ret_code_t estc_ble_update_and_notify(ble_estc_service_t* p_service,
                                             estc_led_char_t     characteristic,
                                             uint8_t const*      p_data,
                                             uint16_t            value_len,
                                             bool                notify) {
    ret_code_t err_code = estc_ble_update_value(p_service, characteristic, p_data, value_len);
    ESTC_RETURN_IF_ERROR(err_code);

    if(!notify ||
       (p_service->connection_handle == BLE_CONN_HANDLE_INVALID) ||
       !p_service->notification_enabled[characteristic]) {
        return NRF_SUCCESS;
    }

    uint16_t               hvx_len = value_len;
    ble_gatts_hvx_params_t hvx_params;
    memset(&hvx_params, 0, sizeof(hvx_params));

    hvx_params.handle = p_service->characteristic_handles[characteristic].value_handle;
    hvx_params.type   = BLE_GATT_HVX_NOTIFICATION;
    hvx_params.offset = 0U;
    hvx_params.p_len  = &hvx_len;
    hvx_params.p_data = (uint8_t*)p_data;

    err_code = sd_ble_gatts_hvx(p_service->connection_handle, &hvx_params);
    if(err_code == NRF_SUCCESS) {
        return ((uint16_t)value_len == hvx_len) ? NRF_SUCCESS : NRF_ERROR_DATA_SIZE;
    }

    return estc_hvx_error_is_non_fatal(err_code) ? NRF_SUCCESS : err_code;
}

ret_code_t estc_ble_service_init(ble_estc_service_t*            p_service,
                                 ble_estc_service_init_t const* p_init) {
    if((p_service == NULL) || (p_init == NULL)) {
        return NRF_ERROR_NULL;
    }

    memset(p_service, 0, sizeof(*p_service));
    p_service->connection_handle = BLE_CONN_HANDLE_INVALID;
    p_service->write_handler     = p_init->write_handler;
    p_service->p_write_context   = p_init->p_context;

    ble_uuid128_t base_uuid    = {ESTC_BASE_UUID};
    ble_uuid_t    service_uuid = {ESTC_SERVICE_UUID, BLE_UUID_TYPE_UNKNOWN};

    ret_code_t err_code = sd_ble_uuid_vs_add(&base_uuid, &service_uuid.type);
    ESTC_RETURN_IF_ERROR(err_code);

    p_service->uuid_type = service_uuid.type;

    for(size_t i = 0; i < ESTC_LED_CHAR_COUNT; ++i) {
        m_char_uuids[i].type = p_service->uuid_type;
    }

    err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                        &service_uuid,
                                        &p_service->service_handle);
    ESTC_RETURN_IF_ERROR(err_code);

    uint8_t state_value      = p_init->initial_state;
    uint8_t saturation_value = p_init->initial_saturation;
    uint8_t level_value      = p_init->initial_value;
    uint8_t hue_value[sizeof(uint16_t)];
    uint16_encode(p_init->initial_hue, hue_value);

    err_code = estc_ble_add_characteristic(p_service, ESTC_LED_CHAR_STATE, &state_value, sizeof(state_value));
    ESTC_RETURN_IF_ERROR(err_code);

    err_code = estc_ble_add_characteristic(p_service, ESTC_LED_CHAR_HUE, hue_value, sizeof(hue_value));
    ESTC_RETURN_IF_ERROR(err_code);

    err_code = estc_ble_add_characteristic(p_service, ESTC_LED_CHAR_SATURATION, &saturation_value, sizeof(saturation_value));
    ESTC_RETURN_IF_ERROR(err_code);

    err_code = estc_ble_add_characteristic(p_service, ESTC_LED_CHAR_VALUE, &level_value, sizeof(level_value));
    ESTC_RETURN_IF_ERROR(err_code);

    return NRF_SUCCESS;
}

ret_code_t estc_ble_service_set_state(ble_estc_service_t* p_service,
                                      uint8_t             state,
                                      bool                notify) {
    if(p_service == NULL) {
        return NRF_ERROR_NULL;
    }

    return estc_ble_update_and_notify(p_service, ESTC_LED_CHAR_STATE, &state, sizeof(state), notify);
}

ret_code_t estc_ble_service_set_hue(ble_estc_service_t* p_service,
                                    uint16_t            hue,
                                    bool                notify) {
    if(p_service == NULL) {
        return NRF_ERROR_NULL;
    }

    uint8_t encoded_hue[sizeof(uint16_t)];
    uint16_encode(hue, encoded_hue);

    return estc_ble_update_and_notify(p_service, ESTC_LED_CHAR_HUE, encoded_hue, sizeof(encoded_hue), notify);
}

ret_code_t estc_ble_service_set_saturation(ble_estc_service_t* p_service,
                                           uint8_t             saturation,
                                           bool                notify) {
    if(p_service == NULL) {
        return NRF_ERROR_NULL;
    }

    return estc_ble_update_and_notify(p_service,
                                      ESTC_LED_CHAR_SATURATION,
                                      &saturation,
                                      sizeof(saturation),
                                      notify);
}

ret_code_t estc_ble_service_set_value(ble_estc_service_t* p_service,
                                      uint8_t             value,
                                      bool                notify) {
    if(p_service == NULL) {
        return NRF_ERROR_NULL;
    }

    return estc_ble_update_and_notify(p_service, ESTC_LED_CHAR_VALUE, &value, sizeof(value), notify);
}

void estc_ble_service_on_ble_event(ble_evt_t const* p_ble_evt, void* p_context) {
    if((p_ble_evt == NULL) || (p_context == NULL)) {
        return;
    }

    ble_estc_service_t* p_service = (ble_estc_service_t*)p_context;

    switch(p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            p_service->connection_handle = p_ble_evt->evt.gap_evt.conn_handle;
            memset(p_service->notification_enabled, 0, sizeof(p_service->notification_enabled));
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            p_service->connection_handle = BLE_CONN_HANDLE_INVALID;
            memset(p_service->notification_enabled, 0, sizeof(p_service->notification_enabled));
            break;

        case BLE_GATTS_EVT_WRITE:
            {
                ble_gatts_evt_write_t const* p_write = &p_ble_evt->evt.gatts_evt.params.write;

                for(size_t i = 0; i < ESTC_LED_CHAR_COUNT; ++i) {
                    if(p_write->handle == p_service->characteristic_handles[i].cccd_handle) {
                        p_service->notification_enabled[i] = ble_srv_is_notification_enabled(p_write->data);
                        return;
                    }

                    if(p_write->handle == p_service->characteristic_handles[i].value_handle) {
                        if(p_service->write_handler != NULL) {
                            p_service->write_handler(p_service,
                                                     (estc_led_char_t)i,
                                                     p_write->data,
                                                     p_write->len,
                                                     p_service->p_write_context);
                        }
                        return;
                    }
                }
            }
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            {
                ret_code_t err_code = sd_ble_gatts_sys_attr_set(p_service->connection_handle, NULL, 0, 0);
                APP_ERROR_CHECK(err_code);
            }
            break;

        default:
            break;
    }
}
