/* SPDX-License-Identifier: GPL-2.0 */

#include <assert.h>
#include <netinet/in.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/cmpxchg.h"
#include "shared/lk/err.h"
#include "shared/lk/limits.h"
#include "shared/lk/list.h"
#include "shared/lk/math64.h"
#include "shared/lk/rcupdate.h"
#include "shared/lk/slab.h"
#include "shared/lk/types.h"
#include "shared/lk/wait.h"

#include "shared/format-msg.h"
#include "shared/fs_info.h"
#include "shared/lk/wait.h"
#include "shared/log.h"
#include "shared/map.h"
#include "shared/msg.h"
#include "shared/parse.h"

struct ngnfs_devd_addrs {
	u8 nr_addrs;
	struct sockaddr_in addrs[];
};

struct ngnfs_map_addr_head {
	struct list_head head;
	struct sockaddr_in addr;
};

/* Eventually this will have more than one map. */
struct ngnfs_maps {
	struct ngnfs_devd_map devd_map;
};

/*
 * The maps are updated using RCU. Add an RCU wrapper for the maps
 * struct so we can use lightweight kfree_rcu() instead of expensive
 * synchronize_rcu() when the maps change.
 */
struct ngnfs_maps_rcu {
	struct rcu_head rcu;
	struct ngnfs_maps maps;
};

struct ngnfs_map_info {
	struct wait_queue_head updates_waitq;
	struct ngnfs_maps_rcu *maps_rcu;
};

/* Parse the IPv4 addr:port in str and add it to addr_list. */
int ngnfs_map_append_addr(u8 *nr_addrs, struct list_head *addr_list, char *str)
{
	struct ngnfs_map_addr_head *ahead;
	int ret;

	if (*nr_addrs == U8_MAX) {
		log("too many -d addresses specified, exceeded limit of %u", U8_MAX);
		return -EINVAL;
	}

	ahead = malloc(sizeof(struct ngnfs_map_addr_head));
	if (!ahead)
		return -ENOMEM;

	ret = parse_ipv4_addr_port(&ahead->addr, str);
	if (ret < 0) {
		log("error parsing -d address");
		goto out;
	}

	list_add_tail(&ahead->head, addr_list);
	(*nr_addrs)++;
	return ret;
out:
	free(ahead);
	return ret;
}

void ngnfs_map_free_addrs(struct list_head *addr_list)
{
	struct ngnfs_map_addr_head *ahead;
	struct ngnfs_map_addr_head *tmp;

	list_for_each_entry_safe(ahead, tmp, addr_list, head) {
		list_del_init(&ahead->head);
		free(ahead);
	}
}

static size_t get_maps_result_size(struct ngnfs_maps *maps)
{
	u8 nr = le64_to_cpu(maps->devd_map.nr_addrs);

	return offsetof(struct ngnfs_msg_get_maps_result, devd_map.addrs[nr]);
}

static void copy_maps(struct ngnfs_maps *dst, struct ngnfs_maps *src)
{
	u8 nr = le64_to_cpu(src->devd_map.nr_addrs);

	memcpy(&dst->devd_map, &src->devd_map, offsetof(struct ngnfs_maps, devd_map.addrs[nr]));
}

static struct ngnfs_maps *msg_to_maps(struct ngnfs_msg_get_maps_result *gmr)
{
	return (struct ngnfs_maps *) &gmr->devd_map;
}

struct ngnfs_msg_get_maps_result *ngnfs_maps_to_msg(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_msg_get_maps_result *msg;
	struct ngnfs_maps *maps;
	size_t alloced;
	size_t sz;
	int ret;

	/* XXX ? for Zach: This always fails the first time - is that intentional? */
        alloced = 0;
        msg = NULL;
        for (;;) {
                rcu_read_lock();
                maps = &rcu_dereference(minf->maps_rcu)->maps;
                sz = get_maps_result_size(maps);
                if (alloced == sz)
                        copy_maps(msg_to_maps(msg), maps);
                rcu_read_unlock();

                if (alloced == sz) {
                        ret = 0;
                        break;
                }

                kfree(msg);
                msg = kmalloc(sz, GFP_NOFS);
                if (!msg) {
                        ret = -ENOMEM;
                        break;
                }
                alloced = sz;
        }

	if (ret < 0)
		return ERR_PTR(ret);

	return msg;
}

static int update_maps(struct ngnfs_fs_info *nfi, struct ngnfs_maps *new_maps)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_maps_rcu *old_rmaps;
	struct ngnfs_maps_rcu *new_rmaps;
	struct ngnfs_maps_rcu *tmp;
	u8 nr = le64_to_cpu(new_maps->devd_map.nr_addrs);

	/* Allocate the RCU wrapper for the new maps */
	new_rmaps = kmalloc(offsetof(struct ngnfs_maps_rcu, maps.devd_map.addrs[nr]), GFP_NOFS);
	if (!new_rmaps)
		return -ENOMEM;

	copy_maps(&new_rmaps->maps, new_maps);

	/* Use cmpxchg to atomically update the pointer to the RCU-wrapped maps */
	do {
		rcu_read_lock();
		old_rmaps = rcu_dereference(minf->maps_rcu);
		rcu_read_unlock();

		tmp = unrcu_pointer(cmpxchg(&minf->maps_rcu, old_rmaps, new_rmaps));
	} while (tmp != old_rmaps);

	wake_up(&minf->updates_waitq);

	if (old_rmaps)
		kfree_rcu(&old_rmaps->rcu);

	return 0;
}

