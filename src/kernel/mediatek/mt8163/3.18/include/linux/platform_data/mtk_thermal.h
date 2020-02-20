#ifndef _MTK_THERMAL_H_
#define _MTK_THERMAL_H_

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/notifier.h>
#include <linux/leds.h>
#include <linux/thermal_framework.h>

struct tmp103_thermal_zone {
	struct thermal_zone_device *tz;
	struct work_struct therm_work;
	struct mtk_thermal_platform_data *pdata;
};

struct mtk_cooler_platform_data {
	char type[THERMAL_NAME_LENGTH];
	unsigned long state;
	unsigned long max_state;
	struct thermal_cooling_device *cdev;
	int level;
	int levels[THERMAL_MAX_TRIPS];
};

#ifdef CONFIG_rsa123
struct cdev_t {
	char type[THERMAL_NAME_LENGTH];
	unsigned long upper;
	unsigned long lower;
};
#endif

struct trip_t {
	unsigned long temp;
	enum thermal_trip_type type;
	unsigned long hyst;
#ifdef CONFIG_rsa123
	struct cdev_t cdev[THERMAL_MAX_TRIPS];
#endif
};

struct mtk_thermal_platform_data {
	int num_trips;
	enum thermal_device_mode mode;
	int polling_delay;
	int shutdown_wait;
	/*
	 * Add a list for sensor params. Note: therm_lock takes
	 * care of list protection.
	 */
	struct list_head ts_list;
	struct thermal_zone_params tzp;
	struct trip_t trips[THERMAL_MAX_TRIPS];
	int num_cdevs;
	char cdevs[THERMAL_MAX_TRIPS][THERMAL_NAME_LENGTH];
};

struct mtk_thermal_platform_data_wrapper {
	struct mtk_thermal_platform_data *data;
	struct thermal_dev_params params;
};

struct alt_cpu_thermal_zone {
	struct thermal_zone_device *tz;
	struct work_struct therm_work;
	struct mtk_thermal_platform_data *pdata;
};

#endif /* _MTK_THERMAL_H_ */
