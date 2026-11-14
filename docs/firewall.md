# Firewall (firewalld, nftables) on the Surface Duo 2

Luma's firewall is firewalld with the nftables backend, as on Fedora. On
Microsoft's 5.4 kernel it needs nf_tables, which that kernel is built
without. It gets nf_tables as signed out-of-tree modules built from the vendor
tree's own sources. The kernel and the boot images stay unchanged.

## Pieces

| Piece | Source | Role |
|---|---|---|
| `nf_tables.ko`, `nf_tables_set.ko`, `nft_*.ko`, `nf_log_*.ko` (28 modules) | the vendor tree's `net/netfilter`, `net/ipv4/netfilter`, `net/ipv6/netfilter`, plus `kernel/netfilter/*.patch`, built by `scripts/duo2-build-netfilter-modules.sh` | nftables for firewalld and for `iptables` (Fedora's is iptables-nft) |
| `usr/lib/modules-load.d/luma-duo2-netfilter.conf` | Duo 2 overlay | loads all of them at boot (see "no module autoloading" below) |
| `etc/firewalld/zones/trusted.xml` | Duo 2 overlay, composed to `/usr/etc` | binds `usb0`, the bring-up USB link, to the `trusted` zone |
| `firewalld.service.d/50-luma-duo2.conf` | Duo 2 overlay | `ProtectHome=yes` in place of `tmpfs` |
| firewalld, nftables, iptables-nft | Fedora / Luma, unchanged | |

The modules are built for the families firewalld uses: inet, ipv4, ipv6 and
netdev. arp and bridge are not built. They cover firewalld's whole default
ruleset and the features it can turn on: zones and policies, services and
ports, rich rules, logging (`LogDenied`), rate limits, masquerade,
forward-ports, ICMP blocks, reverse-path filtering, conntrack helpers and
ipsets. Its nftables flowtable (`NftablesFlowtable`, off by default) is not
covered, since it needs `nf_flow_table`.

Build order (after the user OKs an image build): `duo2-build-vendor.sh` →
`duo2-stage-modules.sh` → `duo2-build-radio-modules.sh`,
`duo2-build-modem.sh`, `duo2-build-netfilter-modules.sh` (any order) →
`duo2-compose-ostree.sh`.

## What was wrong, and why each piece exists

**`# CONFIG_NF_TABLES is not set`.** The kernel has netfilter, conntrack, NAT
and legacy x_tables built in, as Android's netd needs, but not nf_tables.
`nft list tables` failed with "cache initialization failed: Invalid argument".
firewalld then quit ("python-nftables failed ... All hope is lost").
`iptables` failed too, since Fedora's is iptables-nft.

**struct net differs when nf_tables is configured.** In 5.4,
`include/net/net_namespace.h` puts `struct netns_nftables nft` inside
`struct net` only when `CONFIG_NF_TABLES` or `CONFIG_NF_TABLES_MODULE` is
set. Building nf_tables with the option switched on would give the modules a
different `struct net` from the built-in code: every member after `nft`
(`nf_frag`, `nfnl`, `gen`, ...) would sit at the wrong offset. That module
would load and then corrupt the namespace. Modversions would not catch it,
because the CRCs come from the kernel's `Module.symvers`. So the build leaves
the kernel's `autoconf.h` alone, and the modules see exactly the kernel's
`struct net`. Patch 0001 moves nf_tables' per-namespace state into
`net_generic()` storage, registered by the module, as upstream did in 5.13
(`nft_pernet()`). Every `net->nft.x` becomes `nft_pernet(net)->x`, and the
generation helpers in `nf_tables.h` lose their `IS_ENABLED(CONFIG_NF_TABLES)`
guard. The patched header is found ahead of the kernel's own through
`NOSTDINC_FLAGS`.

The families are switched on with `-DCONFIG_NF_TABLES_{INET,IPV4,IPV6,NETDEV}`.
Only the copied sources look at these: no header in the kernel's `include/`
tests them. The only other header conditional on nf_tables is `skb->nf_trace`
in `skbuff.h`. It is also enabled by `CONFIG_NETFILTER_XT_TARGET_TRACE=y`, so
the layout is the same either way.

