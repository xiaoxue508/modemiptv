#include "uplink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>

#define UPLINK_TABLE 1001
#define UPLINK_PREF  1000

static char cur_ip[32];

const char *uplink_ip(void)
{
    return cur_ip;
}

/* device names end up in shell commands: allow only safe chars, < IFNAMSIZ */
int uplink_dev_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= IFNAMSIZ) return 0;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' ||
              s[i] == '-' || s[i] == '.'))
            return 0;
    return 1;
}

static int iface_ipv4(const char *dev, char *out, size_t n)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);
    int rc = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if (rc != 0) return -1;
    if (ifr.ifr_addr.sa_family != AF_INET) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    return inet_ntop(AF_INET, &sin->sin_addr, out, n) ? 0 : -1;
}

/* current IPv4 gateway of dev in the main table (prefer the default route) */
static int iface_gw(const char *dev, char *out, size_t n)
{
    char cmd[96];
    snprintf(cmd, sizeof cmd, "ip -4 route show dev %s 2>/dev/null", dev);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char line[256], any[32] = "", def[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, " via ");
        if (!p) continue;
        p += 4;
        while (*p == ' ') p++;
        char tok[32];
        size_t i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\n' && i < sizeof tok - 1) {
            tok[i] = p[i];
            i++;
        }
        tok[i] = 0;
        struct in_addr a;
        if (!inet_pton(AF_INET, tok, &a)) continue;
        if (!any[0]) snprintf(any, sizeof any, "%s", tok);
        if (!strncmp(line, "default", 7)) {
            snprintf(def, sizeof def, "%s", tok);
            break;
        }
    }
    pclose(f);
    const char *use = def[0] ? def : (any[0] ? any : NULL);
    if (!use) return -1;
    snprintf(out, n, "%s", use);
    return 0;
}

static int rule_matches(const char *ip)
{
    FILE *f = popen("ip rule show pref " /* UPLINK_PREF */ "1000 2>/dev/null", "r");
    if (!f) return 0;
    char line[256];
    int ok = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, ip) && strstr(line, "1001")) { ok = 1; break; }
    pclose(f);
    return ok;
}

void uplink_ensure(int log_changes)
{
    static int auto_warned;

    if (!g.upstream_interface[0]) {
        cur_ip[0] = 0;
        if (g.stbip_auto && log_changes && !auto_warned) {
            auto_warned = 1;
            logmsg("stbip=auto needs upstream_interface= to be set");
        }
        return;
    }
    if (!uplink_dev_ok(g.upstream_interface)) {
        if (log_changes) logmsg("uplink: bad interface name '%s'",
                                g.upstream_interface);
        return;
    }

    char ip[32];
    if (iface_ipv4(g.upstream_interface, ip, sizeof ip) != 0) {
        if (cur_ip[0] && log_changes)
            logmsg("uplink: %s has no IPv4 address (binding disabled)",
                   g.upstream_interface);
        cur_ip[0] = 0;
        return;
    }

    char gw[32];
    int have_gw = iface_gw(g.upstream_interface, gw, sizeof gw) == 0;
    char cmd[224];
    if (g.uplink_policy && have_gw) {
        snprintf(cmd, sizeof cmd,
                 "ip route replace %s/32 dev %s scope link table %d 2>/dev/null",
                 gw, g.upstream_interface, UPLINK_TABLE);
        system(cmd);
        snprintf(cmd, sizeof cmd,
                 "ip route replace default via %s dev %s table %d 2>/dev/null",
                 gw, g.upstream_interface, UPLINK_TABLE);
        system(cmd);
        if (!rule_matches(ip)) {
            system("ip rule del pref " /* UPLINK_PREF */ "1000 2>/dev/null");
            snprintf(cmd, sizeof cmd,
                     "ip rule add pref %d from %s lookup %d 2>/dev/null",
                     UPLINK_PREF, ip, UPLINK_TABLE);
            system(cmd);
        }
        /* platform /24s in the main table (was /etc/udhcpc.user.d/99-iptv-routes):
           LAN-origin traffic (epg.py, manual tools) has no source binding, so it
           needs destination routes or the pppoe default (metric 1) wins and the
           STB source IP is lost.  Refreshed every cycle: network reload / gateway
           change self-heals in <=60s instead of waiting for the DHCP lease. */
        static const char *const plat[] = {
            "60.212.113.0/24", "60.212.114.0/24", "60.212.115.0/24",
            "124.132.240.0/24", "119.180.21.0/24"
        };
        for (size_t i = 0; i < sizeof plat / sizeof *plat; i++) {
            snprintf(cmd, sizeof cmd,
                     "ip route replace %s via %s dev %s metric 10 2>/dev/null",
                     plat[i], gw, g.upstream_interface);
            system(cmd);
        }
    }

    int changed = strcmp(cur_ip, ip) != 0;
    if (changed && log_changes)
        logmsg("uplink: %s -> %s%s%s", g.upstream_interface, ip,
               have_gw ? " gw " : " (no gateway)",
               have_gw ? gw : "");
    snprintf(cur_ip, sizeof cur_ip, "%s", ip);

    if (g.stbip_auto && strcmp(g.stbip, ip))
        snprintf(g.stbip, sizeof g.stbip, "%s", ip);
}
