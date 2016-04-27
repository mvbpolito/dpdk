#ifdef RTE_LIBRTE_VIRTIO_SERIAL /* hide it from coverage */

#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>

#include <rte_log.h>
#include <rte_ethdev.h>

#include "eal_private.h"

#define VIRTIO_SERIAL_PATH "/dev/virtio-ports/dpdk"

struct pollfd pollfds;

static void
process_host_request(char * buf, size_t len)
{
	(void) len;
	//printf("*** '%s' *****\n", buf);
	char action[20] = {0};
	char p_old[RTE_ETH_NAME_MAX_LEN] = {0};
	char p_new[RTE_ETH_NAME_MAX_LEN] = {0};

	sscanf(strtok(buf, ","), "action=%s", action);

	if (!strcmp(action, "add")) {
		sscanf(strtok(NULL, ","), "old=%s", p_old);
		sscanf(strtok(NULL, ","), "new=%s", p_new);
		rte_eth_add_slave_to_ring(p_old, p_new);
	} else if (!strcmp(action, "del")) {
		sscanf(strtok(NULL, ","), "old=%s", p_old);
		rte_eth_remove_slave_from_ring(p_old);
	} else {
		RTE_LOG(ERR, EAL, "Bad action received\n");
	}
}

static void
rte_virtio_serial_sigio_handler(int signal)
{
	(void) signal;

	int ret;
	char buf[512] = {0};

	sigset_t mask;
	sigset_t orig_mask;

	sigemptyset (&mask);
	sigaddset (&mask, SIGIO);

	if (sigprocmask(SIG_BLOCK, &mask, &orig_mask) < 0) {
			RTE_LOG(ERR, EAL, "Cannot sigprocmask");
			return;
	}

	/* is there any data? */
	do {
		ret = poll(&pollfds, 1, 0);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1)
		return;

	char * buf_ptr = &buf[0];
	do {
		ret = read(pollfds.fd, buf, sizeof(buf));
		if(ret == -1)
		{
			/* I think logging from an interrupt is not safe */
			RTE_LOG(ERR, EAL, "Failed to read from device\n");
			return;
		}

		buf_ptr += ret;

	} while(ret != 0);

	process_host_request(buf, ret);

	if (sigprocmask(SIG_SETMASK, &orig_mask, NULL) < 0) {
			RTE_LOG(ERR, EAL, "Cannot sigprocmask");
			return;
	}

}

int rte_eal_virtio_init(void)
{
	int fd;
	int ret;
	struct sigaction action;

	/* open device and configure it as async */
	fd = open(VIRTIO_SERIAL_PATH, O_RDWR);
	if(fd == -1)
	{
		RTE_LOG(ERR, EAL, "Cannot open '%s'!\n", VIRTIO_SERIAL_PATH);
		return -1;
	}

	pollfds.fd = fd;
	pollfds.events = POLLIN;

	ret = fcntl(fd, F_SETOWN, getpid());
	if (ret < 0)
	{
		RTE_LOG(ERR, EAL, "Failed to fcntl F_SETOWN\n");
		return -1;
	}
	ret = fcntl(fd, F_GETFL);
	ret = fcntl(fd, F_SETFL, ret | O_ASYNC | O_NONBLOCK);
	if (ret < 0)
	{
		RTE_LOG(ERR, EAL, "Failed to fcntl O_ASYNC\n");
		return -1;
	}

	/* install signal handler that will be called when data arrives */
	action.sa_handler = rte_virtio_serial_sigio_handler;
	action.sa_flags = 0;
	ret = sigemptyset(&action.sa_mask);
	if (ret)
	{
		RTE_LOG(ERR, EAL, "Failed to sigemptyset\n");
		return -1;
	}

	ret = sigaction(SIGIO, &action, NULL);
	if (ret)
	{
		RTE_LOG(ERR, EAL, "Failed to sigaction\n");
		ret = -errno;
	}

	return 0;
}

#endif
