/*
 * File      : at_socket_rw007.c
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
 * 2018-06-20     chenyong     first version
 * 2019-05-12     chenyong     multi AT socket client support
 */

#include <stdio.h>
#include <string.h>

#include <at_device_rw007.h>

#if !defined(AT_SW_VERSION_NUM) || AT_SW_VERSION_NUM < 0x10300
#error "This AT Client version is older, please check and update latest AT Client!"
#endif

#define LOG_TAG                       "at.skt"
#include <at_log.h>

#if defined(AT_DEVICE_USING_RW007) && defined(AT_USING_SOCKET)

#define RW007_MODULE_SEND_MAX_SIZE     2048
/* set real event by current socket and current state */
#define SET_EVENT(socket, event)       (((socket + 1) << 16) | (event))

/* AT socket event type */
#define RW007_EVENT_CONN_OK            (1L << 0)
#define RW007_EVENT_SEND_OK            (1L << 1)
#define RW007_EVENT_RECV_OK            (1L << 2)
#define RW007_EVNET_CLOSE_OK           (1L << 3)
#define RW007_EVENT_CONN_FAIL          (1L << 4)
#define RW007_EVENT_SEND_FAIL          (1L << 5)

static at_evt_cb_t at_evt_cb_set[] = {
        [AT_SOCKET_EVT_RECV] = NULL,
        [AT_SOCKET_EVT_CLOSED] = NULL,
};

static int rw007_socket_event_send(struct at_device *device, uint32_t event)
{
    return (int) rt_event_send(device->socket_event, event);
}

static int rw007_socket_event_recv(struct at_device *device, uint32_t event, uint32_t timeout, rt_uint8_t option)
{
    int result = RT_EOK;
    rt_uint32_t recved;

    result = rt_event_recv(device->socket_event, event, option | RT_EVENT_FLAG_CLEAR, timeout, &recved);
    if (result != RT_EOK)
    {
        return -RT_ETIMEOUT;
    }

    return recved;
}

/**
 * close socket by AT commands.
 *
 * @param current socket
 *
 * @return  0: close socket success
 *         -1: send AT commands error
 *         -2: wait socket event timeout
 *         -5: no memory
 */
static int rw007_socket_close(struct at_socket *socket)
{
    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    int device_socket = (int) socket->user_data;
    struct at_device *device = (struct at_device *) socket->device;

    resp = at_create_resp(64, 0, RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for rw007 device(%s) response structure.", device->name);
        return -RT_ENOMEM;
    }

    result = at_obj_exec_cmd(device->client, resp, "AT+CIPCLOSE=%d", device_socket);

    if (resp)
    {
        at_delete_resp(resp);
    }
    
    return result;
}

/**
 * create TCP/UDP client or server connect by AT commands.
 *
 * @param socket current socket
 * @param ip server or client IP address
 * @param port server or client port
 * @param type connect socket type(tcp, udp)
 * @param is_client connection is client
 *
 * @return   0: connect success
 *          -1: connect failed, send commands error or type error
 *          -2: wait socket event timeout
 *          -5: no memory
 */
