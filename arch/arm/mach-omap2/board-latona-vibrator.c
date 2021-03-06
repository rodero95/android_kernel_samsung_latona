/* arch/arm/mach-omap2/board-latona-vibrator.c
 * modified from arch/arm/mach-omap2/board-tuna-vibrator.c
 *
 * Copyright (C) 2011 Samsung Electronics Co. Ltd. All Rights Reserved.
 * Copyright (C) 2012 The CyanogenMod Project
 * Author: Rom Lemarchand <rlemarchand@sta.samsung.com>
 * Modified: Daniel Hillenbrand <codeworkx@cyanogenmod.com>
 * Adapted for OMAP3: Dheeraj CVR <cvr.dheeraj@gmail.com>
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

#include <linux/hrtimer.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#include <linux/mutex.h>
#include <asm/mach-types.h>
#include <plat/dmtimer.h>

#include <../../../drivers/staging/android/timed_output.h>

#include "mux.h"
#include <mach/board-latona.h>

#define VIB_GPTIMER_NUM		10
#define PWM_DUTY_MAX		1160
#define MAX_TIMEOUT      10000 /* 10s */

#define LEVEL_MAX      127
#define LEVEL_MIN      0
#define LEVEL_DEFAULT    64
#define LEVEL_THRESHOLD    96

static struct vibrator {
	struct wake_lock wklock;
	struct hrtimer timer;
	struct mutex lock;
	struct omap_dm_timer *gptimer;
	bool enabled;
	unsigned gpio_en;
	unsigned long pwmval;
	unsigned long oldpwmval;
} vibdata;

static ssize_t pwm_value_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{

	int count;

	count = sprintf(buf, "%lu\n", vibdata.pwmval);
	pr_debug("vibrator: pwm value: %lu\n", vibdata.pwmval);

	return count;
}

ssize_t pwm_value_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{

	if (kstrtoul(buf, 0, &vibdata.pwmval))
		pr_err("vibrator: error in storing pwm value\n");
	pr_debug("vibrator: pwm value: %lu\n", vibdata.pwmval);

	return size;
}
static DEVICE_ATTR(pwm_value, S_IRUGO | S_IWUSR,
    pwm_value_show, pwm_value_store);

static ssize_t pwm_max_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
  int count;

  count = sprintf(buf, "%d\n", LEVEL_MAX);
  pr_info("vibrator: pwm max value: %d\n", LEVEL_MAX);

  return count;
}

static DEVICE_ATTR(pwm_max, S_IRUGO | S_IWUSR,
    pwm_max_show, NULL);

static ssize_t pwm_min_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
  int count;

  count = sprintf(buf, "%d\n", LEVEL_MIN);
  pr_info("vibrator: pwm min value: %d\n", LEVEL_MIN);

  return count;
}

static DEVICE_ATTR(pwm_min, S_IRUGO | S_IWUSR,
    pwm_min_show, NULL);

static ssize_t pwm_default_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
  int count;

  count = sprintf(buf, "%d\n", LEVEL_DEFAULT);
  pr_info("vibrator: pwm default value: %d\n", LEVEL_DEFAULT);

  return count;
}

static DEVICE_ATTR(pwm_default, S_IRUGO | S_IWUSR,
    pwm_default_show, NULL);

static ssize_t pwm_threshold_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
  int count;

  count = sprintf(buf, "%d\n", LEVEL_THRESHOLD);
  pr_info("vibrator: pwm threshold value: %d\n", LEVEL_THRESHOLD);

  return count;
}

static DEVICE_ATTR(pwm_threshold, S_IRUGO | S_IWUSR,
    pwm_threshold_show, NULL);

static int pwm_set(unsigned long force)
{
	int pwm_duty;

	pr_debug("vibrator: pwm_set force=%lu\n", force);

	if (unlikely(vibdata.gptimer == NULL))
		return -EINVAL;

	/*
	 * Formula for matching the user space force (-127 to +127)
	 * to Duty cycle.
	 * Duty cycle will vary from 0 to 45('0' means 0% duty cycle,
	 * '45' means 100% duty cycle.
	 * Also if user space force equals to -127 then duty
	 * cycle will be 0 (0%), if force equals to 0 duty cycle
	 * will be 22.5(50%), if +127 then duty cycle will
	 * be 45(100%)
	 */

	pwm_duty = ((force + 128)
			* (PWM_DUTY_MAX >> 1)/128);

	omap_dm_timer_set_load(vibdata.gptimer, 1, -PWM_DUTY_MAX);
	omap_dm_timer_set_match(vibdata.gptimer, 1, -pwm_duty);
	omap_dm_timer_set_pwm(vibdata.gptimer, 0, 1,
			OMAP_TIMER_TRIGGER_OVERFLOW_AND_COMPARE);
	omap_dm_timer_enable(vibdata.gptimer);
	omap_dm_timer_write_counter(vibdata.gptimer, -2);
	omap_dm_timer_save_context(vibdata.gptimer);
	omap_dm_timer_disable(vibdata.gptimer);

	return 0;
}

static void vibrator_off(void)
{
	if (!vibdata.enabled)
		return;
	omap_dm_timer_stop(vibdata.gptimer);
	gpio_set_value(vibdata.gpio_en, 0);
	vibdata.enabled = false;
	wake_unlock(&vibdata.wklock);
}

static int vibrator_get_time(struct timed_output_dev *dev)
{
	if (hrtimer_active(&vibdata.timer)) {
		ktime_t r = hrtimer_get_remaining(&vibdata.timer);
		return ktime_to_ms(r);
	}

	return 0;
}

