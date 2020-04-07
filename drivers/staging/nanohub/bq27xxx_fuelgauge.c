/*
 * Copyright (C) 2017 Mobvoi, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/platform_data/nanohub.h>
#include <linux/delay.h>

#include <linux/power_supply.h>

#include <linux/power/bq27x00_battery.h>

#include "main.h"
#include "comms.h"
#include "bq27xxx_fuelgauge.h"
#include "custom_app_event.h"
#include <linux/reboot.h>


int device_is_charging = 0;
#define DBG_ENABLE 1
#define WAKEUP_TIMEOUT_MS       2000

#define BQ27XXX_FLAG_DSC        BIT(0)
#define BQ27XXX_FLAG_SOCF       BIT(1) /*State-of-Charge threshold final*/
#define BQ27XXX_FLAG_SOC1       BIT(2) /*State-of-Charge threshold 1*/
#define BQ27XXX_FLAG_FC         BIT(9)
#define BQ27XXX_FLAG_OTD        BIT(14)
#define BQ27XXX_FLAG_OTC        BIT(15)

/* BQ27000 has different layout for Flags register */
#define BQ27200_FLAG_EDVF       BIT(0) /*Final End-of-Discharge-Voltage flag*/
#define BQ27200_FLAG_EDV1       BIT(1) /*First End-of-Discharge-Voltage flag*/
#define BQ27200_FLAG_CI         BIT(4) /*Capacity Inaccurate flag*/
#define BQ27200_FLAG_FC         BIT(5)
#define BQ27200_FLAG_CHGS       BIT(7) /*Charge state flag*/

#define SPM_TIMEOUT             (10*60) /* 10minutes */
#define REQUEST_TIMEOUT		20

struct FuelGaugeCfgData {
	uint32_t interval;
	uint8_t on;
} __packed;


static enum power_supply_property bq274xx_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_HEALTH,
};

struct Nanohub_FuelGauge_Info *m_fg_info;


static unsigned int poll_interval = 60;
module_param(poll_interval, uint, 0644);
MODULE_PARM_DESC(poll_interval,
	"battery poll interval in seconds - 0 disables polling");

static int charger_online;

static int request_fuel_gauge_data(struct nanohub_data *data);
static int force_request_fuel_gauge_data(struct nanohub_data *data);
static void request_delayed_func(struct work_struct *work);

