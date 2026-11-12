#!/bin/sh
# Isolated three-namespace path for one qualification run:
#
#   sender ns  --veth--  router ns  --veth--  receiver ns
#   10.B.1.2            10.B.1.1 | 10.B.2.1            10.B.2.2
#
# where B = 100 + ID. netem is attached inside the router namespace only:
# on the interface toward the receiver for the forward direction and on the
# interface toward the sender for the reverse direction.
#
#   topology.sh up ID      create the namespaces and links of run ID (0-99)
#   topology.sh down ID    remove them
#
# Everything is created inside new namespaces named rwq<ID>s, rwq<ID>x and
# rwq<ID>r, and "down" deletes exactly those. The caller's own namespace,
# its routes and its qdiscs are never touched. Needs CAP_NET_ADMIN and
# CAP_SYS_ADMIN: run it in a privileged, disposable container.
set -eu
cmd=${1:?up or down}
id=${2:?run id 0-99}
case "$id" in *[!0-9]*) echo "run id must be a number" >&2; exit 64;; esac
[ "$id" -le 99 ] || { echo "run id must be 0-99" >&2; exit 64; }
s=rwq${id}s; x=rwq${id}x; r=rwq${id}r
b=$((100 + id))

down() {
    for ns in "$s" "$x" "$r"; do
        ip netns del "$ns" 2>/dev/null || true # links inside go with it
    done
}

case "$cmd" in
up)
    down
    ip netns add "$s"; ip netns add "$x"; ip netns add "$r"
    ip link add s0 netns "$s" type veth peer name xs netns "$x"
    ip link add r0 netns "$r" type veth peer name xr netns "$x"
    ip -n "$s" addr add "10.$b.1.2/24" dev s0
    ip -n "$x" addr add "10.$b.1.1/24" dev xs
    ip -n "$r" addr add "10.$b.2.2/24" dev r0
    ip -n "$x" addr add "10.$b.2.1/24" dev xr
    for ns in "$s" "$x" "$r"; do ip -n "$ns" link set lo up; done
    ip -n "$s" link set s0 up; ip -n "$x" link set xs up
    ip -n "$r" link set r0 up; ip -n "$x" link set xr up
    ip -n "$s" route add default via "10.$b.1.1"
    ip -n "$r" route add default via "10.$b.2.1"
    ip netns exec "$x" sysctl -qw net.ipv4.ip_forward=1
    # Large socket buffers are allowed inside the endpoints, as on a tuned host.
    for ns in "$s" "$r"; do
        ip netns exec "$ns" sysctl -qw net.core.rmem_max=16777216 net.core.wmem_max=16777216 2>/dev/null || true
    done
    echo "sender=10.$b.1.2 receiver=10.$b.2.2 router_ns=$x forward_dev=xr reverse_dev=xs"
    ;;
down) down ;;
*) echo "usage: topology.sh up|down ID" >&2; exit 64 ;;
esac
