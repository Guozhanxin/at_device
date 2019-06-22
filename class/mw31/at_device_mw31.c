/*
 * File      : at_socket_mw31.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2006 - 2018, RT-Thread Development Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-06-23     flybreak     first version
 */

#include <stdio.h>
#include <string.h>

#include <at_device_mw31.h>

#if !defined(AT_SW_VERSION_NUM) || AT_SW_VERSION_NUM < 0x10300
    #error "This AT Client version is older, please check and update latest AT Client!"
#endif

#define LOG_TAG                        "at.dev"

#include <at_log.h>

#ifdef AT_DEVICE_USING_MW31

#define MW31_WAIT_CONNECT_TIME      5000
#define MW31_THREAD_STACK_SIZE      1024
#define MW31_THREAD_PRIORITY        (RT_THREAD_PRIORITY_MAX / 2)

/* =============================  mw31 network interface operations ============================= */

static void mw31_get_netdev_info(struct rt_work *work, void *work_data)
{
#define AT_ADDR_LEN     32
    at_response_t resp = RT_NULL;
    char ip[AT_ADDR_LEN] = {0}, mac[AT_ADDR_LEN] = {0};
    char gateway[AT_ADDR_LEN] = {0}, netmask[AT_ADDR_LEN] = {0};
    char dns_server1[AT_ADDR_LEN] = {0}, dns_server2[AT_ADDR_LEN] = {0};
    char dhcp_stat_buf[5] = {0};
    ip_addr_t ip_addr;
    rt_uint32_t mac_addr[6] = {0};
    rt_uint32_t num = 0;
    rt_uint8_t dhcp_stat = 0;
    struct rt_delayed_work *delay_work = (struct rt_delayed_work *)work;
    struct at_device *device = (struct at_device *)work_data;
    struct netdev *netdev = device->netdev;
    struct at_client *client = device->client;

    if (delay_work)
    {
        rt_free(delay_work);
    }

    resp = at_create_resp(512, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%d) response structure.", device->name);
        return;
    }

    /* send mac addr query commond "AT+CIFSR" and wait response */
    if (at_obj_exec_cmd(client, resp, "AT+WMAC?") < 0)
    {
        goto __exit;
    }

    if (at_resp_parse_line_args_by_kw(resp, "+WMAC:", "+WMAC:%s", mac) <= 0)
    {
        LOG_E("mw31 device(%s) parse \"AT+WMAC\" command response data error.", device->name);
        goto __exit;
    }

    /* send addr info query commond "AT+CIPSTA?" and wait response */
    if (at_obj_exec_cmd(client, resp, "AT+WJAPIP?") < 0)
    {
        LOG_E("mw31 device(%s) send \"AT+WJAPIP?\" commands error.", device->name);
        goto __exit;
    }

    if (at_resp_parse_line_args_by_kw(resp, "+WJAPIP?:", "+WJAPIP?:%[^,],%[^,],%[^,],%s", ip, netmask, gateway, dns_server1) < 0)
    {
        LOG_E("mw31 device(%s) prase \"AT+WJAPIP?\" command resposne data error.", device->name);
        goto __exit;
    }

    /* set netdev info */
    inet_aton(gateway, &ip_addr);
    netdev_low_level_set_gw(netdev, &ip_addr);
    inet_aton(netmask, &ip_addr);
    netdev_low_level_set_netmask(netdev, &ip_addr);
    inet_aton(ip, &ip_addr);
    netdev_low_level_set_ipaddr(netdev, &ip_addr);

    sscanf(mac, "%2x%2x%2x%2x%2x%2x",
           &mac_addr[0], &mac_addr[1], &mac_addr[2], &mac_addr[3], &mac_addr[4], &mac_addr[5]);
    for (num = 0; num < netdev->hwaddr_len; num++)
    {
        netdev->hwaddr[num] = mac_addr[num];
    }

    if (rt_strlen(dns_server1) > 0)
    {
        inet_aton(dns_server1, &ip_addr);
        netdev_low_level_set_dns_server(netdev, 0, &ip_addr);
    }

    if (rt_strlen(dns_server2) > 0)
    {
        inet_aton(dns_server2, &ip_addr);
        netdev_low_level_set_dns_server(netdev, 1, &ip_addr);
    }

    /* send DHCP query commond " AT+WDHCP?" and wait response */
    if (at_obj_exec_cmd(client, resp, "AT+WDHCP?") < 0)
    {
        goto __exit;
    }

    /* parse response data, get the DHCP status */
    if (at_resp_parse_line_args_by_kw(resp, "+WDHCP:", "+WDHCP:%s", dhcp_stat_buf) < 0)
    {
        LOG_E("mw31 device(%s) get DHCP status failed.", device->name);
        goto __exit;
    }

    if (rt_strstr(dhcp_stat_buf, "ON"))
    {
        dhcp_stat |= 0x03;
    }
    /* Bit0 - SoftAP DHCP status, Bit1 - Station DHCP status */
    netdev_low_level_set_dhcp_status(netdev, dhcp_stat & 0x02 ? RT_TRUE : RT_FALSE);

