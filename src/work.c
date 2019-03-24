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
#include "list.h"
#include "util.h"
#include "streams.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include "http.h"
#include "log.h"
#include "time.h"
#include <arpa/inet.h>
#include <sys/mman.h>
#include <openssl/ssl.h>
#include <openssl/md5.h>
#include "vhost.h"
#include "queue.h"
#include "http2.h"
#include "pmem.h"
#include "http_pipeline.h"
#include "pmem_hooks.h"
#include "smem.h"
#include "llist.h"

void closeConn(struct work_param* param, struct conn* conn) {
	pfree(conn->pool);
}

void sendReqsess(struct request_session* rs, struct timespec* stt) {
	struct conn* conn = rs->sender;
	struct work_param* param = rs->wp;
	struct response* resp = rs->response;
	struct request* req = rs->request;
	if (!conn->fwed) {
		size_t response_length = 0;
		unsigned char* serialized_response = serializeResponse(rs, &response_length);
		struct timespec stt2;
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stt2);
		double msp = (stt2.tv_nsec / 1000000.0 + stt2.tv_sec * 1000.0) - (stt->tv_nsec / 1000000.0 + stt->tv_sec * 1000.0);
		const char* mip = NULL;
		char tip[48];
		if (conn->addr.tcp6.sin6_family == AF_INET) {
			struct sockaddr_in *sip4 = &conn->addr.tcp4;
			mip = inet_ntop(AF_INET, &sip4->sin_addr, tip, 48);
		} else if (conn->addr.tcp6.sin6_family == AF_INET6) {
			struct sockaddr_in6 *sip6 = &conn->addr.tcp6;
			if (memseq((unsigned char*) &sip6->sin6_addr, 10, 0) && memseq((unsigned char*) &sip6->sin6_addr + 10, 2, 0xff)) {
				mip = inet_ntop(AF_INET, ((unsigned char*) &sip6->sin6_addr) + 12, tip, 48);
			} else mip = inet_ntop(AF_INET6, &sip6->sin6_addr, tip, 48);
		} else if (conn->addr.tcp6.sin6_family == AF_LOCAL) {
			mip = "UNIX";
		} else {
			mip = "UNKNOWN";
		}
		if (mip == NULL) {
			errlog(param->server->logsess, "Invalid IP Address: %s", strerror(errno));
		}
		acclog(param->server->logsess, "%s %s %s/%s%s returned %s took: %f ms", mip, getMethod(req->method), param->server->id, req->vhost->id, req->path, resp->code, msp);
		buffer_ensure_capacity(&conn->write_buffer, response_length);
        uint8_t* loc = conn->write_buffer.buffer + conn->write_buffer.size;
		memcpy(loc, serialized_response, response_length);
		conn->write_buffer.size += response_length;
	} else {
		conn->fwed = 0;
	}
}

void handleRequest(struct timespec* stt, struct request_session* rs) {
	struct response* resp = pmalloc(rs->pool, sizeof(struct response));
	resp->body = NULL;
	resp->parsed = 0;
	resp->code = "500 Internal Server Error";
	resp->version = "HTTP/1.1";
	resp->fromCache = NULL;
	resp->headers = pcalloc(rs->pool, sizeof(struct headers));
	resp->headers->pool = rs->pool;
	rs->response = resp;
	generateResponse(rs);
	sendReqsess(rs, stt);
}

struct uconn {
		int type;
		struct conn* conn;
};

