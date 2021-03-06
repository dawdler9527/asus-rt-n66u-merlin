/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 * Network services
 *
 * Copyright 2004, ASUSTeK Inc.
 * All Rights Reserved.
 * 
 * THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
 * KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
 * SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if_arp.h>
#include <signal.h>
#include <dirent.h>
#include <linux/sockios.h>
#include <bcmnvram.h>
#include <shutils.h>
#include <wlutils.h>
#include <rc.h>
#include <bcmutils.h>
#include <bcmparams.h>
#include <net/route.h>
#include <stdarg.h>

#include <linux/types.h>
#include <linux/ethtool.h>

#include "disk_io_tools.h"

#define	MAX_MAC_NUM	16
static int mac_num;
static char mac_clone[MAX_MAC_NUM][18];

void convert_wan_nvram(char *prefix, int unit);

#define WAN0_ROUTE_TABLE 100
#define WAN1_ROUTE_TABLE 200

#define TEMP_ROUTE_FILE "/tmp/route_main"

int copy_routes(int table){
	char cmd[2048];
	char *route_buf, *follow_info, line[1024];
	int len;

	if(table <= 0){
		memset(cmd, 0, 2048);
		sprintf(cmd, "ip route flush table %d", WAN0_ROUTE_TABLE);
		system(cmd);

		memset(cmd, 0, 2048);
		sprintf(cmd, "ip route flush table %d", WAN1_ROUTE_TABLE);
		system(cmd);

		return 0;
	}

	memset(cmd, 0, 2048);
	sprintf(cmd, "ip route list table main > %s", TEMP_ROUTE_FILE);
	system(cmd);

	route_buf = read_whole_file(TEMP_ROUTE_FILE);
	if(route_buf == NULL || strlen(route_buf) <= 0)
		return -1;

	follow_info = route_buf;
	while(get_line_from_buffer(follow_info, line, 1024) != NULL && strncmp(line, "default", 7) != 0){
		follow_info += strlen(line);

		len = strlen(line);
		line[len-2] = 0;

		memset(cmd, 0, 2048);
		sprintf(cmd, "ip route add table %d %s", table, line);
		system(cmd);
	}

	free(route_buf);

	return 0;
}

int add_multi_routes(){
	int unit, table;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";
	char wan_if[32], wan_ip[32], wan_gate[32];
	int gate_num, wan_state, wan_weight;
	char cmd[2048], *ptr;
	char wan_multi_if[WAN_UNIT_MAX][32], wan_multi_gate[WAN_UNIT_MAX][32];

	system("ip rule flush");
	system("ip rule add from all lookup main pref 32766");
	system("ip rule add from all lookup default pref 32767");

	// clean multi route tables and re-build them.
	copy_routes(0);

	memset(wan_multi_if, 0, sizeof(char)*WAN_UNIT_MAX*32);
	memset(wan_multi_gate, 0, sizeof(char)*WAN_UNIT_MAX*32);

	gate_num = 0;
	for(unit = WAN_UNIT_WANPORT1; unit < WAN_UNIT_MAX; ++unit){ // Multipath
		if(unit != wan_primary_ifunit()
				&& !nvram_match("modem_mode", "2")
				)
			continue;

		snprintf(prefix, sizeof(prefix), "wan%d_", unit);
		wan_state = nvram_get_int(strcat_r(prefix, "state_t", tmp));
		strncpy(wan_if, get_wan_ifname(unit), 32);
		strncpy(wan_ip, nvram_safe_get(strcat_r(prefix, "ipaddr", tmp)), 32);
		strncpy(wan_gate, nvram_safe_get(strcat_r(prefix, "gateway", tmp)), 32);

		// when wan_down().
		if(wan_state == WAN_STATE_DISCONNECTED)
			continue;

		if(strlen(wan_gate) <= 0 || !strcmp(wan_gate, "0.0.0.0"))
			continue;

		if(nvram_match("modem_mode", "2")){
			if(strlen(wan_ip) <= 0 || !strcmp(wan_ip, "0.0.0.0"))
				continue;

			if(unit == 1)
				table = WAN1_ROUTE_TABLE;
			else
				table = WAN0_ROUTE_TABLE;

			// set the rules for multi routing tables.
			memset(cmd, 0, 2048);
			sprintf(cmd, "ip rule add pref 100 from %s table %d", wan_ip, table);
			system(cmd);

			memset(cmd, 0, 2048);
			sprintf(cmd, "ip rule add pref 150 to %s table %d", wan_gate, table);
			system(cmd);

			// set the routes for multi routing tables.
			copy_routes(table);

			memset(cmd, 0, 2048);
			sprintf(cmd, "ip route replace default via %s dev %s table %d", wan_gate, wan_if, table);
			system(cmd);

			strcpy(wan_multi_if[gate_num], wan_if);
			strcpy(wan_multi_gate[gate_num], wan_gate);
			++gate_num;
		}
		else{
			// set the default gateway.
			memset(cmd, 0, 2048);
			sprintf(cmd, "ip route replace default via %s dev %s", wan_gate, wan_if);
			system(cmd);

			break;
		}
	}

	// set the multi default gateway.
	if(nvram_match("modem_mode", "2")){
		if(gate_num > 1){
			memset(cmd, 0, 2048);
			for(unit = WAN_UNIT_WANPORT1; unit < WAN_UNIT_MAX; ++unit){
				snprintf(prefix, sizeof(prefix), "wan%d_", unit);
				wan_weight = nvram_get_int(strcat_r(prefix, "weight", tmp));
				if(wan_weight > 0 && strlen(wan_multi_gate[unit]) > 0){
					if(strlen(cmd) == 0)
						strcpy(cmd, "ip route replace default equalize");

					ptr = cmd+strlen(cmd);
					sprintf(ptr, " nexthop via %s dev %s weight %d", wan_multi_gate[unit], wan_multi_if[unit], wan_weight);
				}
			}

			if(strlen(cmd) > 0)
				system(cmd);
		}
		else if(gate_num == 1){
			memset(cmd, 0, 2048);
			sprintf(cmd, "ip route replace default via %s dev %s", wan_multi_gate[0], wan_multi_if[0]);
			system(cmd);
		}
	}

	system("ip route flush cache");

	return 0;
}

int
add_routes(char *prefix, char *var, char *ifname)
{
	char word[80], *next;
	char *ipaddr, *netmask, *gateway, *metric;
	char tmp[100];

	foreach(word, nvram_safe_get(strcat_r(prefix, var, tmp)), next) {

		netmask = word;
		ipaddr = strsep(&netmask, ":");
		if (!ipaddr || !netmask)
			continue;
		gateway = netmask;
		netmask = strsep(&gateway, ":");
		if (!netmask || !gateway)
			continue;
		metric = gateway;
		gateway = strsep(&metric, ":");
		if (!gateway || !metric)
			continue;
		if (inet_addr_(gateway) == INADDR_ANY) 			// oleg patch
			gateway = nvram_safe_get(strcat_r(prefix, "xgateway", tmp));	// oleg patch

		//route_add(ifname, atoi(metric) + 1, ipaddr, gateway, netmask);
		route_add(ifname, 0, ipaddr, gateway, netmask);
	}

	return 0;
}

static void	// oleg patch , add 
add_wanx_routes(char *prefix, char *ifname, int metric)
{
	char *routes, *msroutes, *tmp;
	char buf[30];

	char ipaddr[] = "255.255.255.255";
	char gateway[] = "255.255.255.255";
	char netmask[] = "255.255.255.255";

	if (!nvram_match("dr_enable_x", "1"))
		return;

	/* routes */
	routes = strdup(nvram_safe_get(strcat_r(prefix, "routes", buf)));
	for (tmp = routes; tmp && *tmp; )
	{
		char *ipaddr = strsep(&tmp, " ");
		char *gateway = strsep(&tmp, " ");
		if (gateway) {
			route_add(ifname, metric + 1, ipaddr, gateway, netmask);
		}
	}
	free(routes);
	
	/* ms routes */
	for (msroutes = nvram_safe_get(strcat_r(prefix, "msroutes", buf)); msroutes && isdigit(*msroutes); )
	{
		/* read net length */
		int bit, bits = strtol(msroutes, &msroutes, 10);
		struct in_addr ip, gw, mask;
		
		if (bits < 1 || bits > 32 || *msroutes != ' ')
			break;
		mask.s_addr = htonl(0xffffffff << (32 - bits));

		/* read network address */
		for (ip.s_addr = 0, bit = 24; bit > (24 - bits); bit -= 8)
		{
			if (*msroutes++ != ' ' || !isdigit(*msroutes))
				goto bad_data;

			ip.s_addr |= htonl(strtol(msroutes, &msroutes, 10) << bit);
		}
		
		/* read gateway */
		for (gw.s_addr = 0, bit = 24; bit >= 0 && *msroutes; bit -= 8)
		{
			if (*msroutes++ != ' ' || !isdigit(*msroutes))
				goto bad_data;

			gw.s_addr |= htonl(strtol(msroutes, &msroutes, 10) << bit);
		}
		
		/* clear bits per RFC */
		ip.s_addr &= mask.s_addr;
		
		strcpy(ipaddr, inet_ntoa(ip));
		strcpy(gateway, inet_ntoa(gw));
		strcpy(netmask, inet_ntoa(mask));
		
		route_add(ifname, metric + 1, ipaddr, gateway, netmask);
		
		if (*msroutes == ' ')
			msroutes++;
	}
bad_data:
	return;
}