void bq27x00_update(struct Nanohub_FuelGauge_Info *fg_info)
{
	struct timeval cur = {0, };
	static struct timeval last = {0, };
	static bool last_charging_status;
#if FUEL_GAUGE_USE_FAKE_CAPACITY
	static uint32_t timer_counter;
#endif

	do_gettimeofday(&cur);

	if (cur.tv_sec == last.tv_sec &&
		last_charging_status == fg_info->charger_online) {
		fg_info->last_update = jiffies;
		return;
	}

#if FUEL_GAUGE_USE_FAKE_CAPACITY
	pr_warn("nanohub: [FG] cache.capacity:%d, fake_capatity:%d, "
			"cache.temperature:%d, cache.flags:%08x, "
		    "charger_online:%d, timer_counter:%d\n",
			fg_info->cache.capacity, fg_info->fake_capacity,
			fg_info->cache.temperature, fg_info->cache.flags,
			fg_info->charger_online, timer_counter);

	/* Sync fake capacity to real capacity */
	pr_info("nanohub: [FG] cur.tv_sec:%ld last.tv_sec:%ld\n",
			cur.tv_sec, last.tv_sec);
	if ((cur.tv_sec - last.tv_sec > SPM_TIMEOUT) && (last.tv_sec != 0)) {
		fg_info->fake_capacity = fg_info->cache.capacity;
		timer_counter = 0;
	} else {
		if (fg_info->cache.capacity < fg_info->fake_capacity) {
			fg_info->cache.capacity = fg_info->fake_capacity;
			if (!fg_info->charger_online && timer_counter == 2) {
				fg_info->fake_capacity--;
				timer_counter = 0;
			} else {
				if (fg_info->charger_online)
					timer_counter = 0;
				else
					timer_counter++;
			}
		} else {
			timer_counter = 0;
			fg_info->fake_capacity = fg_info->cache.capacity;
		}
	}
#endif
	if (!(strnstr(saved_command_line, "androidboot.mode=keep_charging",
		strlen(saved_command_line)))) {
		if ((fg_info->cache.voltage > 4000)
			&& (strnstr(saved_command_line,
						"androidboot.mode=charger",
						strlen(saved_command_line)))) {
				pr_err("fg_info->cache.voltage %d more than 4V reboot the system\n",
				fg_info->cache.voltage);
				machine_restart(NULL);
		}
	}
	if (fg_info->last_capacity != fg_info->cache.capacity) {
		if ((charger_online &&
		  fg_info->last_capacity < fg_info->cache.capacity) ||
		  (!charger_online &&
		  fg_info->last_capacity > fg_info->cache.capacity)) {
			power_supply_changed(&fg_info->bat);
			fg_info->last_capacity = fg_info->cache.capacity;
#ifdef CONFIG_HAS_WAKELOCK
			wake_lock_timeout(&fg_info->wakelock_report,
					msecs_to_jiffies(20));
#else
			__pm_wakeup_event(&fg_info->wakelock_report, 20); /* 20 msecs */
#endif
			do_gettimeofday(&cur);
			fg_info->ts_wakelock_report = timeval_to_ns(&cur);
			fg_info->wakelock_active_time = 20 * 1000000l;
		} else if (!charger_online && (fg_info->last_capacity > 0) &&
			   (fg_info->last_capacity < fg_info->cache.capacity)) {
			pr_err("FG: set last_c %d\n", fg_info->last_capacity);
			fg_info->cache.capacity = fg_info->last_capacity;
		}
	}
	last.tv_sec = cur.tv_sec;
	last_charging_status = fg_info->charger_online;
	fg_info->last_update = jiffies;

}

static void fuelgauge_battery_poll(struct work_struct *work)
{
	struct Nanohub_FuelGauge_Info *fg_info =
		container_of(work, struct Nanohub_FuelGauge_Info, work.work);

	if (!fg_info->requested) {
		pr_info("nanohub: [FG] request data from sensorhub.\n");
		fg_info->requested = 1;
		if (0 != request_fuel_gauge_data(fg_info->hub_data))
			fg_info->requested = 0;

		set_timer_slack(&fg_info->request_delayed_work.timer, 2 * HZ);
		schedule_delayed_work(&fg_info->request_delayed_work,
			REQUEST_TIMEOUT * HZ);
	}
}

static void request_delayed_func(struct work_struct *work)
{
	struct Nanohub_FuelGauge_Info *fg_info =
		container_of(work, struct Nanohub_FuelGauge_Info,
		request_delayed_work.work);

	pr_info("nanohub: %s  fg_info->requested:%d\n", __func__,
		fg_info->requested);

	if (fg_info->requested) {
		pr_info("nanohub: [FG] no response from sensorhub. Send the force request!\n");
		fg_info->requested = 0;
		if (0 != force_request_fuel_gauge_data(fg_info->hub_data))
			fg_info->requested = 0;
	}
}


