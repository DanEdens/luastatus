#include "detect_iface.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <ifaddrs.h>

#include "libls/alloc_utils.h"

// Adopted from i3status/src/first_network_device.c

static
bool
is_wlan_iface(const char *iface)
{
    FILE *f = NULL;
    char *line = NULL;
    size_t nline = 0;
    bool ret = false;

    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/uevent", iface);
    if (!(f = fopen(path, "r"))) {
        goto done;
    }
    while (getline(&line, &nline, f) > 0) {
        if (strcmp(line, "DEVTYPE=wlan\n") == 0) {
            ret = true;
            goto done;
        }
    }

done:
    free(line);
    if (f) {
        fclose(f);
    }
    return ret;
}

char *
detect_wlan_iface(void)
{
    char *result = NULL;
    struct ifaddrs *ifaddr = NULL;

    if (getifaddrs(&ifaddr) < 0) {
        ifaddr = NULL;
        goto done;
    }

    for (struct ifaddrs *cur = ifaddr; cur; cur = cur->ifa_next) {
        if (is_wlan_iface(cur->ifa_name)) {
            result = ls_xstrdup(cur->ifa_name);
            goto done;
        }
    }

done:
    freeifaddrs(ifaddr);
    return result;
}