struct ngnfs_ipv4_addr addr_to_map(struct sockaddr_in *src_addr)
{
	struct ngnfs_ipv4_addr maddr = { };

	maddr.addr = cpu_to_le32(src_addr->sin_addr.s_addr);
	maddr.port = cpu_to_le16(src_addr->sin_port);

	return maddr;
}

struct sockaddr_in map_to_addr(struct ngnfs_ipv4_addr *src_addr)
{
	struct sockaddr_in addr = { };

	addr.sin_addr.s_addr = le32_to_cpu(src_addr->addr);
	addr.sin_port = le16_to_cpu(src_addr->port);
	addr.sin_family = AF_INET;

	return addr;
}

/*
 * Caller is responsible for noticing if the maps have changed and restarting
 * the transaction. TODO: how?
 */
int ngnfs_map_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr)
{
	struct ngnfs_maps_rcu *nm;
	u32 rem;

	rcu_read_lock();

	nm = rcu_dereference(nfi->map_info->maps_rcu);
	div_u64_rem(bnr, le64_to_cpu(nm->maps.devd_map.nr_addrs), &rem);
	*addr = map_to_addr(&nm->maps.devd_map.addrs[rem]);

	rcu_read_unlock();

	return 0;
}

/*
 * Request initial maps from mapd server at addr and wait until they are
 * received.
 */
int ngnfs_maps_request(struct ngnfs_fs_info *nfi, struct sockaddr_in *addr)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_msg_get_maps gm;
	struct ngnfs_msg_desc mdesc;
	int ret;

	gm.map_id = 0; /* XXX future */

	mdesc.type = NGNFS_MSG_GET_MAPS;
	mdesc.addr = addr;
	mdesc.ctl_buf = &gm;
	mdesc.ctl_size = sizeof(gm);
	mdesc.data_page = NULL;
	mdesc.data_size = 0;

	ret = ngnfs_msg_send(nfi, &mdesc);
	if (ret < 0)
		return ret;

	/* TODO: Needs a timeout or way to return an error */
	wait_event(&minf->updates_waitq, minf->maps_rcu != NULL);

	return ret;
}

/*
 * Read the maps sent from the mapd server and load them.
 */
static int map_get_maps_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_maps_result *gmr = mdesc->ctl_buf;
	int ret;

	if (gmr->err < 0)
		return ngnfs_msg_err(gmr->err);

	ret = update_maps(nfi, msg_to_maps(gmr));

	return ret;
}

/*
 * It's surprisingly ok to have duplicate addresses in the array
 * currently because we're not actually mapping the fs scoped block
 * numbers to device block numbers.  Each device must be able to store
 * the entire block space.
 */
static struct ngnfs_maps *addr_list_to_maps(struct list_head *list, u8 nr)
{
	struct ngnfs_map_addr_head *ahead;
	struct ngnfs_maps *maps;
	struct ngnfs_devd_map *da;
	struct ngnfs_ipv4_addr *addr;
	int ret;

	if (nr == 0)
		return ERR_PTR(-EINVAL);

	maps = kmalloc(offsetof(struct ngnfs_maps, devd_map.addrs[nr]), GFP_NOFS);
	if (!maps) {
		ret = -ENOMEM;
		goto out;
	}

	da = &maps->devd_map;
	da->nr_addrs = cpu_to_le64(nr);

	addr = &da->addrs[0];
	list_for_each_entry(ahead, list, head) {
		if (nr-- == 0) {
			ret = -EINVAL;
			goto out;
		}

		*addr = addr_to_map(&ahead->addr);
		addr++;
	}

	if (nr != 0) {
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
out:
	if (ret < 0) {
		kfree(maps);
		return ERR_PTR(ret);
	}
	return maps;
}

int ngnfs_map_addrs_to_maps(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
{
	struct ngnfs_maps *maps;

	if (nr == 0)
		return -EINVAL;

	maps = addr_list_to_maps(list, nr);
	if (maps < 0)
		return PTR_ERR(maps);

	return update_maps(nfi, maps);
}

void ngnfs_map_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;

	if (minf) {
		kfree(minf->maps_rcu);
		kfree(minf);
		nfi->map_info = NULL;
	}
}

int ngnfs_map_setup(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf;
	int ret;

	minf = kzalloc(sizeof(struct ngnfs_map_info), GFP_NOFS);
	if (!minf) {
		ret = -ENOMEM;
		goto out;
	}

	init_waitqueue_head(&minf->updates_waitq);
	nfi->map_info = minf;

	ret = 0;
out:
	if (ret < 0)
		ngnfs_map_destroy(nfi);
	return ret;
}

void ngnfs_map_client_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_MAPS_RESULT, map_get_maps_result);
}

int ngnfs_map_client_setup(struct ngnfs_fs_info *nfi, struct sockaddr_in *mapd_server_addr)
{
	int ret;

	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_MAPS_RESULT, map_get_maps_result);
	if (ret < 0)
		return ret;

	ret = ngnfs_maps_request(nfi, mapd_server_addr);
	if (ret < 0)
		ngnfs_map_client_destroy(nfi);

	return ret;
}