int dump_fuelgauge_cache(struct bq27x00_reg_cache *cache_data)
{
#if DBG_ENABLE
	pr_info("nanohub: [FG] control = 0x%04x; "
		"status = %d; "
		"present = %d; "
		"TEMP = %d; "
		"VOL = %d; "
		"flags = 0x%04x; "
		"FAC = %d; "
		"RM = %d; "
		"FCC = %d; "
		"SOC = %d; "
		"AC = %d; "
		"RMU = %d; "
		"FCCU = %d; "
		"SOCU = %d; "
		"CF = %d; "
		"power_avg = %d; "
		"health = %d; "
		"CDF = %d; "
		"charger_online = %d;\n",
		cache_data->control,
		cache_data->status,
		cache_data->present,
		cache_data->temperature,
		cache_data->voltage,
		cache_data->flags,
		cache_data->FullAvailableCapacity,
		cache_data->RemainingCapacity,
		cache_data->FullChargeCapacity,
		cache_data->capacity,
		(int)((s16)cache_data->AverageCurrent),
		cache_data->RemainingCapacityUnfiltered,
		cache_data->FullChargeCapacityUnfiltered,
		cache_data->StateOfChargeUnfiltered,
		cache_data->charge_full,
		(int)((s16)cache_data->power_avg),
		cache_data->health,
		cache_data->charge_design_full,
		charger_online);
#endif
	return 0;
}

int store_fuelguage_cache(struct bq27x00_reg_cache *cache_data)
{
	struct Nanohub_FuelGauge_Info *fg_info = m_fg_info;

	if (!(fg_info && cache_data))
		return -EINVAL;

	memcpy(&(fg_info->cache), cache_data, sizeof(struct bq27x00_reg_cache));

	bq27x00_update(fg_info);
	if (poll_interval > 0) {
		/* The timer does not have to be accurate. */
		set_timer_slack(&fg_info->work.timer, 2 * HZ);
		schedule_delayed_work(&fg_info->work,
			poll_interval * HZ);

		cancel_delayed_work(&fg_info->request_delayed_work);
	}
	fg_info->requested = 0;

	return 0;
}

int is_fuel_gauge_data(struct nanohub_buf *buf, int len)
{

	uint64_t kAppIdMobvoiFuelGaugeBq27421 =
		MakeAppId(kAppIdVendorMobvoi, kAppIdFuelGauge);

	struct HostHubRawPacket *p_HostHubRawPacket;
	struct SensorAppEventHeader *p_SensorAppEventHeader;

	uint32_t event_id;

	if (len != sizeof(uint32_t) +
		sizeof(struct HostHubRawPacket) +
		sizeof(struct SensorAppEventHeader) +
		sizeof(struct bq27x00_reg_cache))
		return -EINVAL;

	p_HostHubRawPacket =
		(struct HostHubRawPacket *)&(buf->buffer[sizeof(uint32_t)]);
	p_SensorAppEventHeader =
		(struct SensorAppEventHeader *)
		&(buf->buffer[sizeof(uint32_t)
		+ sizeof(struct HostHubRawPacket)]);

	event_id =
		le32_to_cpu((((uint32_t *)(buf)->buffer)[0]) & 0x7FFFFFFF);

	if (event_id != APP_TO_HOST_EVENTID)
		return -EINVAL;
	/*pr_err("nanohub: [FG] appId = 0x%llx, dataLen = %d\n",
		p_HostHubRawPacket->appId, p_HostHubRawPacket->dataLen);*/

	if (p_HostHubRawPacket->appId != kAppIdMobvoiFuelGaugeBq27421) {
		/*pr_err("nanohub: [FG] not appId for fuel gauge.\n");*/
		return -EINVAL;
	}
	if (p_SensorAppEventHeader->msgId != SENSOR_APP_MSG_ID_CUSTOM_USE ||
		p_SensorAppEventHeader->sensorType != SENS_TYPE_FUELGAUGE ||
		p_SensorAppEventHeader->status !=
			SENSOR_APP_EVT_STATUS_SUCCESS) {
		pr_err("nanohub: [FG] bad SensorAppEventHeader");
		pr_err("msgId: 0x%x, sensorType: %d, status: %d\n",
			p_SensorAppEventHeader->msgId,
			p_SensorAppEventHeader->sensorType,
			p_SensorAppEventHeader->status);
		return -EINVAL;
	}
	if (p_HostHubRawPacket->dataLen !=
		sizeof(struct SensorAppEventHeader) +
		sizeof(struct bq27x00_reg_cache)) {
		pr_err("nanohub: [FG] bad dataLen for report packet: %d : %d.\n",
			p_HostHubRawPacket->dataLen,
			sizeof(struct SensorAppEventHeader) +
			sizeof(struct bq27x00_reg_cache));
		return -EINVAL;
	}
	return 0;
}

