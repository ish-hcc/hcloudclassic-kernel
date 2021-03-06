/*
 *  Copyright (C) 2019 Innogrid
 */

/*  writen by cgs 2019 */

#include <linux/module.h>
#include <hcc/sys/types.h>
#include <net/hccgrpc/rpcid.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/lockdep.h>
#include "rpc_internal.h"

#include <hcc/lib/hashtable.h>

struct rpc_service** rpc_services;
unsigned long rpc_desc_id;

hashtable_t* desc_srv[HCC_MAX_NODES];
hashtable_t* desc_clt;

spinlock_t rpc_desc_done_lock[HCC_MAX_NODES];
unsigned long rpc_desc_done_id[HCC_MAX_NODES];

unsigned long rpc_link_send_seq_id[HCC_MAX_NODES];
unsigned long rpc_link_send_ack_id[HCC_MAX_NODES];
unsigned long rpc_link_recv_seq_id[HCC_MAX_NODES];

DEFINE_PER_CPU(struct list_head, rpc_desc_trash);

struct kmem_cache* rpc_desc_cachep;
struct kmem_cache* rpc_desc_send_cachep;
struct kmem_cache* rpc_desc_recv_cachep;
struct kmem_cache* rpc_desc_elem_cachep;
struct kmem_cache* rpc_tx_elem_cachep;
struct kmem_cache* __rpc_synchro_cachep;

static struct lock_class_key rpc_desc_srv_lock_key;
static struct lock_class_key rpc_desc_clt_lock_key;

unsigned long rpc_mask[RPCID_MAX/(sizeof(unsigned long)*8)+1];

/*
 * RPC synchro
 */
struct rpc_synchro* rpc_synchro_new(int max,
                                    char *label,
                                    int order);

/*
 * RPC management
 */
inline
struct rpc_service* rpc_service_init(enum rpcid rpcid,
                                     enum rpc_target rpc_target,
                                     enum rpc_handler rpc_handler,
                                     struct rpc_synchro *rpc_synchro,
                                     rpc_handler_t h,
                                     unsigned long flags){
    struct rpc_service* service;

    service = kmalloc(sizeof(*service), GFP_KERNEL);
    if(!service){
        printk("OOM in rpc_service_init\n");
        return NULL;
    };

    service->id = rpcid;
    service->target = rpc_target;
    service->handler = rpc_handler;
    service->h = h;
    service->synchro = rpc_synchro;
    service->flags = flags;

    return service;
};

int __rpc_register(enum rpcid rpcid,
                   enum rpc_target rpc_target,
                   enum rpc_handler rpc_handler,
                   struct rpc_synchro *rpc_synchro,
                   void* _h, unsigned long flags){
    rpc_handler_t h = (rpc_handler_t)_h;
    rpc_services[rpcid] = rpc_service_init(rpcid, rpc_target, rpc_handler,
                                           rpc_synchro, h, flags);

    rpc_disable(rpcid);
    return 0;
};

struct rpc_desc* rpc_desc_alloc(void){
    struct rpc_desc* desc;
    int in_interrupt;
    int cpu = smp_processor_id();

    in_interrupt = 0;
    if(list_empty(&per_cpu(rpc_desc_trash, cpu))){
        desc = kmem_cache_alloc(rpc_desc_cachep, GFP_ATOMIC);
        if(!desc)
            return NULL;

        in_interrupt = 1;
    }else{
        desc = container_of(per_cpu(rpc_desc_trash, cpu).next,
        struct rpc_desc,
        list);
        list_del(&desc->list);
    };

    memset(desc, 0, sizeof(*desc));
    spin_lock_init(&desc->desc_lock);
    desc->in_interrupt = in_interrupt;
    atomic_set(&desc->usage, 1);
    desc->__synchro = NULL;

    return desc;
};

void rpc_enable(enum rpcid rpcid){
    spin_lock_bh(&waiting_desc_lock);
    if(rpc_services[rpcid]->id == rpcid)
        clear_bit(rpcid, rpc_mask);

    spin_unlock_bh(&waiting_desc_lock);
};

void rpc_enable_all(void){
    int i;

    for(i=0;i<RPCID_MAX;i++)
        rpc_enable(i);

    if(!list_empty(&waiting_desc))
        rpc_wake_up_thread(NULL);
};

void rpc_disable(enum rpcid rpcid){
    if(rpc_services[rpcid]->id == rpcid)
        set_bit(rpcid, rpc_mask);
};

void rpc_undef_handler (struct rpc_desc *desc){
    printk("service %d not registered\n", desc->rpcid);
};

void rpc_desc_get(struct rpc_desc* desc){
    BUG_ON(atomic_read(&desc->usage)==0);
    atomic_inc(&desc->usage);
};

void rpc_desc_put(struct rpc_desc* desc){
    BUG_ON(atomic_read(&desc->usage)==0);
    if(!atomic_dec_and_test(&desc->usage))
        return;

    kmem_cache_free(rpc_desc_cachep, desc);
};

