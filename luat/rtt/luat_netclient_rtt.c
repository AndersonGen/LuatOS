/*
 * File      : netclient.c
 * This file is part of RT-Thread
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
 * 2018-08-10     never        the first version
 */
#include <rtthread.h>
#include <rtdevice.h>

#ifdef RT_USING_SAL

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
//#include "elog.h"
#include "netclient.h"
#include "luat_malloc.h"

#include "rtthread.h"
#define DBG_TAG           "luat.netc"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>



#define BUFF_SIZE (1024)
#define MAX_VAL(A, B) ((A) > (B) ? (A) : (B))
#define STRCMP(a, R, b) (strcmp((a), (b)) R 0)

static netclient_t *netclient_create(void);
static rt_int32_t netclient_destory(netclient_t *thiz);
static rt_int32_t socket_init(netclient_t *thiz, const char *hostname, int port);
static rt_int32_t socket_deinit(netclient_t *thiz);
static rt_int32_t pipe_init(netclient_t *thiz);
static rt_int32_t pipe_deinit(netclient_t *thiz);
static void select_handle(netclient_t *thiz, char *sock_buff);
static rt_int32_t netclient_thread_init(netclient_t *thiz);
static void netclient_thread_entry(void *param);

static rt_uint32_t netc_seq = 1;

uint32_t netc_next_no(void) {
    if (netc_seq > 0xFFFF00) {
        netc_seq = 0xFF;
    }
    return netc_seq++;
}

static void EVENT(int netc_id, tpc_cb_t cb, int lua_ref, int tp, size_t len, void* buff) {
    netc_ent_t* ent;
    LOG_I("netc[%ld] event type=%d", netc_id, tp);
    if (cb == RT_NULL) return;
    if (tp != NETC_EVENT_RECV || len < 0) len = 0;
    //len = 0;
    ent = luat_heap_malloc(sizeof(netc_ent_t));
    if (ent == NULL) {
        LOG_E("netc[%ld] EVENT call rt_malloc return NULL!", netc_id);
        return;
    }
    ent->netc_id = netc_id;
    ent->lua_ref = lua_ref;
    ent->len = len;
    ent->event = tp;
    if (len > 0) {
        ent->buff = luat_heap_malloc(len);
        if (ent->buff == RT_NULL) {
            LOG_E("netc[%ld] EVENT call rt_malloc buff return NULL!", netc_id);
            luat_heap_free(ent);
            return;
        }
        rt_memcpy(ent->buff, buff, len);
    }
    else {
        ent->buff = RT_NULL;
    }
    cb(ent);
}

static rt_int32_t netclient_destory(netclient_t *thiz)
{
    int res = 0;

    if (thiz == RT_NULL)
    {
        LOG_E("netclient del : param is NULL, delete failed");
        return -1;
    }

    LOG_I("netc[%ld] destory begin", thiz->id);

    if (thiz->sock_fd != -1)
        socket_deinit(thiz);

    pipe_deinit(thiz);

    LOG_I("netc[%ld] destory end", thiz->id);
    thiz->closed = 1;
    return 0;
}

static rt_int32_t socket_init(netclient_t *thiz, const char *url, int port)
{
    struct sockaddr_in dst_addr;
    struct hostent *hostname;
    rt_int32_t res = 0;

    if (thiz == RT_NULL)
        return -1;

    thiz->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (thiz->sock_fd == -1)
    {
        LOG_E("netc[%ld] socket init : socket create failed", thiz->id);
        return -1;
    }

    hostname = gethostbyname(url);
    if (hostname == NULL) {
        LOG_W("netc[%ld] dns name relove fail, retry at 2000ms", thiz->id);
        rt_thread_mdelay(2000);
        hostname = gethostbyname(url);
        if (hostname == NULL) {
            LOG_W("netc[%ld] dns name relove fail again, quit", thiz->id);
            return -1;
        }
    }

    // TODO: print ip for hostname
    //LOG_I("socket host=%s port=%d", hostname, port);

    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(port);
    dst_addr.sin_addr = *((struct in_addr *)hostname->h_addr);
    memset(&(dst_addr.sin_zero), 0, sizeof(dst_addr.sin_zero));

    res = connect(thiz->sock_fd, (struct sockaddr *)&dst_addr, sizeof(struct sockaddr));
    if (res == -1)
    {
        LOG_E("netc[%ld] connect failed", thiz->id);
        return -1;
    }

    LOG_I("netc[%ld] connected succeed", thiz->id);

    //send(thiz->sock_fd, str, strlen(str), 0);

    return 0;
}

