/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
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
 *     * Neither the name of Intel Corporation nor the names of its
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/queue.h>
#include <errno.h>
#include <netinet/ip.h>

#include <sys/socket.h>
#include <sys/fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <time.h> 
#include <sys/un.h>
#include <pthread.h> 

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_byteorder.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_atomic.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_malloc.h>
#include <rte_fbk_hash.h>
#include <rte_string_fns.h>

#include "common.h"
#include "args.h"
#include "init.h"

/*
 * When doing reads from the NIC or the client queues,
 * use this batch size
 */
#define PACKET_READ_SIZE 32


enum task_action{
	INIT = 0,
	WAITING,
	CMD_IN,
	ZOMBIE,	/* Not implemented yet */
};

struct thread_data
{
	int thread_id;
	int *socket;
	int  action;
	char command[MSG_SIZE];
	pthread_mutex_t mutex_cmd;
};

struct thread_data thread_data_array[MAX_CLIENTS];
static unsigned thread_count = 0;

/*
 * Local buffers to put packets in, used to send packets in bursts to the
 * clients
 */
struct client_rx_buf {
	struct rte_mbuf *buffer[PACKET_READ_SIZE];
	uint16_t count;
};

/* One buffer per client rx queue - dynamically allocate array */
static struct client_rx_buf *cl_rx_buf;

