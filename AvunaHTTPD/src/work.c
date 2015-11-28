/*
 * work.c
 *
 *  Created on: Nov 18, 2015
 *      Author: root
 */

#include "work.h"
#include "accept.h"
#include "xstring.h"
#include <errno.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include "collection.h"
#include "util.h"
#include "streams.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include "http.h"
#include "log.h"
#include "time.h"
#include <arpa/inet.h>
#include <sys/mman.h>
#include <gnutls/gnutls.h>

void closeConn(struct work_param* param, struct conn* conn) {
	if (conn->tls) {
		if (conn->handshaked) {
			gnutls_bye(conn->session, GNUTLS_SHUT_RDWR);
		}
		gnutls_deinit(conn->session);
	}
	close(conn->fd);
	if (rem_collection(param->conns, conn)) {
		errlog(param->logsess, "Failed to delete connection properly! This is bad!");
	}
	if (conn->readBuffer != NULL) xfree(conn->readBuffer);
	if (conn->writeBuffer != NULL) xfree(conn->writeBuffer);
	xfree(conn);
}

void handleRequest(int wfd, struct timespec* stt, struct conn* conn, struct work_param* param, struct request* req) {
	struct response* resp = xmalloc(sizeof(struct response));
	resp->body = NULL;
	resp->atc = 0;
	resp->code = "500 Internal Server Error";
	resp->version = "HTTP/1.1";
	resp->headers = xmalloc(sizeof(struct headers));
	struct reqsess rs;
	rs.wp = param;
	rs.sender = conn;
	rs.response = resp;
	rs.request = req;
	generateResponse(rs);
	size_t rl = 0;
	unsigned char* rda = serializeResponse(rs, &rl);
	struct timespec stt2;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stt2);
	double msp = (stt2.tv_nsec / 1000000.0 + stt2.tv_sec * 1000.0) - (stt->tv_nsec / 1000000.0 + stt->tv_sec * 1000.0);
	const char* mip = NULL;
	char tip[48];
	if (conn->addr.sa_family == AF_INET) {
		struct sockaddr_in *sip4 = (struct sockaddr_in*) &conn->addr;
		mip = inet_ntop(AF_INET, &sip4->sin_addr, tip, 48);
	} else if (conn->addr.sa_family == AF_INET6) {
		struct sockaddr_in6 *sip6 = (struct sockaddr_in6*) &conn->addr;
		mip = inet_ntop(AF_INET6, &sip6->sin6_addr, tip, 48);
	} else if (conn->addr.sa_family == AF_LOCAL) {
		mip = "UNIX";
	} else {
		mip = "UNKNOWN";
	}
	if (mip == NULL) {
		errlog(param->logsess, "Invalid IP Address: %s", strerror(errno));
	}
	acclog(param->logsess, "%s %s %s returned %s took: %f ms", mip, getMethod(req->method), req->path, resp->code, msp);
	ssize_t mtr = conn->tls ? gnutls_record_send(conn->session, rda, rl) : write(wfd, rda, rl);
	if (mtr < 0 && conn->tls ? gnutls_error_is_fatal(mtr) : errno != EAGAIN) {
		closeConn(param, conn);
		conn = NULL;
	} else if (mtr >= rl) {
		//done writing!
	} else {
		unsigned char* stw = rda + mtr;
		rl -= mtr;
		unsigned char* loc = NULL;
		if (conn->writeBuffer == NULL) {
			conn->writeBuffer = xmalloc(rl); // TODO: max upload?
			conn->writeBuffer_size = rl;
			loc = conn->writeBuffer;
		} else {
			conn->writeBuffer_size += rl;
			conn->writeBuffer = xrealloc(conn->writeBuffer, conn->writeBuffer_size);
			loc = conn->writeBuffer + conn->writeBuffer_size - rl;
		}
		memcpy(loc, stw, rl);
	}
	xfree(rda);
	if (!req->atc) xfree(req->path);
	xfree(req->version);
	freeHeaders(req->headers);
	if (req->body != NULL) {
		if (req->body->freeMime) xfree(req->body->mime_type);
		xfree(req->body->data);
		xfree(req->body);
	}
	xfree(req);
	if (!resp->atc) {
		if (resp->body != NULL) {
			if (resp->body->freeMime) xfree(resp->body->mime_type);
			xfree(resp->body->data);
			xfree(resp->body);
		}
		freeHeaders(resp->headers);
	}
	xfree(resp);
}