int
del_routes(char *prefix, char *var, char *ifname)
{
	char word[80], *next;
	char *ipaddr, *netmask, *gateway, *metric;
	char tmp[100];

	foreach(word, nvram_safe_get(strcat_r(prefix, var, tmp)), next) {
		_dprintf("%s: %s\n", __FUNCTION__, word);
		
		netmask = word;
		ipaddr = strsep(&netmask, ":");
		if (!ipaddr || !netmask)
			continue;
		gateway = netmask;
		netmask = strsep(&gateway, ":");
		if (!netmask || !gateway)
			continue;

		metric = gateway;
		gateway = strsep(&metric, ":");
		if (!gateway || !metric)
			continue;

		if (inet_addr_(gateway) == INADDR_ANY) 	// oleg patch
			gateway = nvram_safe_get("wan0_xgateway");

		route_del(ifname, atoi(metric) + 1, ipaddr, gateway, netmask);
	}

	return 0;
}

void	// oleg patch , add
start_igmpproxy(char *wan_ifname)
{
	static char *igmpproxy_conf = "/tmp/igmpproxy.conf";
	FILE *fp;
	char *altnet = nvram_safe_get("mr_altnet_x");

	if (pids("udpxy")) killall_tk("udpxy");
	if (pids("igmpproxy")) killall_tk("igmpproxy");

	if (nvram_get_int("udpxy_enable_x")) {
		_dprintf("start udpxy [%s]\n", wan_ifname);
		eval("/usr/sbin/udpxy",
			"-m", wan_ifname,
			"-p", nvram_get("udpxy_enable_x"),
			"-a", nvram_get("lan_ifname") ? : "br0");
	}

	if (!nvram_match("mr_enable_x", "1"))
		return;

	_dprintf("start igmpproxy [%s]\n", wan_ifname);

	if ((fp = fopen(igmpproxy_conf, "w")) == NULL) {
		perror(igmpproxy_conf);
		return;
	}

	fprintf(fp, "# automagically generated from web settings\n"
		"quickleave\n\n"
		"phyint %s upstream  ratelimit 0  threshold 1\n"
		"\taltnet %s\n\n"
		"phyint %s downstream  ratelimit 0  threshold 1\n\n",
		wan_ifname,
		*altnet ? altnet : "0.0.0.0/0",
		nvram_get("lan_ifname") ? : "br0");

	fclose(fp);

	eval("/usr/sbin/igmpproxy", igmpproxy_conf);
}

int
wan_prefix(char *ifname, char *prefix)
{
	int unit;

	if ((unit = wan_ifunit(ifname)) < 0 &&
	    (unit = wanx_ifunit(ifname)) < 0)
		return -1;

	sprintf(prefix, "wan%d_", unit);

	return unit;
}

static int
add_wan_routes(char *wan_ifname)
{
	char prefix[] = "wanXXXXXXXXXX_";

	/* Figure out nvram variable name prefix for this i/f */
	if (wan_prefix(wan_ifname, prefix) < 0)
		return -1;

	return add_routes(prefix, "route", wan_ifname);
}

static int
del_wan_routes(char *wan_ifname)
{
	char prefix[] = "wanXXXXXXXXXX_";

	/* Figure out nvram variable name prefix for this i/f */
	if (wan_prefix(wan_ifname, prefix) < 0)
#if 0
		return -1;
#else
		sprintf(prefix, "wan%d_", WAN_UNIT_WANPORT1);
#endif

	return del_routes(prefix, "route", wan_ifname);
}

#ifdef QOS
int enable_qos()
{
#if defined (W7_LOGO) || defined (WIFI_LOGO)
	return 0;
#endif
	int qos_userspec_app_en = 0;
	int rulenum = atoi(nvram_safe_get("qos_rulenum_x")), idx_class = 0;

	/* Add class for User specify, 10:20(high), 10:40(middle), 10:60(low)*/
	if (rulenum) {
		for (idx_class=0; idx_class < rulenum; idx_class++)
		{
			if (atoi(Ch_conv("qos_prio_x", idx_class)) == 1)
			{
				qos_userspec_app_en = 1;
				break;
			}
			else if (atoi(Ch_conv("qos_prio_x", idx_class)) == 6)
			{
				qos_userspec_app_en = 1;
				break;
			}
		}
	}

	if (	(nvram_match("qos_tos_prio", "1") ||
		 nvram_match("qos_pshack_prio", "1") ||
		 nvram_match("qos_service_enable", "1") ||
		 nvram_match("qos_shortpkt_prio", "1")	) ||
		(!nvram_match("qos_manual_ubw","0") && !nvram_match("qos_manual_ubw","")) ||
		(rulenum && qos_userspec_app_en)
	)
	{
		fprintf(stderr, "found QoS rulues\n");
		return 1;
	}
	else
	{
		fprintf(stderr, "no QoS rulues\n");
		return 0;
	}
}
#endif

/*
 * (1) wan[x]_ipaddr_x/wan[x]_netmask_x/wan[x]_gateway_x/...:
 *    static ip or ip get from dhcp
 *
 * (2) wan[x]_xipaddr/wan[x]_xnetmaskwan[x]_xgateway/...:
 *    ip get from dhcp when proto = l2tp/pptp/pppoe
 *
 * (3) wan[x]_ipaddr/wan[x]_netmask/wan[x]_gateway/...:
 *    always keeps the latest updated ip/netmask/gateway in system
 *      static: it is the same as (1)
 *      dhcp: 
 *      	- before getting ip from dhcp server, it is 0.0.0.0
 *      	- after getting ip from dhcp server, it is updated
 *      l2tp/pptp/pppoe with static ip:
 *      	- before getting ip from vpn server, it is the same as (1)
 *      	- after getting ip from vpn server, it is the one from vpn server
 *      l2tp/pptp/pppoe with dhcp ip:
 *      	- before getting ip from dhcp server, it is 0.0.0.0
 *      	- before getting ip from vpn server, it is the one from vpn server
 */