int handle_fuelgauge_data(struct nanohub_buf *buf, int len)
{
	struct bq27x00_reg_cache *p_reg_cache;

	p_reg_cache =
		(struct bq27x00_reg_cache *)
		&(buf->buffer[sizeof(uint32_t)
		+ sizeof(struct HostHubRawPacket)
		+ sizeof(struct SensorAppEventHeader)]);

	dump_fuelgauge_cache(p_reg_cache);
	store_fuelguage_cache(p_reg_cache);
	return 0;
}

static int request_fuel_gauge_data(struct nanohub_data *data)
{
	return __nanohub_send_AP_cmd(data, GPIO_CMD_REQUEST_FUELGAUGE);
}

static int force_request_fuel_gauge_data(struct nanohub_data *data)
{
	return __nanohub_send_AP_cmd(data, GPIO_CMD_FORCE_REQUEST_FUELGAUGE);
}

static int bq27x00_battery_status(
	struct Nanohub_FuelGauge_Info *fg_info,
	union power_supply_propval *val)
{
	int status;

	if (charger_online) {
		if ((fg_info->cache.flags & BQ27XXX_FLAG_FC) ||
		    (fg_info->cache.capacity == 100) )
			status = POWER_SUPPLY_STATUS_FULL;
		else
			status = POWER_SUPPLY_STATUS_CHARGING;
	}
	else
		status = POWER_SUPPLY_STATUS_DISCHARGING;

	val->intval = status;

	return 0;
}

static int bq27x00_battery_capacity_level(
	struct Nanohub_FuelGauge_Info *fg_info,
	union power_supply_propval *val)
{
	int level;

	if (fg_info->cache.flags & BQ27XXX_FLAG_FC)
		level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else if (fg_info->cache.flags & BQ27XXX_FLAG_SOC1)
		level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else if (fg_info->cache.flags & BQ27XXX_FLAG_SOCF)
		level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	else
		level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

	val->intval = level;

	return 0;
}

/*
 * Return the battery Voltage in millivolts
 * Or < 0 if something fails.
 */
static int bq27x00_battery_voltage(
		struct Nanohub_FuelGauge_Info *fg_info,
		union power_supply_propval *val)
{
	val->intval = fg_info->cache.voltage * 1000;

	return 0;
}

/*
 * Return the battery average current in uA
 * Note that current can be negative signed as well
 * Or 0 if something fails.
 */
static int bq27x00_battery_current(
		struct Nanohub_FuelGauge_Info *fg_info,
		union power_supply_propval *val)
{
	int curr = fg_info->cache.AverageCurrent;

	/* Other gauges return signed value */
	val->intval = (int)((s16)curr) * 1000;

	return 0;
}

static int bq27x00_battery_temperature(
		struct Nanohub_FuelGauge_Info *fg_info,
		union power_supply_propval *val)
{
	int curr = fg_info->cache.temperature;

	val->intval = (int)((s16)curr);

	return 0;
}

static int bq27x00_simple_value(int value,
	union power_supply_propval *val)
{
	if (value < 0)
		return value;

	val->intval = value;

	return 0;
}

