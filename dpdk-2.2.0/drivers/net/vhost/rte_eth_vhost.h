/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2015 IGEL Co., Ltd.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of IGEL Co., Ltd. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RTE_ETH_AF_PACKET_H_
#define _RTE_ETH_AF_PACKET_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <rte_virtio_net.h>

int
rte_eth_from_vhost(const char *name, int index,
		     char *iface_name,
		     const unsigned numa_node,
			 struct rte_mempool *mb_pool); 

/**
 * The function convert specified port_id to virtio device structure.
 * The retured device can be used for vhost library APIs.
 * To use vhost library APIs and vhost PMD parallely, below API should
 * not be called, because the API will be called by vhost PMD.
 * - rte_vhost_driver_session_start()
 * Once a device is managed by vhost PMD, below API should not be called.
 * - rte_vhost_driver_unregister()
 * To unregister the device, call Port Hotplug APIs.
 *
 * @param port_id
 *  port number
 * @return
 *  virtio net device structure corresponding to the specified port
 *  NULL will be returned in error cases.
 */
struct virtio_net *rte_eth_vhost_portid2vdev(uint16_t port_id);

#ifdef __cplusplus
}
#endif

#endif