static int rw007_socket_connect(struct at_socket *socket, char *ip, int32_t port, enum at_socket_type type, rt_bool_t is_client)
{
    int result = RT_EOK;
    rt_bool_t retryed = RT_FALSE;
    at_response_t resp = RT_NULL;
    int device_socket = (int) socket->user_data;
    struct at_device *device = (struct at_device *) socket->device;

    RT_ASSERT(ip);
    RT_ASSERT(port >= 0);

    resp = at_create_resp(128, 0, 5 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for rw007 device(%s) response structure.", device->name);
        return -RT_ENOMEM;
    }

__retry:
    if (is_client)
    {
        switch (type)
        {
        case AT_SOCKET_TCP:
            /* send AT commands to connect TCP server */
            if (at_obj_exec_cmd(device->client, resp, 
                    "AT+CIPSTART=%d,\"TCP\",\"%s\",%d,60", device_socket, ip, port) < 0)
            {
                result = -RT_ERROR;
            }
            break;

        case AT_SOCKET_UDP:
            if (at_obj_exec_cmd(device->client, resp, 
                    "AT+CIPSTART=%d,\"UDP\",\"%s\",%d", device_socket, ip, port) < 0)
            {
                result = -RT_ERROR;
            }
            break;

        default:
            LOG_E("rw007 device(%s) not supported connect type %d.", device->name, type);
            result = -RT_ERROR;
            goto __exit;
        }
    }

    if (result != RT_EOK && retryed == RT_FALSE)
    {
        LOG_D("rw007 device(%s) socket (%d) connect failed, maybe the socket was not be closed at the last time and now will retry.", 
                device->name, socket);
        if (rw007_socket_close(socket) < 0)
        {
            goto __exit;
        }
        retryed = RT_TRUE;
        result = RT_EOK;
        goto __retry;
    }

__exit:
    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * send data to server or client by AT commands.
 *
 * @param socket current socket
 * @param buff send buffer
 * @param bfsz send buffer size
 * @param type connect socket type(tcp, udp)
 *
 * @return >=0: the size of send success
 *          -1: send AT commands error or send data error
 *          -2: waited socket event timeout
 *          -5: no memory
 */
static int rw007_socket_send(struct at_socket *socket, const char *buff, size_t bfsz, enum at_socket_type type)
{
    int result = RT_EOK;
    int event_result = 0;
    size_t cur_pkt_size = 0, sent_size = 0;
    at_response_t resp = RT_NULL;
    int device_socket = (int) socket->user_data;
    struct at_device *device = (struct at_device *) socket->device;
    struct at_device_rw007 *rw007 = (struct at_device_rw007 *) device->user_data;
    rt_mutex_t lock = device->client->lock;

    RT_ASSERT(buff);
    RT_ASSERT(bfsz > 0);

    resp = at_create_resp(128, 2, 5 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for rw007 device(%s) response structure.", device->name);
        return -RT_ENOMEM;
    }

    rt_mutex_take(lock, RT_WAITING_FOREVER);

    /* set current socket for send URC event */
    rw007->user_data = (void *) device_socket;

    /* set AT client end sign to deal with '>' sign */
    at_obj_set_end_sign(device->client, '>');

    while (sent_size < bfsz)
    {
        if (bfsz - sent_size < RW007_MODULE_SEND_MAX_SIZE)
        {
            cur_pkt_size = bfsz - sent_size;
        }
        else
        {
            cur_pkt_size = RW007_MODULE_SEND_MAX_SIZE;
        }

        /* send the "AT+CIPSEND" commands to AT server than receive the '>' response on the first line */
        if (at_obj_exec_cmd(device->client, resp, "AT+CIPSEND=%d,%d", device_socket, cur_pkt_size) < 0)
        {
            result = -RT_ERROR;
            goto __exit;
        }

        /* send the real data to server or client */
        result = (int) at_client_obj_send(device->client, buff + sent_size, cur_pkt_size);
        if (result == 0)
        {
            result = -RT_ERROR;
            goto __exit;
        }

        /* waiting result event from AT URC */
        if (rw007_socket_event_recv(device, SET_EVENT(device_socket, 0), 10 * RT_TICK_PER_SECOND, RT_EVENT_FLAG_OR) < 0)
        {
            LOG_E("rw007 device(%s) socket (%d) send failed, wait connect result timeout.", device->name, device_socket);
            result = -RT_ETIMEOUT;
            goto __exit;
        }
        /* waiting OK or failed result */
        event_result = rw007_socket_event_recv(device, RW007_EVENT_SEND_OK | RW007_EVENT_SEND_FAIL, 
                            5 * RT_TICK_PER_SECOND, RT_EVENT_FLAG_OR);
        if (event_result  < 0)
        {
            LOG_E("rw007 device(%s) socket (%d) send failed, wait connect OK|FAIL timeout.", device->name, device_socket);
            result = -RT_ETIMEOUT;
            goto __exit;
        }
        /* check result */
        if (event_result & RW007_EVENT_SEND_FAIL)
        {
            LOG_E("rw007 device(%s) socket (%d) send failed.", device->name, device_socket);
            result = -RT_ERROR;
            goto __exit;
        }

        sent_size += cur_pkt_size;
    }

__exit:
    /* reset the end sign for data */
    at_obj_set_end_sign(device->client, 0);

    rt_mutex_release(lock);

    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * domain resolve by AT commands.
 *
 * @param name domain name
 * @param ip parsed IP address, it's length must be 16
 *
 * @return  0: domain resolve success
 *         -2: wait socket event timeout
 *         -5: no memory
 */
static int rw007_domain_resolve(const char *name, char ip[16])
{
#define RESOLVE_RETRY        5

    int i, result = RT_EOK;
    char recv_ip[16] = { 0 };
    at_response_t resp = RT_NULL;
    struct at_device *device = RT_NULL;

    RT_ASSERT(name);
    RT_ASSERT(ip);

    device = at_device_get_first_initialized();
    if (device == RT_NULL)
    {
        LOG_E("get first initialization rw007 device failed.");
        return -RT_ERROR;
    }

    resp = at_create_resp(128, 0, 20 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL)
    {
        LOG_E("no memory for rw007 device response structure.", device->name);
        return -RT_ENOMEM;
    }

    for (i = 0; i < RESOLVE_RETRY; i++)
    {
        if (at_obj_exec_cmd(device->client, resp, "AT+CIPDOMAIN=\"%s\"", name) < 0)
        {
            result = -RT_ERROR;
            goto __exit;
        }

        /* parse the third line of response data, get the IP address */
        if (at_resp_parse_line_args_by_kw(resp, "+CIPDOMAIN:", "+CIPDOMAIN:%s", recv_ip) < 0)
        {
            rt_thread_mdelay(100);
            /* resolve failed, maybe receive an URC CRLF */
            continue;
        }

        if (rt_strlen(recv_ip) < 8)
        {
            rt_thread_mdelay(100);
            /* resolve failed, maybe receive an URC CRLF */
            continue;
        }
        else
        {
            rt_strncpy(ip, recv_ip, 15);
            ip[15] = '\0';
            break;
        }
    }

__exit:
    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;

}

/**
 * set AT socket event notice callback
 *
 * @param event notice event
 * @param cb notice callback
 */
static void rw007_socket_set_event_cb(at_socket_evt_t event, at_evt_cb_t cb)
{
    if (event < sizeof(at_evt_cb_set) / sizeof(at_evt_cb_set[1]))
    {
        at_evt_cb_set[event] = cb;
    }
}

static const struct at_socket_ops rw007_socket_ops =
{
    rw007_socket_connect,
    rw007_socket_close,
    rw007_socket_send,
    rw007_domain_resolve,
    rw007_socket_set_event_cb,
};

static void urc_send_func(struct at_client *client, const char *data, rt_size_t size)
{
    int device_socket = 0;
    struct at_device *device = RT_NULL;
    struct at_device_rw007 *rw007 = RT_NULL;
    char *client_name = client->device->parent.name;

    RT_ASSERT(data && size);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_CLIENT, client_name);
    if (device == RT_NULL)
    {
        LOG_E("get rw007 device by client name(%s) failed.", client_name);
        return;
    }
    rw007 = (struct at_device_rw007 *) device->user_data;
    device_socket = (int) rw007->user_data;

    if (rt_strstr(data, "SEND OK"))
    {
        rw007_socket_event_send(device, SET_EVENT(device_socket, RW007_EVENT_SEND_OK));
    }
    else if (rt_strstr(data, "SEND FAIL"))
    {
        rw007_socket_event_send(device, SET_EVENT(device_socket, RW007_EVENT_SEND_FAIL));
    }
}

static void urc_send_bfsz_func(struct at_client *client, const char *data, rt_size_t size)
{
    static int cur_send_bfsz = 0;

    RT_ASSERT(data && size);

    sscanf(data, "Recv %d bytes", &cur_send_bfsz);
}

static void urc_close_func(struct at_client *client, const char *data, rt_size_t size)
{
    int device_socket = 0;
    struct at_socket *socket = RT_NULL;
    struct at_device *device = RT_NULL;
    char *client_name = client->device->parent.name; 

    RT_ASSERT(data && size);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_CLIENT, client_name);
    if (device == RT_NULL)
    {
       LOG_E("get rw007 device by client name(%s) failed.", client_name);
        return;
    }

    sscanf(data, "%d,CLOSED", &device_socket);
    /* get at socket object by device socket descriptor */
    socket = &(device->sockets[device_socket]);

    /* notice the socket is disconnect by remote */
    if (at_evt_cb_set[AT_SOCKET_EVT_CLOSED])
    {
        at_evt_cb_set[AT_SOCKET_EVT_CLOSED](socket, AT_SOCKET_EVT_CLOSED, RT_NULL, 0);
    }
}

