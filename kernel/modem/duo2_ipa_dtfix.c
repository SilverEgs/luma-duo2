// SPDX-License-Identifier: GPL-2.0
/*
 * Repair qcom,scaling-exceptions on the Duo 2's IPA node at runtime.
 *
 * The stock string list "wdi", "100", "600", "1200" went through a dtc
 * dtb->dts->dtb round trip that printed it as "wdi\0100\0600\01200"; the
 * recompile read \010 and \012 as octal escapes and stored the 11 bytes
 * "wdi\x08" "000\n00". ipa3_plat_drv_probe() then fails with -EFAULT and
 * the IPA (and with it every modem data path) never comes up.
 *
 * This module only swaps the property's value pointer for the correct
 * bytes when the mangled value is found, and stays resident (no exit) so
 * the replacement buffer lives as long as the tree does. Rebind the ipa
 * platform driver afterwards (docs/modem.md). Not needed once the DTB is
 * built by a duo2-mkdtb-usb.sh that preserves the string list.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>

static const char mangled[] = "wdi\010000\01200";       /* 11 bytes with NUL */
static const char fixed[] = "wdi\0" "100\0" "600\0" "1200"; /* 17 bytes with NUL */

static int __init duo2_ipa_dtfix_init(void)
{
	struct device_node *np;
	struct property *pp;
	int n, ret = 0;

	np = of_find_node_by_path("/soc/qcom,ipa@1e00000");
	if (!np)
		return -ENODEV;

	pp = of_find_property(np, "qcom,scaling-exceptions", NULL);
	if (!pp) {
		ret = -ENOENT;
		goto out;
	}

	if (pp->length == sizeof(fixed) &&
	    !memcmp(pp->value, fixed, sizeof(fixed))) {
		pr_info("duo2_ipa_dtfix: property already correct\n");
		goto out;
	}

	if (pp->length != sizeof(mangled) ||
	    memcmp(pp->value, mangled, sizeof(mangled))) {
		pr_err("duo2_ipa_dtfix: unexpected value (len %d), not touching it\n",
		       pp->length);
		ret = -EINVAL;
		goto out;
	}

	pp->value = (void *)fixed;
	pp->length = sizeof(fixed);
	n = of_property_count_strings(np, "qcom,scaling-exceptions");
	pr_info("duo2_ipa_dtfix: qcom,scaling-exceptions repaired (%d strings)\n", n);
out:
	of_node_put(np);
	return ret;
}
module_init(duo2_ipa_dtfix_init);

MODULE_DESCRIPTION("Duo 2: repair the dtc-mangled IPA scaling-exceptions property");
MODULE_LICENSE("GPL v2");