__exit:
    if (resp)
    {
        at_delete_resp(resp);
    }
}

static int mw31_net_init(struct at_device *device);

static int mw31_netdev_set_up(struct netdev *netdev)
{
    struct at_device *device = RT_NULL;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL)
    {
        LOG_E("get mw31 device by netdev name(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    if (device->is_init == RT_FALSE)
    {
        mw31_net_init(device);
        netdev_low_level_set_status(netdev, RT_TRUE);
        LOG_D("the network interface device(%s) set up status", netdev->name);
    }

    return RT_EOK;
}

static int mw31_netdev_set_down(struct netdev *netdev)
{
    struct at_device *device = RT_NULL;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL)
    {
        LOG_E("get mw31 device by netdev name(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    if (device->is_init == RT_TRUE)
    {
        device->is_init = RT_FALSE;
        netdev_low_level_set_status(netdev, RT_FALSE);
        LOG_D("the network interface device(%s) set down status", netdev->name);
    }

    return RT_EOK;
}

static int mw31_netdev_set_addr_info(struct netdev *netdev, ip_addr_t *ip_addr, ip_addr_t *netmask, ip_addr_t *gw)
{
#define IPADDR_RESP_SIZE       128
#define IPADDR_SIZE            16

    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device *device = RT_NULL;
    char mw31_ip_addr[IPADDR_SIZE] = {0};
    char mw31_gw_addr[IPADDR_SIZE] = {0};
    char mw31_netmask_addr[IPADDR_SIZE] = {0};

    RT_ASSERT(netdev);
    RT_ASSERT(ip_addr || netmask || gw);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL)
    {
        LOG_E("get mw31 device by netdev name(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(IPADDR_RESP_SIZE, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) response structure.", device->name);
        result = -RT_ENOMEM;
        goto __exit;
    }

    /* Convert numeric IP address into decimal dotted ASCII representation. */
    if (ip_addr)
        rt_memcpy(mw31_ip_addr, inet_ntoa(*ip_addr), IPADDR_SIZE);
    else
        rt_memcpy(mw31_ip_addr, inet_ntoa(netdev->ip_addr), IPADDR_SIZE);

    if (gw)
        rt_memcpy(mw31_gw_addr, inet_ntoa(*gw), IPADDR_SIZE);
    else
        rt_memcpy(mw31_gw_addr, inet_ntoa(netdev->gw), IPADDR_SIZE);

    if (netmask)
        rt_memcpy(mw31_netmask_addr, inet_ntoa(*netmask), IPADDR_SIZE);
    else
        rt_memcpy(mw31_netmask_addr, inet_ntoa(netdev->netmask), IPADDR_SIZE);

    /* send addr info set commond "AT+CIPSTA_CUR=<ip>[,<gateway>,<netmask>]" and wait response */
    if (at_obj_exec_cmd(device->client, resp, "AT+CIPSTA_CUR=\"%s\",\"%s\",\"%s\"",
                        mw31_ip_addr, mw31_gw_addr, mw31_netmask_addr) < 0)
    {
        LOG_E("mw31 device(%s) set address information failed.", device->name);
        result = -RT_ERROR;
    }
    else
    {
        /* Update netdev information */
        if (ip_addr)
            netdev_low_level_set_ipaddr(netdev, ip_addr);

        if (gw)
            netdev_low_level_set_gw(netdev, gw);

        if (netmask)
            netdev_low_level_set_netmask(netdev, netmask);

        LOG_D("mw31 device(%s) set address information successfully.", device->name);
    }

__exit:
    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

static int mw31_netdev_set_dns_server(struct netdev *netdev, uint8_t dns_num, ip_addr_t *dns_server)
{
#define DNS_RESP_SIZE           128

    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device *device = RT_NULL;

    RT_ASSERT(netdev);
    RT_ASSERT(dns_server);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL)
    {
        LOG_E("get mw31 device by netdev name(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(DNS_RESP_SIZE, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) response structure.", device->name);
        return -RT_ENOMEM;
    }

    /* send dns server set commond "AT+CIPDNS_CUR=<enable>[,<DNS    server0>,<DNS   server1>]" and wait response */
    if (at_obj_exec_cmd(device->client, resp, "AT+CIPDNS_CUR=1,\"%s\"", inet_ntoa(*dns_server)) < 0)
    {
        LOG_E("mw31 device(%s) set DNS server(%s) failed.", device->name, inet_ntoa(*dns_server));
        result = -RT_ERROR;
    }
    else
    {
        netdev_low_level_set_dns_server(netdev, dns_num, dns_server);
        LOG_D("mw31 device(%s) set DNS server(%s) successfully.", device->name, inet_ntoa(*dns_server));
    }

    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

static int mw31_netdev_set_dhcp(struct netdev *netdev, rt_bool_t is_enabled)
{
#define MW31_STATION     1
#define RESP_SIZE           128

    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device *device = RT_NULL;

    RT_ASSERT(netdev);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL)
    {
        LOG_E("get AT device by netdev name(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(RESP_SIZE, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) response structure.", device->name);
        return -RT_ENOMEM;
    }

    /* send dhcp set commond "AT+CWDHCP_CUR=<mode>,<en>" and wait response */
    if (at_obj_exec_cmd(device->client, resp, "AT+CWDHCP_CUR=%d,%d", MW31_STATION, is_enabled) < 0)
    {
        LOG_E("mw31 device(%s) set DHCP status(%d) failed.", device->name, is_enabled);
        result = -RT_ERROR;
        goto __exit;
    }
    else
    {
        netdev_low_level_set_dhcp_status(netdev, is_enabled);
        LOG_D("mw31 device(%d) set DHCP status(%d) successfully.", device->name, is_enabled);
    }

__exit:
    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

#ifdef NETDEV_USING_PING
static int mw31_netdev_ping(struct netdev *netdev, const char *host,
                            size_t data_len, uint32_t timeout, struct netdev_ping_resp *ping_resp)
{
#define MW31_PING_IP_SIZE         16

    rt_err_t result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device *device = RT_NULL;
    char ip_addr[MW31_PING_IP_SIZE] = {0};
    int req_time;

    RT_ASSERT(netdev);
    RT_ASSERT(host);
    RT_ASSERT(ping_resp);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL)
    {
        LOG_E("get mw31 device by netdev name(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(64, 0, timeout);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) response structure.", device->name);
        return -RT_ENOMEM;
    }

    /* send domain commond "AT+CIPDOMAIN=<domain name>" and wait response */
    if (at_obj_exec_cmd(device->client, resp, "AT+CIPDOMAIN=\"%s\"", host) < 0)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    /* parse the third line of response data, get the IP address */
    if (at_resp_parse_line_args_by_kw(resp, "+CIPDOMAIN:", "+CIPDOMAIN:%s", ip_addr) < 0)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    /* send ping commond "AT+PING=<IP>" and wait response */
    if (at_obj_exec_cmd(device->client, resp, "AT+PING=\"%s\"", host) < 0)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    if (at_resp_parse_line_args_by_kw(resp, "+", "+%d", &req_time) < 0)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    if (req_time)
    {
        inet_aton(ip_addr, &(ping_resp->ip_addr));
        ping_resp->data_len = data_len;
        ping_resp->ttl = 0;
        ping_resp->ticks = req_time;
    }

__exit:
    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}
#endif /* NETDEV_USING_PING */

#ifdef NETDEV_USING_NETSTAT
void mw31_netdev_netstat(struct netdev *netdev)
{
#define MW31_NETSTAT_RESP_SIZE         320
#define MW31_NETSTAT_TYPE_SIZE         4
#define MW31_NETSTAT_IPADDR_SIZE       17
#define MW31_NETSTAT_EXPRESSION        "+CIPSTATUS:%*d,\"%[^\"]\",\"%[^\"]\",%d,%d,%*d"

    at_response_t resp = RT_NULL;
    struct at_device *device = RT_NULL;
    int remote_port, local_port, i;
    char *type = RT_NULL;
    char *ipaddr = RT_NULL;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL)
    {
        LOG_E("get mw31 device by netdev name(%s) failed.", netdev->name);
        return;
    }

    type = (char *) rt_calloc(1, MW31_NETSTAT_TYPE_SIZE);
    ipaddr = (char *) rt_calloc(1, MW31_NETSTAT_IPADDR_SIZE);
    if ((type && ipaddr) == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) response structure.", device->name);
        goto __exit;
    }

    resp = at_create_resp(MW31_NETSTAT_RESP_SIZE, 0, 5 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) response structure.", device->name);
        goto __exit;
    }

    /* send network connection information commond "AT+CIPSTATUS" and wait response */
    if (at_obj_exec_cmd(device->client, resp, "AT+CIPSTATUS") < 0)
    {
        goto __exit;
    }

    for (i = 1; i <= resp->line_counts; i++)
    {
        if (strstr(at_resp_get_line(resp, i), "+CIPSTATUS"))
        {
            /* parse the third line of response data, get the network connection information */
            if (at_resp_parse_line_args(resp, i, MW31_NETSTAT_EXPRESSION, type, ipaddr, &remote_port, &local_port) < 0)
            {
                goto __exit;
            }
            else
            {
                LOG_RAW("%s: %s:%d ==> %s:%d\n", type, inet_ntoa(netdev->ip_addr), local_port, ipaddr, remote_port);
            }
        }
    }