void run_work(struct work_param* param) {
	if (pipe(param->pipes) != 0) {
		errlog(param->logsess, "Failed to create pipe! %s", strerror(errno));
		return;
	}
	unsigned char wb;
	unsigned char* mbuf = xmalloc(1024);
	char tip[48];
	while (1) {
		pthread_rwlock_rdlock(&param->conns->data_mutex);
		size_t cc = param->conns->count;
		struct pollfd fds[cc + 1];
		struct conn* conns[cc];
		int fdi = 0;
		for (int i = 0; i < param->conns->size; i++) {
			struct conn* conn = param->conns->data[i];
			if (conn != NULL) {
				conns[fdi] = conn;
				fds[fdi].fd = conn->fd;
				fds[fdi].events = POLLIN | ((conn->writeBuffer_size > 0 || (conn->tls && gnutls_record_get_direction(conn->session))) ? POLLOUT : 0);
				fds[fdi++].revents = 0;
				if (fdi == cc) break;
			}
		}
		pthread_rwlock_unlock(&param->conns->data_mutex);
		fds[cc].fd = param->pipes[0];
		fds[cc].events = POLLIN;
		fds[cc].revents = 0;
		int cp = poll(fds, cc + 1, -1);
		if (cp < 0) {
			errlog(param->logsess, "Poll error in worker thread! %s", strerror(errno));
		} else if (cp == 0) continue;
		else if ((fds[cc].revents & POLLIN) == POLLIN) {
			if (read(param->pipes[0], &wb, 1) < 1) errlog(param->logsess, "Error reading from pipe, infinite loop COULD happen here.");
			if (cp-- == 1) continue;
		}
		for (int i = 0; i < cc; i++) {
			int re = fds[i].revents;
			if (re == 0) continue;
			if (conns[i]->tls && !conns[i]->handshaked) {
				int r = gnutls_handshake(conns[i]->session);
				if (gnutls_error_is_fatal(r)) {
					closeConn(param, conns[i]);
					goto cont;
				} else if (r == GNUTLS_E_SUCCESS) {
					conns[i]->handshaked = 1;
				}
				continue;
			}
			if ((re & POLLIN) == POLLIN) {
				int tr = 0;
				ioctl(fds[i].fd, FIONREAD, &tr);
				unsigned char* loc;
				if (conns[i]->readBuffer == NULL) {
					conns[i]->readBuffer = xmalloc(tr); // TODO: max upload?
					conns[i]->readBuffer_size = tr;
					loc = conns[i]->readBuffer;
				} else {
					conns[i]->readBuffer_size += tr;
					conns[i]->readBuffer = xrealloc(conns[i]->readBuffer, conns[i]->readBuffer_size);
					loc = conns[i]->readBuffer + conns[i]->readBuffer_size - tr;
				}
				ssize_t r = 0;
				if (r == 0 && tr == 0) { // nothing to read, but wont block.
					ssize_t x = 0;
					if (conns[i]->tls) {
						x = gnutls_record_recv(conns[i]->session, loc + r, tr - r);
						if (x <= 0 && gnutls_error_is_fatal(x)) {
							closeConn(param, conns[i]);
							conns[i] = NULL;
							goto cont;
						} else if (x <= 0) {
							goto cont;
						}
					} else {
						x = read(fds[i].fd, loc + r, tr - r);
						if (x <= 0) {
							closeConn(param, conns[i]);
							conns[i] = NULL;
							goto cont;
						}
					}
					r += x;
				}
				while (r < tr) {
					ssize_t x = 0;
					if (conns[i]->tls) {
						x = gnutls_record_recv(conns[i]->session, loc + r, tr - r);
						if (x <= 0 && gnutls_error_is_fatal(x)) {
							closeConn(param, conns[i]);
							conns[i] = NULL;
							goto cont;
						} else if (x <= 0) {
							goto cont;
						}
					} else {
						x = read(fds[i].fd, loc + r, tr - r);
						if (x <= 0) {
							closeConn(param, conns[i]);
							conns[i] = NULL;
							goto cont;
						}
					}
					r += x;
				}
				reqp: if (conns[i]->reqPosting != NULL && conns[i]->postLeft > 0) {
					if (conns[i]->readBuffer_size >= conns[i]->postLeft) {
						struct timespec stt;
						clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stt);
						memcpy(conns[i]->reqPosting->body->data + conns[i]->reqPosting->body->len - conns[i]->postLeft, conns[i]->readBuffer, conns[i]->postLeft);
						size_t os = conns[i]->readBuffer_size;
						conns[i]->readBuffer_size -= conns[i]->postLeft;
						conns[i]->readBuffer_checked = 0;
						memmove(conns[i]->readBuffer, conns[i]->readBuffer + conns[i]->postLeft, conns[i]->readBuffer_size);
						conns[i]->postLeft -= os;
						if (conns[i]->postLeft == 0) {
							handleRequest(fds[i].fd, &stt, conns[i], param, conns[i]->reqPosting);
							conns[i]->reqPosting = NULL;
						}
					} else goto pc;
				}
				static unsigned char tm[4] = { 0x0D, 0x0A, 0x0D, 0x0A };
				int ml = 0;
				for (int x = conns[i]->readBuffer_checked; x < conns[i]->readBuffer_size; x++) {
					if (conns[i]->readBuffer[x] == tm[ml]) {
						ml++;
						if (ml == 4) {
							unsigned char* reqd = xmalloc(x + 2);
							memcpy(reqd, conns[i]->readBuffer, x + 1);
							reqd[x + 1] = 0;
							conns[i]->readBuffer_size -= x + 1;
							conns[i]->readBuffer_checked = 0;
							memmove(conns[i]->readBuffer, conns[i]->readBuffer + x + 1, conns[i]->readBuffer_size);
							struct timespec stt;
							clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stt);
							struct request* req = xmalloc(sizeof(struct request));
							if (parseRequest(req, (char*) reqd, param->maxPost) < 0) {
								errlog(param->logsess, "Malformed Request!");
								xfree(req);
								xfree(reqd);
								closeConn(param, conns[i]);
								goto cont;
							}
							if (req->body != NULL) {
								conns[i]->reqPosting = req;
								conns[i]->postLeft = req->body->len;
								goto reqp;
							}
							handleRequest(fds[i].fd, &stt, conns[i], param, req);
						}
					} else ml = 0;
				}
				pc: if (conns[i] != NULL) {
					if (conns[i]->readBuffer_size >= 10) conns[i]->readBuffer_checked = conns[i]->readBuffer_size - 10;
					else conns[i]->readBuffer_checked = 0;
				}
			}
			if ((re & POLLOUT) == POLLOUT && conns[i] != NULL) {
				ssize_t mtr = conns[i]->tls ? gnutls_record_send(conns[i]->session, conns[i]->writeBuffer, conns[i]->writeBuffer_size) : write(fds[i].fd, conns[i]->writeBuffer, conns[i]->writeBuffer_size);
				if (mtr < 0 && (conns[i]->tls ? gnutls_error_is_fatal(mtr) : mtr != EAGAIN)) {
					closeConn(param, conns[i]);
					conns[i] = NULL;
					goto cont;
				} else if (mtr < 0) {
					goto cont;
				} else if (mtr < conns[i]->writeBuffer_size) {
					memmove(conns[i]->writeBuffer, conns[i]->writeBuffer + mtr, conns[i]->writeBuffer_size - mtr);
					conns[i]->writeBuffer_size -= mtr;
					conns[i]->writeBuffer = xrealloc(conns[i]->writeBuffer, conns[i]->writeBuffer_size);
				} else {
					conns[i]->writeBuffer_size = 0;
					xfree(conns[i]->writeBuffer);
					conns[i]->writeBuffer = NULL;
				}
			}
			if ((re & POLLERR) == POLLERR) { //TODO: probably a HUP
				//printf("POLLERR in worker poll! This is bad!\n");
			}
			if ((re & POLLHUP) == POLLHUP && conns[i] != NULL) {
				closeConn(param, conns[i]);
				conns[i] = NULL;
			}
			if ((re & POLLNVAL) == POLLNVAL) {
				errlog(param->logsess, "Invalid FD in worker poll! This is bad!");
			}
			cont: if (--cp == 0) break;
		}
	}
	xfree(mbuf);
}