static void urc_recv_func(struct at_client *client, const char *data, rt_size_t size)
{
    int device_socket = 0;
    rt_int32_t timeout = 0;
    rt_size_t bfsz = 0, temp_size = 0;
    char *recv_buf = RT_NULL, temp[8] = {0};
    struct at_socket *socket = RT_NULL;
    struct at_device *device = RT_NULL;
    char *client_name = client->device->parent.name; 

    RT_ASSERT(data && size);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_CLIENT, client_name);
    if (device == RT_NULL)
    {
        LOG_E("get rw007 device by client name(%s) failed.", client_name);
        return;
    }

    /* get the current socket and receive buffer size by receive data */
    sscanf(data, "+IPD,%d,%d:", &device_socket, (int *) &bfsz);
    /* get receive timeout by receive buffer length */
    timeout = bfsz;

    if (device_socket < 0 || bfsz == 0)
    {
        return;
    }

    recv_buf = (char *) rt_calloc(1, bfsz);
    if (recv_buf == RT_NULL)
    {
        LOG_E("no memory for rw007 device(%s) URC receive buffer (%d).", device->name, bfsz);
        /* read and clean the coming data */
        while (temp_size < bfsz)
        {
            if (bfsz - temp_size > sizeof(temp))
            {
                at_client_obj_recv(client, temp, sizeof(temp), timeout);
            }
            else
            {
                at_client_obj_recv(client, temp, bfsz - temp_size, timeout);
            }
            temp_size += sizeof(temp);
        }
        return;
    }

    /* sync receive data */
    if (at_client_obj_recv(client, recv_buf, bfsz, timeout) != bfsz)
    {
        LOG_E("rw007 device(%s) receive size(%d) data failed.", device->name, bfsz);
        rt_free(recv_buf);
        return;
    }

    /* get at socket object by device socket descriptor */
    socket = &(device->sockets[device_socket]);

    /* notice the receive buffer and buffer size */
    if (at_evt_cb_set[AT_SOCKET_EVT_RECV])
    {
        at_evt_cb_set[AT_SOCKET_EVT_RECV](socket, AT_SOCKET_EVT_RECV, recv_buf, bfsz);
    }
}

static struct at_urc urc_table[] = 
{
    {"SEND OK",          "\r\n",           urc_send_func},
    {"SEND FAIL",        "\r\n",           urc_send_func},
    {"Recv",             "bytes\r\n",      urc_send_bfsz_func},
    {"",                 ",CLOSED\r\n",    urc_close_func},
    {"+IPD",             ":",              urc_recv_func},
};

int rw007_socket_init(struct at_device *device)
{
    RT_ASSERT(device);

    /* register URC data execution function  */
    at_obj_set_urc_table(device->client, urc_table, sizeof(urc_table) / sizeof(urc_table[0]));

    return RT_EOK;
}

int rw007_socket_class_register(struct at_device_class *class)
{
    RT_ASSERT(class);

    class->socket_num = AT_DEVICE_RW007_SOCKETS_NUM;
    class->socket_ops = &rw007_socket_ops;

    return RT_EOK;
}

#endif /* AT_DEVICE_USING_RW007 && AT_USING_SOCKET */
