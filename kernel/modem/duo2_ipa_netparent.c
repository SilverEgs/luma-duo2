// SPDX-License-Identifier: GPL-2.0
/*
 * Live-test stand-in for a one-line rmnet_ipa.c fix: give the vendor IPA
 * WAN netdev (rmnet_ipa0) the IPA platform device as its sysfs parent.
 *
 * Microsoft's msm-5.4 rmnet_ipa.c allocates rmnet_ipa0 without
 * SET_NETDEV_DEV(), so it lands in /sys/devices/virtual/net and has no
 * driver. ModemManager's qcom-soc plugin only binds an IPA net port whose
 * parent driver is "ipa" (77-mm-qcom-soc.rules: DRIVERS=="ipa"), and refuses
 * to create a QMI modem without a net port.
 *
 * Writing an interface name to /sys/module/duo2_ipa_netparent/parameters/
 * reparent moves that netdev under 1e00000.qcom,ipa (device_move, which
 * emits a "move" uevent). rmnet_ipa re-registers the netdev on every modem
 * power-up, so a udev rule re-applies it on each "add" (docs/modem.md).
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/string.h>
#include <net/net_namespace.h>

static int reparent_set(const char *val, const struct kernel_param *kp)
{
	char name[IFNAMSIZ];
	struct device_node *np;
	struct platform_device *pdev;
	struct net_device *ndev;
	int ret;

	strscpy(name, val, sizeof(name));
	strim(name);
	if (strncmp(name, "rmnet_ipa", 9))
		return -EINVAL;

	np = of_find_node_by_path("/soc/qcom,ipa@1e00000");
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	if (!pdev->dev.driver) {
		ret = -EPROBE_DEFER;
		goto out_pdev;
	}

	rtnl_lock();
	ndev = __dev_get_by_name(&init_net, name);
	if (!ndev) {
		ret = -ENOENT;
	} else if (ndev->dev.parent == &pdev->dev) {
		ret = 0;
	} else if (ndev->dev.parent) {
		ret = -EBUSY;
	} else {
		ret = device_move(&ndev->dev, &pdev->dev, DPM_ORDER_NONE);
		pr_info("duo2_ipa_netparent: %s -> %s: %d\n", name,
			dev_name(&pdev->dev), ret);
	}
	rtnl_unlock();
out_pdev:
	put_device(&pdev->dev);
	return ret;
}

static const struct kernel_param_ops reparent_ops = {
	.set = reparent_set,
};
module_param_cb(reparent, &reparent_ops, NULL, 0200);
MODULE_PARM_DESC(reparent, "netdev name to move under the IPA platform device");

static int __init duo2_ipa_netparent_init(void)
{
	return 0;
}
module_init(duo2_ipa_netparent_init);

static void __exit duo2_ipa_netparent_exit(void)
{
}
module_exit(duo2_ipa_netparent_exit);

MODULE_DESCRIPTION("Duo 2: parent rmnet_ipa netdevs to the IPA device for ModemManager");
MODULE_LICENSE("GPL v2");
