/*
 * accept.c
 *
 *  Created on: Nov 18, 2015
 *      Author: root
 */
#include "accept.h"
#include "util.h"
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include "xstring.h"
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdlib.h>
#include <poll.h>
#include "work.h"
#include <unistd.h>

void run_accept(struct accept_param* param) {
	static int one = 1;
	static unsigned char onec = 1;
	struct timeval timeout;
	timeout.tv_sec = 60;
	timeout.tv_usec = 0;
	struct pollfd spfd;
	spfd.events = POLLIN;
	spfd.revents = 0;
	spfd.fd = param->server_fd;
	while (1) {
		struct conn* c = xmalloc(sizeof(struct conn));
		memset(&c->addr, 0, sizeof(struct sockaddr));
		c->addrlen = sizeof(struct sockaddr);
		c->readBuffer = NULL;
		c->readBuffer_size = 0;
		c->readBuffer_checked = 0;
		c->writeBuffer = NULL;
		c->writeBuffer_size = 0;
		if (poll(&spfd, 1, -1) < 0) {
			errlog(param->logsess, "Error while polling server: %s", strerror(errno));
			xfree(c);
			continue;
		}
		if ((spfd.revents ^ POLLIN) != 0) {
			errlog(param->logsess, "Error after polling server: %i (poll revents), closing server!", spfd.revents);
			xfree(c);
			close(param->server_fd);
			break;
		}
		spfd.revents = 0;
		int cfd = accept(param->server_fd, &c->addr, &c->addrlen);
		if (cfd < 0) {
			if (errno == EAGAIN) continue;
			errlog(param->logsess, "Error while accepting client: %s", strerror(errno));
			xfree(c);
			continue;
		}
		c->fd = cfd;
		if (setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout))) printf("Setting recv timeout failed! %s", strerror(errno));
		if (setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, (char *) &timeout, sizeof(timeout))) printf("Setting send timeout failed! %s", strerror(errno));
		if (setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (void *) &one, sizeof(one))) printf("Setting TCP_NODELAY failed! %s", strerror(errno));
		if (fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK) < 0) {
			errlog(param->logsess, "Setting O_NONBLOCK failed! %s, this error cannot be recovered, closing client.", strerror(errno));
			close(cfd);
			continue;
		}
		struct work_param* work = param->works[rand() % param->works_count];
		if (add_collection(work->conns, c)) { // TODO: send to lowest load, not random
			if (errno == EINVAL) {
				errlog(param->logsess, "Too many open connections! Closing client.");
			} else {
				errlog(param->logsess, "Collection failure! Closing client. %s", strerror(errno));
			}
			close(cfd);
			continue;
		}
		if (write(work->pipes[1], &onec, 1) < 1) {
			errlog(param->logsess, "Failed to write to wakeup pipe! Things may slow down. %s", strerror(errno));
		}
	}
}