__exit:
    if (resp)
    {
        at_delete_resp(resp);
    }

    if (type)
    {
        rt_free(type);
    }

    if (ipaddr)
    {
        rt_free(ipaddr);
    }
}
#endif /* NETDEV_USING_NETSTAT */

static const struct netdev_ops mw31_netdev_ops =
{
    mw31_netdev_set_up,
    mw31_netdev_set_down,

    mw31_netdev_set_addr_info,
    mw31_netdev_set_dns_server,
    mw31_netdev_set_dhcp,

#ifdef NETDEV_USING_PING
    mw31_netdev_ping,
#endif
#ifdef NETDEV_USING_NETSTAT
    mw31_netdev_netstat,
#endif
};

static struct netdev *mw31_netdev_add(const char *netdev_name)
{
#define ETHERNET_MTU        1500
#define HWADDR_LEN          6
    struct netdev *netdev = RT_NULL;

    RT_ASSERT(netdev_name);

    netdev = (struct netdev *) rt_calloc(1, sizeof(struct netdev));
    if (netdev == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) netdev structure.", netdev_name);
        return RT_NULL;
    }

    netdev->mtu = ETHERNET_MTU;
    netdev->ops = &mw31_netdev_ops;
    netdev->hwaddr_len = HWADDR_LEN;