void update_wan_state(char *prefix, int state, int reason)
{
	char tmp[100], tmp1[100], *ptr;

	_dprintf("%s(%s, %d, %d)\n", __FUNCTION__, prefix, state, reason);

	nvram_set_int(strcat_r(prefix, "state_t", tmp), state);
	nvram_set_int(strcat_r(prefix, "sbstate_t", tmp), 0);

	// 20110610, reset auxstate each time state is changed
	nvram_set_int(strcat_r(prefix, "auxstate_t", tmp), 0);

	if (state == WAN_STATE_INITIALIZING)
	{
		nvram_set(strcat_r(prefix, "proto_t", tmp), nvram_safe_get(strcat_r(prefix, "proto", tmp1)));

		/* reset wanX_* variables */
		if (nvram_match(strcat_r(prefix, "dhcpenable_x", tmp), "0")) {
			nvram_set(strcat_r(prefix, "ipaddr", tmp), nvram_safe_get(strcat_r(prefix, "ipaddr_x", tmp1)));
			nvram_set(strcat_r(prefix, "netmask", tmp), nvram_safe_get(strcat_r(prefix, "netmask_x", tmp1)));
			nvram_set(strcat_r(prefix, "gateway", tmp), nvram_safe_get(strcat_r(prefix, "gateway_x", tmp1)));
		}
		else {
			nvram_set(strcat_r(prefix, "ipaddr", tmp), "0.0.0.0");
			nvram_set(strcat_r(prefix, "netmask", tmp), "0.0.0.0");
			nvram_set(strcat_r(prefix, "gateway", tmp), "0.0.0.0");
		}

		strcpy(tmp1, "");
		if (nvram_match(strcat_r(prefix, "dnsenable_x", tmp), "0")) {
			ptr = nvram_safe_get(strcat_r(prefix, "dns1_x", tmp));
			if (ptr && *ptr && strcmp(ptr, "0.0.0.0"))
				sprintf(tmp1, "%s", ptr);
			ptr = nvram_safe_get(strcat_r(prefix, "dns2_x", tmp));
			if (ptr && *ptr && strcmp(ptr, "0.0.0.0"))
				sprintf(tmp1 + strlen(tmp1), "%s%s", strlen(tmp1) ? " " : "", ptr);
		}
		nvram_set(strcat_r(prefix, "dns", tmp), tmp1);

		/* reset wanX_x* VPN variables */
		nvram_set(strcat_r(prefix, "xipaddr", tmp), nvram_safe_get(strcat_r(prefix, "ipaddr", tmp1)));
		nvram_set(strcat_r(prefix, "xnetmask", tmp), nvram_safe_get(strcat_r(prefix, "netmask", tmp1)));
		nvram_set(strcat_r(prefix, "xgateway", tmp), nvram_safe_get(strcat_r(prefix, "gateway", tmp1)));
		nvram_set(strcat_r(prefix, "xdns", tmp), nvram_safe_get(strcat_r(prefix, "dns", tmp1)));
	
#ifdef RTCONFIG_IPV6
		nvram_set(strcat_r(prefix, "6rd_ip4size", tmp), "");
		nvram_set(strcat_r(prefix, "6rd_router", tmp), "");
		nvram_set(strcat_r(prefix, "6rd_prefix", tmp), "");
		nvram_set(strcat_r(prefix, "6rd_prefixlen", tmp), "");
#endif
	}
	else if (state == WAN_STATE_STOPPED) {
		// Save Stopped Reason
		// keep ip info if it is stopped from connected
		nvram_set_int(strcat_r(prefix, "sbstate_t", tmp), reason);
	}

#ifdef WEB_REDIRECT
	// notify the change to wanduck
	kill_pidfile_s("/var/run/wanduck.pid", SIGUSR1);
#endif
}

#ifdef RTCONFIG_IPV6
// for one ipv6 in current stage
void update_wan6_state(char *prefix, int state, int reason)
{
	char tmp[100], tmp1[100], *ptr;

	_dprintf("%s(%s, %d, %d)\n", __FUNCTION__, prefix, state, reason);

	nvram_set_int(strcat_r(prefix, "state_t", tmp), state);
	nvram_set_int(strcat_r(prefix, "sbstate_t", tmp), 0);

	if (state == WAN_STATE_INITIALIZING)
	{
	}
	else if (state == WAN_STATE_STOPPED) {
		// Save Stopped Reason
		// keep ip info if it is stopped from connected
		nvram_set_int(strcat_r(prefix, "sbstate_t", tmp), reason);
	}
}
#endif

