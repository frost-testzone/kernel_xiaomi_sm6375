// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/cpu_cooling.h>
#include <linux/energy_model.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#define LUT_MAX_ENTRIES			40U
#define LUT_SRC				GENMASK(31, 30)
#define LUT_L_VAL			GENMASK(7, 0)
#define LUT_CORE_COUNT			GENMASK(18, 16)
#define LUT_VOLT			GENMASK(11, 0)
#define LUT_ROW_SIZE			32
#define CLK_HW_DIV			2
#define MAX_FN_SIZE			20
#define MAX_ROW				2

enum {
	REG_ENABLE,
	REG_FREQ_LUT,
	REG_VOLT_LUT,
	REG_PERF_STATE,

	REG_ARRAY_SIZE,
};

static unsigned long cpu_hw_rate, xo_rate;
static const u16 *offsets;
static unsigned int lut_row_size = LUT_ROW_SIZE;
static unsigned int lut_max_entries = LUT_MAX_ENTRIES;
static bool accumulative_counter;
static bool perf_lock_support;

struct cpufreq_qcom {
	struct cpufreq_frequency_table *table;
	void __iomem *base;
	void __iomem *pdmem_base;
	void __iomem **sdpm_base;
	int sdpm_base_count;
	cpumask_t related_cpus;
};

static const u16 cpufreq_qcom_std_offsets[REG_ARRAY_SIZE] = {
	[REG_ENABLE]		= 0x0,
	[REG_FREQ_LUT]		= 0x110,
	[REG_VOLT_LUT]		= 0x114,
	[REG_PERF_STATE]	= 0x920,
};

static const u16 cpufreq_qcom_epss_std_offsets[REG_ARRAY_SIZE] = {
	[REG_ENABLE]		= 0x0,
	[REG_FREQ_LUT]		= 0x100,
	[REG_VOLT_LUT]		= 0x200,
	[REG_PERF_STATE]	= 0x320,
};

static struct cpufreq_qcom *qcom_freq_domain_map[NR_CPUS];

static int
qcom_cpufreq_hw_target_index(struct cpufreq_policy *policy,
			     unsigned int index)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	unsigned long freq = policy->freq_table[index].frequency;
	int i;

	if (perf_lock_support) {
		if (c->pdmem_base)
			writel_relaxed(index, c->pdmem_base);
	}

	for (i = 0; i < c->sdpm_base_count && freq > policy->cur; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	writel_relaxed(index, policy->driver_data + offsets[REG_PERF_STATE]);
	arch_set_freq_scale(policy->related_cpus, freq,
			    policy->cpuinfo.max_freq);

	for (i = 0; i < c->sdpm_base_count && freq < policy->cur; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	return 0;
}

static unsigned int qcom_cpufreq_hw_get(unsigned int cpu)
{
	struct cpufreq_policy *policy;
	unsigned int index;

	policy = cpufreq_cpu_get_raw(cpu);
	if (!policy)
		return 0;

	index = readl_relaxed(policy->driver_data + offsets[REG_PERF_STATE]);
	index = min(index, lut_max_entries - 1);

	return policy->freq_table[index].frequency;
}

static unsigned int
qcom_cpufreq_hw_fast_switch(struct cpufreq_policy *policy,
			    unsigned int target_freq)
{
	int index;

	index = policy->cached_resolved_idx;
	if (index < 0)
		return 0;

	if (qcom_cpufreq_hw_target_index(policy, index))
		return 0;

	return policy->freq_table[index].frequency;
}

static int qcom_cpufreq_hw_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c;
	struct device *cpu_dev;
	int ret;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("%s: failed to get cpu%d device\n", __func__,
				policy->cpu);
		return -ENODEV;
	}

	c = qcom_freq_domain_map[policy->cpu];
	if (!c) {
		pr_err("No scaling support for CPU%d\n", policy->cpu);
		return -ENODEV;
	}

	cpumask_copy(policy->cpus, &c->related_cpus);

	ret = dev_pm_opp_get_opp_count(cpu_dev);
	if (ret <= 0)
		dev_err(cpu_dev, "OPP table is not ready\n");

	policy->freq_table = c->table;
	policy->driver_data = c->base;
	policy->fast_switch_possible = true;
	policy->dvfs_possible_from_any_cpu = true;

	dev_pm_opp_of_register_em(policy->cpus);

	return 0;
}

