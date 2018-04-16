#include "net.h"

#include "ikcp.h"
#include "mem.h"
#include "trophy.h"
#include "debug.h"

#include <vitasdk.h>
#include <stdlib.h>
#include <stdio.h>

#define NET_SIZE       0x80000 // Size of net module buffer

static char vita_ip[32];
static uint64_t vita_addr;
static SceUID isNetAvailable = 0;
static SceUID packetMutex = -1;

int net_loaded() {
    return isNetAvailable != 0;
}

int net_init() {
    if (isNetAvailable) return 1;
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
        sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    int ret = sceNetShowNetstat();
    if (ret == SCE_NET_ERROR_ENOTINIT) {
        isNetAvailable = (SceUID)malloc(NET_SIZE);
        if (!isNetAvailable) return 1;
        SceNetInitParam initparam;
        initparam.memory = (void*)isNetAvailable;
        initparam.size = NET_SIZE;
        initparam.flags = 0;
        sceNetInit(&initparam);
    }
    sceNetCtlInit();
    SceNetCtlInfo info;
    sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
    sceClibSnprintf(vita_ip, 32, "%s", info.ip_address);
    sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &vita_addr);
    packetMutex = sceKernelCreateMutex("rcsvr_packet_mutex", 0, 0, 0);
    return 0;
}

void net_finish() {
    if (packetMutex >= 0) {
        sceKernelDeleteMutex(packetMutex);
        packetMutex = -1;
    }
    if (isNetAvailable) {
        free((void*)isNetAvailable);
        isNetAvailable = 0;
    }
}

typedef struct send_buf {
    struct send_buf *next;
    SceNetSockaddrIn addr;
    int len;
    char buf[0];
} send_buf;

static int udp_fd = -1;
static int epoll_fd = -1;
static uint16_t kcp_port = 0;
static SceNetSockaddrIn saddr_remote = {};
static ikcpcb *kcp = NULL;

static send_buf *shead = NULL, *stail = NULL;

static int total_count = 0, recv_size = 0;
static char recv_buf[0x1200];

static void _kcp_disconnect() {
    if (kcp != NULL) {
        ikcp_release(kcp);
        kcp = NULL;
    }
    recv_size = 0;
    memset(&saddr_remote, 0, sizeof(SceNetSockaddrIn));
}

static void _kcp_send(const char *buf, int len, SceNetSockaddrIn *addr, int is_kcp) {
    send_buf *b = (send_buf*)malloc(sizeof(send_buf) + (is_kcp ? (len + 4) : len));
    memcpy(&b->addr, addr, sizeof(SceNetSockaddrIn));
    b->len = is_kcp ? (len + 4) : len;
    if (is_kcp) {
        b->buf[0] = 'K';
        memcpy(b->buf + 4, buf, len);
    } else
        memcpy(b->buf, buf, len);
    b->next = NULL;
    if (shead == NULL) {
        sceKernelLockMutex(packetMutex, 1, NULL);
        shead = b;
        stail = b;
        sceKernelUnlockMutex(packetMutex, 1);
        SceNetEpollEvent event = {SCE_NET_EPOLLIN | SCE_NET_EPOLLOUT};
        int ret = sceNetEpollControl(epoll_fd, SCE_NET_EPOLL_CTL_MOD, udp_fd, &event);
        if (ret != 0)
            log_error("sceNetEpollControl(): 0x%08X\n", ret);
    } else {
        sceKernelLockMutex(packetMutex, 1, NULL);
        stail->next = b;
        stail = b;
        sceKernelUnlockMutex(packetMutex, 1);
    }
}

static int _kcp_output(const char *buf, int len, ikcpcb *kcp, void *user) {
    _kcp_send(buf, len, &saddr_remote, 1);
    return 0;
}
/*
static void _kcp_writelog(const char *log, struct IKCPCB *kcp, void *user) {
    log_debug("%s\n", log);
}
*/

static void _kcp_clear() {
    _kcp_disconnect();
    if (udp_fd != -1) {
        sceNetSocketClose(udp_fd);
        udp_fd = -1;
    }
    if (epoll_fd != -1) {
        sceNetEpollDestroy(epoll_fd);
        epoll_fd = -1;
    }
    while (shead != NULL) {
        send_buf *rem = shead;
        shead = shead->next;
        free(rem);
    }
    stail = NULL;
}

void _kcp_send_cmd(int op, const void *buf, int len) {
    if (kcp == NULL) return;
    uint32_t sendbuf[0x208];
    sendbuf[0] = op;
    sendbuf[1] = len;
    if (buf != NULL) memcpy(&sendbuf[2], buf, len);
    ikcp_send(kcp, (const char *)sendbuf, len + 8);
}