void
start_wan_if(int unit)
{
	char *wan_ifname;
	char *wan_proto;
	char tmp[100], tmp1[100], prefix[] = "wanXXXXXXXXXX_";
	char eabuf[32];
	int s;
	struct ifreq ifr;
	pid_t pid;
	FILE *fp;
	int retry, lock;

	_dprintf("%s(%d)\n", __FUNCTION__, unit);
	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	if(nvram_match("modem_mode", "0") && unit != wan_primary_ifunit()){
		_dprintf("%s(%d): Not primary interface.\n", __FUNCTION__, unit);
		return;
	}

	/* variable should exist? */
	if (nvram_match(strcat_r(prefix, "enable", tmp), "0")) {
		update_wan_state(prefix, WAN_STATE_DISABLED, 0);
		return;
	}

	update_wan_state(prefix, WAN_STATE_INITIALIZING, 0);

#ifdef RTCONFIG_USB_MODEM
	if(unit == WAN_UNIT_USBPORT){
		if(strcmp(nvram_safe_get("usb_path1"), "modem") && strcmp(nvram_safe_get("usb_path2"), "modem"))
			return;

		retry = 0;
		while((lock = file_lock("3g")) == -1 && retry < MAX_WAIT_FILE)
			sleep(1);
		if(lock == -1)
			return;
TRACE_PT("3g begin.\n");

		if((fp = fopen(PPP_CONF_FOR_3G, "r")) != NULL){
			fclose(fp);

			//system("pppd call 3g");
			char *pppd_argv[] = { "/usr/sbin/pppd", "call", "3g", "nodetach", "nochecktime", NULL};
			char pppd_pid[8];
			int orig_pid = nvram_get_int(strcat_r(prefix, "pppd_pid", tmp));
			int wait_time = 0;
			if(orig_pid > 1){
				kill(orig_pid, SIGHUP);
				sleep(1);
				while(check_process_exist(orig_pid) && wait_time < MAX_WAIT_FILE){
TRACE_PT("kill 3g's pppd.\n");
					++wait_time;
					kill(orig_pid, SIGTERM);
					sleep(1);
				}

				if(check_process_exist(orig_pid)){
					kill(orig_pid, SIGKILL);
					sleep(1);
				}
			}

			if(!nvram_match("stop_conn_3g", "1")){
				_eval(pppd_argv, NULL, 0, &pid);

				memset(pppd_pid, 0, 8);
				sprintf(pppd_pid, "%d", pid);
				nvram_set(strcat_r(prefix, "pppd_pid", tmp), pppd_pid);

				update_wan_state(prefix, WAN_STATE_CONNECTING, 0);
			}
			else
TRACE_PT("stop_conn_3g was set.\n");
		}

TRACE_PT("3g end.\n");
		file_unlock(lock);
		return;
	}
#endif

	convert_wan_nvram(prefix, unit);

	/* make sure the connection exists and is enabled */ 
	wan_ifname = nvram_get(strcat_r(prefix, "ifname", tmp));
	if (!wan_ifname)
		return;

	wan_proto = nvram_get(strcat_r(prefix, "proto", tmp));
	if (!wan_proto || !strcmp(wan_proto, "disabled")) {
		update_wan_state(prefix, WAN_STATE_DISABLED, 0);
		return;
	}

	/* Set i/f hardware address before bringing it up */
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
		update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_SYSTEM_ERR);
		return;
	}

	strncpy(ifr.ifr_name, wan_ifname, IFNAMSIZ);

	/* Since WAN interface may be already turned up (by vlan.c),
		if WAN hardware address is specified (and different than the current one),
		we need to make it down for synchronizing hwaddr. */
	if (ioctl(s, SIOCGIFHWADDR, &ifr)) {
		close(s);
		update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_SYSTEM_ERR);
		return;
	}

	ether_atoe(nvram_safe_get(strcat_r(prefix, "hwaddr", tmp)), eabuf);

	if ((bcmp(eabuf, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN)))
	{
		/* current hardware address is different than user specified */
		ifconfig(wan_ifname, 0, NULL, NULL);
	}

	/* Configure i/f only once, specially for wireless i/f shared by multiple connections */
	if (ioctl(s, SIOCGIFFLAGS, &ifr)) {
		close(s);
		update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_SYSTEM_ERR);
		return;
	}

	if (!(ifr.ifr_flags & IFF_UP)) {
		fprintf(stderr, "** wan_ifname: %s is NOT UP\n", wan_ifname);

		/* Sync connection nvram address and i/f hardware address */
		memset(ifr.ifr_hwaddr.sa_data, 0, ETHER_ADDR_LEN);

		if (nvram_match(strcat_r(prefix, "hwaddr", tmp), "") ||
		    !ether_atoe(nvram_safe_get(strcat_r(prefix, "hwaddr", tmp)), ifr.ifr_hwaddr.sa_data) ||
		    !memcmp(ifr.ifr_hwaddr.sa_data, "\0\0\0\0\0\0", ETHER_ADDR_LEN)) {
			if (ioctl(s, SIOCGIFHWADDR, &ifr)) {
				fprintf(stderr, "ioctl fail. continue\n");
				close(s);
				update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_SYSTEM_ERR);
				return;
			}
			nvram_set(strcat_r(prefix, "hwaddr", tmp), ether_etoa(ifr.ifr_hwaddr.sa_data, eabuf));
		}
		else {
			ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
			ioctl(s, SIOCSIFHWADDR, &ifr);
		}

		/* Bring up i/f */
		ifconfig(wan_ifname, IFUP, NULL, NULL);
	}
	close(s);

	if(unit == wan_primary_ifunit())
		start_pppoe_relay(wan_ifname);

	enable_ip_forward();

	int timeout = 5;

	/* 
	* Configure PPPoE connection. The PPPoE client will run 
	* ip-up/ip-down scripts upon link's connect/disconnect.
	*/
	if (strcmp(wan_proto, "pppoe") == 0 ||
	    strcmp(wan_proto, "pptp") == 0 ||
	    strcmp(wan_proto, "l2tp") == 0) 	// oleg patch
	{
		int demand = nvram_get_int(strcat_r(prefix, "pppoe_idletime", tmp)) &&
			     strcmp(wan_proto, "l2tp");	/* L2TP does not support idling */

		/* update demand option */
		nvram_set(strcat_r(prefix, "pppoe_demand", tmp), demand ? "1" : "0");

		/* launch dhcp client and wait for lease forawhile */
		if (nvram_match(strcat_r(prefix, "dhcpenable_x", tmp), "1")) 
		{
			char *wan_hostname = nvram_get(strcat_r(prefix, "hostname", tmp));
/* TODO: wan_hostname -> computer_name -> productid ? it's optional for dhcp */ 
/*		if (!(wan_hostname && *wan_hostname  && is_valid_hostname(wan_hostname)))
			wan_hostname = nvram_safe_get("computer_name");
*/
			char *dhcp_argv[] = { "udhcpc",
				"-i", wan_ifname,
				"-p", (sprintf(tmp, "/var/run/udhcpc%d.pid", unit), tmp),
				"-s", "/tmp/udhcpc",
				"-b",
				wan_hostname && *wan_hostname ? "-H" : NULL,
				wan_hostname && *wan_hostname ? wan_hostname : NULL,
				NULL};

			// not dhcp+vpn for switch_wantag!none, 
			// TODO: better handle could be a UI option for dhcp+vpn?
			if(nvram_match("switch_wantag", "none") && nvram_match(strcat_r(prefix, "vpndhcp", tmp1), "1")) {
				/* Start dhcp daemon */
				_eval(dhcp_argv, NULL, 0, NULL);
			}
		}
		else {
/* TODO: remake it as macro */
			if ((inet_network(nvram_safe_get(strcat_r(prefix, "xipaddr", tmp))) &
			     inet_network(nvram_safe_get(strcat_r(prefix, "xnetmask", tmp)))) ==
			    (inet_network(nvram_safe_get("lan_ipaddr")) &
			     inet_network(nvram_safe_get("lan_netmask")))) {
				update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_INVALID_IPADDR);
				return;
			}

			/* Bring up WAN interface */
			ifconfig(wan_ifname, IFUP, 
				nvram_safe_get(strcat_r(prefix, "xipaddr", tmp)),
				nvram_safe_get(strcat_r(prefix, "xnetmask", tmp)));

			/* start firewall */
// TODO: handle different lan_ifname
			start_firewall(unit, 0);

			/* setup static wan routes via physical device */
			add_routes("wan_", "route", wan_ifname);

			/* and set default route if specified with metric 1 */
			if (inet_addr_(nvram_safe_get(strcat_r(prefix, "xgateway", tmp))) &&
			    !nvram_match(strcat_r(prefix, "heartbeat_x", tmp), ""))
				route_add(wan_ifname, 2, "0.0.0.0",
					nvram_safe_get(strcat_r(prefix, "xgateway", tmp)), "0.0.0.0");

			/* start multicast router on Static+VPN physical interface */
			start_igmpproxy(wan_ifname);
		
			/* update resolv.conf */
			add_ns(wan_ifname);
		}

		/* launch pppoe client daemon */
		start_pppd(prefix);

		/* ppp interface name is referenced from this point on */
		wan_ifname = nvram_safe_get(strcat_r(prefix, "pppoe_ifname", tmp));

		/* Pretend that the WAN interface is up */
		if (nvram_match(strcat_r(prefix, "pppoe_demand", tmp), "1")) 
		{
			timeout = 5;
			/* Wait for pppx to be created */
			while (ifconfig(wan_ifname, IFUP, NULL, NULL) && timeout--)
				sleep(1);

			/* Retrieve IP info */
			if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
				update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_SYSTEM_ERR);
				return;
			}
			strncpy(ifr.ifr_name, wan_ifname, IFNAMSIZ);

			/* Set temporary IP address */
			if (ioctl(s, SIOCGIFADDR, &ifr))
				perror(wan_ifname);

			nvram_set(strcat_r(prefix, "ipaddr", tmp), inet_ntoa(sin_addr(&ifr.ifr_addr)));
			nvram_set(strcat_r(prefix, "netmask", tmp), "255.255.255.255");

			/* Set temporary P-t-P address */
			if (ioctl(s, SIOCGIFDSTADDR, &ifr))
					perror(wan_ifname);
				nvram_set(strcat_r(prefix, "gateway", tmp), inet_ntoa(sin_addr(&ifr.ifr_dstaddr)));

			close(s);

			/* 
			* Preset routes so that traffic can be sent to proper pppx even before 
			* the link is brought up.
			*/

			preset_wan_routes(wan_ifname);
		}

		update_wan_state(prefix, WAN_STATE_CONNECTING, 0);
	}
	/* 
	* Configure DHCP connection. The DHCP client will run 
	* 'udhcpc bound'/'udhcpc deconfig' upon finishing IP address 
	* renew and release.
	*/
	else if (strcmp(wan_proto, "dhcp") == 0) {
		char *wan_hostname = nvram_get(strcat_r(prefix, "hostname", tmp));
/* TODO: wan_hostname -> computer_name -> productid ? it's optional for dhcp */ 
/*		if (!(wan_hostname && *wan_hostname  && is_valid_hostname(wan_hostname)))
			wan_hostname = nvram_safe_get("computer_name");
*/
		char *dhcp_argv[] = { "udhcpc",
			"-i", wan_ifname,
			"-p", (sprintf(tmp, "/var/run/udhcpc%d.pid", unit), tmp),
			"-s", "/tmp/udhcpc",
			wan_hostname && *wan_hostname ? "-H" : NULL,
			wan_hostname && *wan_hostname ? wan_hostname : NULL,
			NULL};

		/* Start pre-authenticator */
		if (start_auth(unit, 0) == 0) {
			update_wan_state(prefix, WAN_STATE_CONNECTING, 0);
		}

		/* Start dhcp daemon */
		_eval(dhcp_argv, NULL, 0, &pid);

		update_wan_state(prefix, WAN_STATE_CONNECTING, 0);
	}
	/* Configure static IP connection. */
	else if (strcmp(wan_proto, "static") == 0) {
/* TODO: remake it as macro */
		if ((inet_network(nvram_safe_get(strcat_r(prefix, "ipaddr", tmp))) &
		     inet_network(nvram_safe_get(strcat_r(prefix, "netmask", tmp)))) ==
		    (inet_network(nvram_safe_get("lan_ipaddr")) &
		     inet_network(nvram_safe_get("lan_netmask")))) {
			update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_INVALID_IPADDR);
			return;
		}

		/* Assign static IP address to i/f */
		ifconfig(wan_ifname, IFUP,
			nvram_safe_get(strcat_r(prefix, "ipaddr", tmp)), 
			nvram_safe_get(strcat_r(prefix, "netmask", tmp)));

		/* Start pre-authenticator */
		if (start_auth(unit, 0) == 0) {
			update_wan_state(prefix, WAN_STATE_CONNECTING, 0);
		}

		/* We are done configuration */
		wan_up(wan_ifname);
	}

	_dprintf("%s(): %s %s\n", __FUNCTION__,
		nvram_safe_get(strcat_r(prefix, "ipaddr", tmp)),
		nvram_safe_get(strcat_r(prefix, "netmask", tmp)));
}