static struct freq_attr *qcom_cpufreq_hw_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&cpufreq_freq_attr_scaling_boost_freqs,
	NULL
};

static void qcom_cpufreq_ready(struct cpufreq_policy *policy)
{
	static struct thermal_cooling_device *cdev[NR_CPUS];
	struct device_node *np;
	unsigned int cpu = policy->cpu;

	if (cdev[cpu])
		return;

	np = of_cpu_device_node_get(cpu);
	if (WARN_ON(!np))
		return;

	/*
	 * For now, just loading the cooling device;
	 * thermal DT code takes care of matching them.
	 */
	if (of_find_property(np, "#cooling-cells", NULL)) {
		cdev[cpu] = of_cpufreq_cooling_register(policy);
		if (IS_ERR(cdev[cpu])) {
			pr_err("running cpufreq for CPU%d without cooling dev: %ld\n",
			       cpu, PTR_ERR(cdev[cpu]));
			cdev[cpu] = NULL;
		}
	}

	of_node_put(np);
}

static int qcom_cpufreq_hw_suspend(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	unsigned long freq = policy->freq_table[0].frequency;
	int i;

	for (i = 0; i < c->sdpm_base_count && freq < policy->cur; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	return 0;
}

static int qcom_cpufreq_hw_resume(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	unsigned long freq = policy->cur;
	int i;

	for (i = 0; i < c->sdpm_base_count; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	return 0;
}

static struct cpufreq_driver cpufreq_qcom_hw_driver = {
	.flags		= CPUFREQ_STICKY | CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= qcom_cpufreq_hw_target_index,
	.get		= qcom_cpufreq_hw_get,
	.init		= qcom_cpufreq_hw_cpu_init,
	.fast_switch    = qcom_cpufreq_hw_fast_switch,
	.name		= "qcom-cpufreq-hw",
	.attr		= qcom_cpufreq_hw_attr,
	.boost_enabled	= true,
	.ready		= qcom_cpufreq_ready,
	.suspend	= qcom_cpufreq_hw_suspend,
	.resume		= qcom_cpufreq_hw_resume,
};

static bool of_find_freq(u32 *of_table, int of_len, long frequency)
{
	int i;

	if (!of_table)
		return true;

	for (i = 0; i < of_len; i++) {
		if (frequency == of_table[i])
			return true;
	}

	return false;
}

static int qcom_cpufreq_hw_read_lut(struct platform_device *pdev,
				    struct cpufreq_qcom *c, u32 max_cores,
				    int domain_index)
{
	struct device *dev = &pdev->dev, *cpu_dev;
	u32 data, src, lval, i, core_count, prev_cc, prev_freq = 0, freq, volt;
	unsigned long cpu;
	int ret, of_len;
	u32 *of_table = NULL;
	char tbl_name[] = "qcom,cpufreq-table-##";
	bool invalidate_freq;

	c->table = devm_kcalloc(dev, lut_max_entries + 1,
				sizeof(*c->table), GFP_KERNEL);
	if (!c->table)
		return -ENOMEM;

	snprintf(tbl_name, sizeof(tbl_name), "qcom,cpufreq-table-%d",
		 domain_index);
	if (of_find_property(dev->of_node, tbl_name, &of_len) && of_len > 0) {
		of_len /= sizeof(*of_table);

		of_table = devm_kcalloc(dev, of_len, sizeof(*of_table),
					GFP_KERNEL);
		if (!of_table) {
			ret = -ENOMEM;
			goto err_cpufreq_table;
		}

		ret = of_property_read_u32_array(dev->of_node, tbl_name,
						 of_table, of_len);
		if (ret)
			goto err_of_table;
	}

	cpu = cpumask_first(&c->related_cpus);
	cpu_dev = get_cpu_device(cpu);

	prev_cc = 0;

	for (i = 0; i < lut_max_entries; i++) {
		data = readl_relaxed(c->base + offsets[REG_FREQ_LUT] +
				      i * lut_row_size);
		src = FIELD_GET(LUT_SRC, data);
		lval = FIELD_GET(LUT_L_VAL, data);
		core_count = FIELD_GET(LUT_CORE_COUNT, data);

		data = readl_relaxed(c->base + offsets[REG_VOLT_LUT] +
				      i * lut_row_size);
		volt = FIELD_GET(LUT_VOLT, data) * 1000;

		if (src)
			freq = xo_rate * lval / 1000;
		else
			freq = cpu_hw_rate / 1000;

		c->table[i].frequency = freq;
		dev_dbg(dev, "index=%d freq=%d, core_count %d\n",
				i, c->table[i].frequency, core_count);

		if (!of_find_freq(of_table, of_len, c->table[i].frequency)) {
			invalidate_freq = true;
			c->table[i].frequency = CPUFREQ_ENTRY_INVALID;
		} else {
			invalidate_freq = false;
			if (core_count != max_cores)
				c->table[i].flags = CPUFREQ_BOOST_FREQ;

			/*
			 * Two of the same frequencies with the same core counts means
			 * end of table.
			 */
			if (i > 0 && prev_freq == freq && prev_cc == core_count)
				break;
		}

		prev_cc = core_count;
		prev_freq = freq;

		if (cpu_dev && !invalidate_freq)
			dev_pm_opp_add(cpu_dev, freq * 1000, volt);
	}

	c->table[i].frequency = CPUFREQ_TABLE_END;

	if (cpu_dev)
		dev_pm_opp_set_sharing_cpus(cpu_dev, &c->related_cpus);

	if (of_table)
		devm_kfree(dev, of_table);

	return 0;

err_of_table:
	devm_kfree(dev, of_table);
err_cpufreq_table:
	devm_kfree(dev, c->table);
	return ret;
}

static void qcom_get_related_cpus(int index, struct cpumask *m)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	int cpu, ret;

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np)
			continue;

		ret = of_parse_phandle_with_args(cpu_np, "qcom,freq-domain",
				"#freq-domain-cells", 0, &args);
		of_node_put(cpu_np);
		if (ret < 0)
			continue;

		if (index == args.args[0])
			cpumask_set_cpu(cpu, m);
	}
}