**No inet NAT in the built-in NAT core.** `nf_nat_inet_register_fn()` is
compiled into `nf_nat_proto.c` only with `CONFIG_NF_TABLES_INET` and
`CONFIG_NFT_NAT`. firewalld's NAT chains (`nat_PREROUTING`, `nat_POSTROUTING`
and `nat_OUTPUT`) are in its `inet firewalld` table. Patch 0002 has
`nft_chain_nat` register an inet chain through the exported
`nf_nat_ipv{4,6}_register_fn()`, each with a copy of the chain's hook ops
whose `pf` is set to that family. That is what the missing function does with
the original ops. The copies live until the chain is unregistered, because the
NAT core finds a hook by its ops pointer.

**No module autoloading.** `CONFIG_STATIC_USERMODEHELPER` sends every
`request_module()` to Android's `/system/bin/core_collector`, which does not
exist here. So when nf_tables needs `nft-expr-ct`, `nft-chain-2-nat` and the
like, nothing loads them, and the rule fails with ENOENT ("Could not process
rule: No such file or directory"). `modules-load.d` loads all 28 modules at
boot, before firewalld starts.

**ProtectHome.** systemd could not set up `ProtectHome=tmpfs` (firewalld's
unit default) next to the read-only `/usr` on this kernel ("Failed to set up
mount namespacing: /./var/home: Invalid argument"). The drop-in uses
`ProtectHome=yes`.

**usb0 has no NetworkManager connection.** NetworkManager's own udev rules
leave USB gadget interfaces unmanaged, so the `duo2-usb` connection never
activates. No connection can carry a firewalld zone for `usb0`, so the
interface is bound to `trusted` in firewalld's configuration. The ssh link,
and the DHCP that `luma-duo2-usb-dhcp` serves the workstation, are then never
filtered. Without that binding `usb0` would fall into the default zone,
`FedoraWorkstation`. That zone still allows ssh but not DHCP to the phone.
`wlan0` and the modem's data links get the zone of their NetworkManager
connection, which is the default zone unless a connection sets one.

## Verified on the phone (2026-11-14, live, before an image build)

The modules were copied into the transient `/usr`, and firewalld was started
with a timed rollback armed (`systemd-run --on-active=150` stopping firewalld
and flushing the ruleset). The rollback was cancelled once a fresh ssh login
over `usb0` worked.

- vermagic `5.4.233-qgki-c1-p-0_0_0 SMP preempt mod_unload modversions
  aarch64`, signed with the vendor build's key; the running kernel is that
  build (`#14 ... Fri Nov 13 20:02:55 EST 2026`). modpost reported no
  unresolved symbols.
- All 28 modules load. No WARN, BUG or CFI report in dmesg throughout.
- A test table exercised `ct`, sets with intervals, `log`, `limit`,
  `reject` (TCP reset and icmpx), `fib` in prerouting, `meta mark set`,
  `redirect` and `masquerade` in inet NAT chains. It loaded, was deleted and
  was loaded twice more. On the data path, ssh to port 2222 was redirected to
  22 by the inet NAT chain and the login succeeded. A TCP connect to port 9
  got the reset from the `reject` rule. The counters matched.
- `iptables -L`, `ip6tables -L`, `iptables -t nat -L`, and a chain using
  `-m comment -m conntrack -j REJECT` (through `nft_compat`) all work.
- firewalld: `systemctl start firewalld` succeeds. `firewall-cmd --state`
  says `running`, the default zone is `FedoraWorkstation`, and `usb0` is in
  `trusted`. `nft list ruleset` shows `table inet firewalld` (533 lines).
  Runtime `--add-service`, `--add-masquerade`, a rich rule with log, limit
  and reject, `--add-forward-port`, `--set-log-denied=all` and a full
  `--reload` all succeed. After the reload, ssh over `usb0` still works.
  firewalld's table-owner probe fails on 5.4 (that flag arrived in 5.12), and
  firewalld goes on without it.
- Afterwards: NetworkManager (Wi-Fi scan sees networks), ModemManager (modem
  listed), BlueZ (adapter powered), the touch service, gdm, sshd and the USB
  DHCP server are all active.

Not yet verified across a real boot: that `modules-load.d` brings the
modules up before firewalld, and that the composed `trusted.xml` arrives in
`/etc`.

## Open

- Module autoloading would work if `/system/bin/core_collector` ran
  `modprobe`. But the static usermode helper also handles core dumps and every
  other kernel-initiated helper, so it is left alone for now.
- `usb0` in `trusted` suits the bring-up image, whose USB link is a root ssh
  link to the workstation. A user-facing image should drop that binding, or
  the gadget network, with it.
- The nftables flowtable, the arp and bridge families, `nft_tproxy`,
  `nft_queue`, `nft_osf` and `nft_synproxy` are not built. firewalld does not
  use them by default.