struct rpc_desc_send* rpc_desc_send_alloc(void){
    struct rpc_desc_send* desc_send;

    desc_send = kmem_cache_alloc(rpc_desc_send_cachep, GFP_ATOMIC);
    if(!desc_send)
        return NULL;

    atomic_set(&desc_send->seq_id, 0);
    spin_lock_init(&desc_send->lock);
    INIT_LIST_HEAD(&desc_send->list_desc_head);
    desc_send->flags = 0;

    return desc_send;
};

struct rpc_desc_recv* rpc_desc_recv_alloc(void){
    struct rpc_desc_recv* desc_recv;

    desc_recv = kmem_cache_alloc(rpc_desc_recv_cachep, GFP_ATOMIC);
    if(!desc_recv)
        return NULL;

    atomic_set(&desc_recv->seq_id, 0);
    atomic_set(&desc_recv->nbunexpected, 0);
    INIT_LIST_HEAD(&desc_recv->list_desc_head);
    INIT_LIST_HEAD(&desc_recv->list_provided_head);
    INIT_LIST_HEAD(&desc_recv->list_signal_head);
    desc_recv->iter = NULL;
    desc_recv->iter_provided = NULL;
    desc_recv->received_packets = 0;
    desc_recv->flags = 0;

    return desc_recv;
};

int init_rpc(void)
{
    int i, res;
    struct rpc_service *rpc_undef_service;

    printk("HCC: init_rpc");

    rpc_desc_cachep = kmem_cache_create("rpc_desc",
                                        sizeof(struct rpc_desc),
                                        0, 0, NULL);
    if(!rpc_desc_cachep)
        return -ENOMEM;

    rpc_desc_send_cachep = kmem_cache_create("rpc_desc_send",
                                             sizeof(struct rpc_desc_send),
                                             0, 0, NULL);
    if(!rpc_desc_send_cachep)
        return -ENOMEM;

    rpc_desc_recv_cachep = kmem_cache_create("rpc_desc_recv",
                                             sizeof(struct rpc_desc_recv),
                                             0, 0, NULL);
    if(!rpc_desc_recv_cachep)
        return -ENOMEM;

    rpc_tx_elem_cachep = kmem_cache_create("rpc_tx_elem",
                                           sizeof(struct rpc_tx_elem),
                                           0, 0, NULL);
    if(!rpc_tx_elem_cachep)
        return -ENOMEM;


    rpc_desc_elem_cachep = kmem_cache_create("rpc_desc_elem",
                                             sizeof(struct rpc_desc_elem),
                                             0, 0, NULL);
    if(!rpc_desc_elem_cachep)
        return -ENOMEM;

    __rpc_synchro_cachep = kmem_cache_create("__rpc_synchro",
                                             sizeof(struct __rpc_synchro),
                                             0, 0, NULL);
    if(!__rpc_synchro_cachep)
        return -ENOMEM;

    memset(rpc_mask, 0, sizeof(rpc_mask));

    rpc_services = kmalloc(sizeof(*rpc_services)*(RPCID_MAX+1),
                           GFP_KERNEL);
    if(!rpc_services)
        return -ENOMEM;

    rpc_undef_service = rpc_service_init(RPC_UNDEF,
                                         RPC_TARGET_NODE,
                                         RPC_HANDLER_KTHREAD_VOID,
                                         NULL,
                                         rpc_undef_handler, 0);


    for(i=0;i<RPCID_MAX;i++)
        rpc_services[i] = rpc_undef_service;

    for_each_possible_cpu(i){
        INIT_LIST_HEAD(&per_cpu(rpc_desc_trash, i));
    };

    rpc_desc_id = 1;

    for(i=0; i<HCC_MAX_NODES; i++){
        desc_srv[i] = hashtable_new(32);
        if(!desc_srv[i])
            return -ENOMEM;

        lockdep_set_class(&desc_srv[i]->lock, &rpc_desc_srv_lock_key);
        rpc_desc_done_id[i] = 0;
        spin_lock_init(&rpc_desc_done_lock[i]);
    };

    desc_clt = hashtable_new(32);
    if(!desc_clt)
        return -ENOMEM;

    lockdep_set_class(&desc_clt->lock, &rpc_desc_clt_lock_key);

    for (i = 0; i < HCC_MAX_NODES; i++) {
        rpc_link_send_seq_id[i] = 1;
        rpc_link_send_ack_id[i] = 0;
        rpc_link_recv_seq_id[i] = 1;
    }

    res = worker_pool_init();
    if(res)
        return res;

    res = comlayer_init();
    if(res)
        return res;

    res = rpclayer_init();
    if(res)
        return res;

//    res = rpc_monitor_init();
//    if(res)
//        return res;

    printk(KERN_INFO "HCC: GRPC initialisation done\n");

    return 0;
}