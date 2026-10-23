/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The table below was recovered from the .data section of the stock
 * vendor-oplus-hardware-touch-V2-service (SHA-256
 * 219d1b5aed650dfb7f1fd99c3869f26aad1aa3ef4920a500ef8cca975cd82e3f).
 * It has 96 entries of 0x5c bytes each.
 */

#include "TouchNodeMap.h"

#include <cstdio>

namespace aidl::vendor::oplus::hardware::touch {
namespace {

constexpr TouchNode kTouchNodes[] = {
    {1, 1, "/proc/touchpanel", "double_tap_enable", 0x100},
    {2, 1, "/proc/touchpanel", "coordinate", 0x100},
    {3, 1, "/proc/touchpanel", "baseline_test", 0x1000},
    {4, 1, "/proc/touchpanel", "calibration", 0x1000},
    {5, 1, "/proc/touchpanel", "oplus_tp_direction", 0x100},
    {6, 1, "/proc/touchpanel", "oplus_tp_limit_whitelist", 0x100},
    {7, 1, "/proc/touchpanel", "oplus_tp_limit_enable", 0x100},
    {8, 1, "/proc/touchpanel", "kernel_grip_handle", 0x2000},
    {9, 1, "/proc/touchpanel", "report_rate_white_list", 0x100},
    {10, 1, "/proc/touchpanel", "charge_detect", 0x100},
    {11, 1, "/proc/touchpanel", "wireless_charge_detect", 0x100},
    {12, 1, "/proc/touchpanel", "headset_detect", 0x100},
    {13, 1, "/proc/touchpanel", "black_screen_test", 0x2000},
    {14, 1, "/proc/touchpanel", "baseline_result", 0x19000},
    {15, 1, "/proc/touchpanel", "black_screen_result", 0x19000},
    {16, 1, "/proc/touchpanel", "tp_aging_test", 0x2000},
    {17, 1, "/proc/touchpanel", "debug_info/delta", 0x5000},
    {18, 1, "/proc/touchpanel", "debug_info/baseline", 0x19000},
    {19, 1, "/proc/touchpanel", "earsense/hover_selfdata", 0x2000},
    {20, 1, "/proc/touchpanel", "debug_info/health_monitor", 0x19000},
    {21, 1, "/proc/touchpanel", "double_tap_enable_indep", 0x100},
    {22, 1, "/proc/touchpanel", "oplus_optimized_time", 0x100},
    {23, 1, "/proc/touchpanel", "kernel_grip_default_para", 0x19000},
    {187, 1, "/proc/touchpanel", "touch_scene_default_para", 0x19000},
    {188, 1, "/proc/touchpanel", "touch_scene_para", 0x19000},
    {24, 1, "/proc/touchpanel", "smooth_level", 0x100},
    {25, 1, "/proc/touchpanel", "sensitive_level", 0x100},
    {661, 1, "/proc/touchpanel", "force_water_mode", 0x100},
    {179, 1, "/proc/touchpanel", "diaphragm_touch", 0x100},
    {26, 1, "/proc/touchpanel", "game_switch_enable", 0x100},
    {27, 1, "/proc/touchpanel", "ps_status", 0x100},
    {28, 1, "/proc/touchpanel", "leather_cover_enable", 0x100},
    {192, 1, "/proc/touchpanel", "glove_mode_enable", 0x100},
    {29, 1, "/proc/touchpanel", "report_point_first", 0x100},
    {30, 1, "/proc/touchpanel", "fd_touch_count", 0x100},
    {31, 1, "/proc/touchpanel", "fp_enable", 0x100},
    {34, 1, "/proc/touchpanel", "tpedge_limit_enable", 0x100},
    {35, 1, "/proc/touchpanel", "tp_switch_dead_zone", 0x100},
    {36, 1, "/proc/touchpanel", "tp_switch_corner_dead_l_zone", 0x100},
    {37, 1, "/proc/touchpanel", "tp_switch_corner_dead_p_zone", 0x100},
    {32, 1, "/proc/touchpanel", "pencil_connected", 0x100},
    {38, 1, "/proc/touchpanel", "pencil_control", 0x100},
    {170, 1, "/proc/touchpanel", "pencil_opp", 0x100},
    {39, 1, "/proc/touchpanel", "gesture_support", 0x100},
    {167, 1, "/proc/touchpanel", "palm_to_sleep", 0x100},
    {171, 1, "/proc/touchpanel", "reclining_mode", 0x100},
    {172, 1, "/proc/touchpanel", "aiunit_game_info", 0x19000},
    {173, 1, "/proc/touchpanel", "game_status", 0x100},
    {180, 1, "/proc/touchpanel", "waterproof", 0x100},
    {181, 1, "/proc/touchpanel", "tp_fw_update", 0x100},
    {186, 1, "/proc/touchpanel", "rus_tp_fw_update", 0x100},
    {182, 1, "/proc/touchpanel", "report_rate", 0x100},
    {183, 1, "/proc/touchpanel", "game_mode", 0x100},
    {184, 1, "/proc/touchpanel", "high_frame_enable", 0x100},
    {185, 1, "/proc/touchpanel", "fingerprint_active", 0x100},
    {189, 1, "/proc/touchpanel", "communicate_test", 0x100},
    {193, 1, "/proc/touchpanel", "disable_touch_event", 0x100},
    {194, 1, "/proc/touchpanel", "pocket_prevent_mode", 0x100},
    {195, 1, "/proc/touchpanel", "under_water", 0x100},
    {196, 1, "/proc/touchpanel", "fp_enroll", 0x100},
    {198, 1, "/proc/touchpanel", "fp_unlock_status", 0x100},
    {199, 1, "/proc/touchpanel", "fingerprint_prevent", 0x100},
    {160, 1, "/proc/touchpanel", "debug_level", 0x100},
    {161, 1, "/proc/touchpanel", "debug_info/main_register", 0x1000},
    {162, 1, "/proc/touchpanel", "debug_info/self_delta", 0x5000},
    {163, 1, "/proc/touchpanel", "debug_info/self_raw", 0x1000},
    {164, 1, "/proc/touchpanel", "debug_info/data_record", 0x19000},
    {190, 1, "/proc/touchpanel", "debug_info/data_limit", 0x19000},
    {165, 1, "/proc/touchpanel", "debug_info/reserve", 0x1000},
    {240, 1, "/proc/touchpanel", "debug_info/privacy_enable", 0x1000},
    {40, 1, "/proc/colorctrl", "color_ctrl", 0x100},
    {41, 1, "/proc/colorctrl", "temperature", 0x100},
    {42, 1, "/proc/colorctrl", "voltage", 0x100},
    {43, 1, "/proc/colorctrl", "cover_color", 0x100},
    {44, 1, "/proc/colorctrl", "ave_aging_vol", 0x100},
    {80, 2, "/proc/devinfo", "audio_mainboard", 0x100},
    {81, 2, "/proc/devinfo", "distribution", 0x100},
    {83, 2, "/proc/devinfo", "lcd", 0x100},
    {84, 2, "/proc/devinfo", "mainboard", 0x100},
    {85, 2, "/proc/devinfo", "tp", 0x100},
    {120, 1, "/proc/tristatekey", "hall_data", 0x100},
    {121, 1, "/proc/tristatekey", "hall_data_calib", 0x100},
    {122, 1, "/proc/tristatekey", "tri_state", 0x100},
    {123, 1, "/proc/tristatekey", "dump_register", 0x1000},
    {166, 1, "/proc/touchpanel", "pencil_healthinfo", 0x80},
    {200, 1, "/proc/pogopin", "kpdmcu_fw_check", 0x100},
    {201, 1, "/proc/pogopin", "kpdmcu_fw_update", 0x100},
    {202, 1, "/proc/pogopin", "kbd_touch_status", 0x100},
    {203, 1, "/proc/pogopin", "kbd_touch_disable", 0x100},
    {204, 1, "/proc/pogopin", "kbd_keypad_status", 0x100},
    {205, 1, "/proc/pogopin", "kbd_touch_gesture", 0x100},
    {178, 1, "/dev", "tcm_hbp0", 0x100},
    {197, 1, "/proc/touchpanel", "fp_grip_enable", 0x100},
    {679, 1, "/proc/touchpanel", "debug_info/ic_reset", 0x100},
    {241, 1, "/proc/touchpanel", "low_battery_cap", 0x100},
    {244, 1, "/proc/touchpanel", "incell_panel", 0x100},
};

const TouchNode* findTouchNode(int32_t nodeFlag) {
    for (const auto& node : kTouchNodes) {
        if (node.flag == nodeFlag) {
            return &node;
        }
    }
    return nullptr;
}

}  // namespace

std::optional<std::string> getTouchNodePath(int32_t deviceId, int32_t nodeFlag,
                                            size_t* readSize) {
    const TouchNode* node = findTouchNode(nodeFlag);
    if (node == nullptr) {
        return std::nullopt;
    }

    std::string path;
    if (deviceId == 0) {
        path = std::string(node->directory) + "/" + node->name;
    } else if (node->devicePosition == 1) {
        // Stock: "%s%d%s%s" with directory, deviceId, "/", name.
        path = std::string(node->directory) + std::to_string(deviceId) + "/" + node->name;
    } else if (node->devicePosition == 2) {
        // Stock: "%s%s%s%d" with directory, "/", name, deviceId.
        path = std::string(node->directory) + "/" + node->name + std::to_string(deviceId);
    } else {
        return std::nullopt;
    }

    if (readSize != nullptr) {
        *readSize = node->readSize;
    }
    return path;
}

std::string listTouchNodes(int32_t deviceId) {
    std::string result;
    char line[256];

    for (const auto& node : kTouchNodes) {
        size_t readSize = 0;
        auto path = getTouchNodePath(deviceId, node.flag, &readSize);
        if (!path.has_value()) {
            continue;
        }

        snprintf(line, sizeof(line), "HBP node:[%d]  path:%s\n", node.flag, path->c_str());
        result += line;
    }

    return result;
}

}  // namespace aidl::vendor::oplus::hardware::touch