static rt_int32_t socket_deinit(netclient_t *thiz)
{
    int res = 0;

    if (thiz == RT_NULL)
    {
        LOG_E("socket deinit : param is NULL, socket deinit failed");
        return -1;
    }
    if (thiz->sock_fd < 0)
        return 0;

    res = closesocket(thiz->sock_fd);
    //RT_ASSERT(res == 0);

    thiz->sock_fd = -1;

    LOG_I("netc[%ld] socket close succeed", thiz->id);

    return 0;
}

static rt_int32_t pipe_init(netclient_t *thiz)
{
    char dev_name[32];
    rt_pipe_t *pipe = RT_NULL;

    if (thiz == RT_NULL)
    {
        LOG_E("pipe init : param is NULL");
        return -1;
    }

    snprintf(thiz->pipe_name, sizeof(thiz->pipe_name), "p%06X", thiz->id);

    pipe = rt_pipe_create(thiz->pipe_name, PIPE_BUFSZ);
    if (pipe == RT_NULL)
    {
        thiz->pipe_name[0] = 0x00;
        LOG_E("netc[%ld] pipe create failed", thiz->id);
        return -1;
    }

    snprintf(dev_name, sizeof(dev_name), "/dev/%s", thiz->pipe_name);
    thiz->pipe_read_fd = open(dev_name, O_RDONLY, 0);
    if (thiz->pipe_read_fd < 0)
        goto fail_read;

    thiz->pipe_write_fd = open(dev_name, O_WRONLY, 0);
    if (thiz->pipe_write_fd < 0)
        goto fail_write;

    LOG_I("netc[%ld] pipe init succeed", thiz->id);
    return 0;

fail_write:
    close(thiz->pipe_read_fd);
fail_read:
    rt_pipe_delete(thiz->pipe_name);
    thiz->pipe_name[0] = 0x00;
    return -1;
}

static rt_int32_t pipe_deinit(netclient_t *thiz)
{
    int res = 0;

    if (thiz == RT_NULL)
    {
        LOG_E("pipe deinit : param is NULL, pipe deinit failed");
        return -1;
    }

    if (thiz->pipe_read_fd != -1) {
        close(thiz->pipe_read_fd);
        thiz->pipe_read_fd = -1;
        res ++;
    }

    if (thiz->pipe_write_fd != -1) {
        res = close(thiz->pipe_write_fd);
        thiz->pipe_write_fd = -1;
        res ++;
    }
    if (thiz->pipe_name[0] != 0) {
        rt_pipe_delete(thiz->pipe_name);
        res ++;
    }
    if (res)
        LOG_I("netc[%ld] pipe close succeed", thiz->id);
    return 0;
}

static rt_int32_t netclient_thread_init(netclient_t *thiz)
{
    rt_thread_t th;
    char tname[12];
    rt_sprintf(tname, "n%06X", thiz->id);
    th = rt_thread_create(tname, netclient_thread_entry, thiz, 2048, 20, 10);
    if (th == RT_NULL)
    {
        LOG_E("netc[%ld] thread create fail", thiz->id);
        return -1;
    }

    if (rt_thread_startup(th) != RT_EOK) {
        rt_thread_detach(th);
        LOG_E("netc[%ld] thread start fail", thiz->id);
        return -1;
    }

    return 0;
}