static void _kcp_search_start(int type) {
    _kcp_send_cmd(type, NULL, 0);
    total_count = 0;
}

static void _kcp_search_end(int err) {
    if (err == 0)
        _kcp_send_cmd(total_count > 100 ? 0x30000 : 0x20000, NULL, 0);
    else
        _kcp_send_cmd(0x30001, NULL, 0);
}

static void _kcp_search_cb(const uint32_t *data, int size, int data_len) {
    total_count += size;
    if (total_count <= 0x40) {
        int i;
        uint32_t buf[0xC0] = {0};
        for (i = 0; i < size; ++i) {
            uint32_t addr = data[i] | 0x80000000U;
            buf[i * 3] = addr;
            memcpy(&buf[i * 3 + 1], (const void*)addr, data_len);
        }
        _kcp_send_cmd(0x10000, &buf, size * 4 * 3);
    }
}

static void _kcp_trophy_list_end(int err) {
    if (err == 0)
        _kcp_send_cmd(0x8001, NULL, 0);
    else
        _kcp_send_cmd(0x8002, NULL, 0);
}

static void _kcp_trophy_list(int id, int grade, int hidden, int unlocked, const char *name, const char *desc) {
    char buf[240];
    *(int*)buf = id;
    *(int*)(buf+4) = grade;
    *(int*)(buf+8) = hidden;
    *(int*)(buf+12) = unlocked;
    memcpy(buf + 16, name, 64);
    buf[79] = 0;
    memcpy(buf + 80, desc, 160);
    buf[239] = 0;
    _kcp_send_cmd(0x8000, buf, 240);
}

static void _kcp_trophy_unlock(int ret, int id, int platid) {
    log_debug("Unlock result: %d %d %d\n", ret, id, platid);
    if (ret < 0) {
        _kcp_send_cmd(0x8110, NULL, 0);
    } else {
        int buf[2];
        buf[0] = id;
        buf[1] = platid;
        _kcp_send_cmd(0x8100, buf, 8);
    }
}

static void _process_kcp_packet(int cmd, const char *buf, int len) {
    if (cmd == -1) {
        _kcp_disconnect();
        return;
    }
    int op = cmd >> 8;
    int type = cmd & 0xFF;
    switch(op) {
    case 0:
    case 1:
        mem_search_reset();
        /* fallthrough */
    case 2:
        mem_start_search(type, op == 1, buf, len, _kcp_search_cb, _kcp_search_start, _kcp_search_end);
        break;
    case 0x80:
        trophy_list(_kcp_trophy_list, _kcp_trophy_list_end);
        break;
    case 0x81:
        if (type == 0) {
            if (len < 4)
                _kcp_send_cmd(0x8110, NULL, 0);
            else
                trophy_unlock(*(int*)buf, _kcp_trophy_list, _kcp_trophy_unlock);
        } else {
            trophy_unlock_all(_kcp_trophy_list, _kcp_trophy_unlock);
        }
        break;
    }
}

void net_kcp_listen(uint16_t port) {
    _kcp_clear();

    int ret;
    SceNetSockaddrIn sockaddr;

    kcp_port = port;

    udp_fd = sceNetSocket("kcp_udp_socket", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    const int nb = 1, br = 1;
    ret = sceNetSetsockopt(udp_fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb));
    if (ret != 0)
        log_error("sceNetSetsockopt(SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO): 0x%08X\n", ret);
    ret = sceNetSetsockopt(udp_fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &br, sizeof(br));
    if (ret != 0)
        log_error("sceNetSetsockopt(SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST): 0x%08X\n", ret);

    sockaddr.sin_family = SCE_NET_AF_INET;
    sockaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    sockaddr.sin_port = sceNetHtons(port);
    ret = sceNetBind(udp_fd, (SceNetSockaddr *)&sockaddr, sizeof(sockaddr));
    if (ret != 0)
        log_error("sceNetBind(): 0x%08X\n", ret);
    if (ret != 0)
        log_error("sceNetSetsockopt(): 0x%08X\n", ret);
    epoll_fd = sceNetEpollCreate("kcp_udp_epoll", 0);
    if (epoll_fd < 0)
        log_error("sceNetEpollCreate(): %d\n", epoll_fd);

    SceNetEpollEvent event = {SCE_NET_EPOLLIN};
    ret = sceNetEpollControl(epoll_fd, SCE_NET_EPOLL_CTL_ADD, udp_fd, &event);
    if (ret != 0)
        log_error("sceNetEpollControl(): 0x%08X\n", ret);
}

