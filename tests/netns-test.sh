#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2020 OpenVPN, Inc.
#
#  Author:	Antonio Quartulli <antonio@openvpn.net>

OVPN_CLI=./ovpn-cli

function create_ns() {
	ip -n peer$1 link del tun0
	ip -n peer$1 link del veth$1
	ip netns del peer$1
	ip netns add peer$1
}

function setup_ns() {
	ip link set veth$1 netns peer$1
	ip -n peer$1 addr add $2/24 dev veth$1
	ip -n peer$1 link set veth$1 up

	ip -n peer$1 link add tun0 type ovpn-dco
	ip -n peer$1 addr add $3 dev tun0
	ip -n peer$1 link set tun0 up

	ip netns exec peer$1 $OVPN_CLI tun0 start $4
	ip netns exec peer$1 $OVPN_CLI tun0 add_peer $2 $4 $5 $6
	ip netns exec peer$1 $OVPN_CLI tun0 set_key $1 data64.key
}

create_ns 0
create_ns 1

ip link del veth0
ip link add veth0 type veth peer veth1

setup_ns 0 10.10.10.1 5.5.5.1/24 1 10.10.10.2 2
setup_ns 1 10.10.10.2 5.5.5.2/24 2 10.10.10.1 1
