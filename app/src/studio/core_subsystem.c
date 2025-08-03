/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>

ZMK_RPC_SUBSYSTEM(core)

#define CORE_RESPONSE(type, ...) ZMK_RPC_RESPONSE(core, type, __VA_ARGS__)

static bool encode_device_info_name(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, CONFIG_ZMK_KEYBOARD_NAME, strlen(CONFIG_ZMK_KEYBOARD_NAME));
}

#if IS_ENABLED(CONFIG_HWINFO)
static bool encode_device_info_serial_number(pb_ostream_t *stream, const pb_field_t *field,
                                             void *const *arg) {
    uint8_t id_buffer[32];
    const ssize_t id_size = hwinfo_get_device_id(id_buffer, ARRAY_SIZE(id_buffer));

    if (id_size <= 0) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, id_buffer, id_size);
}

#endif // IS_ENABLED(CONFIG_HWINFO)

zmk_studio_Response get_device_info(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_GetDeviceInfoResponse resp = zmk_core_GetDeviceInfoResponse_init_zero;

    resp.name.funcs.encode = encode_device_info_name;
#if IS_ENABLED(CONFIG_HWINFO)
    resp.serial_number.funcs.encode = encode_device_info_serial_number;
#endif // IS_ENABLED(CONFIG_HWINFO)

    return CORE_RESPONSE(get_device_info, resp);
}

zmk_studio_Response get_lock_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_LockState resp = zmk_studio_core_get_lock_state();

    return CORE_RESPONSE(get_lock_state, resp);
}

zmk_studio_Response reset_settings(const zmk_studio_Request *req) {
    LOG_DBG("");
    ZMK_RPC_SUBSYSTEM_SETTINGS_RESET_FOREACH(sub) {
        int ret = sub->callback();
        if (ret < 0) {
            LOG_ERR("Failed to reset settings: %d", ret);
            return CORE_RESPONSE(reset_settings, false);
        }
    }

    return CORE_RESPONSE(reset_settings, true);
}

// ============================================
// DISCOVERY MODE SUPPORT FOR HYBRID MATRIX
// ============================================
#if IS_ENABLED(CONFIG_ZMK_GK_HYBRID_MATRIX)

#include <zmk_driver_gk_hall_effect/drivers/kscan/gk_hybrid_matrix.h>

/**
 * @brief Start discovery mode for hybrid matrix calibration
 *
 * Enables real-time sampling of both mechanical and hall sensor values
 * for all switch positions. Sample rate is configured in firmware DTS.
 */
zmk_studio_Response start_discovery(const zmk_studio_Request *req) {
    LOG_DBG("Starting discovery mode for hybrid matrix");

    // Start discovery mode using DTS-configured sample rate
    int ret = gk_hm_start_discovery();

    zmk_core_StartDiscoveryResponse resp = zmk_core_StartDiscoveryResponse_init_zero;
    resp.success = (ret == 0);

    if (ret < 0) {
        LOG_ERR("Failed to start discovery mode: %d", ret);
        resp.error_message = "Failed to initialize discovery mode";
    } else {
        LOG_INF("Discovery mode started successfully");
    }

    return CORE_RESPONSE(start_discovery, resp);
}

/**
 * @brief Stop discovery mode
 *
 * Disables real-time sampling and returns to normal keyboard operation.
 */
zmk_studio_Response stop_discovery(const zmk_studio_Request *req) {
    LOG_DBG("Stopping discovery mode for hybrid matrix");

    int ret = gk_hm_stop_discovery();

    zmk_core_StopDiscoveryResponse resp = zmk_core_StopDiscoveryResponse_init_zero;
    resp.success = (ret == 0);

    if (ret < 0) {
        LOG_ERR("Failed to stop discovery mode: %d", ret);
        resp.error_message = "Failed to stop discovery mode";
    } else {
        LOG_INF("Discovery mode stopped successfully");
    }

    return CORE_RESPONSE(stop_discovery, resp);
}

/**
 * @brief Set configuration for a single switch
 *
 * Updates the switch type (mechanical vs hall-effect) and threshold
 * for a specific matrix position. Configuration is automatically saved
 * to persistent storage.
 */
zmk_studio_Response set_switch_config(const zmk_studio_Request *req) {
    LOG_DBG("Setting switch configuration");

    const zmk_core_SetSwitchConfigRequest *config_req =
        &req->subsystem.core.request_type.set_switch_config;

    zmk_core_SetSwitchConfigResponse resp = zmk_core_SetSwitchConfigResponse_init_zero;

    // Call existing settings function that handles both setting and saving
    int ret = gk_hm_set_switch_config(config_req->row, config_req->col, config_req->is_hall_effect,
                                      config_req->threshold);

    if (ret < 0) {
        LOG_ERR("Failed to set switch config for (%d,%d): %d", config_req->row, config_req->col,
                ret);

        resp.success = false;
        resp.error_message = "Failed to update switch configuration";
        return CORE_RESPONSE(set_switch_config, resp);
    }

    resp.success = true;

    LOG_DBG("Updated switch (%d,%d): hall_effect=%s, threshold=%d", config_req->row,
            config_req->col, config_req->is_hall_effect ? "true" : "false", config_req->threshold);

    return CORE_RESPONSE(set_switch_config, resp);
}

// Register the new discovery handlers with the existing core subsystem
ZMK_RPC_SUBSYSTEM_HANDLER(core, start_discovery, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, stop_discovery, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, set_switch_config, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif // CONFIG_ZMK_GK_HYBRID_MATRIX

ZMK_RPC_SUBSYSTEM_HANDLER(core, get_device_info, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_lock_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, reset_settings, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int core_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    struct zmk_studio_core_lock_state_changed *lock_ev = as_zmk_studio_core_lock_state_changed(eh);

    if (!lock_ev) {
        return -ENOTSUP;
    }

    LOG_DBG("Mapped a lock state event properly");

    *n = ZMK_RPC_NOTIFICATION(core, lock_state_changed, lock_ev->state);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(core, core_event_mapper, zmk_studio_core_lock_state_changed);