void net_kcp_process(uint32_t tick) {
    if (epoll_fd < 0) {
        sceKernelDelayThread(16666);
        return;
    }
    if (kcp != NULL) {
        ikcp_update(kcp, tick);
        while(1) {
            int hr = ikcp_recv(kcp, recv_buf + recv_size, 0x1200 - recv_size);
            if (hr < 0) break;
            recv_size += hr;
            while(recv_size >= 8) {
                uint32_t len = *(uint32_t*)&recv_buf[4];
                if (len > 0x800) {
                    _kcp_send_cmd(-1, NULL, 0);
                    break;
                }
                if (len + 8 > recv_size) break;
                log_debug("Got kcp packet: %d\n", len + 8);
                _process_kcp_packet(*(int*)recv_buf, recv_buf + 8, len);
                if (len + 8 < recv_size) {
                    recv_size -= len + 8;
                    memmove(recv_buf, recv_buf + len + 8, recv_size);
                } else recv_size = 0;
            }
        }
    }
    SceNetEpollEvent events[1];
    int count = sceNetEpollWait(epoll_fd, events, 1, 16666);
    if (count == 0) return;
    if (count < 0) {
        net_kcp_listen(kcp_port);
        sceKernelDelayThread(16666);
        return;
    }
    if (events[0].events & SCE_NET_EPOLLIN) {
        char buf[1500];
        SceNetSockaddrIn saddr = {};
        unsigned int fromlen = sizeof(saddr);
        ssize_t len = sceNetRecvfrom(udp_fd, buf, 1500, 0, (SceNetSockaddr*)&saddr, &fromlen);
        if (len > 0) {
            log_debug("SceNetRecvfrom(): %d\n", len);
            switch(buf[0]) {
                case 'B': {
                    _kcp_send("B", 1, &saddr, 0);
                    break;
                }
                case 'C': {
                    _kcp_disconnect();
                    memcpy(&saddr_remote, &saddr, sizeof(SceNetSockaddrIn));
                    static uint32_t conv = 0xC0DE;
                    kcp = ikcp_create(conv, NULL);
                    ikcp_nodelay(kcp, 0, 50, 0, 0);
                    ikcp_setmtu(kcp, 1440);
                    kcp->output = _kcp_output;
                    /*
                    kcp->writelog = _kcp_writelog;
                    kcp->logmask = 4095;
                    */
                    char n[256];
                    n[0] = 'C';
                    *(uint32_t*)&n[1] = conv;
                    sceAppMgrAppParamGetString(0, 12, &n[5], 16);
                    sceAppMgrAppParamGetString(0, 9, &n[14], 256 - 14);
                    _kcp_send(n, strlen(n + 14) + 14, &saddr, 0);
                    ++conv;
                    break;
                }
                case 'N':
                    mem_search_reset();
                    /* fallthrough */
                case 'S': {
                    buf[len] = 0;
                    uint32_t n;
                    n = strtoul(buf + 1, NULL, 10);
                    log_debug("Searching %u\n", n);
                    mem_search(1, 1, &n, 4, _kcp_search_cb);
                    break;
                }
                case 'W': {
                    buf[len] = 0;
                    char *bufend;
                    uint32_t n = strtoul(buf + 1, &bufend, 16);
                    while(*bufend == ' ') ++bufend;
                    uint32_t val = strtoul(bufend, NULL, 10);
                    log_debug("Writing %u to 0x%08X\n", val, n);
                    mem_set(n, &val, 4);
                    break;
                }
                case 'K': {
                    if (kcp == NULL
                        || saddr.sin_addr.s_addr != saddr_remote.sin_addr.s_addr
                        || saddr.sin_port != saddr_remote.sin_port) {
                        _kcp_send("D", 1, &saddr, 0);
                        break;
                    }
                    ikcp_input(kcp, buf + 4, len - 4);
                    break;
                }
                case 'D': {
                    _kcp_disconnect();
                    break;
                }
            }
        }
    }
    if (events[0].events & SCE_NET_EPOLLOUT) {
        while (shead != NULL) {
            int ret = sceNetSendto(udp_fd, shead->buf, shead->len, 0, (const SceNetSockaddr*)&shead->addr, sizeof(SceNetSockaddrIn));
            if (ret < 0) {
                if (ret == SCE_NET_ERROR_EAGAIN)
                    return;
                log_error("sceNetSendto(): 0x%08X\n", ret);
                return;
            }
            send_buf *rem = shead;
            shead = shead->next;
            free(rem);
        }
        stail = NULL;
        events[0].events = SCE_NET_EPOLLIN;
        int ret = sceNetEpollControl(epoll_fd, SCE_NET_EPOLL_CTL_MOD, udp_fd, events);
        if (ret != 0)
            log_error("sceNetEpollControl(): 0x%08X\n", ret);
    }
}