int handleRead(struct conn* conn, int ct, struct work_param* param) {
	reqp: ;
	if (ct == 0 && conn->currently_posting != NULL && conn->postLeft > 0) {
		if (conn->read_buffer.size >= conn->postLeft) {
			struct timespec stt;
			clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stt);
			memcpy(conn->currently_posting->request->body->data + conn->currently_posting->request->body->len - conn->postLeft, conn->read_buffer.buffer, conn->postLeft);
			size_t os = conn->read_buffer.size;
            conn->read_buffer.size -= conn->postLeft;
			conn->readBuffer_checked = 0;
			memmove(conn->read_buffer.buffer, conn->read_buffer.buffer + conn->postLeft, conn->read_buffer.size);
			conn->postLeft -= os;
			if (conn->postLeft == 0) {
				handleRequest(&stt, conn->currently_posting);
				pfree(conn->currently_posting->pool);
				conn->currently_posting = NULL;
			}
		} else goto pc;
	}
	static unsigned char newlines[4] = { 0x0D, 0x0A, 0x0D, 0x0A };
	int ml = 0;
	uint8_t* readBuffer = (ct == 0 ? conn->read_buffer.buffer : (ct == 1 ? conn->fw_read_buffer.buffer : 0));
	sin: ;
	if (ct == 1 && conn->stream_type >= 0) { //todo ct==2
		int se = 0;
		if (conn->stream_type == 0) {
			size_t fw_buffer_size = conn->fw_read_buffer.size;
			if (conn->forwarding_request->response->fromCache != NULL) {
				if (conn->stream_md5 == NULL) {
					conn->stream_md5 = pmalloc(conn->pool, sizeof(MD5_CTX));
					MD5_Init(conn->stream_md5);
				}
				MD5_Update(conn->stream_md5, readBuffer, fw_buffer_size);
			}
			buffer_ensure_capacity(&conn->write_buffer, fw_buffer_size);
			if (conn->forwarding_request->request->atc) {
			    buffer_ensure_capacity(&conn->cache_buffer, fw_buffer_size);
				memcpy(conn->cache_buffer.buffer + conn->cache_buffer.size, readBuffer, fw_buffer_size);
                conn->cache_buffer.size += fw_buffer_size;
			}
			memcpy(conn->write_buffer.buffer + conn->write_buffer.size, readBuffer, fw_buffer_size);
            conn->write_buffer.size += fw_buffer_size;
			size_t new_size = conn->fw_read_buffer.size - fw_buffer_size;
			if (new_size > 0) {
				memmove(conn->fw_read_buffer.buffer, conn->fw_read_buffer.buffer + fw_buffer_size, new_size);
			}
            conn->fw_read_buffer.size -= fw_buffer_size;
			conn->streamed += fw_buffer_size;
			conn->fw_readBuffer_checked = 0;
			if (conn->streamed >= conn->stream_len) {
                struct response* fwd_response = conn->forwarding_request->response;
                if (conn->forwarding_request->request->atc) {
				    struct vhost* vhost = conn->forwarding_request->request->vhost;
					const char* content_type = header_get(fwd_response->headers, "Content-Type");
					int is_dynamic_type = hashset_has(conn->forwarding_request->request->vhost->sub.rproxy.dynamic_types, content_type);

					if (!is_dynamic_type) {
						conn->forwarding_request->request->atc = 1;
						struct scache* new_scache = pmalloc(vhost->pool, sizeof(struct scache));
						if (fwd_response->body == NULL) {
                            fwd_response->body = pmalloc(conn->pool, sizeof(struct body));
						}
                        fwd_response->body->len = buffer_consume(&conn->cache_buffer, &fwd_response->body->data);
						pclaim(conn->forwarding_request->pool, fwd_response->body->data);
                        fwd_response->body->stream_type = -1;
                        fwd_response->body->stream_fd = -1;
                        fwd_response->body->mime_type = content_type;
						new_scache->content_encoding = header_get(fwd_response->headers, "Content-Encoding") != NULL;
						new_scache->code = fwd_response->code;
						new_scache->headers = fwd_response->headers;
						new_scache->request_path = conn->forwarding_request->request->path;
						if (fwd_response->body == NULL) {
							new_scache->etag[0] = '\"';
							memset(new_scache->etag + 1, '0', 32);
							new_scache->etag[33] = '\"';
							new_scache->etag[34] = 0;
						} else {
							MD5_CTX md5ctx;
							MD5_Init(&md5ctx);
							MD5_Update(&md5ctx, fwd_response->body->data, fwd_response->body->len);
							unsigned char rawmd5[16];
							MD5_Final(rawmd5, &md5ctx);
							new_scache->etag[34] = 0;
							new_scache->etag[0] = '\"';
							for (int i = 0; i < 16; i++) {
								snprintf(new_scache->etag + (i * 2) + 1, 3, "%02X", rawmd5[i]);
							}
							new_scache->etag[33] = '\"';
						}
						header_setoradd(fwd_response->headers, "ETag", new_scache->etag);
						cache_add(&conn->forwarding_request->request->vhost->sub.rproxy.cache, new_scache);
                        fwd_response->fromCache = new_scache;
					} else {
						conn->forwarding_request->request->atc = 0;
					}
					/*if (!is_dynamic_type && !patc) {
						struct body* body = smalloc(sizeof(struct body));
						fwd_response->fromCache->body = body;
						body->data = conn->staticStreamCacheBuffer;
						conn->staticStreamCacheBuffer = NULL;
						body->len = conn->sscbl;
						body->stream_type = -1;
						body->stream_fd = -1;
						body->mime_type = content_type;
						body->freeMime = 0;
					}*/
				}
				if (conn->stream_md5 != NULL) {
					unsigned char rawmd5[16];
					MD5_Final(rawmd5, conn->stream_md5);
					conn->stream_md5 = NULL;
					for (int i = 0; i < 16; i++) {
						snprintf(fwd_response->fromCache->etag + (i * 2) + 1, 3, "%02X", rawmd5[i]);
					}
					header_setoradd(fwd_response->fromCache->headers, "ETag", fwd_response->fromCache->etag);
				}
				conn->stream_type = -1;
				conn->streamed = 0;
				queue_pop(conn->fwqueue);
				se = 1;
			}
		} else if (conn->stream_type == 1) { // chunked already

		}
		if (!se) goto pc;
	}
	struct buffer* active_buffer = ct == 0 ? &conn->read_buffer : &conn->fw_read_buffer;
	size_t active_buffer_checked = ct == 0 ? conn->readBuffer_checked : conn->fw_readBuffer_checked;
	for (size_t checking_index = active_buffer_checked; checking_index < active_buffer->size; checking_index++) {
		if (readBuffer[checking_index] == newlines[ml]) {
			ml++;
			if (ml == 4) {
				struct request_session* rs = NULL;
				struct mempool* req_pool;
				if (ct == 1) {
					rs = queue_peek(conn->fwqueue);
					req_pool = rs->pool;
				} else {
					req_pool = mempool_new();
					pchild(conn->pool, req_pool);
				}
				unsigned char* request_headers = pmalloc(req_pool, checking_index + 2);
				memcpy(request_headers, readBuffer, checking_index + 1);
				request_headers[checking_index + 1] = 0;
				active_buffer->size -= checking_index + 1;
				if (ct == 0) {
					conn->readBuffer_checked = 0;
				} else if (ct == 1) {
					conn->fw_readBuffer_checked = 0;
				}
				memmove(readBuffer, readBuffer + checking_index + 1, active_buffer->size);
				struct timespec stt;
				clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stt);

				if (ct == 0) {
					struct request* req = pmalloc(req_pool, sizeof(struct request));
					rs = pmalloc(req_pool, sizeof(struct request_session));
					rs->wp = param;
					rs->sender = conn;
					rs->response = NULL;
					rs->request = req;
					rs->pool = req_pool;
					if (parseRequest(rs, (char*) request_headers, param->server->max_post) < 0) {
						errlog(param->server->logsess, "Malformed Request!\n%s", request_headers);
						closeConn(param, conn);
						return 1;
					}
					if (req->body != NULL) {
						conn->currently_posting = rs;
						conn->postLeft = req->body->len;
						goto reqp;
					}
					handleRequest(&stt, rs);
					pfree(req_pool);
				} else if (ct == 1) {
					if (parseResponse(rs, (char*) request_headers) < 0) {
						errlog(param->server->logsess, "Malformed Response!");
						closeConn(param, conn);
						return 1;
					}
					if (rs->response->body != NULL) {
						rs->response->body->mime_type = header_get(rs->response->headers, "Content-Type");
					}
					sendReqsess(rs, &stt);
					if (rs->response->body != NULL && rs->response->body->stream_type >= 0) {
						conn->stream_fd = rs->response->body->stream_fd;
                        rs->response->body->stream_fd = -1;
						phook(conn->pool, close_hook, (void*) conn->stream_fd);
						conn->stream_type = rs->response->body->stream_type;
						conn->stream_len = rs->response->body->len;
						conn->forwarding_request = rs;
						goto sin;
					} else {
						conn->stream_type = -1;
						queue_pop(conn->fwqueue);
						pfree(req_pool);
					}
				}
			}
		} else ml = 0;
	}
	pc:
	if (conn->read_buffer.size >= 10)
		conn->readBuffer_checked = conn->read_buffer.size - 10;
	else
		conn->readBuffer_checked = 0;
	return 0;
}
/*
int finalizeHeaders2(struct conn* conn, struct work_param* param) {

	return 0;
}

int writeFrame(struct conn* conn, struct frame* frame, struct work_param* param) {
	unsigned char head[9];
	memcpy(head, &frame->length + sizeof(size_t) - 3, 3);
	head[3] = frame->type;
	head[4] = frame->flags;
	memcpy(head + 5, &frame->stream, sizeof(uint32_t));
	if (conn->writeBuffer == NULL) {
		conn->writeBuffer = smalloc(frame->length + 9);
		conn->writeBuffer_size = 0;
	} else {
		conn->writeBuffer = srealloc(conn->writeBuffer, conn->writeBuffer_size + frame->length + 9);
	}
	memcpy(conn->writeBuffer + conn->writeBuffer_size, head, 9);
	memcpy(conn->writeBuffer + conn->writeBuffer_size + 9, frame->uf, frame->length);
	return 0;
}

void freeFrame(struct frame* frame) {
	free(frame->uf);
}

int handleRead2(struct conn* conn, int ct, struct work_param* param) {
	if (conn->readBuffer_size >= 9) {
		size_t len = 0;
		memcpy(&len + sizeof(len) - 3, conn->readBuffer, 3);
		if (conn->readBuffer_size >= 9 + len) {
			struct frame* frame = smalloc(sizeof(struct frame));
			frame->length = len;
			frame->type = conn->readBuffer[3];
			frame->flags = conn->readBuffer[4];
			frame->stream = 0;
			frame->strobj = NULL;
			memcpy(&frame->stream + sizeof(size_t) - 4, conn->readBuffer + 5, 4);
			if (frame->stream > 0) for (int i = 0; i < conn->http2_stream_size; i++) {
				if (conn->http2_stream[i]->id == frame->stream) {
					frame->strobj = conn->http2_stream[i];
					break;
				}
			}
			printf("%i\n", frame->type);
			unsigned char* lframe = conn->readBuffer + 9;
			if (frame->type == FRAME_DATA_ID) {
				if (frame->strobj == NULL) {
					errno = EINVAL;
					return -1;
				}
				unsigned char pad = 0;
				size_t dl = len;
				if (frame->flags & 0x8 == 0x8) {
					if (len < 1) {
						errno = EINVAL;
						return -1;
					}
					pad = lframe[0];
					if (len < pad + 1) {
						errno = EINVAL;
						return -1;
					}
					lframe++;
					len -= 1 + pad;
				}
				if (frame->strobj->http2_dataBuffer == NULL) {
					frame->strobj->http2_dataBuffer = smalloc(len);
					frame->strobj->http2_dataBuffer_size = 0;
				} else {
					frame->strobj->http2_dataBuffer = srealloc(frame->strobj->http2_dataBuffer, frame->strobj->http2_dataBuffer_size + len);
				}
				memcpy(frame->strobj->http2_dataBuffer, frame->strobj->http2_dataBuffer_size + lframe, len);
				frame->strobj->http2_dataBuffer_size += len;
			} else if (frame->type == FRAME_HEADERS_ID) {
				if (frame->strobj == NULL) {
					frame->strobj = smalloc(sizeof(struct http2_stream)); //TODO reuse mem space in conn->http2_stream
					frame->strobj->id = frame->stream;
					frame->strobj->http2_dataBuffer = NULL;
					frame->strobj->http2_headerBuffer = NULL;
					frame->strobj->http2_dataBuffer_size = 0;
					frame->strobj->http2_headerBuffer_size = 0;
					if (conn->http2_stream == NULL) {
						conn->http2_stream = smalloc(sizeof(struct http2_stream*));
						conn->http2_stream_size = 0;
					} else {
						conn->http2_stream = srealloc(conn->http2_stream, sizeof(struct http2_stream*) * (conn->http2_stream_size + 1));
					}
					conn->http2_stream[conn->http2_stream_size] = frame->strobj;
					conn->http2_stream_size++;
				}
				unsigned char pad = 0;
				size_t dl = len;
				if (frame->flags & 0x8 == 0x8) {
					if (len < 1) {
						errno = EINVAL;
						return -1;
					}
					pad = lframe[0];
					if (len < pad + 1) {
						errno = EINVAL;
						return -1;
					}
					lframe++;
					len -= 1 + pad;
				}
				if (frame->flags & 0x20 == 0x20) {
					if (len < 5) {
						errno = EINVAL;
						return -1;
					}
					len -= 5; //TODO: stream dependency
				}
				if (frame->strobj->http2_headerBuffer == NULL) {
					frame->strobj->http2_headerBuffer = smalloc(len);
					frame->strobj->http2_headerBuffer_size = 0;
				} else {
					frame->strobj->http2_headerBuffer = srealloc(frame->strobj->http2_headerBuffer, frame->strobj->http2_headerBuffer_size + len);
				}
				memcpy(frame->strobj->http2_headerBuffer, frame->strobj->http2_headerBuffer_size + lframe, len);
				frame->strobj->http2_headerBuffer_size += len;
				if (frame->flags & 0x4 == 0x4) {
					if (finalizeHeaders2(conn, param) == -1) return -1;
				}
			} else if (frame->type == FRAME_PRIORITY_ID) {
				//TODO: impl
			} else if (frame->type == FRAME_RST_STREAM_ID) {
				//TODO: impl
			} else if (frame->type == FRAME_SETTINGS_ID) {
				if (len % 6 == 0) {
					for (int i = 0; i < len / 6; i++) {
						int id = 0;
						memcpy(&id + sizeof(int) - 2, lframe + len + (i * 6), 2);
						uint32_t val = 0;
						memcpy(&val, lframe + len + (i * 6) + 2, 4);

					}
				}
			} else if (frame->type == FRAME_PUSH_PROMISE_ID) {
				//TODO impl
			} else if (frame->type == FRAME_PING_ID) {

			} else if (frame->type == FRAME_GOAWAY_ID) {
				closeConn(param, conn);
				errno = ECONNRESET;
				return -1;
			} else if (frame->type == FRAME_WINDOW_UPDATE_ID) {

			} else if (frame->type == FRAME_CONTINUATION_ID) {
				if (frame->strobj == NULL) {
					closeConn(param, conn);
					errno = ECONNRESET;
					return -1;
				}
				size_t dl = len;
				if (frame->strobj->http2_headerBuffer == NULL) {
					frame->strobj->http2_headerBuffer = smalloc(len);
					frame->strobj->http2_headerBuffer_size = 0;
				} else {
					frame->strobj->http2_headerBuffer = srealloc(frame->strobj->http2_headerBuffer, frame->strobj->http2_headerBuffer_size + len);
				}
				memcpy(frame->strobj->http2_headerBuffer, frame->strobj->http2_headerBuffer_size + lframe, len);
				frame->strobj->http2_headerBuffer_size += len;
				if (frame->flags & 0x4 == 0x4) {
					if (finalizeHeaders2(conn, param) == -1) return -1;
				}
			}
			if (frame->flags & 0x1 == 0x1) {
				//close stream
			}
			conn->readBuffer_size -= 9 + len;
			memmove(conn->readBuffer, conn->readBuffer + 9 + len, conn->readBuffer_size);
		}
	}
	return 0;
}*/