static void select_handle(netclient_t *thiz, char *sock_buff)
{
    fd_set fds;
    rt_int32_t max_fd = 0, res = 0;

    max_fd = MAX_VAL(thiz->sock_fd, thiz->pipe_read_fd) + 1;
    FD_ZERO(&fds);

    while (1)
    {
        FD_SET(thiz->sock_fd, &fds);
        FD_SET(thiz->pipe_read_fd, &fds);

        res = select(max_fd, &fds, RT_NULL, RT_NULL, RT_NULL);

        /* exception handling: exit */
        if (res <= 0) {
            LOG_I("netc[%ld] select result=%d, goto cleanup", thiz->id, res);
            goto exit;
        }

        /* socket is ready */
        if (FD_ISSET(thiz->sock_fd, &fds))
        {
            #if 1
            res = recv(thiz->sock_fd, sock_buff, BUFF_SIZE, 0);

            if (res > 0) {
                LOG_I("netc[%ld] data recv len=%d", thiz->id, res);
                if (thiz->rx) {
                    EVENT(thiz->id, thiz->rx, thiz->cb_recv, NETC_EVENT_RECV, res, sock_buff);
                }
            }
            else {
                LOG_I("netc[%ld] recv return error=%d", thiz->id, res);
                if (thiz->rx) {
                    EVENT(thiz->id, thiz->rx, thiz->cb_error, NETC_EVENT_ERROR, res, sock_buff);
                }
                goto exit;
            }
            #endif
            //EVENT(thiz->id, thiz->rx, thiz->cb_recv, NETC_EVENT_RECV, res, sock_buff);
        }

        /* pipe is read */
        if (FD_ISSET(thiz->pipe_read_fd, &fds))
        {
            /* read pipe */
            res = read(thiz->pipe_read_fd, sock_buff, BUFF_SIZE);

            if (res <= 0) {
                thiz->closed = 0;
                goto exit;
            }
            else if (thiz->closed) {
                goto exit;
            }
            else if (res > 0) {
                send(thiz->sock_fd, sock_buff, res, 0);
            }
        }
    }
exit:
    LOG_I("netc[%ld] select loop exit, cleanup", thiz->id);
    return;
}

static void netclient_thread_entry(void *param)
{
    netclient_t *thiz = param;
    char *sock_buff = RT_NULL;

    if (socket_init(thiz, thiz->hostname, thiz->port) != 0) {
        LOG_W("netc[%ld] connect fail", thiz->id);
        if (thiz->rx) {
            EVENT(thiz->id, thiz->rx, thiz->cb_connect, NETC_EVENT_CONNECT_OK, 0, RT_NULL);
        }
        goto netc_exit;
    }
    else {
        LOG_I("netc[%ld] connect ok", thiz->id);
        if (thiz->rx) {
            EVENT(thiz->id, thiz->rx, thiz->cb_connect, NETC_EVENT_CONNECT_FAIL, 0, RT_NULL);
        }
    }

    sock_buff = rt_malloc(BUFF_SIZE);
    if (sock_buff == RT_NULL)
    {
        LOG_E("netc[%ld] fail to malloc sock_buff!!!", thiz->id);
        goto netc_exit;
    }

    memset(sock_buff, 0, BUFF_SIZE);

    select_handle(thiz, sock_buff);

    rt_free(sock_buff);
    //if (thiz != NULL) {
        thiz->closed = 1;
        netclient_destory(thiz);
    //    if (thiz->rx) {
    //        EVENT(thiz->id, thiz->rx, thiz->cb_close, NETC_EVENT_CLOSE, 0, RT_NULL);
    //    }
    //}
netc_exit:
    thiz->closed = 1;
    EVENT(thiz->id, thiz->rx, 0, NETC_EVENT_END, 0, RT_NULL);
    LOG_W("netc[%ld] thread end", thiz->id);
}

int32_t netclient_start(netclient_t * thiz) {

    if (pipe_init(thiz) != 0)
        goto quit;

    if (netclient_thread_init(thiz) != 0)
        goto quit;

    LOG_I("netc[%ld] start succeed", thiz->id);
    return 0;

quit:
    netclient_destory(thiz);
    return 1;
}

void netclient_close(netclient_t *thiz)
{
    LOG_I("netc[%ld] deinit start", thiz->id);
    int fd = thiz->sock_fd;
    if (fd != -1 && fd != 0) {
        closesocket(fd);
    }
    rt_thread_mdelay(1);
    netclient_destory(thiz);
    LOG_I("netc[%ld] deinit end", thiz->id);
}

int32_t netclient_send(netclient_t *thiz, const void *buff, size_t len)
{
    size_t bytes = 0;

    if (thiz == RT_NULL)
    {
        LOG_W("netclient send : param is NULL");
        return -1;
    }

    if (buff == RT_NULL)
    {
        LOG_W("netc[%ld] send : buff is NULL", thiz->id);
        return -1;
    }
    if (thiz->pipe_write_fd == -1) {
        LOG_W("netc[%ld] socket is closed!!!", thiz->id);
        return -1;
    }

    //LOG_D("netc[%ld] send data len=%d buff=[%s]", this->id, len, buff);

    bytes = write(thiz->pipe_write_fd, buff, len);
    return bytes;
}

#endif
