/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 lzghzr. All Rights Reserved.
 */

#include <compiler.h>
#include <hook.h>
#include <kpmodule.h>
#include <kputils.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/math64.h>

#include "qbc_utils.h"
#include "battchg.h"

KPM_NAME("qti_battery_charger");
KPM_VERSION(QBC_VERSION);
KPM_LICENSE("GPL v2");
KPM_AUTHOR("lzghzr");
KPM_DESCRIPTION("set battery_psy_get_prop value based on voltage");

int (*do_init_module)(struct module* mod) = 0;
int (*battery_psy_get_prop)(struct power_supply* psy, enum power_supply_property prop, union power_supply_propval* pval) = 0;
static int (*orig_battery_psy_get_prop)(struct power_supply* psy, enum power_supply_property prop, union power_supply_propval* pval) = 0;

char MODULE_NAME[] = "qti_battery_charger";
char MODEL_NAME[] = "SNYSCA6";

// 电池电压-容量映射配置 (3.10V=0%, 4.48V=100%)
static struct battery_voltage_map {
    int voltage_uv;   // 电压（微伏）
    int capacity_pct; // 对应容量百分比
} voltage_capacity_map[] = {
    {3100000,  0},  // 3.10V -> 0%
    {3300000,  10}, // 3.30V -> 10%
    {3500000,  25}, // 3.50V -> 25%
    {3700000,  40}, // 3.70V -> 40%
    {3850000,  55}, // 3.85V -> 55%
    {4000000,  70}, // 4.00V -> 70%
    {4150000,  85}, // 4.15V -> 85%
    {4300000,  95}, // 4.30V -> 95%
    {4480000, 100}, // 4.48V -> 100%
};
#define VOLTAGE_MAP_SIZE (sizeof(voltage_capacity_map) / sizeof(voltage_capacity_map[0]))

// 根据当前电压计算平滑容量百分比
static int calculate_smooth_capacity(int voltage_now_uv) 
{
    if (voltage_now_uv <= voltage_capacity_map[0].voltage_uv) {
        return voltage_capacity_map[0].capacity_pct;
    }
    if (voltage_now_uv >= voltage_capacity_map[VOLTAGE_MAP_SIZE-1].voltage_uv) {
        return voltage_capacity_map[VOLTAGE_MAP_SIZE-1].capacity_pct;
    }

    for (int i = 0; i < VOLTAGE_MAP_SIZE - 1; i++) {
        if (voltage_now_uv >= voltage_capacity_map[i].voltage_uv && 
            voltage_now_uv < voltage_capacity_map[i+1].voltage_uv) {
            
            int voltage_range = voltage_capacity_map[i+1].voltage_uv - voltage_capacity_map[i].voltage_uv;
            int capacity_range = voltage_capacity_map[i+1].capacity_pct - voltage_capacity_map[i].capacity_pct;
            int position = voltage_now_uv - voltage_capacity_map[i].voltage_uv;
            
            if (voltage_range <= 0) {
                return voltage_capacity_map[i].capacity_pct;
            }
            
            return voltage_capacity_map[i].capacity_pct + 
                   div_s64((s64)capacity_range * position, voltage_range);
        }
    }
    
    return 50;
}

void battery_psy_get_prop_after(hook_fargs3_t* args, void* udata) {
    enum power_supply_property prop = args->arg1;
    union power_supply_propval* pval = (typeof(pval))args->arg2;

    switch (prop) {
    case POWER_SUPPLY_PROP_CAPACITY: {
        union power_supply_propval volt_val;
        int ret = orig_battery_psy_get_prop(
            (struct power_supply*)args->arg0, 
            POWER_SUPPLY_PROP_VOLTAGE_NOW, 
            &volt_val);
        
        if (ret == 0 && volt_val.intval > 0) {
            pval->intval = calculate_smooth_capacity(volt_val.intval);
        } else {
            if (pval->intval < 10) pval->intval = 10;
        }
        break;
    }
    case POWER_SUPPLY_PROP_MODEL_NAME:
        memcpy((char*)pval->strval, MODEL_NAME, sizeof(MODEL_NAME));
        break;
    default:
        break;
    }
}

static long hook_battery_psy_get_prop() {
    battery_psy_get_prop = (typeof(battery_psy_get_prop))kallsyms_lookup_name("battery_psy_get_prop");
    pr_info("kernel function battery_psy_get_prop addr: %llx\n", battery_psy_get_prop);
    
    if (!battery_psy_get_prop) {
        return -1;
    }

    orig_battery_psy_get_prop = battery_psy_get_prop;
    
    hook_func(battery_psy_get_prop, 3, NULL, battery_psy_get_prop_after, NULL);
    return 0;
}

void do_init_module_after(hook_fargs1_t* args, void* udata) {
    struct module* mod = (typeof(mod))args->arg0;
    if (unlikely(!memcmp(mod->name, MODULE_NAME, sizeof(MODULE_NAME)))) {
        unhook_func(do_init_module);
        hook_battery_psy_get_prop();
    }
}

static long hook_do_init_module() {
    do_init_module = 0;
    do_init_module = (typeof(do_init_module))kallsyms_lookup_name("do_init_module");
    pr_info("kernel function do_init_module addr: %llx\n", do_init_module);
    if (!do_init_module) {
        return -1;
    }

    hook_err_t err = hook_wrap1(do_init_module, 0, do_init_module_after, 0);
    if (err) {
        pr_err("hook do_init_module after error: %d\n", err);
        return -2;
    } else {
        pr_info("hook do_init_module after success\n");
    }
    return 0;
}

static long inline_hook_init(const char* args, const char* event, void* __user reserved) {
    int rc;
    rc = hook_battery_psy_get_prop();
    if (rc < 0) {
        rc = hook_do_init_module();
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

static long inline_hook_exit(void* __user reserved) {
    unhook_func(do_init_module);
    unhook_func(battery_psy_get_prop);
    return 0;
}

KPM_INIT(inline_hook_init);
KPM_EXIT(inline_hook_exit);