void
stop_wan_if(int unit)
{
	char *wan_ifname;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";
	char *wan_proto, active_proto[32];
	int pppd_pid = 0;

	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	/* make sure the connection exists and is enabled */ 
	wan_ifname = nvram_get(strcat_r(prefix, "ifname", tmp));
	if (!wan_ifname)
		return;

	/* Backup active wan_proto for later restore, if it have been updated by ui */
	wan_proto = nvram_safe_get(strcat_r(prefix, "proto", tmp));
	strncpy(active_proto, wan_proto, sizeof(active_proto));

	/* Set previous wan_proto as active */
	wan_proto = nvram_safe_get(strcat_r(prefix, "proto_t", tmp));
	if (*wan_proto && strcmp(active_proto, wan_proto) != 0)
	{
		_dprintf("%s %sproto_t=%s\n", __FUNCTION__, prefix, wan_proto);
		nvram_set(strcat_r(prefix, "proto", tmp), wan_proto);
		nvram_unset(strcat_r(prefix, "proto_t", tmp));
	}

	// Handel for each interface
	if(unit == wan_primary_ifunit()){
		killall_tk("stats");
		killall_tk("ntpclient");

#ifdef RTCONFIG_IPV6
		stop_wan6();
#endif

		/* Shutdown and kill all possible tasks */
#if 0
		killall_tk("ip-up");
		killall_tk("ip-down");
#ifdef RTCONFIG_IPV6
		killall_tk("ipv6-up");
		killall_tk("ipv6-down");
#endif
#endif
		killall_tk("igmpproxy");	// oleg patch
	}

	pppd_pid = nvram_get_int(strcat_r(prefix, "pppd_pid", tmp));
	int wait_time = 0;
	if(pppd_pid > 1){
		kill(pppd_pid, SIGHUP);
		sleep(1);
		while(check_process_exist(pppd_pid) && wait_time < MAX_WAIT_FILE){
TRACE_PT("kill pppd.\n");
			++wait_time;
			kill(pppd_pid, SIGTERM);
			sleep(1);
		}

		if(check_process_exist(pppd_pid)){
			kill(pppd_pid, SIGKILL);
			sleep(1);
		}

		nvram_set(strcat_r(prefix, "pppd_pid", tmp), "");
	}

	if(unit == WAN_UNIT_WANPORT1){
		killall_tk("l2tpd");	// oleg patch

		if (pids("udhcpc")) {
			logmessage("stop_wan()", "perform DHCP release");
			killall("udhcpc", SIGUSR2);
		}
		killall("udhcpc", SIGTERM);

	}

	/* Stop pre-authenticator */
	stop_auth(unit, 0);

	/* Bring down WAN interfaces */
	// Does it have to?
	// ifconfig(wan_ifname, 0, NULL, NULL);
	if(!strncmp(wan_ifname, "eth", 3) || !strncmp(wan_ifname, "vlan", 4))
		ifconfig(wan_ifname, IFUP, "0.0.0.0", NULL);

	update_wan_state(prefix, WAN_STATE_STOPPED, WAN_STOPPED_REASON_NONE);

	// wait for release finished ?
	sleep(2);

	/* Restore active wan_proto value */
	_dprintf("%s %sproto=%s\n", __FUNCTION__, prefix, active_proto);
	nvram_set(strcat_r(prefix, "proto", tmp), active_proto);
}

#if 0
// TODO: handle different wan interface
int
update_resolvconf(char *wan_ifname, int up)
{
	FILE *fp;
	char tmp1[32], tmp2[32], prefix[] = "wanXXXXXXXXXX_";
	char word[256], *next;
	int lock;
	int unit;
	char *wan_dns;

	unit = wan_primary_ifunit();
	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	if (nvram_match(strcat_r(prefix, "proto", tmp2), "static") ||
		!nvram_match(strcat_r(prefix, "dnsenable_x", tmp1), "1"))
		return 0;

	lock = file_lock("resolv");

	if (!(fp = fopen("/tmp/resolv.conf", "w+"))) {
		file_unlock(lock);
		perror("/tmp/resolv.conf");
		return errno;
	}

	wan_dns = nvram_safe_get(strcat_r(prefix, "dns", tmp1));
	foreach(word, (strlen(wan_dns) ? wan_dns : nvram_safe_get(strcat_r(prefix, "xdns", tmp2))), next)
		fprintf(fp, "nameserver %s\n", word);

	fclose(fp);

	eval("touch", "/tmp/resolv.conf");
	unlink("/etc/resolv.conf");
	symlink("/tmp/resolv.conf", "/etc/resolv.conf");

	file_unlock(lock);

#ifdef RTCONFIG_DNSMASQ
        restart_dnsmasq();
#else
	restart_dns();
#endif

	return 0;
}

#else
int
add_ns(char *wan_ifname)
{
	FILE *fp;
	char tmp[32], tmp2[32], prefix[] = "wanXXXXXXXXXX_";
	char word[100], *next;
	char line[100];
	int lock, unit;
	char *wanx_dns;
	char *wanx_xdns;

	/* Figure out nvram variable name prefix for this i/f */
	unit = wan_primary_ifunit();
	if (wan_prefix(wan_ifname, prefix) < 0 ||
	    (strcmp(wan_ifname, get_wan_ifname(unit)) &&
	     strcmp(wan_ifname, get_wanx_ifname(unit))))
		return -1;

//	if (nvram_match(strcat_r(prefix, "proto", tmp), "static") ||
//		!nvram_match(strcat_r(prefix, "dnsenable_x", tmp), "1"))
//		return 0;

	lock = file_lock("resolv");

	/* Open resolv.conf to read */
	if (!(fp = fopen("/tmp/resolv.conf", "w+"))) { //r+ -> w+
		file_unlock(lock);
		perror("/tmp/resolv.conf");
		return errno;
	}

	wanx_dns = nvram_safe_get(strcat_r(prefix, "dns", tmp));
	wanx_xdns = nvram_safe_get(strcat_r(prefix, "xdns", tmp2));
//	dbG("wan_ifname: %s, wan_proto: %s, wanx_dns: %s(%d), wanx_xdns: %s(%d)\n", wan_ifname, nvram_safe_get(strcat_r(prefix, "proto", tmp)), wanx_dns, strlen(wanx_dns), wanx_xdns, strlen(wanx_xdns));

	/* Append only those not in the original list */
//	foreach(word, nvram_safe_get(strcat_r(prefix, "dns", tmp)), next) {
        foreach(word, (strlen(wanx_dns) ? wanx_dns : wanx_xdns), next) {
		fseek(fp, 0, SEEK_SET);
		while (fgets(line, sizeof(line), fp)) {
			char *token = strtok(line, " \t\n");

			if (!token || strcmp(token, "nameserver") != 0)
				continue;
			if (!(token = strtok(NULL, " \t\n")))
				continue;

			if (!strcmp(token, word))
				break;
		}
		if (feof(fp))
		{
			fprintf(fp, "nameserver %s\n", word);
//			dbG("add dns %s for i/f %s\n", word, wan_ifname);
		}
	}
	fclose(fp);

	eval("touch", "/tmp/resolv.conf");
	chmod("/tmp/resolv.conf", 0666);
	unlink("/etc/resolv.conf");
	symlink("/tmp/resolv.conf", "/etc/resolv.conf");

	file_unlock(lock);

	/* notify dnsmasq */
	kill_pidfile_s("/var/run/dnsmasq.pid", SIGHUP);
	return 0;
}

int
del_ns(char *wan_ifname)
{
	FILE *fp, *fp2;
	char tmp[32], tmp2[32], prefix[] = "wanXXXXXXXXXX_";
	char word[100], *next;
	char line[100];
	int lock, unit;
	char *wanx_dns;
	char *wanx_xdns;
	int match;

	unit = wan_primary_ifunit();
	if (strcmp(wan_ifname, get_wan_ifname(unit)) &&
	    strcmp(wan_ifname, get_wanx_ifname(unit)))
		return -1;

	/* Figure out nvram variable name prefix for this i/f */
	if (wan_prefix(wan_ifname, prefix) < 0)
/* ???? what is it */
#if 0
		return -1;
#else
		sprintf(prefix, "wan%d_", WAN_UNIT_WANPORT1);
#endif

	lock = file_lock("resolv");

	/* Open resolv.conf to read */
	if (!(fp = fopen("/tmp/resolv.conf", "r"))) {
		file_unlock(lock);
		perror("fopen /tmp/resolv.conf");
		return errno;
	}
	/* Open resolv.tmp to save updated name server list */
	if (!(fp2 = fopen("/tmp/resolv.tmp", "w"))) {
		fclose(fp);
		file_unlock(lock);
		perror("fopen /tmp/resolv.tmp");
		return errno;
	}

	wanx_dns = nvram_safe_get(strcat_r(prefix, "dns", tmp));
	wanx_xdns = nvram_safe_get(strcat_r(prefix, "xdns", tmp2));
//	dbG("wan_ifname: %s, wan_proto: %s, wanx_dns: %s(%d), wanx_xdns: %s(%d)\n", wan_ifname, nvram_safe_get(strcat_r(prefix, "proto", tmp)), wanx_dns, strlen(wanx_dns), wanx_xdns, strlen(wanx_xdns));

	/* Copy updated name servers */
	if (strlen(wanx_dns) || strlen(wanx_xdns))
	while (fgets(line, sizeof(line), fp)) {
		char *token = strtok(line, " \t\n");

		if (!token || strcmp(token, "nameserver") != 0)
			continue;
		if (!(token = strtok(NULL, " \t\n")))
			continue;

		match = 0;
//		foreach(word, nvram_safe_get(strcat_r(prefix, "dns", tmp)), next)
		foreach(word, (strlen(wanx_dns) ? wanx_dns : wanx_xdns), next)
			if (!strcmp(word, token))
			{
				match = 1;
				break;
			}

		if (!match && !next)
		{
			fprintf(fp2, "nameserver %s\n", token);
//			dbG("store dns %s for i/f %s\n", token, wan_ifname);
		}
	}
	fclose(fp);
	fclose(fp2);

	/* Use updated file as resolv.conf */
	unlink("/tmp/resolv.conf");
	rename("/tmp/resolv.tmp", "/tmp/resolv.conf");
	eval("touch", "/tmp/resolv.conf");
	unlink("/etc/resolv.conf");
	symlink("/tmp/resolv.conf", "/etc/resolv.conf");

	file_unlock(lock);

	/* notify dnsmasq */
	kill_pidfile_s("/var/run/dnsmasq.pid", SIGHUP);
	return 0;
}
#endif