#ifdef SAL_USING_AT
    extern int sal_at_netdev_set_pf_info(struct netdev * netdev);
    /* set the network interface socket/netdb operations */
    sal_at_netdev_set_pf_info(netdev);
#endif

    netdev_register(netdev, netdev_name, RT_NULL);

    return netdev;
}

/* =============================  mw31 device operations ============================= */

#define AT_SEND_CMD(client, resp, cmd)                                     \
    do {                                                                   \
        (resp) = at_resp_set_info((resp), 256, 0, 5 * RT_TICK_PER_SECOND); \
        if (at_obj_exec_cmd((client), (resp), (cmd)) < 0)                  \
        {                                                                  \
            result = -RT_ERROR;                                            \
            goto __exit;                                                   \
        }                                                                  \
    } while(0)                                                             \

static void mw31_netdev_start_delay_work(struct at_device *device)
{
    struct rt_delayed_work *net_work = RT_NULL;
    net_work = (struct rt_delayed_work *)rt_calloc(1, sizeof(struct rt_delayed_work));
    if (net_work == RT_NULL)
    {
        return;
    }

    rt_delayed_work_init(net_work, mw31_get_netdev_info, (void *)device);
    rt_work_submit(&(net_work->work), RT_TICK_PER_SECOND);
}

static void mw31_init_thread_entry(void *parameter)
{
#define INIT_RETRY    2

    struct at_device *device = (struct at_device *) parameter;
    struct at_device_mw31 *mw31 = (struct at_device_mw31 *) device->user_data;
    struct at_client *client = device->client;
    at_response_t resp = RT_NULL;
    rt_err_t result = RT_EOK;
    rt_size_t i = 0, retry_num = INIT_RETRY;

    LOG_D("mw31 device(%s) initialize start.", device->name);

    /* wait mw31 device startup finish */
    if (at_client_obj_wait_connect(client, MW31_WAIT_CONNECT_TIME))
    {
        return;
    }

    resp = at_create_resp(128, 0, 5 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%d) response structure.", device->name);
        return;
    }

    while (retry_num--)
    {
        /* reset module */
        AT_SEND_CMD(client, resp, "AT+REBOOT");
        /* reset waiting delay */
        rt_thread_mdelay(2000);
//        /* disable echo */
//        AT_SEND_CMD(client, resp, "AT+UARTE=0");
        /* set current mode to Wi-Fi station */
        AT_SEND_CMD(client, resp, "AT+WSAPQ");
        /* get module version */
        AT_SEND_CMD(client, resp, "AT+FWVER?");
        /* show module version */
        for (i = 0; i < resp->line_counts - 1; i++)
        {
            LOG_D("%s", at_resp_get_line(resp, i + 1));
        }

        /* connect to WiFi AP */
        if (at_obj_exec_cmd(client, at_resp_set_info(resp, 128, 0, 20 * RT_TICK_PER_SECOND),
                            "AT+WJAP=%s,%s", mw31->wifi_ssid, mw31->wifi_password) != RT_EOK)
        {
            LOG_E("AT device(%s) network initialize failed, check ssid(%s) and password(%s).",
                  device->name, mw31->wifi_ssid, mw31->wifi_password);
            result = -RT_ERROR;
            goto __exit;
        }

__exit:
        if (result == RT_EOK)
        {
            break;
        }
        else
        {
            rt_thread_mdelay(1000);
            LOG_I("mw31 device(%s) initialize retry...", device->name);
        }
    }

    if (resp)
    {
        at_delete_resp(resp);
    }

    if (result != RT_EOK)
    {
        netdev_low_level_set_status(device->netdev, RT_FALSE);
        LOG_E("mw31 device(%s) network initialize failed(%d).", device->name, result);
    }
    else
    {
        device->is_init = RT_TRUE;
        netdev_low_level_set_status(device->netdev, RT_TRUE);
        netdev_low_level_set_link_status(device->netdev, RT_TRUE);

        LOG_I("mw31 device(%s) network initialize successfully.", device->name);
    }
}

