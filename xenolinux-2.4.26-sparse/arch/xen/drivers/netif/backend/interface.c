/******************************************************************************
 * arch/xen/drivers/netif/backend/interface.c
 * 
 * Network-device interface management.
 * 
 * Copyright (c) 2004, Keir Fraser
 */

#include "common.h"
#include <linux/rtnetlink.h>

#define NETIF_HASHSZ 1024
#define NETIF_HASH(_d,_h) (((int)(_d)^(int)(_h))&(NETIF_HASHSZ-1))

static netif_t *netif_hash[NETIF_HASHSZ];
static struct net_device *bridge_dev;
static struct net_bridge *bridge_br;

netif_t *netif_find_by_handle(domid_t domid, unsigned int handle)
{
    netif_t *netif = netif_hash[NETIF_HASH(domid, handle)];
    while ( (netif != NULL) && 
            ((netif->domid != domid) || (netif->handle != handle)) )
        netif = netif->hash_next;
    return netif;
}

void __netif_disconnect_complete(netif_t *netif)
{
    ctrl_msg_t            cmsg;
    netif_be_disconnect_t disc;

    /*
     * These can't be done in __netif_disconnect() because at that point there
     * may be outstanding requests at the disc whose asynchronous responses
     * must still be notified to the remote driver.
     */
    unbind_evtchn_from_irq(netif->evtchn);
    vfree(netif->tx); /* Frees netif->rx as well. */
    rtnl_lock();
    (void)br_del_if(bridge_br, netif->dev);
    (void)dev_close(netif->dev);
    rtnl_unlock();

    /* Construct the deferred response message. */
    cmsg.type         = CMSG_NETIF_BE;
    cmsg.subtype      = CMSG_NETIF_BE_DISCONNECT;
    cmsg.id           = netif->disconnect_rspid;
    cmsg.length       = sizeof(netif_be_disconnect_t);
    disc.domid        = netif->domid;
    disc.netif_handle = netif->handle;
    disc.status       = NETIF_BE_STATUS_OKAY;
    memcpy(cmsg.msg, &disc, sizeof(disc));

    /*
     * Make sure message is constructed /before/ status change, because
     * after the status change the 'netif' structure could be deallocated at
     * any time. Also make sure we send the response /after/ status change,
     * as otherwise a subsequent CONNECT request could spuriously fail if
     * another CPU doesn't see the status change yet.
     */
    mb();
    if ( netif->status != DISCONNECTING )
        BUG();
    netif->status = DISCONNECTED;
    mb();

    /* Send the successful response. */
    ctrl_if_send_response(&cmsg);
}

void netif_create(netif_be_create_t *create)
{
    domid_t            domid  = create->domid;
    unsigned int       handle = create->netif_handle;
    struct net_device *dev;
    netif_t          **pnetif, *netif;
    char               name[IFNAMSIZ];

    snprintf(name, IFNAMSIZ, "vif%u.%u", domid, handle);
    dev = alloc_netdev(sizeof(netif_t), name, ether_setup);
    if ( dev == NULL )
    {
        DPRINTK("Could not create netif: out of memory\n");
        create->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        return;
    }

    netif = dev->priv;
    memset(netif, 0, sizeof(*netif));
    netif->domid  = domid;
    netif->handle = handle;
    netif->status = DISCONNECTED;
    spin_lock_init(&netif->rx_lock);
    spin_lock_init(&netif->tx_lock);
    atomic_set(&netif->refcnt, 0);
    netif->dev = dev;

    netif->credit_bytes = netif->remaining_credit = ~0UL;
    netif->credit_usec  = 0UL;
    /*init_ac_timer(&new_vif->credit_timeout);*/

    pnetif = &netif_hash[NETIF_HASH(domid, handle)];
    while ( *pnetif != NULL )
    {
        if ( ((*pnetif)->domid == domid) && ((*pnetif)->handle == handle) )
        {
            DPRINTK("Could not create netif: already exists\n");
            create->status = NETIF_BE_STATUS_INTERFACE_EXISTS;
            kfree(dev);
            return;
        }
        pnetif = &(*pnetif)->hash_next;
    }

    dev->hard_start_xmit = netif_be_start_xmit;
    dev->get_stats       = netif_be_get_stats;
    memcpy(dev->dev_addr, create->mac, ETH_ALEN);

    /* Disable queuing. */
    dev->tx_queue_len = 0;

    /* XXX In bridge mode we should force a different MAC from remote end. */
    dev->dev_addr[2] ^= 1;

    if ( register_netdev(dev) != 0 )
    {
        DPRINTK("Could not register new net device\n");
        create->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        kfree(dev);
        return;
    }

    netif->hash_next = *pnetif;
    *pnetif = netif;

    DPRINTK("Successfully created netif\n");
    create->status = NETIF_BE_STATUS_OKAY;
}

void netif_destroy(netif_be_destroy_t *destroy)
{
    domid_t       domid  = destroy->domid;
    unsigned int  handle = destroy->netif_handle;
    netif_t     **pnetif, *netif;

    pnetif = &netif_hash[NETIF_HASH(domid, handle)];
    while ( (netif = *pnetif) != NULL )
    {
        if ( (netif->domid == domid) && (netif->handle == handle) )
        {
            if ( netif->status != DISCONNECTED )
                goto still_connected;
            goto destroy;
        }
        pnetif = &netif->hash_next;
    }

    destroy->status = NETIF_BE_STATUS_INTERFACE_NOT_FOUND;
    return;

 still_connected:
    destroy->status = NETIF_BE_STATUS_INTERFACE_CONNECTED;
    return;

 destroy:
    *pnetif = netif->hash_next;
    unregister_netdev(netif->dev);
    kfree(netif->dev);
    destroy->status = NETIF_BE_STATUS_OKAY;
}

