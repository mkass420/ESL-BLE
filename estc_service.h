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
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE
 */

#ifndef ESTC_SERVICE_H__
#define ESTC_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>

#include "app_util.h"
#include "ble.h"
#include "sdk_errors.h"

#define ESTC_BASE_UUID           {0x4E, 0x22, 0xFC, 0xA8, 0x85, 0x18, 0x0C, 0x8D, 0xF8, 0x40, 0x8B, 0xF0, 0x00, 0x00, 0xC2, 0x3E}
#define ESTC_SERVICE_UUID        0x6969
#define ESTC_LED_STATE_UUID      0x6970
#define ESTC_LED_HUE_UUID        0x6971
#define ESTC_LED_SATURATION_UUID 0x6972
#define ESTC_LED_VALUE_UUID      0x6973

typedef enum {
    ESTC_LED_CHAR_STATE = 0,
    ESTC_LED_CHAR_HUE,
    ESTC_LED_CHAR_SATURATION,
    ESTC_LED_CHAR_VALUE,
    ESTC_LED_CHAR_COUNT
} estc_led_char_t;

typedef struct ble_estc_service_s ble_estc_service_t;

typedef void (*estc_service_write_handler_t)(ble_estc_service_t* p_service,
                                             estc_led_char_t     characteristic,
                                             uint8_t const*      p_data,
                                             uint16_t            len,
                                             void*               p_context);

typedef struct {
    estc_service_write_handler_t write_handler;
    void*                        p_context;
    uint8_t                      initial_state;
    uint16_t                     initial_hue;
    uint8_t                      initial_saturation;
    uint8_t                      initial_value;
} ble_estc_service_init_t;

struct ble_estc_service_s {
    uint16_t                     service_handle;
    uint16_t                     connection_handle;
    ble_gatts_char_handles_t     characteristic_handles[ESTC_LED_CHAR_COUNT];
    uint8_t                      uuid_type;
    bool                         notification_enabled[ESTC_LED_CHAR_COUNT];
    estc_service_write_handler_t write_handler;
    void*                        p_write_context;
};

ret_code_t estc_ble_service_init(ble_estc_service_t*            p_service,
                                 ble_estc_service_init_t const* p_init);

void estc_ble_service_on_ble_event(ble_evt_t const* p_ble_evt, void* p_context);

ret_code_t estc_ble_service_set_state(ble_estc_service_t* p_service,
                                      uint8_t             state,
                                      bool                notify);

ret_code_t estc_ble_service_set_hue(ble_estc_service_t* p_service,
                                    uint16_t            hue,
                                    bool                notify);

ret_code_t estc_ble_service_set_saturation(ble_estc_service_t* p_service,
                                           uint8_t             saturation,
                                           bool                notify);

ret_code_t estc_ble_service_set_value(ble_estc_service_t* p_service,
                                      uint8_t             value,
                                      bool                notify);

#endif /* ESTC_SERVICE_H__ */