static int mw31_net_init(struct at_device *device)
{
#ifdef AT_DEVICE_MW31_INIT_ASYN
    rt_thread_t tid;

    tid = rt_thread_create("mw31_net_init", mw31_init_thread_entry, (void *) device,
                           MW31_THREAD_STACK_SIZE, MW31_THREAD_PRIORITY, 20);
    if (tid)
    {
        rt_thread_startup(tid);
    }
    else
    {
        LOG_E("create mw31 device(%s) initialize thread failed.", device->name);
        return -RT_ERROR;
    }
#else
    mw31_init_thread_entry(device);
#endif /* AT_DEVICE_MW31_INIT_ASYN */

    return RT_EOK;
}

static void urc_func(struct at_client *client, const char *data, rt_size_t size)
{
    struct at_device *device = RT_NULL;
    char *client_name = client->device->parent.name;

    RT_ASSERT(client && data && size);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_CLIENT, client_name);
    if (device == RT_NULL)
    {
        LOG_E("get mw31 device by client name(%s) failed.", client_name);
        return;
    }

    if (rt_strstr(data, "STATION_UP"))
    {
        LOG_I("mw31 device(%s) WIFI is connected.", device->name);

        if (device->is_init)
        {
            netdev_low_level_set_link_status(device->netdev, RT_TRUE);

            mw31_netdev_start_delay_work(device);
        }
    }
    else if (rt_strstr(data, "STATION_DOWN"))
    {
        LOG_I("mw31 device(%s) WIFI is disconnect.", device->name);

        if (device->is_init)
        {
            netdev_low_level_set_link_status(device->netdev, RT_FALSE);
        }
    }
}

static const struct at_urc urc_table[] =
{
    {"+WEVENT:",   "\r\n",           urc_func},
};