void netif_connect(netif_be_connect_t *connect)
{
    domid_t       domid  = connect->domid;
    unsigned int  handle = connect->netif_handle;
    unsigned int  evtchn = connect->evtchn;
    unsigned long tx_shmem_frame = connect->tx_shmem_frame;
    unsigned long rx_shmem_frame = connect->rx_shmem_frame;
    struct vm_struct *vma;
    pgprot_t      prot;
    int           error;
    netif_t      *netif;
    struct net_device *eth0_dev;

    netif = netif_find_by_handle(domid, handle);
    if ( unlikely(netif == NULL) )
    {
        DPRINTK("netif_connect attempted for non-existent netif (%u,%u)\n", 
                connect->domid, connect->netif_handle); 
        connect->status = NETIF_BE_STATUS_INTERFACE_NOT_FOUND;
        return;
    }

    if ( netif->status != DISCONNECTED )
    {
        connect->status = NETIF_BE_STATUS_INTERFACE_CONNECTED;
        return;
    }

    if ( (vma = get_vm_area(2*PAGE_SIZE, VM_IOREMAP)) == NULL )
    {
        connect->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        return;
    }

    prot = __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED);
    error  = direct_remap_area_pages(&init_mm, 
                                     VMALLOC_VMADDR(vma->addr),
                                     tx_shmem_frame<<PAGE_SHIFT, PAGE_SIZE,
                                     prot, domid);
    error |= direct_remap_area_pages(&init_mm, 
                                     VMALLOC_VMADDR(vma->addr) + PAGE_SIZE,
                                     rx_shmem_frame<<PAGE_SHIFT, PAGE_SIZE,
                                     prot, domid);
    if ( error != 0 )
    {
        if ( error == -ENOMEM )
            connect->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        else if ( error == -EFAULT )
            connect->status = NETIF_BE_STATUS_MAPPING_ERROR;
        else
            connect->status = NETIF_BE_STATUS_ERROR;
        vfree(vma->addr);
        return;
    }

    netif->evtchn         = evtchn;
    netif->irq            = bind_evtchn_to_irq(evtchn);
    netif->tx_shmem_frame = tx_shmem_frame;
    netif->rx_shmem_frame = rx_shmem_frame;
    netif->tx             = 
        (netif_tx_interface_t *)vma->addr;
    netif->rx             = 
        (netif_rx_interface_t *)((char *)vma->addr + PAGE_SIZE);
    netif->status         = CONNECTED;
    netif_get(netif);

    rtnl_lock();

    (void)dev_open(netif->dev);
    (void)br_add_if(bridge_br, netif->dev);

    /*
     * The default config is a very simple binding to eth0.
     * If eth0 is being used as an IP interface by this OS then someone
     * must add eth0's IP address to nbe-br, and change the routing table
     * to refer to nbe-br instead of eth0.
     */
    (void)dev_open(bridge_dev);
    if ( (eth0_dev = __dev_get_by_name("eth0")) != NULL )
    {
        (void)dev_open(eth0_dev);
        (void)br_add_if(bridge_br, eth0_dev);
    }

    rtnl_unlock();

    (void)request_irq(netif->irq, netif_be_int, 0, netif->dev->name, netif);
    netif_start_queue(netif->dev);

    connect->status = NETIF_BE_STATUS_OKAY;
}

int netif_disconnect(netif_be_disconnect_t *disconnect, u8 rsp_id)
{
    domid_t       domid  = disconnect->domid;
    unsigned int  handle = disconnect->netif_handle;
    netif_t      *netif;

    netif = netif_find_by_handle(domid, handle);
    if ( unlikely(netif == NULL) )
    {
        DPRINTK("netif_disconnect attempted for non-existent netif"
                " (%u,%u)\n", disconnect->domid, disconnect->netif_handle); 
        disconnect->status = NETIF_BE_STATUS_INTERFACE_NOT_FOUND;
        return 1; /* Caller will send response error message. */
    }

    if ( netif->status == CONNECTED )
    {
        netif->status = DISCONNECTING;
        netif->disconnect_rspid = rsp_id;
        wmb(); /* Let other CPUs see the status change. */
        netif_stop_queue(netif->dev);
        free_irq(netif->irq, NULL);
        netif_deschedule(netif);
        netif_put(netif);
    }

    return 0; /* Caller should not send response message. */
}

void netif_interface_init(void)
{
    memset(netif_hash, 0, sizeof(netif_hash));
    if ( br_add_bridge("nbe-br") != 0 )
        BUG();
    bridge_dev = __dev_get_by_name("nbe-br");
    bridge_br  = (struct net_bridge *)bridge_dev->priv;
    bridge_br->bridge_hello_time = bridge_br->hello_time = 0;
    bridge_br->bridge_forward_delay = bridge_br->forward_delay = 0;
    bridge_br->stp_enabled = 0;
}