int
dump_ns()
{
	FILE *fp;
	char line[100];
	int i = 0;

	if (!(fp = fopen("/tmp/resolv.conf", "r+"))) {
		perror("/tmp/resolv.conf");
		return errno;
	}

	fseek(fp, 0, SEEK_SET);
	while (fgets(line, sizeof(line), fp)) {
		char *token = strtok(line, " \t\n");

		if (!token || strcmp(token, "nameserver") != 0)
			continue;
		if (!(token = strtok(NULL, " \t\n")))
			continue;
		dbG("current name server: %s\n", token);
		i++;
	}
	fclose(fp);
	if (!i) dbG("current name server: %s\n", "NULL");

	return 0;
}

#ifdef RTCONFIG_IPV6
void wan6_up(const char *wan_ifname)
{
	struct in_addr addr4;
	struct in6_addr addr;
	static char addr6[INET6_ADDRSTRLEN];

	int service = get_ipv6_service();

	if (service != IPV6_DISABLED) {
		if ((nvram_get_int("ipv6_accept_ra") & 1) != 0)
			accept_ra(wan_ifname);
	}

	switch (service) {
	case IPV6_NATIVE:
		eval("ip", "route", "add", "::/0", "dev", (char *)wan_ifname, "metric", "2048");
		break;
	case IPV6_NATIVE_DHCP:
		eval("ip", "route", "add", "::/0", "dev", (char *)wan_ifname);
		stop_dhcp6c();
		start_dhcp6c();
		break;
	case IPV6_6TO4:
	case IPV6_6IN4:
	case IPV6_6RD:
		stop_ipv6_tunnel();
		if (service == IPV6_6TO4) {
			addr4.s_addr = 0;
			memset(&addr, 0, sizeof(addr));
			inet_aton(get_wanip(), &addr4);
			addr.s6_addr16[0] = htons(0x2002);
			ipv6_mapaddr4(&addr, 16, &addr4, 0);
			addr.s6_addr16[3] = htons(0x0001);
			inet_ntop(AF_INET6, &addr, addr6, sizeof(addr6));
			nvram_set("ipv6_prefix", addr6);
		}
		else if (service == IPV6_6RD) {
			addr4.s_addr = 0;
			memset(&addr, 0, sizeof(addr));
			inet_aton(get_wanip(), &addr4);
#if 0
			addr.s6_addr16[0] = htons(0x2001);
			addr.s6_addr16[1] = htons(0x55c);
#else
			inet_pton(AF_INET6, nvram_safe_get("ipv6_6rd_prefix"), &addr);
#endif
			ipv6_mapaddr4(&addr, 32, &addr4, nvram_get_int("ipv6_6rd_ip4size"));
			inet_ntop(AF_INET6, &addr, addr6, sizeof(addr6));
			nvram_set("ipv6_prefix", addr6);
		}
		start_ipv6_tunnel();
		// FIXME: give it a few seconds for DAD completion
		sleep(2);
		break;
	}
}

void wan6_down(const char *wan_ifname)
{
	stop_ipv6_tunnel();
	stop_dhcp6c();
	nvram_set("ipv6_get_dns", "");
}

void start_wan6(void)
{
	// support wan_unit=0 first
	// call wan6_up directly	
	wan6_up(get_wan6face());
}

void stop_wan6(void)
{
	// support wan_unit=0 first
	// call wan6_down directly
	wan6_down(get_wan6face());
}

#endif

void
wan_up(char *wan_ifname)	// oleg patch, replace
{
	char tmp[100];
	char prefix[] = "wanXXXXXXXXXX_";
	char prefix_x[] = "wanXXXXXXXXX_";
	char *wan_proto, *gateway;
	int wan_unit;

	_dprintf("%s(%s)\n", __FUNCTION__, wan_ifname);

	/* Figure out nvram variable name prefix for this i/f */
	if ((wan_unit = wan_ifunit(wan_ifname)) < 0)
	{
		/* called for dhcp+ppp */
		if ((wan_unit = wanx_ifunit(wan_ifname)) < 0)
			return;

		snprintf(prefix, sizeof(prefix), "wan%d_", wan_unit);
		snprintf(prefix_x, sizeof(prefix_x), "wan%d_x", wan_unit);

		start_firewall(wan_unit, 0);
		
		/* setup static wan routes via physical device */
		add_routes(prefix, "mroute", wan_ifname);

		/* and one supplied via DHCP */
		add_wanx_routes(prefix_x, wan_ifname, 0);

		gateway = nvram_safe_get(strcat_r(prefix_x, "gateway", tmp));

		/* and default route with metric 1 */
		if (inet_addr_(gateway) != INADDR_ANY)
		{
			char word[100], *next;

			route_add(wan_ifname, 2, "0.0.0.0", gateway, "0.0.0.0");

			/* ... and to dns servers as well for demand ppp to work */
			if (nvram_match(strcat_r(prefix, "dnsenable_x", tmp),"1"))
				foreach(word, nvram_safe_get(strcat_r(prefix_x, "dns", tmp)), next)
			{
				in_addr_t mask = inet_addr(nvram_safe_get(strcat_r(prefix_x, "netmask", tmp)));
				if ((inet_addr(word) & mask) != (inet_addr(nvram_safe_get(strcat_r(prefix_x, "ipaddr", tmp))) & mask))
					route_add(wan_ifname, 2, word, gateway, "255.255.255.255");
			}
		}

		/* start multicast router on DHCP+VPN physical interface */
		start_igmpproxy(wan_ifname);
#if 0
		update_resolvconf(wan_ifname, 1);
#else
		add_ns(wan_ifname);
#endif
		return;
	}

	snprintf(prefix, sizeof(prefix), "wan%d_", wan_unit);

	add_routes(prefix, "mroute", wan_ifname);
	wan_proto = nvram_safe_get(strcat_r(prefix, "proto", tmp));

	/* Set default route to gateway if specified */
	if (strcmp(wan_proto, "dhcp") == 0 || strcmp(wan_proto, "static") == 0)
	{
		/* the gateway is in the local network */
		route_add(wan_ifname, 0, nvram_safe_get(strcat_r(prefix, "gateway", tmp)),
			NULL, "255.255.255.255");
	}

	/* default route via default gateway */
	add_multi_routes();

	/* hack: avoid routing cycles, when both peer and server has the same IP */
	if (strcmp(wan_proto, "pptp") == 0 || strcmp(wan_proto, "l2tp") == 0) {
		/* delete gateway route as it's no longer needed */
		route_del(wan_ifname, 0, nvram_safe_get(strcat_r(prefix, "gateway", tmp)),
			"0.0.0.0", "255.255.255.255");
	}

	/* Install interface dependent static routes */
	add_wan_routes(wan_ifname);

	/* setup static wan routes via physical device     */
	/* for other cases, MAN route has been added above */
	if (strcmp(wan_proto, "dhcp") == 0 || strcmp(wan_proto, "static") == 0)
	{
		nvram_set("wan_xgateway", nvram_safe_get(strcat_r(prefix, "gateway", tmp)));
		add_routes("wan_", "route", wan_ifname);
	}

	/* and one supplied via DHCP */
	if (strcmp(wan_proto, "dhcp") == 0)
		add_wanx_routes(prefix, wan_ifname, 0);

	/* Add dns servers to resolv.conf */
#if 0
	update_resolvconf(wan_ifname, 1);
#else
	add_ns(wan_ifname);
#endif

	/* Set connected state */
	update_wan_state(prefix, WAN_STATE_CONNECTED, 0);

	// TODO: handle different lan_ifname?
	start_firewall(wan_unit, 0);
	//start_firewall(wan_ifname, nvram_safe_get(strcat_r(prefix, "ipaddr", tmp)),
	//	nvram_safe_get("lan_ifname"), nvram_safe_get("lan_ipaddr"));

	/* Start post-authenticator */
	if (start_auth(wan_unit, 1) == 0) {
		update_wan_state(prefix, WAN_STATE_CONNECTING, 0);
	}

	if(wan_unit != wan_primary_ifunit())
		return;

	/* start multicast router when not VPN */
	if (strcmp(wan_proto, "dhcp") == 0 ||
	    strcmp(wan_proto, "static") == 0)
	{
		start_igmpproxy(wan_ifname);
	}

#ifdef RTCONFIG_IPV6
	wan6_up(get_wan6face());
#endif

	//add_iQosRules(wan_ifname);
	start_iQos();

	stop_upnp();
	start_upnp();

	stop_ddns();
	start_ddns();

	/* Sync time */
	refresh_ntpc();

#if defined(RTCONFIG_APP_PREINSTALLED) || defined(RTCONFIG_APP_NETINSTALLED)
	char *apps_update = { "app_update.sh", NULL };
	int pid;

	if(test_if_dir("/opt/lib/ipkg")){
_dprintf("wan_up: update the APP's lists...\n");
		_eval(apps_update, NULL, 0, &pid);
	}
#endif
}