static int mw31_init(struct at_device *device)
{
    struct at_device_mw31 *mw31 = (struct at_device_mw31 *) device->user_data;

    /* initialize AT client */
    at_client_init(mw31->client_name, mw31->recv_line_num);

    device->client = at_client_get(mw31->client_name);
    if (device->client == RT_NULL)
    {
        LOG_E("mw31 device(%s) initialize failed, get AT client(%s) failed.",
              mw31->device_name, mw31->client_name);
        return -RT_ERROR;
    }

    /* register URC data execution function  */
    at_obj_set_urc_table(device->client, urc_table, sizeof(urc_table) / sizeof(urc_table[0]));

#ifdef AT_USING_SOCKET
    mw31_socket_init(device);
#endif

    /* add mw31 device to the netdev list */
    device->netdev = mw31_netdev_add(mw31->device_name);
    if (device->netdev == RT_NULL)
    {
        LOG_E("mw31 device(%s) initialize failed, get network interface device failed.", mw31->device_name);
        return -RT_ERROR;
    }

    /* initialize mw31 device network */
    return mw31_netdev_set_up(device->netdev);
}

static int mw31_deinit(struct at_device *device)
{
    return mw31_netdev_set_down(device->netdev);
}

/* reset eap8266 device and initialize device network again */
static int mw31_reset(struct at_device *device)
{
    int result = RT_EOK;
    struct at_client *client = device->client;

    /* send "AT+RST" commonds to mw31 device */
    result = at_obj_exec_cmd(client, RT_NULL, "AT+RST");
    rt_thread_mdelay(1000);

    /* waiting 10 seconds for mw31 device reset */
    device->is_init = RT_FALSE;
    if (at_client_obj_wait_connect(client, MW31_WAIT_CONNECT_TIME))
    {
        return -RT_ETIMEOUT;
    }

    /* initialize mw31 device network */
    mw31_net_init(device);

    device->is_init = RT_TRUE;

    return result;
}

/* change eap8266 wifi ssid and password information */
static int mw31_wifi_info_set(struct at_device *device, struct at_device_ssid_pwd *info)
{
    int result = RT_EOK;
    struct at_response *resp = RT_NULL;

    if (info->ssid == RT_NULL || info->password == RT_NULL)
    {
        LOG_E("input mw31 wifi ssid(%s) and password(%s) error.", info->ssid, info->password);
        return -RT_ERROR;
    }

    resp = at_create_resp(128, 0, 20 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for mw31 device(%s) response structure.", device->name);
        return -RT_ENOMEM;
    }

    /* connect to input wifi ap */
    if (at_obj_exec_cmd(device->client, resp, "AT+CWJAP=\"%s\",\"%s\"", info->ssid, info->password) != RT_EOK)
    {
        LOG_E("mw31 device(%s) wifi connect failed, check ssid(%s) and password(%s).",
              device->name, info->ssid, info->password);
        result = -RT_ERROR;
    }

    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

static int mw31_control(struct at_device *device, int cmd, void *arg)
{
    int result = -RT_ERROR;

    RT_ASSERT(device);

    switch (cmd)
    {
    case AT_DEVICE_CTRL_POWER_ON:
    case AT_DEVICE_CTRL_POWER_OFF:
    case AT_DEVICE_CTRL_LOW_POWER:
    case AT_DEVICE_CTRL_SLEEP:
    case AT_DEVICE_CTRL_WAKEUP:
    case AT_DEVICE_CTRL_NET_CONN:
    case AT_DEVICE_CTRL_NET_DISCONN:
    case AT_DEVICE_CTRL_GET_SIGNAL:
    case AT_DEVICE_CTRL_GET_GPS:
    case AT_DEVICE_CTRL_GET_VER:
        LOG_W("mw31 not support the control command(%d).", cmd);
        break;
    case AT_DEVICE_CTRL_RESET:
        result = mw31_reset(device);
        break;
    case AT_DEVICE_CTRL_SET_WIFI_INFO:
        result = mw31_wifi_info_set(device, (struct at_device_ssid_pwd *) arg);
        break;
    default:
        LOG_E("input error control command(%d).", cmd);
        break;
    }

    return result;
}

static const struct at_device_ops mw31_device_ops =
{
    mw31_init,
    mw31_deinit,
    mw31_control,
};

static int mw31_device_class_register(void)
{
    struct at_device_class *class = RT_NULL;

    class = (struct at_device_class *) rt_calloc(1, sizeof(struct at_device_class));
    if (class == RT_NULL)
    {
        LOG_E("no memory for mw31 device class create.");
        return -RT_ENOMEM;
    }

    /* fill MW31 device class object */
#ifdef AT_USING_SOCKET
    mw31_socket_class_register(class);
#endif
    class->device_ops = &mw31_device_ops;

    return at_device_class_register(class, AT_DEVICE_CLASS_MW31);
}
INIT_DEVICE_EXPORT(mw31_device_class_register);

#endif /* AT_DEVICE_USING_MW31 */
