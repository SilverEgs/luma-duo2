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
 * power-up, so luma-duo2-modem re-applies it each time (docs/modem.md).
 *
 * It also gives the IPA device the two attributes mainline's IPA driver has
 * and ModemManager reads to open the modem's data port through QMI DPM
 * (mm-port-qmi.c dpm_open_port()):
 *
 *   modem/rx_endpoint_id   APPS_WAN_CONS, the IPA endpoint the AP receives on
 *   modem/tx_endpoint_id   APPS_WAN_PROD, the one it sends on
 *
 * the endpoint pair rmnet_ipa's RMNET_IOCTL_GET_EP_PAIR reports to Android's
 * netmgrd. Without DPM the modem knows no data port for the embedded
 * endpoint and answers every WDA request about it with InvalidArgument.
 */
#include <linux/ipa.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/string.h>
#include <net/net_namespace.h>

static struct platform_device *ipa_pdev(void)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_find_node_by_path("/soc/qcom,ipa@1e00000");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	return pdev;
}

static int reparent_set(const char *val, const struct kernel_param *kp)
{
	char name[IFNAMSIZ];
	struct platform_device *pdev;
	struct net_device *ndev;
	int ret;

	strscpy(name, val, sizeof(name));
	strim(name);
	if (strncmp(name, "rmnet_ipa", 9))
		return -EINVAL;

	pdev = ipa_pdev();
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

/* An endpoint the IPA has not allocated reads as an empty value, which
 * ModemManager takes as "no endpoint" and skips DPM. */
static ssize_t endpoint_show(enum ipa_client_type client, char *buf)
{
	int ep = ipa_get_ep_mapping(client);

	return ep < 0 ? 0 : sprintf(buf, "%d\n", ep);
}

static ssize_t rx_endpoint_id_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return endpoint_show(IPA_CLIENT_APPS_WAN_CONS, buf);
}
static DEVICE_ATTR_RO(rx_endpoint_id);

static ssize_t tx_endpoint_id_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return endpoint_show(IPA_CLIENT_APPS_WAN_PROD, buf);
}
static DEVICE_ATTR_RO(tx_endpoint_id);

static struct attribute *modem_attrs[] = {
	&dev_attr_rx_endpoint_id.attr,
	&dev_attr_tx_endpoint_id.attr,
	NULL,
};

static const struct attribute_group modem_group = {
	.name = "modem",
	.attrs = modem_attrs,
};

static struct platform_device *modem_group_dev;

static int __init duo2_ipa_netparent_init(void)
{
	struct platform_device *pdev = ipa_pdev();
	int ret;

	if (!pdev)
		return -ENODEV;
	ret = sysfs_create_group(&pdev->dev.kobj, &modem_group);
	if (ret) {
		put_device(&pdev->dev);
		return ret;
	}
	modem_group_dev = pdev;
	return 0;
}
module_init(duo2_ipa_netparent_init);

static void __exit duo2_ipa_netparent_exit(void)
{
	sysfs_remove_group(&modem_group_dev->dev.kobj, &modem_group);
	put_device(&modem_group_dev->dev);
}
module_exit(duo2_ipa_netparent_exit);

MODULE_DESCRIPTION("Duo 2: what ModemManager expects of the IPA net port (parent, endpoint IDs)");
MODULE_LICENSE("GPL v2");