static int bq27x00_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct timeval cur = {0, };
	struct Nanohub_FuelGauge_Info *fg_info =
		container_of(psy, struct Nanohub_FuelGauge_Info, bat);
	int ret = 0;

	mutex_lock(&fg_info->lock);
	if (time_is_before_jiffies(fg_info->last_update + 5 * HZ)) {
		cancel_delayed_work_sync(&fg_info->work);
		fuelgauge_battery_poll(&fg_info->work.work);
	}
	mutex_unlock(&fg_info->lock);

	if (psp != POWER_SUPPLY_PROP_PRESENT && fg_info->cache.flags < 0) {
		ret = -ENODEV;
		goto GET_PROPERTY_OUT;
	}

	if (fg_info->requested)
		msleep(150);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = bq27x00_battery_status(fg_info, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = bq27x00_battery_voltage(fg_info, val);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = fg_info->cache.flags < 0 ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = bq27x00_battery_current(fg_info, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (fg_info->last_capacity == 0)
			ret = bq27x00_simple_value(fg_info->cache.capacity,
				val);
		else
			ret = bq27x00_simple_value(fg_info->last_capacity,
				val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		ret = bq27x00_battery_capacity_level(fg_info, val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = bq27x00_battery_temperature(fg_info, val);
		break;
/*	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = bq27x00_simple_value(fg_info->cache.time_to_empty, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		ret = bq27x00_simple_value(fg_info->cache.time_to_empty_avg,
				val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		ret = bq27x00_simple_value(fg_info->cache.time_to_full, val);
		break;
*/
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = bq27x00_simple_value(
				(fg_info->cache.NominalAvailableCapacity)*1000,
				val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = bq27x00_simple_value(fg_info->cache.charge_full, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = bq27x00_simple_value(fg_info->cache.charge_design_full,
				val);
		break;
/*	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = bq27x00_simple_value(fg_info->cache.cycle_count, val);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		ret = bq27x00_simple_value(fg_info->cache.energy, val);
		break;
*/
	case POWER_SUPPLY_PROP_POWER_AVG:
		ret = bq27x00_simple_value(fg_info->cache.power_avg, val);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = bq27x00_simple_value(fg_info->cache.health, val);
		break;
	default:
		ret = -EINVAL;
		goto GET_PROPERTY_OUT;
	}

GET_PROPERTY_OUT:
#ifdef CONFIG_HAS_WAKELOCK
	if (wake_lock_active(&fg_info->wakelock_report)) {
		wake_unlock(&fg_info->wakelock_report);
	}
#else
#endif
	do_gettimeofday(&cur);
	fg_info->wakelock_active_time =
		timeval_to_ns(&cur) - fg_info->ts_wakelock_report;
	return ret;
}

static void bq27x00_external_power_changed(struct power_supply *psy)
{
	struct Nanohub_FuelGauge_Info *fg_info =
		container_of(psy, struct Nanohub_FuelGauge_Info, bat);
	union power_supply_propval prop = {0,};
	int rc, online = 0;

	rc = fg_info->usb_psy->get_property(fg_info->usb_psy,
				POWER_SUPPLY_PROP_ONLINE, &prop);
	if (rc)
		pr_err("nanohub: [FG] Couldn't read USB online property, rc=%d\n", rc);
	else
		online = prop.intval;

	pr_debug("nanohub: [FG] %s: online = %d\n", __func__, online);
	charger_online = online;

	device_is_charging = online;
	fg_info->charger_online = online;
	cancel_delayed_work_sync(&fg_info->work);
	schedule_delayed_work(&fg_info->work, 0);
}

static void set_properties_array(
		struct Nanohub_FuelGauge_Info *fg_info,
		enum power_supply_property *props, int num_props)
{
	int tot_sz = num_props * sizeof(enum power_supply_property);

	fg_info->bat.properties =
		devm_kzalloc(fg_info->dev, tot_sz, GFP_KERNEL);

	if (fg_info->bat.properties) {
		memcpy(fg_info->bat.properties, props, tot_sz);
		fg_info->bat.num_properties = num_props;
	} else {
		fg_info->bat.num_properties = 0;
	}
}


static char *batt_supplied_from[] = {
	"usb",
};

int bq27x00_powersupply_init(struct device *dev,
			struct nanohub_data *hub_data)
{
	int ret;
	char *name;
	int retval = 0;
	int num = 0;
	struct Nanohub_FuelGauge_Info *fg_info;
	struct bq27x00_reg_cache default_cache_data = {
		2, 1, 8330, 365, 3710, 136, 380, 300, 402, 0,
		263, 291, 292, 77, 29100, 50, 0, 1, 415, 26800};
	struct power_supply *usb_psy;

	usb_psy = power_supply_get_by_name("usb");
	if (!usb_psy) {
		pr_debug("nanohub: [FG] USB psy not found; deferring probe\n");
		return -EPROBE_DEFER;
	}

	fg_info = kzalloc(sizeof(struct Nanohub_FuelGauge_Info), GFP_KERNEL);

	if (!fg_info) {
		ret = -ENOMEM;
		pr_err("nanohub: [FG]failed to allocate fg_info\n");
		goto batt_failed_1;
	}
	name = kasprintf(GFP_KERNEL, "%s-%d", "nanohub_fuelgauge", num);
	if (!name) {
		pr_err("nanohub: [FG]failed to allocate device name\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}
	fg_info->usb_psy = usb_psy;
	fg_info->hub_data = hub_data;
	fg_info->hub_data->fg_info = fg_info;
	fg_info->dev = dev;
	fg_info->bat.name = name;
	fg_info->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	set_properties_array(fg_info, bq274xx_battery_props,
		ARRAY_SIZE(bq274xx_battery_props));
	fg_info->bat.get_property = bq27x00_battery_get_property;
	fg_info->bat.external_power_changed = bq27x00_external_power_changed;
	fg_info->bat.supplied_from = batt_supplied_from;
	fg_info->bat.num_supplies = ARRAY_SIZE(batt_supplied_from);

	fg_info->pre_interval = poll_interval;


	INIT_DELAYED_WORK(&fg_info->work, fuelgauge_battery_poll);
	INIT_DELAYED_WORK(&fg_info->request_delayed_work, request_delayed_func);
	mutex_init(&fg_info->lock);

#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_init(&fg_info->wakelock_report, WAKE_LOCK_SUSPEND,
		       "fuelgauge_wakelock_report");
#else
	wakeup_source_init(&fg_info->wakelock_report, "fuelgauge_wakelock_report");
#endif

	retval = power_supply_register(fg_info->dev, &fg_info->bat);
	if (ret) {
		pr_err("nanohub: [FG]failed to register battery: %d\n", ret);
		goto batt_failed_3;
	}

	memcpy(&(fg_info->cache), &default_cache_data,
		sizeof(struct bq27x00_reg_cache));
	/*bq27x00_update(fg_info);*/
	fg_info->last_update = jiffies;

	request_fuel_gauge_data(fg_info->hub_data);
	fg_info->requested = 1;

	m_fg_info = fg_info;

	return retval;

batt_failed_3:
	kfree(name);
batt_failed_2:
	kfree(fg_info);
batt_failed_1:

	return retval;
}

void bq27x00_powersupply_unregister(void)
{
	/*
	 * power_supply_unregister call bq27x00_battery_get_property which
	 * call bq27x00_battery_poll.
	 * Make sure that bq27x00_battery_poll will not call
	 * schedule_delayed_work again after unregister (which cause OOPS).
	 */
	struct Nanohub_FuelGauge_Info *fg_info = m_fg_info;

	poll_interval = 0;
	power_supply_unregister(&fg_info->bat);
	cancel_delayed_work_sync(&fg_info->work);
	mutex_destroy(&fg_info->lock);
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_destroy(&fg_info->wakelock_report);
#else
	wakeup_source_trash(&fg_info->wakelock_report);
#endif
	kfree(fg_info->bat.name);
	kfree(fg_info);
}