static void vibrator_enable(struct timed_output_dev *dev, int value)
{
	mutex_lock(&vibdata.lock);

	/* make sure pwmval is between 0 and 127 */
	if(vibdata.pwmval > LEVEL_MAX) {
        vibdata.pwmval = LEVEL_MAX;
    } else if (vibdata.pwmval < LEVEL_MIN) {
        vibdata.pwmval = LEVEL_MIN;
	}

	/* set the current pwmval */
	if (vibdata.pwmval != vibdata.oldpwmval) {
		pwm_set(vibdata.pwmval);
		vibdata.oldpwmval = vibdata.pwmval;
	}

	/* cancel previous timer and set GPIO according to value */
	hrtimer_cancel(&vibdata.timer);

	if (value) {
		pr_debug("vibrator: value=%d, pwm value=%lu\n", value, vibdata.pwmval);

		wake_lock(&vibdata.wklock);

		gpio_set_value(vibdata.gpio_en, 1);
		omap_dm_timer_start(vibdata.gptimer);

		vibdata.enabled = true;

		if (value > 0) {
			if (value > MAX_TIMEOUT)
				value = MAX_TIMEOUT;

			hrtimer_start(&vibdata.timer,
				ns_to_ktime((u64)value * NSEC_PER_MSEC),
				HRTIMER_MODE_REL);
		}
	} else {
		vibrator_off();
	}

	mutex_unlock(&vibdata.lock);
}

static struct timed_output_dev to_dev = {
	.name		= "vibrator",
	.get_time	= vibrator_get_time,
	.enable		= vibrator_enable,
};

static enum hrtimer_restart vibrator_timer_func(struct hrtimer *timer)
{
	vibrator_off();
	return HRTIMER_NORESTART;
}

static int __init vibrator_init(void)
{
	int ret;

	pr_debug("vibrator_init()\n");

	vibdata.enabled = false;

	hrtimer_init(&vibdata.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vibdata.timer.function = vibrator_timer_func;

	vibdata.gptimer = omap_dm_timer_request_specific(VIB_GPTIMER_NUM);
	if (vibdata.gptimer == NULL)
		return -1;

	ret = omap_dm_timer_set_source(vibdata.gptimer,
		OMAP_TIMER_SRC_SYS_CLK);
	if (ret < 0) {
		pr_err("vibrator_init(): timer_set_source failed\n");
		goto err_dm_timer_src;
	}

	omap_dm_timer_set_load(vibdata.gptimer, 1, -PWM_DUTY_MAX);
	omap_dm_timer_set_match(vibdata.gptimer, 1, -PWM_DUTY_MAX+5);
	omap_dm_timer_set_pwm(vibdata.gptimer, 0, 1,
		OMAP_TIMER_TRIGGER_OVERFLOW_AND_COMPARE);
	omap_dm_timer_enable(vibdata.gptimer);
	omap_dm_timer_write_counter(vibdata.gptimer, -2);
	omap_dm_timer_disable(vibdata.gptimer);

	wake_lock_init(&vibdata.wklock, WAKE_LOCK_SUSPEND, "vibrator");
	mutex_init(&vibdata.lock);

	vibdata.pwmval = LEVEL_DEFAULT;
	vibdata.oldpwmval = LEVEL_MIN;

	ret = timed_output_dev_register(&to_dev);
	if (ret < 0) {
		pr_err("vibrator_init(): failed to register timed_output device\n");
		goto err_to_dev_reg;
	}

     /* User controllable pwm level */
  ret = device_create_file(to_dev.dev, &dev_attr_pwm_value);
  if (ret < 0) {
    pr_err("vibrator_init(): create sysfs fail: pwm_value\n");
  }

  ret = device_create_file(to_dev.dev, &dev_attr_pwm_max);
  if (ret < 0) {
    pr_err("vibrator_init(): create sysfs fail: pwm_max\n");
  }

  ret = device_create_file(to_dev.dev, &dev_attr_pwm_min);
  if (ret < 0) {
    pr_err("vibrator_init(): create sysfs fail: pwm_min\n");
  }

  ret = device_create_file(to_dev.dev, &dev_attr_pwm_default);
  if (ret < 0) {
    pr_err("vibrator_init(): create sysfs fail: pwm_default\n");
  }

  ret = device_create_file(to_dev.dev, &dev_attr_pwm_threshold);
  if (ret < 0) {
    pr_err("vibrator_init(): create sysfs fail: pwm_threshold\n");
  }

	return 0;

err_to_dev_reg:
	mutex_destroy(&vibdata.lock);
	wake_lock_destroy(&vibdata.wklock);

err_dm_timer_src:
	omap_dm_timer_free(vibdata.gptimer);
	vibdata.gptimer = NULL;

	return -1;
}

static int __init omap3_latona_vibrator_init(void)
{
	int ret;

	pr_debug("omap3_latona_vibrator_init()\n");

	vibdata.gpio_en = OMAP_GPIO_VIBTONE_EN;

	omap_mux_init_gpio(vibdata.gpio_en, OMAP_PIN_OUTPUT |
						OMAP_PIN_OFF_OUTPUT_LOW);

	gpio_direction_output(vibdata.gpio_en, 0);

	ret = vibrator_init();
	if (ret < 0)
		pr_err("omap3_latona_vibrator_init(): vibrator_init() failed\n");

	return ret;
}

/*
 * This is needed because the vibrator is dependent on omap_dm_timers which get
 * initialized at device_init time
 */
late_initcall(omap3_latona_vibrator_init);