struct remove_conn_node_arg {
	struct llist* list;
	struct llist_node* node;
};

void remove_conn_node(struct remove_conn_node_arg* node) {
	llist_del(node->list, node->node);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"
void run_work(struct work_param* param) {
	if (pipe(param->pipes) != 0) {
		errlog(param->server->logsess, "Failed to create pipe! %s", strerror(errno));
		return;
	}
	struct mempool* pool = mempool_new();
	unsigned char wb;
	struct llist* claimed_connections = llist_new(pool);
    struct list* uconns = list_new(64, pool);
    struct pollfd* poll_fds = pmalloc(pool, sizeof(struct pollfd) * 64);
    size_t poll_fds_cap = 64;
    while (1) {
	    // by only doing one pop per loop, we effectively create a QoS in which serving existing connections is prioritized over new ones, but also so some better load balancing.
	    struct conn* new_conn = queue_maybepop(param->server->prepared_connections);
	    if (new_conn != NULL) {
	        struct remove_conn_node_arg* arg = pmalloc(new_conn->pool, sizeof(struct remove_conn_node_arg));
			arg->list = claimed_connections;
			arg->node = llist_append(claimed_connections, new_conn);
	        phook(new_conn->pool, remove_conn_node, arg);
	    }
		size_t mfds = claimed_connections->size + 1;
		uconns->count = 0;
		uconns->size = 0;
        struct mempool* iter_pool = mempool_new();
        size_t poll_fd_index = 0;
        if (poll_fds_cap < mfds * 3 + 1) {
            while (poll_fds_cap < mfds * 3 + 1) {
                poll_fds_cap *= 2;
            }
            poll_fds = prealloc(pool, poll_fds, sizeof(struct pollfd) * poll_fds_cap);
        }
        ITER_LLIST(claimed_connections, value) {
        	struct conn* conn = value;
            if (conn->fwqueue != NULL) {
                pthread_mutex_lock(&conn->fwqueue->data_mutex);
                if (conn->fwqueue->size > 0) {
                    mfds += 1;
                    struct uconn* uconn = pmalloc(iter_pool, sizeof(struct uconn));
                    uconn->conn = conn;
                    uconn->type = 1;
                    list_add(uconns, uconn);
                    struct pollfd* pollfd = &poll_fds[poll_fd_index++];
                    pollfd->fd = conn->fw_fd;
                    pollfd->events = POLLIN;
                    pollfd->revents = 0;
                }
                pthread_mutex_unlock(&conn->fwqueue->data_mutex);
            }
            if (conn->stream_type >= 0 && conn->stream_fd != conn->fw_fd) { // TODO: finish impl
                mfds += 1;
                struct uconn* uconn = pmalloc(iter_pool, sizeof(struct uconn));
                uconn->conn = conn;
                uconn->type = 2;
                list_add(uconns, uconn);
                struct pollfd* pollfd = &poll_fds[poll_fd_index++];
                pollfd->fd = conn->stream_fd;
                pollfd->events = POLLIN;
                pollfd->revents = 0;
            }
            struct uconn* uconn = pmalloc(iter_pool, sizeof(struct uconn));
            uconn->conn = conn;
            uconn->type = 0;
            list_add(uconns, uconn);
            struct pollfd* pollfd = &poll_fds[poll_fd_index++];
            pollfd->fd = conn->fd;
            pollfd->events = POLLIN | ((conn->write_buffer.size > 0 || (conn->tls && !conn->handshaked && conn->ssl_nextdir == 2)) ? POLLOUT : 0);
            pollfd->revents = 0;
			ITER_LLIST_END();
        }
        struct pollfd* pollfd = &poll_fds[poll_fd_index++];
        pollfd->fd = param->pipes[0];
        pollfd->events = POLLIN;
        pollfd->revents = 0;
        int poll_count = poll(poll_fds, mfds, -1);
		if (poll_count < 0) {
			errlog(param->server->logsess, "Poll error in worker thread! %s", strerror(errno));
		} else if (poll_count == 0) {
			pfree(iter_pool);
			continue;
		} else if ((poll_fds[mfds - 1].revents & POLLIN) == POLLIN) {
			if (read(param->pipes[0], &wb, 1) < 1) errlog(param->server->logsess, "Error reading from pipe, infinite loop COULD happen here.");
			if (poll_count-- == 1) {
                pfree(iter_pool);
                continue;
			}
		}
		for (int i = 0; i < mfds - 1 && poll_count > 0; i++) {
			int revents = poll_fds[i].revents;
			if (revents == 0) continue;
			struct uconn* uconn = uconns->data[i];
			struct conn* conn = uconn->conn;
			int connection_type = uconn->type;
			if (connection_type != 0 && connection_type != 1) {
				errlog(param->server->logsess, "Invalid connection type! %i", connection_type);
				continue;
			}

            poll_count--;

			if ((revents & POLLHUP) == POLLHUP && conn != NULL) {
				closeConn(param, conn);
				continue;
			}
			if ((revents & POLLERR) == POLLERR) { //TODO: probably a HUP
				closeConn(param, conn);
                continue;
			}
			if ((revents & POLLNVAL) == POLLNVAL) {
				errlog(param->server->logsess, "Invalid FD in worker poll! This is bad!");
				closeConn(param, conn);
                continue;
			}
			if (connection_type == 0 && conn->tls && !conn->handshaked) {
				int r = SSL_accept(conn->session);
				if (r == 1) {
					conn->handshaked = 1;
				} else if (r == 2) {
					closeConn(param, conn);
                    continue;
				} else {
					int err = SSL_get_error(conn->session, r);
					if (err == SSL_ERROR_WANT_READ) conn->ssl_nextdir = 1;
					else if (err == SSL_ERROR_WANT_WRITE) conn->ssl_nextdir = 2;
					else {
						closeConn(param, conn);
                        continue;
					}
				}
                continue;
			} else if (connection_type == 1 && conn->fw_tls && !conn->fw_handshaked) {
				int r = SSL_accept(conn->fw_session);
				if (r == 1) {
					conn->fw_handshaked = 1;
				} else if (r == 2) {
					closeConn(param, conn);
                    continue;
				} else {
					int err = SSL_get_error(conn->fw_session, r);
					if (err == SSL_ERROR_WANT_READ) conn->fw_ssl_nextdir = 1;
					else if (err == SSL_ERROR_WANT_WRITE) conn->fw_ssl_nextdir = 2;
					else {
						closeConn(param, conn);
                        continue;
					}
				}
                continue;
			}
			if ((revents & POLLIN) == POLLIN) {
				size_t tr = 0;
				int ftr = 0;
				if (connection_type == 1 ? conn->fw_tls : (connection_type == 0 ? conn->tls : 0)) {
					if (connection_type == 0) tr = SSL_pending(conn->session);
					else if (connection_type == 1) tr = SSL_pending(conn->fw_session);
					if (tr == 0) {
						tr += 4096;
						ftr = 1;
					}
				} else {
					ioctl(poll_fds[i].fd, FIONREAD, &tr);
				}
				uint8_t* loc;
				if (connection_type == 0) {
                    buffer_ensure_capacity(&conn->read_buffer, tr);
                    loc = conn->read_buffer.buffer + conn->read_buffer.size;
				} else if (connection_type == 1) {
                    buffer_ensure_capacity(&conn->fw_read_buffer, tr);
                    loc = conn->fw_read_buffer.buffer + conn->fw_read_buffer.size;
				}
				ssize_t r = 0;
				if (r == 0 && tr == 0) { // nothing to read, but wont block.
					ssize_t x = 0;
					if (conn->tls) {
						if (connection_type == 0) x = SSL_read(conn->session, loc + r, tr - r);
						else if (connection_type == 1) x = SSL_read(conn->fw_session, loc + r, tr - r);
						if (x <= 0) {
							int serr = SSL_get_error(conn->session, x);
							if (serr == SSL_ERROR_WANT_WRITE || serr == SSL_ERROR_WANT_READ) continue;
							if (connection_type == 1) {
								errlog(param->server->logsess, "TLS Error receiving from backend server! %i", SSL_get_error(connection_type == 0 ? conn->session : conn->fw_session, x));
							}
							closeConn(param, conn);
                            continue;
                        } else {
							if (connection_type == 0) {
								conn->read_buffer.size += x;
							} else if (connection_type == 1) {
								conn->fw_read_buffer.size += x;
							}
						}
					} else {
						x = read(poll_fds[i].fd, loc + r, tr - r);
						if (x <= 0) {
							closeConn(param, conn);
                            continue;
						} else {
							if (connection_type == 0) {
                                conn->read_buffer.size += x;
							} else if (connection_type == 1) {
                                conn->fw_read_buffer.size += x;
							}
						}
					}
					r += x;
				}
				while (r < tr) {
					ssize_t x = 0;
					if (conn->tls) {
						if (connection_type == 0) x = SSL_read(conn->session, loc + r, tr - r);
						else if (connection_type == 1) x = SSL_read(conn->fw_session, loc + r, tr - r);
						if (x <= 0) {
							int serr = SSL_get_error(conn->session, x);
							if (serr == SSL_ERROR_WANT_WRITE || serr == SSL_ERROR_WANT_READ) goto cont;
							if (connection_type == 1) {
								errlog(param->server->logsess, "TLS Error receiving from backend server! %i", SSL_get_error(connection_type == 0 ? conn->session : conn->fw_session, x));
							}
							closeConn(param, conn);
							goto cont;
						} else {
							if (connection_type == 0) {
                                conn->read_buffer.size += x;
							} else if (connection_type == 1) {
                                conn->fw_read_buffer.size += x;
							}
						}
						if (ftr) break;
					} else {
						x = read(poll_fds[i].fd, loc + r, tr - r);
						if (x <= 0) {
							closeConn(param, conn);
							goto cont;
						} else {
							if (connection_type == 0) {
                                conn->read_buffer.size += x;
							} else if (connection_type == 1) {
                                conn->fw_read_buffer.size += x;
							}
						}
					}
					r += x;
				}
				int p = 0;
				if (conn->proto == 0) p = handleRead(conn, connection_type, param);
				// else if (conn->proto == 1) p = handleRead2(conn, connection_type, param);
				if (p == 1) {
                    continue;
				}
			}
			if ((revents & POLLOUT) == POLLOUT && conn != NULL) {
				ssize_t mtr = conn->tls ? SSL_write(conn->session, conn->write_buffer.buffer, conn->write_buffer.size) : write(poll_fds[i].fd, conn->write_buffer.buffer, conn->write_buffer.size);
				int serr = (conn->tls && mtr < 0) ? SSL_get_error(conn->session, mtr) : 0;
				if (mtr < 0 && (conn->tls ? ((serr != SSL_ERROR_SYSCALL || errno != EAGAIN) && serr != SSL_ERROR_WANT_WRITE && serr != SSL_ERROR_WANT_READ) : errno != EAGAIN)) { // use error queue?
					closeConn(param, conn);
                    continue;
				} else if (mtr < 0) {
                    continue;
				} else if (mtr < conn->write_buffer.size) {
					memmove(conn->write_buffer.buffer, conn->write_buffer.buffer + mtr, conn->write_buffer.size - mtr);
                    conn->write_buffer.size -= mtr;
				} else {
                    conn->write_buffer.size = 0;
				}
			}
		    cont:;
		}
		pfree(iter_pool);
	}
}
#pragma clang diagnostic pop
