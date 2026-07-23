/* lwIP options for PRetroCalc OS (NO_SYS poll mode) */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define SYS_LIGHTWEIGHT_PROT        0

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_SEG            16
#define MEMP_NUM_PBUF               8
#define PBUF_POOL_SIZE              8
#define PBUF_POOL_BUFSIZE           1600

#define LWIP_TCP                    1
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)

#define LWIP_DNS                    1
#define DNS_TABLE_SIZE              2
#define DNS_MAX_NAME_LENGTH         128

#define LWIP_IPV4                   1
#define LWIP_ICMP                   1
#define LWIP_DHCP                   1
#define LWIP_UDP                    1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

#define LWIP_CHKSUM_ALGORITHM       3

#endif
