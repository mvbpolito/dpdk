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

static ssize_t safewrite(int fd, const char *buf, size_t count, int eagain_ret)
{
	ssize_t ret, len;
	int flags;
	int nonblock;

	nonblock = 0;
	flags = fcntl(fd, F_GETFL);
	if (flags > 0 && flags & O_NONBLOCK)
		nonblock = 1;

	len = count;
	while (len > 0) {
		ret = write(fd, buf, len);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN) {
				if (nonblock && eagain_ret) {
					return -EAGAIN;
				} else {
					continue;
				}
			}
			return -errno;
		} else if (ret == 0) {
			break;
		} else {
			buf += ret;
			len -= ret;
		}
	}
	return count - len;
}

static int
process_host_request(char * buf, size_t len)
{
	char action[20] = {0};
	char p_old[RTE_ETH_NAME_MAX_LEN] = {0};
	char p_new[RTE_ETH_NAME_MAX_LEN] = {0};
	int err;
	char * str;
	if (len <= 0)
		return -1;

	str = strtok(buf, ",");
	if (str == NULL)
		goto error;

	err = sscanf(str, "action=%s", action);
	if (err != 1)
		goto error;

	if (!strcmp(action, "add")) {
		str = strtok(NULL, ",");
		if (str == NULL)
			goto error;

		err = sscanf(str, "old=%s", p_old);
		if (err != 1)
			goto error;

		err = sscanf(strtok(NULL, ","), "new=%s", p_new);
		if (err != 1)
			goto error;
		err = rte_eth_add_bypass_to_ring(p_old, p_new);
		if (err != 0)
			goto error;
	} else if (!strcmp(action, "del")) {
		str = strtok(NULL, ",");
		err = sscanf(str, "old=%s", p_old);
		if (err != 1)
			goto error;
		err = rte_eth_remove_bypass_from_ring(p_old);
		if (err != 0)
			goto error;
	} else {
		RTE_LOG(ERR, EAL, "Bad action received\n");
		goto error;
	}

	return 0;

error:
	RTE_LOG(ERR, EAL, "process_host_request error\n");
	return -1;
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

	char * buf_ptr = &buf[0];
	int n = 0;

	ret = read(pollfds.fd, buf, sizeof(buf));
	if (ret == -1) {
		/* I think logging from an interrupt is not safe */
		RTE_LOG(ERR, EAL, "Failed to read from device\n");
		return;
	}

	buf_ptr += ret;
	n += ret;

	if (n > 0) {
		ret = process_host_request(buf, n);
		if (ret == 0) {
			char ok[] = "OK";
			ret = safewrite(pollfds.fd, ok, sizeof(ok), 0);
			if(ret != sizeof(ok)) {
				RTE_LOG(ERR, EAL, "Fail to write ok virtio-serial\n");
			}
		} else {
			char nok[] = "NOK";
			ret = safewrite(pollfds.fd, nok, sizeof(nok), 0);
			if(ret != sizeof(nok)) {
				RTE_LOG(ERR, EAL, "Fail to write ok virtio-serial\n");
			}
		}
	}
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
	if (fd == -1)
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