static const char *
get_printable_mac_addr(uint8_t port)
{
	static const char err_address[] = "00:00:00:00:00:00";
	static char addresses[RTE_MAX_ETHPORTS][sizeof(err_address)];

	if (unlikely(port >= RTE_MAX_ETHPORTS))
		return err_address;
	if (unlikely(addresses[port][0]=='\0')){
		struct ether_addr mac;
		rte_eth_macaddr_get(port, &mac);
		snprintf(addresses[port], sizeof(addresses[port]),
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
				mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
	}
	return addresses[port];
}

/*
 * This function displays the recorded statistics for each port
 * and for each client. It uses ANSI terminal codes to clear
 * screen when called. It is called from a single non-master
 * thread in the server process, when the process is run with more
 * than one lcore enabled.
 */
static void
do_stats_display(void)
{
	unsigned i, j;
	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
	uint64_t port_tx[RTE_MAX_ETHPORTS], port_tx_drop[RTE_MAX_ETHPORTS];
	uint64_t client_tx[MAX_CLIENTS], client_tx_drop[MAX_CLIENTS];

	/* to get TX stats, we need to do some summing calculations */
	memset(port_tx, 0, sizeof(port_tx));
	memset(port_tx_drop, 0, sizeof(port_tx_drop));
	memset(client_tx, 0, sizeof(client_tx));
	memset(client_tx_drop, 0, sizeof(client_tx_drop));

	for (i = 0; i < num_clients; i++){
		const volatile struct tx_stats *tx = &ports->tx_stats[i];
		for (j = 0; j < ports->num_ports; j++){
			/* assign to local variables here, save re-reading volatile vars */
			const uint64_t tx_val = tx->tx[ports->id[j]];
			const uint64_t drop_val = tx->tx_drop[ports->id[j]];
			port_tx[j] += tx_val;
			port_tx_drop[j] += drop_val;
			client_tx[i] += tx_val;
			client_tx_drop[i] += drop_val;
		}
	}

	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("PORTS\n");
	printf("-----\n");
	for (i = 0; i < ports->num_ports; i++)
		printf("Port %u: '%s'\t", (unsigned)ports->id[i],
				get_printable_mac_addr(ports->id[i]));
	printf("\n\n");
	for (i = 0; i < ports->num_ports; i++){
		printf("Port %u - rx: %9"PRIu64"\t"
				"tx: %9"PRIu64"\n",
				(unsigned)ports->id[i], ports->rx_stats.rx[i],
				port_tx[i]);
	}

	printf("\nCLIENTS\n");
	printf("-------\n");
	for (i = 0; i < num_clients; i++){
		const unsigned long long rx = clients[i].stats.rx;
		const unsigned long long rx_drop = clients[i].stats.rx_drop;
		printf("Client %2u - rx: %9llu, rx_drop: %9llu\n"
				"            tx: %9"PRIu64", tx_drop: %9"PRIu64"\n",
				i, rx, rx_drop, client_tx[i], client_tx_drop[i]);
	}

	printf("\n");
}

/*
 * The function called from each non-master lcore used by the process.
 * The test_and_set function is used to randomly pick a single lcore on which
 * the code to display the statistics will run. Otherwise, the code just
 * repeatedly sleeps.
 */
static int
sleep_lcore(__attribute__((unused)) void *dummy)
{
	/* Used to pick a display thread - static, so zero-initialised */
	static rte_atomic32_t display_stats;

	/* Only one core should display stats */
	if (rte_atomic32_test_and_set(&display_stats)) {
		const unsigned sleeptime = 1;
		RTE_LOG(INFO, APP, "Core %u displaying statistics\n", rte_lcore_id());
		
		/* Longer initial pause so above printf is seen */
		sleep(sleeptime * 3);

		/* Loop forever: sleep always returns 0 or <= param */
		while (sleep(sleeptime) <= sleeptime)
			do_stats_display();
	}
	return 0;
}

/*
 * Function to set all the client statistic values to zero.
 * Called at program startup.
 */
static void
clear_stats(void)
{
	unsigned i;

	for (i = 0; i < num_clients; i++)
		clients[i].stats.rx = clients[i].stats.rx_drop = 0;
}

/*
 * send a burst of traffic to a client, assuming there are packets
 * available to be sent to this client
 
static void
flush_rx_queue(uint16_t client)
{
	uint16_t j;
	struct client *cl;

	if (cl_rx_buf[client].count == 0)
		return;

	cl = &clients[client];
	if (rte_ring_enqueue_bulk(cl->rx_q, (void **)cl_rx_buf[client].buffer,
			cl_rx_buf[client].count) != 0){
		for (j = 0; j < cl_rx_buf[client].count; j++)
			rte_pktmbuf_free(cl_rx_buf[client].buffer[j]);
		cl->stats.rx_drop += cl_rx_buf[client].count;
	}
	else
		cl->stats.rx += cl_rx_buf[client].count;

	cl_rx_buf[client].count = 0;
}
 */

/*
 * marks a packet down to be sent to a particular client process
 
static inline void
enqueue_rx_packet(uint8_t client, struct rte_mbuf *buf)
{
	cl_rx_buf[client].buffer[cl_rx_buf[client].count++] = buf;
}
 */

/*
 * This function takes a group of packets and routes them
 * individually to the client process. Very simply round-robins the packets
 * without checking any of the packet contents.
 
static void
process_packets(uint32_t port_num __rte_unused,
		struct rte_mbuf *pkts[], uint16_t rx_count)
{
	uint16_t i;
	uint8_t client = 0;

	for (i = 0; i < rx_count; i++) {
		enqueue_rx_packet(client, pkts[i]);

		if (++client == num_clients)
			client = 0;
	}

	for (i = 0; i < num_clients; i++)
		flush_rx_queue(i);
}
 */


/*
 * Function called by the master lcore of the DPDK process.
 *
static void
do_packet_forwarding(void)
{
	// indexes the port[] array 
	unsigned port_num = 0; 

	for (;;) {
		struct rte_mbuf *buf[PACKET_READ_SIZE];
		uint16_t rx_count;

		// read a port
		rx_count = rte_eth_rx_burst(ports->id[port_num], 0, \
				buf, PACKET_READ_SIZE);
		ports->rx_stats.rx[port_num] += rx_count;

		// Now process the NIC packets read 
		if (likely(rx_count > 0))
			process_packets(port_num, buf, rx_count);

		// move to next port 
		if (++port_num == ports->num_ports)
			port_num = 0;
	}
}
 */

/*
 * This will handle each client comm
 * */
static void *client_handler(void * arg)
{
	struct thread_data *my_data;
	my_data = (struct thread_data *) arg;
	//my_data->thread_id = pthread_self();

	RTE_LOG(DEBUG, APP, "my data thread_id = %d\n", (my_data->thread_id));
	RTE_LOG(DEBUG, APP, "my data sock = %d\n", *(my_data->socket));
	RTE_LOG(DEBUG, APP, "my data action = %d\n", my_data->action);
	RTE_LOG(DEBUG, APP, "Tracking curr thread count = %d\n", thread_count);
	
	//Get the socket descriptor
	int sock = *(my_data->socket);
	int read_size, error;
	char client_message[MSG_SIZE];
    
	RTE_LOG(DEBUG, APP, "client handling socket = %d\n", sock);	
	
	read_size=0;
	while (1)
	{
		
		memset(client_message, '\0', MSG_SIZE);
		usleep(10);
		

		/* check if there's command in queue*/
		if (my_data->action == 2000)
		{
			pthread_mutex_lock ( &(my_data->mutex_cmd));
			/* command in queue executed*/
			my_data->action = 1000;
			strcpy (client_message, my_data->command);
			error = send(sock, client_message, MSG_SIZE, MSG_NOSIGNAL);
			if (error  == -1)
			{
	       			perror("send 1");
				break;
			}
			memset(client_message, '\0', MSG_SIZE);
			pthread_mutex_unlock (&(my_data->mutex_cmd));
		}
		else
		{
			continue;
		}


		if ((read_size = recv(sock , client_message , MSG_SIZE , 0)) > 0) 
		{	
			client_message[read_size] = '\n';
			RTE_LOG(DEBUG, APP, "Reply from client: %s", client_message);
			fflush(stdout);
			printf("main > ");
		} 
		else 
		{
			if (read_size < 0)
			{
				perror("ERR: Receive Error: No Reply\n");
			}
			else 
				RTE_LOG(INFO, APP, "client closed connection\n");
			break;
		}
		
	

	}
       
	if(read_size == 0)
	{
		RTE_LOG(INFO, APP, "Client Disconnected\n");
	}
	else if(read_size < 0)
	{
		perror("ERR: Receive Error: no reply\n");
	}
         
	//Free the socket pointer
	close( *(my_data->socket));
	free(my_data->socket);
	pthread_mutex_unlock (&(my_data->mutex_cmd));
	thread_count--;
	/* before exiting, reset values */ 
	my_data->action = -1;
	my_data->thread_id = 0;
	pthread_mutex_destroy( &(my_data->mutex_cmd));
	printf("\nmain > ");
	
	fflush(stdout);
	pthread_exit(NULL);
	return 0;
}

/*
 * This will handle connection for each client
 * */
static void *connection_handler(void *socket_desc)
{
	//Get the socket descriptor
	int s1 = *(int*)socket_desc;
	unsigned int addr_len;

	int s2, *s3;
	pthread_t threads[MAX_CLIENTS];
	struct sockaddr_un remote;

	char client_message[MSG_SIZE];
	int read_size;

	addr_len = sizeof(remote);
     
	//Send some messages to the client
    while (1)
	{
		RTE_LOG(INFO, APP, "Accept conection Thread running\n");
		s2 = accept(s1, (struct sockaddr *)&remote, &addr_len);

		sprintf(client_message, "VPP server ");
		write(s2 , client_message , MSG_SIZE);
		memset(client_message, '\0', MSG_SIZE);
		/* set an invalid client id*/
		int client_id = 99999; 

		read_size = recv(s2 , client_message , MSG_SIZE , 0);
		if(read_size == 0)
		{
			RTE_LOG(INFO, APP, "Read Empty: Assume Client disconnected\n");
			fflush(stdout);
		}
		else if(read_size == -1)
		{
			perror("ERR: recv failed");
		}
		else
		{
			client_message[read_size] = '\0';
			RTE_LOG(DEBUG, APP, "RCV client message: %s\n", client_message);
			client_id = atoi (client_message);
			RTE_LOG(INFO, APP, "Client ID %d Thread running\n", client_id);

		}
		memset(client_message, '\0', MSG_SIZE);	


	        s3 = malloc(1);
	        *s3 = s2;

		thread_data_array[client_id].thread_id = client_id;
		thread_data_array[client_id].socket = s3;
		RTE_LOG(DEBUG, APP, "Connection Handler socket id: %d\n", *(thread_data_array[client_id].socket));
		/* init thread data and rest action to ready for command input*/
		thread_data_array[client_id].action = 1000;
		memset(thread_data_array[client_id].command, '\0', MSG_SIZE);
   		pthread_mutex_init(& (thread_data_array[client_id].mutex_cmd), NULL);

	        if( pthread_create( &threads[client_id], 
				NULL ,  
				client_handler, 
				(void*) &thread_data_array[client_id]) < 0)
	        {
	            perror("could not create client thread");
	            break;
	        }
         	thread_count++;
	        printf("Handler assigned\n");
			RTE_LOG(INFO, APP, "Handler starts for Client ID %d\n", client_id);
        }
	if (s2 < 0)
	{
		perror("accept failed");
		return 0;
	}
	pthread_exit(NULL);
	return 0;
}

int
main(int argc, char *argv[])
{

	int s1, *sock_desc, len;
	struct sockaddr_un local;
	pthread_t connection_handling_thread;

	/* initialise the system */
	if (init(argc, argv) < 0 )
		return -1;
	
	/* Force stdout to be a line buffer*/
	setvbuf(stdout, NULL, _IOLBF, 0);
	
	RTE_LOG(INFO, APP, "Finished Process Init.\n");

	cl_rx_buf = calloc(num_clients, sizeof(cl_rx_buf[0]));

	/* clear statistics */
	clear_stats();

	/* put all other cores to sleep bar master */
	rte_eal_mp_remote_launch(sleep_lcore, NULL, SKIP_MASTER);

	unlink(SOCK_PATH);
	if ((s1 = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) 
	{
		perror("socket");
		exit(1);
	}

	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, SOCK_PATH);
	unlink(local.sun_path);
	len = strlen(local.sun_path) + sizeof(local.sun_family);
	if (bind(s1, (struct sockaddr *)&local, len) == -1) 
	{
		perror("bind");
		exit(1);
	}

	if (listen(s1, 5) == -1) {
		perror("listen");
		exit(1);
	}

	sock_desc = malloc(1);
        *sock_desc = s1;

	/*Create connection handling thread */
	if( pthread_create( &connection_handling_thread, NULL ,  connection_handler , (void*) sock_desc) < 0)
	{
	            perror("could not create connection handler thread");
	            return -1;
	}

	const char delim[2] = ";";
	char *token;
	int client_id;
	char command_input [MSG_SIZE];

	sleep (10);

	memset(command_input, '\0', MSG_SIZE);
	while (1)
	{
		/* Output our prompt */
		fputs("main > ", stdout);
		fgets(command_input, MSG_SIZE, stdin);
		
		/* check input command requirements */
		if (strlen (command_input) < 4) 
			continue;
		if (strpbrk(";", command_input)== NULL)
			continue;			

		token = strtok(command_input,delim);
		printf( "token %s\n", token );
		client_id = atoi (token);
		printf( "client id %d\n", client_id );
		printf( "client action %d\n", thread_data_array[client_id].action );
		/* Check if client being initialize, waiting for command*/
		if (thread_data_array[client_id].action == 1000)
		{
			token = strtok(NULL, delim);
			printf( "token %s\n", token );
			/* Command for submitted*/
			thread_data_array[client_id].action =2000;
			pthread_mutex_lock (&(thread_data_array[client_id].mutex_cmd));
			strcpy( thread_data_array[client_id].command, token);
			pthread_mutex_unlock (&(thread_data_array[client_id].mutex_cmd));
		}
		printf("main > ");
	}

    printf("main end.\n");
	return 0;

}
