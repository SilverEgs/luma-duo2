// SPDX-License-Identifier: GPL-2.0
/*
 * luma-duo2-ipa-wan: set up the vendor IPA's WAN data pipes on the Duo 2's
 * msm-5.4 kernel, the part of Android's netmgrd that Luma needs
 * (docs/modem.md, "Mobile data").
 *
 * rmnet_ipa (techpack/dataipa, rmnet_ipa.c) registers rmnet_ipa0 when the
 * modem boots but only connects its GSI pipes to the IPA when user space
 * sends RMNET_IOCTL_EXTENDED requests on it:
 *
 *   SET_EGRESS_DATA_FORMAT   APPS_WAN_PROD, AP -> IPA -> modem (uplink)
 *   SET_INGRESS_DATA_FORMAT  APPS_WAN_CONS, modem -> IPA -> AP (downlink),
 *                            plus the IPA's default WAN route and its QMAP
 *                            header template
 *   ADD_MUX_CHANNEL          names a QMAP mux id for the IPA's interface
 *                            table (tethering offload; harmless without it)
 *
 * Until then nothing reaches the AP and rmnet_ipa0 drops what it is given.
 * Both directions are set up as plain QMAP (MAPv1): no checksum offload, no
 * uplink aggregation, and no AGG_DATA on the downlink, so the IPA splits its
 * aggregation frames and hands rmnet_ipa0 one QMAP packet at a time. That is
 * what the upstream rmnet driver (ModemManager's multiplexing backend, built
 * as a module for this kernel) parses. MAPv5 checksum offload, which stock
 * Android uses, needs Qualcomm's rmnet_core.
 *
 * The ingress pipe can be connected only once per rmnet_ipa0 instance (a
 * second SET_INGRESS_DATA_FORMAT fails with the pipe already allocated);
 * rmnet_ipa re-creates the netdev at every modem start, so callers set up
 * each new instance once (luma-duo2-modem keeps the ifindex it configured).
 *
 * Usage:
 *   luma-duo2-ipa-wan [-i IFACE] endpoints
 *       print "rx_endpoint_id=N tx_endpoint_id=M", the IPA endpoints of
 *       the modem data path from the AP's side (APPS_WAN_CONS and
 *       APPS_WAN_PROD), as RMNET_IOCTL_GET_EP_PAIR reports them
 *   luma-duo2-ipa-wan [-i IFACE] setup [MUX_ID...]
 *       connect the egress and ingress pipes, then register each mux id
 *       (default 1..8)
 */
#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* include/uapi/linux/msm_rmnet.h (msm-5.4) */
#define RMNET_IOCTL_EXTENDED			0x89FD
#define RMNET_IOCTL_GET_SUPPORTED_FEATURES	0x0000
#define RMNET_IOCTL_ADD_MUX_CHANNEL		0x0005
#define RMNET_IOCTL_SET_EGRESS_DATA_FORMAT	0x0006
#define RMNET_IOCTL_SET_INGRESS_DATA_FORMAT	0x0007
#define RMNET_IOCTL_GET_EP_PAIR			0x0010

#define RMNET_IOCTL_FEAT_SET_EGRESS_DATA_FORMAT		(1 << 1)
#define RMNET_IOCTL_FEAT_SET_INGRESS_DATA_FORMAT	(1 << 2)

#define RMNET_IOCTL_EGRESS_FORMAT_MAP		(1 << 1)
#define RMNET_IOCTL_EGRESS_FORMAT_MUXING	(1 << 3)
#define RMNET_IOCTL_INGRESS_FORMAT_MAP		(1 << 1)
#define RMNET_IOCTL_INGRESS_FORMAT_DEAGGREGATION (1 << 2)
#define RMNET_IOCTL_INGRESS_FORMAT_DEMUXING	(1 << 3)

struct rmnet_ioctl_extended_s {
	uint32_t extended_ioctl;
	union {
		uint32_t data;
		int8_t if_name[IFNAMSIZ];
		struct {
			uint32_t mux_id;
			int8_t vchannel_name[IFNAMSIZ];
		} rmnet_mux_val;
		struct {
			uint32_t consumer_pipe_num;
			uint32_t producer_pipe_num;
		} ipa_ep_pair;
		struct {
			uint32_t data;
			uint32_t agg_size;
			uint32_t agg_count;
		} ingress_format;
		struct {
			int8_t if_name[IFNAMSIZ];
			uint16_t mtu_v4;
			uint16_t mtu_v6;
		} mtu_params;
		struct {
			uint32_t mux_id;
			int8_t vchannel_name[IFNAMSIZ];
			uint8_t mac[6];
		} rmnet_mux_val_v2;
	} u;
};

/* The IPA's own limit (rmnet_ipa.c MAX_NUM_OF_MUX_CHANNEL is 15). */
#define MAX_MUX 15

static const char *iface = "rmnet_ipa0";
static int sock = -1;