static int qcom_cpu_resources_init(struct platform_device *pdev,
				   unsigned int cpu, int index,
				   unsigned int max_cores)
{
	struct cpufreq_qcom *c;
	struct resource *res;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	char pdmem_name[MAX_FN_SIZE] = {};
	char sdpm_name[MAX_FN_SIZE] = {};
	int ret, cpu_r, len, reg, size, i, j;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	offsets = of_device_get_match_data(&pdev->dev);
	if (!offsets)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (!of_property_read_bool(dev->of_node, "qcom,skip-enable-check")) {
		/* HW should be in enabled state to proceed */
		if (!(readl_relaxed(base + offsets[REG_ENABLE]) & 0x1)) {
			dev_err(dev, "Domain-%d cpufreq hardware not enabled\n",
				 index);
			return -ENODEV;
		}
	}

	accumulative_counter = !of_property_read_bool(dev->of_node,
						"qcom,no-accumulative-counter");
	c->base = base;

	qcom_get_related_cpus(index, &c->related_cpus);
	if (!cpumask_weight(&c->related_cpus)) {
		dev_err(dev, "Domain-%d failed to get related CPUs\n", index);
		return -ENONET;
	}

	ret = qcom_cpufreq_hw_read_lut(pdev, c, max_cores, index);
	if (ret) {
		dev_err(dev, "Domain-%d failed to read LUT\n", index);
		return ret;
	}

	perf_lock_support = of_property_read_bool(dev->of_node,
					"qcom,perf-lock-support");
	if (perf_lock_support) {
		snprintf(pdmem_name, sizeof(pdmem_name), "pdmem-domain%d",
								index);
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
								pdmem_name);
		if (!res)
			dev_err(dev, "PDMEM domain-%d failed\n", index);

		base = devm_ioremap_resource(dev, res);
		if (IS_ERR(base))
			dev_err(dev, "Failed to map PDMEM domain-%d\n", index);
		else
			c->pdmem_base = base;
	}

	snprintf(sdpm_name, sizeof(sdpm_name), "qcom,sdpm-cx-mx-%d", index);
	len = of_property_count_u32_elems(dev->of_node, sdpm_name);
	if (len > 0) {
		c->sdpm_base_count = len / MAX_ROW;
		c->sdpm_base = devm_kcalloc(dev, c->sdpm_base_count,
					sizeof(*c->sdpm_base), GFP_KERNEL);
		if (!c->sdpm_base)
			return -ENOMEM;

		for (i = 0, j = 0; i < c->sdpm_base_count; i++, j += 2) {
			of_property_read_u32_index(dev->of_node, sdpm_name,
								j, &reg);
			of_property_read_u32_index(dev->of_node, sdpm_name,
								j + 1, &size);
			base = devm_ioremap(dev, reg, size);
			if (IS_ERR(base)) {
				dev_err(dev, "Failed to map base %d domain-%d\n",
						i, index);
				return PTR_ERR(base);
			}

			c->sdpm_base[i] = base;
		}
	}

	for_each_cpu(cpu_r, &c->related_cpus)
		qcom_freq_domain_map[cpu_r] = c;

	return 0;
}

