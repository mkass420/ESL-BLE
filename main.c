/**
 * Copyright (c) 2014 - 2021, Nordic Semiconductor ASA
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/** @file
 *
 * @defgroup estc_gatt main.c
 * @{
 * @ingroup estc_templates
 * @brief ESTC-GATT LED control application.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_error.h"
#include "app_timer.h"
#include "ble.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "boards.h"
#include "bsp_btn_ble.h"
#include "nrf.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"
#include "nrf_gpio.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"

#include "nrf_log.h"
#include "nrf_log_backend_usb.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"

#include "estc_service.h"
#include "led_pwm.h"
#include "led_state.h"
#include "led_storage.h"

#define DEVICE_NAME           "ESTC-SVC"
#define APP_ADV_INTERVAL      300
#define APP_ADV_DURATION      18000
#define APP_BLE_OBSERVER_PRIO 3
#define APP_BLE_CONN_CFG_TAG  1

#define MIN_CONN_INTERVAL MSEC_TO_UNITS(100, UNIT_1_25_MS)
#define MAX_CONN_INTERVAL MSEC_TO_UNITS(200, UNIT_1_25_MS)
#define SLAVE_LATENCY     0
#define CONN_SUP_TIMEOUT  MSEC_TO_UNITS(4000, UNIT_10_MS)

#define FIRST_CONN_PARAMS_UPDATE_DELAY APP_TIMER_TICKS(5000)
#define NEXT_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(30000)
#define MAX_CONN_PARAMS_UPDATE_COUNT   3
#define STATUS_LED_BLINK_INTERVAL      APP_TIMER_TICKS(400)

#define DEAD_BEEF 0xDEADBEEF

#define RETURN_IF_ERROR(err_code)     \
    do {                              \
        ret_code_t err_ = (err_code); \
        if(err_ != NRF_SUCCESS) {     \
            return err_;              \
        }                             \
    } while(0)

NRF_BLE_GATT_DEF(m_gatt);
NRF_BLE_QWR_DEF(m_qwr);
BLE_ADVERTISING_DEF(m_advertising);
APP_TIMER_DEF(m_status_led_timer_id);

static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;
static bool     m_status_led_blinking;
static bool     m_status_led_is_on;

static ble_uuid_t m_adv_uuids[] =
    {
        {ESTC_SERVICE_UUID, BLE_UUID_TYPE_UNKNOWN},
};

static ble_estc_service_t m_estc_service;
static led_state_t        m_led_state;

static void advertising_start(void);

static uint16_t decode_u16_le(uint8_t const* p_data) {
    return (uint16_t)p_data[0] | ((uint16_t)p_data[1] << 8);
}

static void status_led_on(void) {
    nrf_gpio_pin_clear(LED1_G);
    m_status_led_is_on = true;
}

static void status_led_off(void) {
    nrf_gpio_pin_set(LED1_G);
    m_status_led_is_on = false;
}

static void status_led_toggle(void) {
    if(m_status_led_is_on) {
        status_led_off();
    }
    else {
        status_led_on();
    }
}

static void status_led_timer_stop(void) {
    ret_code_t err_code = app_timer_stop(m_status_led_timer_id);
    if((err_code != NRF_SUCCESS) && (err_code != NRF_ERROR_INVALID_STATE)) {
        APP_ERROR_CHECK(err_code);
    }
}

static void status_led_timer_handler(void* p_context) {
    UNUSED_PARAMETER(p_context);

    if(m_status_led_blinking) {
        status_led_toggle();
    }
}

static void status_led_set_advertising(void) {
    status_led_timer_stop();
    m_status_led_blinking = true;
    status_led_on();
    ret_code_t err_code = app_timer_start(m_status_led_timer_id, STATUS_LED_BLINK_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);
}

static void status_led_set_connected(void) {
    status_led_timer_stop();
    m_status_led_blinking = false;
    status_led_on();
}

static void status_led_set_idle(void) {
    status_led_timer_stop();
    m_status_led_blinking = false;
    status_led_off();
}

static ret_code_t led_service_restore_characteristic(estc_led_char_t characteristic) {
    switch(characteristic) {
        case ESTC_LED_CHAR_STATE:
            return estc_ble_service_set_state(&m_estc_service,
                                              m_led_state.power_on ? 1U : 0U,
                                              false);

        case ESTC_LED_CHAR_HUE:
            return estc_ble_service_set_hue(&m_estc_service, m_led_state.hue, false);

        case ESTC_LED_CHAR_SATURATION:
            return estc_ble_service_set_saturation(&m_estc_service, m_led_state.saturation, false);

        case ESTC_LED_CHAR_VALUE:
            return estc_ble_service_set_value(&m_estc_service, m_led_state.value, false);

        default:
            return NRF_ERROR_INVALID_PARAM;
    }
}

static ret_code_t led_service_publish_characteristic(estc_led_char_t characteristic) {
    switch(characteristic) {
        case ESTC_LED_CHAR_STATE:
            return estc_ble_service_set_state(&m_estc_service,
                                              m_led_state.power_on ? 1U : 0U,
                                              true);

        case ESTC_LED_CHAR_HUE:
            return estc_ble_service_set_hue(&m_estc_service, m_led_state.hue, true);

        case ESTC_LED_CHAR_SATURATION:
            return estc_ble_service_set_saturation(&m_estc_service, m_led_state.saturation, true);

        case ESTC_LED_CHAR_VALUE:
            return estc_ble_service_set_value(&m_estc_service, m_led_state.value, true);

        default:
            return NRF_ERROR_INVALID_PARAM;
    }
}

static ret_code_t led_state_commit(estc_led_char_t characteristic) {
    ret_code_t err_code;

    led_pwm_apply(&m_led_state);

    err_code = led_storage_schedule_save(&m_led_state);
    if(err_code != NRF_SUCCESS) {
        NRF_LOG_WARNING("LED state save was not scheduled: 0x%08x", (unsigned int)err_code);
    }

    RETURN_IF_ERROR(led_service_publish_characteristic(characteristic));
    return NRF_SUCCESS;
}

static void estc_service_write_handler(ble_estc_service_t* p_service,
                                       estc_led_char_t     characteristic,
                                       uint8_t const*      p_data,
                                       uint16_t            len,
                                       void*               p_context) {
    ret_code_t err_code = NRF_SUCCESS;

    switch(characteristic) {
        case ESTC_LED_CHAR_STATE:
            if((len != sizeof(uint8_t)) || (p_data[0] > 1U)) {
                err_code = led_service_restore_characteristic(characteristic);
                APP_ERROR_CHECK(err_code);
                return;
            }

            if(m_led_state.power_on == (p_data[0] != 0U)) {
                return;
            }

            m_led_state.power_on = (p_data[0] != 0U);
            err_code             = led_state_commit(characteristic);
            APP_ERROR_CHECK(err_code);
            return;

        case ESTC_LED_CHAR_HUE:
            if(len != sizeof(uint16_t)) {
                err_code = led_service_restore_characteristic(characteristic);
                APP_ERROR_CHECK(err_code);
                return;
            }

            {
                uint16_t const hue = decode_u16_le(p_data);
                if(!led_state_is_valid_hue(hue)) {
                    err_code = led_service_restore_characteristic(characteristic);
                    APP_ERROR_CHECK(err_code);
                    return;
                }

                if(m_led_state.hue == hue) {
                    return;
                }

                m_led_state.hue = hue;
            }

            err_code = led_state_commit(characteristic);
            APP_ERROR_CHECK(err_code);
            return;

        case ESTC_LED_CHAR_SATURATION:
            if((len != sizeof(uint8_t)) || !led_state_is_valid_saturation(p_data[0])) {
                err_code = led_service_restore_characteristic(characteristic);
                APP_ERROR_CHECK(err_code);
                return;
            }

            if(m_led_state.saturation == p_data[0]) {
                return;
            }

            m_led_state.saturation = p_data[0];
            err_code               = led_state_commit(characteristic);
            APP_ERROR_CHECK(err_code);
            return;

        case ESTC_LED_CHAR_VALUE:
            if((len != sizeof(uint8_t)) || !led_state_is_valid_value(p_data[0])) {
                err_code = led_service_restore_characteristic(characteristic);
                APP_ERROR_CHECK(err_code);
                return;
            }

            if(m_led_state.value == p_data[0]) {
                return;
            }

            m_led_state.value = p_data[0];
            err_code          = led_state_commit(characteristic);
            APP_ERROR_CHECK(err_code);
            return;

        default:
            return;
    }
}

void assert_nrf_callback(uint16_t line_num, uint8_t const* p_file_name) {
    app_error_handler(DEAD_BEEF, line_num, p_file_name);
}

static void timers_init(void) {
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&m_status_led_timer_id,
                                APP_TIMER_MODE_REPEATED,
                                status_led_timer_handler);
    APP_ERROR_CHECK(err_code);
}

static void gap_params_init(void) {
    ret_code_t              err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (uint8_t const*)DEVICE_NAME,
                                          strlen(DEVICE_NAME));
    APP_ERROR_CHECK(err_code);

    err_code = sd_ble_gap_appearance_set(BLE_APPEARANCE_UNKNOWN);
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}

static void gatt_init(void) {
    ret_code_t err_code = nrf_ble_gatt_init(&m_gatt, NULL);
    APP_ERROR_CHECK(err_code);
}

static void nrf_qwr_error_handler(uint32_t nrf_error) {
    APP_ERROR_HANDLER(nrf_error);
}

static void services_init(void) {
    ret_code_t              err_code;
    nrf_ble_qwr_init_t      qwr_init     = {0};
    ble_estc_service_init_t service_init = {0};

    qwr_init.error_handler = nrf_qwr_error_handler;

    err_code = nrf_ble_qwr_init(&m_qwr, &qwr_init);
    APP_ERROR_CHECK(err_code);

    service_init.write_handler      = estc_service_write_handler;
    service_init.initial_state      = m_led_state.power_on ? 1U : 0U;
    service_init.initial_hue        = m_led_state.hue;
    service_init.initial_saturation = m_led_state.saturation;
    service_init.initial_value      = m_led_state.value;

    err_code = estc_ble_service_init(&m_estc_service, &service_init);
    APP_ERROR_CHECK(err_code);

    m_adv_uuids[0].type = m_estc_service.uuid_type;
}

static void on_conn_params_evt(ble_conn_params_evt_t* p_evt) {
    if(p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED) {
        ret_code_t err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
        APP_ERROR_CHECK(err_code);
    }
}

static void conn_params_error_handler(uint32_t nrf_error) {
    APP_ERROR_HANDLER(nrf_error);
}

static void conn_params_init(void) {
    ret_code_t             err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}

static void sleep_mode_enter(void) {
    status_led_set_idle();

    ret_code_t err_code = bsp_btn_ble_sleep_mode_prepare();
    APP_ERROR_CHECK(err_code);

    err_code = sd_power_system_off();
    APP_ERROR_CHECK(err_code);
}

static void on_adv_evt(ble_adv_evt_t ble_adv_evt) {
    switch(ble_adv_evt) {
        case BLE_ADV_EVT_FAST:
            NRF_LOG_INFO("Advertising started");
            status_led_set_advertising();
            break;

        case BLE_ADV_EVT_IDLE:
            NRF_LOG_INFO("Advertising stopped, entering sleep");
            sleep_mode_enter();
            break;

        default:
            break;
    }
}

static void ble_evt_handler(ble_evt_t const* p_ble_evt, void* p_context) {
    UNUSED_PARAMETER(p_context);

    ret_code_t err_code = NRF_SUCCESS;

    switch(p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected (conn_handle: %d)", p_ble_evt->evt.gap_evt.conn_handle);
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            status_led_set_idle();
            break;

        case BLE_GAP_EVT_CONNECTED:
            NRF_LOG_INFO("Connected (conn_handle: %d)", p_ble_evt->evt.gap_evt.conn_handle);
            status_led_set_connected();
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            err_code      = nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
            {
                ble_gap_phys_t const phys =
                    {
                        .rx_phys = BLE_GAP_PHY_AUTO,
                        .tx_phys = BLE_GAP_PHY_AUTO,
                    };
                err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        default:
            break;
    }

    estc_ble_service_on_ble_event(p_ble_evt, &m_estc_service);
}

static void ble_stack_init(void) {
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    uint32_t ram_start = 0;
    err_code           = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);

    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}

static void bsp_event_handler(bsp_event_t event) {
    switch(event) {
        case BSP_EVENT_SLEEP:
            sleep_mode_enter();
            break;

        case BSP_EVENT_DISCONNECT:
            {
                ret_code_t err_code = sd_ble_gap_disconnect(m_conn_handle,
                                                            BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
                if(err_code != NRF_ERROR_INVALID_STATE) {
                    APP_ERROR_CHECK(err_code);
                }
            }
            break;

        default:
            break;
    }
}

static void advertising_init(void) {
    ret_code_t             err_code;
    ble_advertising_init_t init;

    memset(&init, 0, sizeof(init));

    init.advdata.name_type               = BLE_ADVDATA_FULL_NAME;
    init.advdata.flags                   = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    init.advdata.uuids_complete.uuid_cnt = 1U;
    init.advdata.uuids_complete.p_uuids  = m_adv_uuids;

    init.config.ble_adv_fast_enabled  = true;
    init.config.ble_adv_fast_interval = APP_ADV_INTERVAL;
    init.config.ble_adv_fast_timeout  = APP_ADV_DURATION;
    init.evt_handler                  = on_adv_evt;

    err_code = ble_advertising_init(&m_advertising, &init);
    APP_ERROR_CHECK(err_code);

    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);
}

static void buttons_init(void) {
    ret_code_t err_code = bsp_init(BSP_INIT_BUTTONS, bsp_event_handler);
    APP_ERROR_CHECK(err_code);

    err_code = bsp_btn_ble_init(NULL, NULL);
    APP_ERROR_CHECK(err_code);
}

static void status_led_init(void) {
    nrf_gpio_cfg_output(LED1_G);
    status_led_set_idle();
}

static void log_init(void) {
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);

    NRF_LOG_DEFAULT_BACKENDS_INIT();
}

static void power_management_init(void) {
    ret_code_t err_code = nrf_pwr_mgmt_init();
    APP_ERROR_CHECK(err_code);
}

static void idle_state_handle(void) {
    if(!NRF_LOG_PROCESS()) {
        nrf_pwr_mgmt_run();
    }

    LOG_BACKEND_USB_PROCESS();
}

static void advertising_start(void) {
    ret_code_t err_code = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
    APP_ERROR_CHECK(err_code);
}

int main(void) {
    log_init();
    timers_init();
    buttons_init();
    status_led_init();
    power_management_init();
    ble_stack_init();

    led_state_init_defaults(&m_led_state);

    ret_code_t err_code = led_pwm_init();
    APP_ERROR_CHECK(err_code);

    err_code = led_storage_init(&m_led_state);
    APP_ERROR_CHECK(err_code);

    led_pwm_apply(&m_led_state);

    gap_params_init();
    gatt_init();
    services_init();
    advertising_init();
    conn_params_init();

    NRF_LOG_INFO("ESTC LED service started");
    advertising_start();

    for(;;) {
        idle_state_handle();
    }
}

/**
 * @}
 */
