/* SPDX-License-Identifier: GPL-2.0 */

#include <assert.h>
#include <netinet/in.h>
#include <string.h>

#include "shared/lk/err.h"
#include "shared/lk/list.h"
#include "shared/lk/types.h"

#include "shared/format-msg.h"
#include "shared/fs_info.h"
#include "shared/map.h"
#include "shared/msg.h"

#include "mapd/recv.h"

static ssize_t msg_size(struct ngnfs_msg_get_maps_result *gmr)
{
	u8 nr = gmr->devd_map.nr_addrs;

	return offsetof(struct ngnfs_msg_get_maps_result, devd_map.addrs[nr]);
}

/*
 * Receive and respond to a message from the client requesting initial maps on startup.
 */
static int map_get_maps(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_maps_result res = { };
	struct ngnfs_msg_get_maps_result *resp;
	struct ngnfs_msg_desc res_mdesc;
	int ret;

	/* XXX permissions? other checks? */

	resp = ngnfs_maps_to_msg(nfi);
	if (resp < 0) {
		ret = PTR_ERR(resp);
		resp = &res; /* XXX remove when ctl_buf is fixed size again */
	} else {
		ret = 0;
	}

	res_mdesc.type = NGNFS_MSG_GET_MAPS_RESULT;
	res_mdesc.addr = mdesc->addr;
	res_mdesc.ctl_buf = resp;
	res_mdesc.ctl_size = msg_size(resp);
	res_mdesc.data_page = NULL;
	res_mdesc.data_size = 0;

	ret = ngnfs_msg_send(nfi, &res_mdesc);

	if (ret == 0)
		free(resp);

	return ret;
}

void mapd_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_MAPS, map_get_maps);
}

int mapd_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
{
	int ret;

	ret = ngnfs_map_addrs_to_maps(nfi, list, nr);
	if (ret < 0)
		return ret;

	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_MAPS, map_get_maps);
	if (ret < 0)
		mapd_destroy(nfi);

	return ret;
}