static int qcom_resources_init(struct platform_device *pdev)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	struct clk *clk;
	unsigned int cpu;
	int ret;

	clk = clk_get(&pdev->dev, "xo");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	xo_rate = clk_get_rate(clk);
	clk_put(clk);

	clk = clk_get(&pdev->dev, "alternate");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	cpu_hw_rate = clk_get_rate(clk) / CLK_HW_DIV;
	clk_put(clk);

	of_property_read_u32(pdev->dev.of_node, "qcom,lut-row-size",
			      &lut_row_size);

	of_property_read_u32(pdev->dev.of_node, "qcom,lut-max-entries",
			      &lut_max_entries);

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_dbg(&pdev->dev, "Failed to get cpu %d device\n",
				cpu);
			continue;
		}

		ret = of_parse_phandle_with_args(cpu_np, "qcom,freq-domain",
						  "#freq-domain-cells", 0,
						   &args);
		of_node_put(cpu_np);
		if (ret)
			return ret;

		if (qcom_freq_domain_map[cpu])
			continue;

		ret = qcom_cpu_resources_init(pdev, cpu, args.args[0],
					      args.args[1]);
		if (ret)
			return ret;
	}

	return 0;
}

static int qcom_cpufreq_hw_driver_probe(struct platform_device *pdev)
{
	int rc;

	/* Get the bases of cpufreq for domains */
	rc = qcom_resources_init(pdev);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq resource init failed\n");
		return rc;
	}

	rc = cpufreq_register_driver(&cpufreq_qcom_hw_driver);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq HW driver failed to register\n");
		return rc;
	}

	of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
	dev_dbg(&pdev->dev, "QCOM CPUFreq HW driver initialized\n");

	return 0;
}

static int qcom_cpufreq_hw_driver_remove(struct platform_device *pdev)
{
	struct device *cpu_dev;
	int cpu;

	for_each_possible_cpu(cpu) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;

		dev_pm_opp_remove_all_dynamic(cpu_dev);
	}

	return cpufreq_unregister_driver(&cpufreq_qcom_hw_driver);
}

static const struct of_device_id qcom_cpufreq_hw_match[] = {
	{ .compatible = "qcom,cpufreq-hw", .data = &cpufreq_qcom_std_offsets },
	{ .compatible = "qcom,cpufreq-hw-epss",
				   .data = &cpufreq_qcom_epss_std_offsets },
	{}
};

static struct platform_driver qcom_cpufreq_hw_driver = {
	.probe = qcom_cpufreq_hw_driver_probe,
	.remove = qcom_cpufreq_hw_driver_remove,
	.driver = {
		.name = "qcom-cpufreq-hw",
		.of_match_table = qcom_cpufreq_hw_match,
	},
};

static int __init qcom_cpufreq_hw_init(void)
{
	return platform_driver_register(&qcom_cpufreq_hw_driver);
}
subsys_initcall(qcom_cpufreq_hw_init);

static void __exit qcom_cpufreq_hw_exit(void)
{
	platform_driver_unregister(&qcom_cpufreq_hw_driver);
}
module_exit(qcom_cpufreq_hw_exit);

MODULE_DESCRIPTION("QCOM CPUFREQ HW Driver");
MODULE_LICENSE("GPL v2");
