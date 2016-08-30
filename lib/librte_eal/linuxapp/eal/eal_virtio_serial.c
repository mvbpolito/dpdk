#ifdef RTE_LIBRTE_VIRTIO_SERIAL /* hide it from coverage */

#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>

#include <rte_log.h>
#include <rte_ethdev.h>

#include "eal_private.h"

#define VIRTIO_SERIAL_PATH "/dev/virtio-ports/dpdk"

struct virtio_args {
	int fd; /* file descriptor */
};

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

	RTE_LOG(DEBUG, EAL, "%s()\n", __FUNCTION__);

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
		err = rte_eth_add_bypass_to_ring(p_old, p_new, 1);
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

static void *
rte_virtio_serial_handler(void *args_)
{
	struct virtio_args *args = (struct virtio_args *) args_;

	int ret;
	char buf[512] = {0};

	char * buf_ptr = &buf[0];
	int n = 0;

	for (;;) {
		ret = read(args->fd, buf, sizeof(buf));
		if (ret == -1) {
			RTE_LOG(ERR, EAL, "Failed to read from virtio device\n");
			continue;
		}

		buf_ptr += ret;
		n += ret;

		if (n > 0) {
			ret = process_host_request(buf, n);
			if (ret == 0) {
				char ok[] = "OK";
				ret = safewrite(args->fd, ok, sizeof(ok), 0);
				if(ret != sizeof(ok)) {
					RTE_LOG(ERR, EAL, "Fail to write ok virtio-serial\n");
				}
			} else {
				char nok[] = "NOK";
				ret = safewrite(args->fd, nok, sizeof(nok), 0);
				if(ret != sizeof(nok)) {
					RTE_LOG(ERR, EAL, "Fail to write ok virtio-serial\n");
				}
			}
		}
	}

	return NULL;
}

int rte_eal_virtio_init(void)
{
	int fd;

	pthread_t tid;
	pthread_attr_t attr;

	/* open device and configure it as async */
	fd = open(VIRTIO_SERIAL_PATH, O_RDWR);
	if (fd == -1) {
		RTE_LOG(ERR, EAL, "Cannot open '%s'!\n", VIRTIO_SERIAL_PATH);
		return -1;
	}

	/* read all the data that is in the device and thow it away */
	for(;;) {
		fd_set set;
		struct timeval tv;
		FD_ZERO(&set);
		FD_SET(fd, &set);
		char buf[512] = {0};
		int ret;

		tv.tv_usec = 0;
		tv.tv_sec = 0;

		ret = select(1, &set, NULL, NULL, &tv);
		if (ret == -1) {
			RTE_LOG(ERR, EAL, "select() failed in virtio device\n");
			break;
		} else if (ret == 0) {
			RTE_LOG(DEBUG, EAL, "virtio_serial nothing else to clean\n");
			break;
		}

		ret = read(fd, buf, sizeof(buf));
		if (ret == -1) {
			RTE_LOG(ERR, EAL, "Failed to read from virtio device\n");
			break;
		}

		RTE_LOG(DEBUG, EAL, "virtio_serial clean loop\n");
	}

	struct virtio_args *args = malloc(sizeof(*args));
	args->fd = fd;
	pthread_attr_init(&attr);

	pthread_create(&tid, &attr, rte_virtio_serial_handler, (void *)args);

	return 0;
}

#endif
