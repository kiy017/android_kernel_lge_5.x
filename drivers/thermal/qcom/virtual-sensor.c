// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/thermal.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/math64.h>

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#define VIRTUAL_SENSOR_DRIVER "virtual-sensor"

enum aggr_logic {
	VIRT_MINIMUM,
	VIRT_MAXIMUM,
	/*
	 * (sum(temp[i] * coefficients[i]) + avg_offset) / avg_denominator,
	 * as the 4.19 of-thermal virtual sensors (e.g. LG's vts-virt-therm).
	 */
	VIRT_WEIGHTED_AVG,
};

struct virtual_sensor_data {
	struct thermal_zone_device	*tz_dev;
	struct device		    *dev;
	int				num_sensors;
	int32_t			   last_reading;
	enum aggr_logic		   logic;
	int				sensor_id;
	struct sensor_names *sensors_name;
	int32_t			   *coefficients;
	int32_t			   avg_offset;
	int32_t			   avg_denominator;
	struct thermal_zone_device	*tz[0];
};

struct sensor_names {
	const char *sensor_name;
};

static int virtual_sensor_read(void *data, int *temp)
{
	struct virtual_sensor_data *vs_sens = (struct virtual_sensor_data *)data;
	int ret = 0, idx = 0, curr_temp = 0;
	s64 wavg_sum = 0;

	curr_temp = (vs_sens->logic == VIRT_MAXIMUM) ? INT_MIN : INT_MAX;

	for (idx = 0; idx < vs_sens->num_sensors; idx++) {
		int sens_temp = 0;

		ret = thermal_zone_get_temp(vs_sens->tz[idx], &sens_temp);
		if (ret) {
			pr_err("virtual zone: sensor[%s] read error:%d\n",
					vs_sens->tz[idx]->type, ret);
			return ret;
		}

		switch (vs_sens->logic) {
		case VIRT_MINIMUM:
			curr_temp = MIN(curr_temp, sens_temp);
			break;
		case VIRT_MAXIMUM:
			curr_temp = MAX(curr_temp, sens_temp);
			break;
		case VIRT_WEIGHTED_AVG:
			wavg_sum += (s64)sens_temp * vs_sens->coefficients[idx];
			break;
		default:
			break;
		}
	}
	if (vs_sens->logic == VIRT_WEIGHTED_AVG)
		curr_temp = div_s64(wavg_sum + vs_sens->avg_offset,
					vs_sens->avg_denominator);
	vs_sens->last_reading = *temp = curr_temp;

	return 0;
}

static struct thermal_zone_of_device_ops virtual_sensor_ops = {
	.get_temp = virtual_sensor_read,
};

static int virtual_sensor_parse_wavg(struct device *dev,
		struct device_node *np, struct virtual_sensor_data *vs_sens)
{
	u32 val;
	int ret;

	ret = of_property_count_u32_elems(np, "qcom,coefficients");
	if (ret != vs_sens->num_sensors) {
		dev_err(dev, "%pOFn: need %d coefficients, got %d\n",
				np, vs_sens->num_sensors, ret);
		return -EINVAL;
	}

	vs_sens->coefficients = devm_kcalloc(dev, vs_sens->num_sensors,
				sizeof(*vs_sens->coefficients), GFP_KERNEL);
	if (!vs_sens->coefficients)
		return -ENOMEM;

	/* negative weights are stored as two's complement cells */
	ret = of_property_read_u32_array(np, "qcom,coefficients",
				(u32 *)vs_sens->coefficients, vs_sens->num_sensors);
	if (ret)
		return ret;

	vs_sens->avg_offset = 0;
	if (!of_property_read_u32(np, "qcom,avg-offset", &val))
		vs_sens->avg_offset = (int32_t)val;

	vs_sens->avg_denominator = 1;
	if (!of_property_read_u32(np, "qcom,avg-denominator", &val))
		vs_sens->avg_denominator = (int32_t)val;
	if (!vs_sens->avg_denominator) {
		dev_err(dev, "%pOFn: avg-denominator is 0\n", np);
		return -EINVAL;
	}

	return 0;
}

static int virtual_sensor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *subsys_np = NULL;
	struct virtual_sensor_data *vs_sens;
	struct thermal_zone_device *tzd;
	int ret, sensors_cnt = 0, idx = 0, sens_id = 0;
	u32 res;

	for_each_available_child_of_node(np, subsys_np) {
		ret = of_property_count_strings(subsys_np, "sensor-names");
		if (ret < 0) {
			dev_err(dev, "Couldn't get sensor names. %d\n", ret);
			return ret;
		}

		sensors_cnt = ret;
		vs_sens = devm_kzalloc(dev, struct_size(vs_sens, tz, sensors_cnt), GFP_KERNEL);
		if (!vs_sens)
			return -ENOMEM;

		ret = of_property_read_u32(subsys_np, "qcom,logic", &res);
		if (ret < 0)
			return ret;

		vs_sens->logic = res;
		vs_sens->num_sensors = sensors_cnt;

		if (vs_sens->logic == VIRT_WEIGHTED_AVG) {
			ret = virtual_sensor_parse_wavg(dev, subsys_np, vs_sens);
			if (ret)
				return ret;
		}
		vs_sens->dev = dev;
		vs_sens->sensor_id = sens_id;
		vs_sens->sensors_name = devm_kcalloc(dev, sensors_cnt + 1,
							sizeof(*vs_sens->sensors_name), GFP_KERNEL);
		if (!vs_sens->sensors_name)
			return -ENOMEM;

		for (idx = 0; idx < vs_sens->num_sensors; idx++) {
			ret = of_property_read_string_index(subsys_np,
							"sensor-names",
							idx,
							&vs_sens->sensors_name[idx].sensor_name);
			if (ret < 0) {
				dev_err(dev, "Couldn't get sensor name index: %d. %d\n",
								idx, ret);
			}

			tzd = thermal_zone_get_zone_by_name(vs_sens->sensors_name[idx].sensor_name);
			if (IS_ERR(tzd)) {
				ret = PTR_ERR(tzd);
				pr_err("No thermal zone found for sensor:%s. err:%d\n",
							vs_sens->sensors_name[idx].sensor_name,
							ret);
				return ret;
			}
			vs_sens->tz[idx] = tzd;
		}
		vs_sens->tz_dev = devm_thermal_zone_of_sensor_register(&pdev->dev,
							vs_sens->sensor_id,
							vs_sens,
							&virtual_sensor_ops);
		if (IS_ERR(vs_sens->tz_dev)) {
			ret = IS_ERR(vs_sens->tz_dev);
			if (ret != -ENODEV)
				dev_err(dev, "sensor register failed. ret:%d\n", ret);
			vs_sens->tz_dev = NULL;
			return ret;
		}
		sens_id++;
	}

	return 0;
}

static const struct of_device_id virtual_sensor_match[] = {
	{.compatible = "qcom,vs-sensor"},
	{}
};

static struct platform_driver virtual_sensor_driver = {
	.probe  = virtual_sensor_probe,
	.driver = {
		.name = VIRTUAL_SENSOR_DRIVER,
		.of_match_table = virtual_sensor_match,
	},
};
module_platform_driver(virtual_sensor_driver);
MODULE_DESCRIPTION("QTI VIRTUAL SENSOR driver");