void
wan_down(char *wan_ifname)
{
	int wan_unit;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";
	char *wan_proto;

	_dprintf("%s(%s)\n", __FUNCTION__, wan_ifname);


	/* dhcp/static under vpn is skipped */
	if ((wan_unit = wan_ifunit(wan_ifname)) < 0)
		return;

	/* Figure out nvram variable name prefix for this i/f */
	if(wan_prefix(wan_ifname, prefix) < 0)
		return;

	wan_proto = nvram_safe_get(strcat_r(prefix, "proto", tmp));

	/* Stop post-authenticator */
	stop_auth(wan_unit, 1);

	/* Remove default route to gateway if specified */
	if(wan_unit == wan_primary_ifunit())
		route_del(wan_ifname, 0, "0.0.0.0", 
			nvram_safe_get(strcat_r(prefix, "gateway", tmp)),
			"0.0.0.0");

	/* Remove interface dependent static routes */
	del_wan_routes(wan_ifname);

	/* Update resolv.conf
	 * Leave as is if no dns servers left for demand to work */
#if 0
	if (*nvram_safe_get(strcat_r(prefix, "xdns", tmp)))
		nvram_unset(strcat_r(prefix, "dns", tmp));
	update_resolvconf(wan_ifname, 0);
#else
	del_ns(wan_ifname);

	if (nvram_match(strcat_r(prefix, "dnsenable_x", tmp), "1") &&
		strlen(nvram_safe_get(strcat_r(prefix, "xdns", tmp)))) 
		nvram_unset(strcat_r(prefix, "dns", tmp));
#endif

	if (strcmp(wan_proto, "static") == 0)
		ifconfig(wan_ifname, IFUP, NULL, NULL);

	update_wan_state(prefix, WAN_STATE_DISCONNECTED, WAN_STOPPED_REASON_NONE);

	if(nvram_match("modem_mode", "2"))
		add_multi_routes();
}

int
wan_ifunit(char *wan_ifname)
{
	int unit;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";

	if ((unit = ppp_ifunit(wan_ifname)) >= 0) {
		return unit;
	} else {
		for (unit = WAN_UNIT_WANPORT1; unit < WAN_UNIT_MAX; unit ++) {
			snprintf(prefix, sizeof(prefix), "wan%d_", unit);
			if (nvram_match(strcat_r(prefix, "ifname", tmp), wan_ifname) &&
			    (nvram_match(strcat_r(prefix, "proto", tmp), "dhcp") ||
			     nvram_match(strcat_r(prefix, "proto", tmp), "static")))
				return unit;
		}
	}

	return -1;
}

int
wanx_ifunit(char *wan_ifname)
{
	int unit;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";

	for (unit = WAN_UNIT_WANPORT1; unit < WAN_UNIT_MAX; unit ++) {
		snprintf(prefix, sizeof(prefix), "wan%d_", unit);

		if (nvram_match(strcat_r(prefix, "ifname", tmp), wan_ifname) &&
		    (nvram_match(strcat_r(prefix, "proto", tmp), "pppoe") ||
		     nvram_match(strcat_r(prefix, "proto", tmp), "pptp") ||
		     nvram_match(strcat_r(prefix, "proto", tmp), "l2tp")))
			return unit;
	}
	return -1;
}

int
preset_wan_routes(char *wan_ifname)
{
	int unit = -1;

	if((unit = wan_ifunit(wan_ifname)) < 0)
		if((unit = wanx_ifunit(wan_ifname)) < 0)
			return -1;

	/* Set default route to gateway if specified */
	if(unit == wan_primary_ifunit())
		route_add(wan_ifname, 0, "0.0.0.0", "0.0.0.0", "0.0.0.0");

	/* Install interface dependent static routes */
	add_wan_routes(wan_ifname);
	return 0;
}

char *
get_lan_ipaddr()
{
	int s;
	struct ifreq ifr;
	struct sockaddr_in *inaddr;
	struct in_addr ip_addr;

	/* Retrieve IP info */
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
#if 0
		return strdup("0.0.0.0");
#else
	{
		memset(&ip_addr, 0x0, sizeof(ip_addr));        
		return inet_ntoa(ip_addr);
	}
#endif

	strncpy(ifr.ifr_name, "br0", IFNAMSIZ);
	inaddr = (struct sockaddr_in *)&ifr.ifr_addr;
	inet_aton("0.0.0.0", &inaddr->sin_addr);	

	/* Get IP address */
	ioctl(s, SIOCGIFADDR, &ifr);
	close(s);	

	ip_addr = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr;
//	fprintf(stderr, "current LAN IP address: %s\n", inet_ntoa(ip_addr));
	return inet_ntoa(ip_addr);
}

int
ppp0_as_default_route()
{
	int i, n, found;
	FILE *f;
	unsigned int dest, mask;
	char buf[256], device[256];

	n = 0;
	found = 0;
	mask = 0;
	device[0] = '\0';

	if (f = fopen("/proc/net/route", "r"))
	{
		while (fgets(buf, sizeof(buf), f) != NULL)
		{
			if (++n == 1 && strncmp(buf, "Iface", 5) == 0)
				continue;

			i = sscanf(buf, "%255s %x %*s %*s %*s %*s %*s %x",
						device, &dest, &mask);

			if (i != 3)
				break;

			if (device[0] != '\0' && dest == 0 && mask == 0)
			{
				found = 1;
				break;
			}
		}

		fclose(f);

		if (found && !strcmp("ppp0", device))
			return 1;
		else
			return 0;
	}

	return 0;
}

int
found_default_route()
{
	int i, n, found;
	FILE *f;
	unsigned int dest, mask;
	char buf[256], device[256];
	char *wanif;
	int wan_unit = wan_primary_ifunit();
        
	n = 0;
	found = 0;
	mask = 0;
	device[0] = '\0';

	if (f = fopen("/proc/net/route", "r"))
	{
		while (fgets(buf, sizeof(buf), f) != NULL)
		{
			if (++n == 1 && strncmp(buf, "Iface", 5) == 0)
				continue;

			i = sscanf(buf, "%255s %x %*s %*s %*s %*s %*s %x",
						device, &dest, &mask);

			if (i != 3)
			{
//				fprintf(stderr, "junk in buffer");
				break;
			}

			if (device[0] != '\0' && dest == 0 && mask == 0)
			{
//				fprintf(stderr, "default route dev: %s\n", device);
				found = 1;
				break;
			}
		}

		fclose(f);

		wanif = get_wan_ifname(wan_unit);

		if (found && !strcmp(wanif, device))
		{
//			fprintf(stderr, "got default route!\n");
			return 1;
		}
		else
		{
			fprintf(stderr, "no default route!\n");
			return 0;
		}
	}

	fprintf(stderr, "no default route!!!\n");
	return 0;
}