static int ext(uint32_t cmd, struct rmnet_ioctl_extended_s *e)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
	e->extended_ioctl = cmd;
	ifr.ifr_data = (void *)e;
	return ioctl(sock, RMNET_IOCTL_EXTENDED, &ifr);
}

static int endpoints(void)
{
	struct rmnet_ioctl_extended_s e;

	memset(&e, 0, sizeof(e));
	if (ext(RMNET_IOCTL_GET_EP_PAIR, &e) < 0) {
		fprintf(stderr, "%s: GET_EP_PAIR: %s\n", iface, strerror(errno));
		return 1;
	}
	/* rmnet_ipa's names are from the IPA's side: its producer towards the
	 * AP is APPS_WAN_CONS (the AP receives), its consumer APPS_WAN_PROD. */
	printf("rx_endpoint_id=%u tx_endpoint_id=%u\n",
	       e.u.ipa_ep_pair.producer_pipe_num,
	       e.u.ipa_ep_pair.consumer_pipe_num);
	return 0;
}

static int setup(int nmux, char **mux)
{
	struct rmnet_ioctl_extended_s e;
	int i, rc = 0;

	memset(&e, 0, sizeof(e));
	if (ext(RMNET_IOCTL_GET_SUPPORTED_FEATURES, &e) < 0) {
		fprintf(stderr, "%s: not an rmnet_ipa device: %s\n", iface,
			strerror(errno));
		return 1;
	}
	if ((e.u.data & (RMNET_IOCTL_FEAT_SET_EGRESS_DATA_FORMAT |
			 RMNET_IOCTL_FEAT_SET_INGRESS_DATA_FORMAT)) !=
	    (RMNET_IOCTL_FEAT_SET_EGRESS_DATA_FORMAT |
	     RMNET_IOCTL_FEAT_SET_INGRESS_DATA_FORMAT)) {
		fprintf(stderr, "%s: no data format ioctls (features 0x%x)\n",
			iface, e.u.data);
		return 1;
	}

	/*
	 * Egress first, as netmgrd does. With qcom,rmnet-ctl-enable in the
	 * device tree rmnet_ipa also connects the low-latency pipe, and when
	 * that fails it returns the error although the WAN pipe itself is up;
	 * only an error before the WAN pipe exists leaves the interface
	 * unusable, and the ingress step that follows would not succeed then
	 * either.
	 */
	memset(&e, 0, sizeof(e));
	e.u.data = RMNET_IOCTL_EGRESS_FORMAT_MAP | RMNET_IOCTL_EGRESS_FORMAT_MUXING;
	if (ext(RMNET_IOCTL_SET_EGRESS_DATA_FORMAT, &e) < 0)
		fprintf(stderr, "%s: SET_EGRESS_DATA_FORMAT: %s (low-latency pipe; "
			"the WAN pipe is checked by the ingress step)\n",
			iface, strerror(errno));

	memset(&e, 0, sizeof(e));
	e.u.data = RMNET_IOCTL_INGRESS_FORMAT_MAP |
		   RMNET_IOCTL_INGRESS_FORMAT_DEAGGREGATION |
		   RMNET_IOCTL_INGRESS_FORMAT_DEMUXING;
	if (ext(RMNET_IOCTL_SET_INGRESS_DATA_FORMAT, &e) < 0) {
		fprintf(stderr, "%s: SET_INGRESS_DATA_FORMAT: %s\n", iface,
			strerror(errno));
		return 1;
	}

	for (i = 0; i < (nmux ? nmux : 8); i++) {
		unsigned long id = nmux ? strtoul(mux[i], NULL, 0) : (unsigned long)i + 1;

		if (id < 1 || id > 254 || i >= MAX_MUX) {
			fprintf(stderr, "bad or too many mux ids at %s\n",
				nmux ? mux[i] : "?");
			return 2;
		}
		memset(&e, 0, sizeof(e));
		e.u.rmnet_mux_val.mux_id = id;
		snprintf((char *)e.u.rmnet_mux_val.vchannel_name, IFNAMSIZ,
			 "rmnet_data%lu", id - 1);
		if (ext(RMNET_IOCTL_ADD_MUX_CHANNEL, &e) < 0) {
			fprintf(stderr, "%s: ADD_MUX_CHANNEL %lu: %s\n", iface, id,
				strerror(errno));
			rc = 1;
		}
	}
	return rc;
}

static void usage(void)
{
	fprintf(stderr, "usage: luma-duo2-ipa-wan [-i IFACE] endpoints|setup [MUX_ID...]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "i:")) != -1) {
		if (opt == 'i')
			iface = optarg;
		else
			usage();
	}
	if (optind >= argc)
		usage();

	sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		perror("socket");
		return 1;
	}
	if (!strcmp(argv[optind], "endpoints"))
		return endpoints();
	if (!strcmp(argv[optind], "setup"))
		return setup(argc - optind - 1, argv + optind + 1);
	usage();
	return 2;
}