long print_num_of_connections()
{
	char buf[256];
	char entries[16], others[256];
	long num_of_entries;

	FILE *fp = fopen("/proc/net/stat/nf_conntrack", "r");
	if (!fp) {
		fprintf(stderr, "no connection!\n");
		return;
	}

	fgets(buf, 256, fp);
	fgets(buf, 256, fp);
	fclose(fp);

	memset(entries, 0x0, 16);
	sscanf(buf, "%s %s", entries, others);
	num_of_entries = strtoul(entries, NULL, 16);

	fprintf(stderr, "connection count: %ld\n", num_of_entries);
	return num_of_entries;
}


#ifdef RTCONFIG_DSL
void init_dsl_before_start_wan(void)
{
	// eth2 could not start up in original initialize routine
	// it must put eth2 start-up code here
	dbG("enable eth2 and power up all LAN ports\n");
	eval("ifconfig", "eth2", "up"); 			
	eval("8367r", "14");				
	eval("brctl", "addif", "br0", "eth2.2");
	start_dsl();
}
#endif

void
start_wan(void)
{
	int unit;

	if (!is_routing_enabled())
		return;

	/* Create links */
	mkdir("/tmp/ppp", 0777);
	mkdir("/tmp/ppp/peers", 0777);
	symlink("/sbin/rc", "/tmp/ppp/ip-up");
	symlink("/sbin/rc", "/tmp/ppp/ip-down");
#ifdef RTCONFIG_IPV6
	symlink("/sbin/rc", "/tmp/ppp/ipv6-up");
	symlink("/sbin/rc", "/tmp/ppp/ipv6-down");
#endif
	symlink("/sbin/rc", "/tmp/udhcpc");
#ifdef RTCONFIG_EAPOL
	symlink("/sbin/rc", "/tmp/wpa_cli");
#endif
//	symlink("/dev/null", "/tmp/ppp/connect-errors");

	/* Start each configured and enabled wan connection and its undelying i/f */
	for(unit = WAN_UNIT_WANPORT1; unit < WAN_UNIT_MAX; ++unit)
		start_wan_if(unit);

	/* Report stats */
	if (!nvram_match("stats_server", "")) {
		char *stats_argv[] = { "stats", nvram_safe_get("stats_server"), NULL };
		_eval(stats_argv, NULL, 5, NULL);
	}
}

void
stop_wan(void)
{
	int unit;

	if (!is_routing_enabled())
		return;

	/* Start each configured and enabled wan connection and its undelying i/f */
	for(unit = WAN_UNIT_WANPORT1; unit < WAN_UNIT_MAX; ++unit)
		stop_wan_if(unit);

	/* Remove dynamically created links */
#ifdef RTCONFIG_EAPOL
	unlink("/tmp/wpa_cli");
#endif
	unlink("/tmp/udhcpc");
	unlink("/tmp/ppp/ip-up");
	unlink("/tmp/ppp/ip-down");
	rmdir("/tmp/ppp");
}

void convert_wan_nvram(char *prefix, int unit)
{
	char tmp[100];
	char macbuf[32];

	_dprintf("%s(%s)\n", __FUNCTION__, prefix);

	// setup hwaddr			
	strcpy(macbuf, nvram_safe_get(strcat_r(prefix, "hwaddr_x", tmp)));
	if (strlen(macbuf)!=0 && strcasecmp(macbuf, "FF:FF:FF:FF:FF:FF"))
		nvram_set(strcat_r(prefix, "hwaddr", tmp), macbuf);
#ifdef CONFIG_BCMWL5
	else nvram_set(strcat_r(prefix, "hwaddr", tmp), nvram_safe_get("et0macaddr"));
#elif defined RTCONFIG_RALINK
	else nvram_set(strcat_r(prefix, "hwaddr", tmp), nvram_safe_get("et1macaddr"));
#endif

	// sync proto
	if(nvram_match(strcat_r(prefix, "proto", tmp), "static")) {	
		nvram_set(strcat_r(prefix, "dhcpenable_x", tmp), "0");
	}
	// backlink unit for ppp
	nvram_set_int(strcat_r(prefix, "unit", tmp), unit);
}

void dumparptable()
{
	char buf[256];
	char ip_entry[32], hw_type[8], flags[8], hw_address[32], mask[32], device[8];
	char *macbuf;

	FILE *fp = fopen("/proc/net/arp", "r");
	if (!fp) {
		fprintf(stderr, "no proc fs mounted!\n");
		return;
	}

	mac_num = 0;

	while (fgets(buf, 256, fp) && (mac_num < MAX_MAC_NUM - 2)) {
		sscanf(buf, "%s %s %s %s %s %s", ip_entry, hw_type, flags, hw_address, mask, device);

		if (!strcmp(device, "br0") && strlen(hw_address)!=0)
		{
			strcpy(mac_clone[mac_num++], hw_address);
		}
	}
	fclose(fp);

	strcpy(macbuf, nvram_safe_get("wan0_hwaddr_x"));

	// try pre-set mac 	
	if (strlen(macbuf)!=0 && strcasecmp(macbuf, "FF:FF:FF:FF:FF:FF"))
		strcpy(mac_clone[mac_num++], macbuf);
	
	// try original mac
	strcpy(mac_clone[mac_num++], nvram_safe_get("et0macaddr"));
	
	if (mac_num)
	{
		fprintf(stderr, "num of mac: %d\n", mac_num);
		int i;
		for (i = 0; i < mac_num; i++)
			fprintf(stderr, "mac to clone: %s\n", mac_clone[i]);
	}
}

int
autodet_main(int argc, char *argv[])
{
	int i;
	int unit=0; // need to handle multiple wan
	char prefix[]="wanXXXXXX_", tmp[100];
	char hwaddr_x[32];
	int status;

	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	nvram_set_int("autodet_state", AUTODET_STATE_INITIALIZING);	
	nvram_set_int("autodet_auxstate", AUTODET_STATE_INITIALIZING);

	
	// it shouldnot happen, because it is only called in default mode
	if (!nvram_match(strcat_r(prefix, "proto", tmp), "dhcp")) {
		nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_NODHCP);
		return;
	}

	if (!get_wanports_status()) {
		nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_NOLINK);
		return;
	}

	if (get_wan_state(unit)==WAN_STATE_CONNECTED)
	{
		i = nvram_get_int(strcat_r(prefix, "lease", tmp));

		if(i<60&&is_private_subnet(strcat_r(prefix, "ipaddr", tmp))) {
			sleep(i);
		}
		//else {
		//	nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_OK);
		//	return;
		//}
	}

 	status = discover_all();
	
	// check for pppoe status only, 
	if (get_wan_state(unit)==WAN_STATE_CONNECTED) {
		nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_OK);
		if(status==2)
			nvram_set_int("autodet_auxstate", AUTODET_STATE_FINISHED_WITHPPPOE);
		return;
	}
	else if(status==2) {
		nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_WITHPPPOE);
		return;
	}

	nvram_set_int("autodet_state", AUTODET_STATE_CHECKING);

	dumparptable();

	// backup hwaddr_x
	strcpy(hwaddr_x, nvram_safe_get(strcat_r(prefix, "hwaddr_x", tmp)));
	//nvram_set(strcat_r(prefix, "hwaddr_x", tmp), "");

	i=0;

	while(i<mac_num) {
		_dprintf("try clone %s\n", mac_clone[i]);
		
		nvram_set(strcat_r(prefix, "hwaddr_x", tmp), mac_clone[i]);
		notify_rc_and_wait("restart_wan");
		sleep(5);
		if(get_wan_state(unit)==WAN_STATE_CONNECTED)
			break;
		i++;
	}

	// restore hwaddr_x
	nvram_set(strcat_r(prefix, "hwaddr_x", tmp), hwaddr_x);

	if(i==mac_num) {
		nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_FAIL);
	}
	else if(i==mac_num-1) { // OK in original mac
		nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_OK);
	}
	else // OK in cloned mac
	{
		nvram_set_int("autodet_state", AUTODET_STATE_FINISHED_OK);
		nvram_set(strcat_r(prefix, "hwaddr_x", tmp), mac_clone[i]);
		nvram_commit();
	}
}

