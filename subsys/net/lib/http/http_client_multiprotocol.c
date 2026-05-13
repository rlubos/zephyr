/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client_multiprotocol.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>

#include "headers/client_internal.h"

LOG_MODULE_REGISTER(net_http_client_mp, CONFIG_NET_HTTP_LOG_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_HTTP_CLIENT_VERSION_2), "Some HTTP version needs to be enabled");

/* TODO
 * [ ] Complete HTTP2 implementation
 *     [ ] 1xx interim responses and trailers, distinct from response headers
 * [ ] QUIC transport
 * [ ] HTTP3 support
 * [ ] HTTP1 support
 */

#define HTTP_WAIT_SLICE_MS 1000

/* Room for the decimal form of the largest size_t. */
#define CONTENT_LENGTH_STR_SIZE sizeof("18446744073709551615")

/* Deadlines of asynchronous transactions are enforced from a work queue of
 * the client's own: resetting a stream sends to a peer that may have stopped
 * reading, which must not stall the system work queue.
 */
static K_KERNEL_STACK_DEFINE(http_client_wq_stack, CONFIG_HTTP_CLIENT_WORKQUEUE_STACK_SIZE);
static struct k_work_q http_client_wq;
static K_MUTEX_DEFINE(http_client_wq_lock);
static bool http_client_wq_started;

static void http_client_wq_start(void)
{
	const struct k_work_queue_config cfg = {
		.name = "http_client",
	};

	k_mutex_lock(&http_client_wq_lock, K_FOREVER);

	if (!http_client_wq_started) {
		k_work_queue_init(&http_client_wq);
		k_work_queue_start(&http_client_wq, http_client_wq_stack,
				   K_KERNEL_STACK_SIZEOF(http_client_wq_stack),
				   CLAMP(CONFIG_HTTP_CLIENT_WORKQUEUE_PRIORITY,
					 K_HIGHEST_APPLICATION_THREAD_PRIO,
					 K_LOWEST_APPLICATION_THREAD_PRIO), &cfg);
		http_client_wq_started = true;
	}

	k_mutex_unlock(&http_client_wq_lock);
}

static const char *http_version_txt(enum http_client_version version)
{
	switch (version) {
	case HTTP_CLIENT_VERSION_1_1:
		return "HTTP/1.1";
	case HTTP_CLIENT_VERSION_2:
		return "HTTP/2";
	case HTTP_CLIENT_VERSION_3:
		return "HTTP/3";
	default:
		return "";
	}
}

static const struct http_client_ops *http_client_get_ops(enum http_client_version version)
{
	const struct http_client_ops *ops;

	switch (version) {
	case HTTP_CLIENT_VERSION_1_1:
		ops = NULL; /* Placeholder for future implementation */
		break;
	case HTTP_CLIENT_VERSION_2:
		ops = http_client_get_http2_ops();
		break;
	case HTTP_CLIENT_VERSION_3:
		ops = NULL; /* Placeholder for future implementation */
		break;
	default:
		ops = NULL;
		break;
	}

	return ops;
}

static void http_session_init(struct http_client_session *session,
			      struct http_client_session_config *config)
{
	memset(session, 0, sizeof(*session));
	memcpy(&session->server_addr, &config->server_addr, config->addrlen);
	session->addrlen = config->addrlen;
	session->host = config->host;
	session->async = config->async;
}

static struct http_client_stream *http_stream_alloc(struct http_client_session *session)
{
	ARRAY_FOR_EACH(session->streams, i) {
		struct http_client_stream *stream = &session->streams[i];

		if (stream->is_used) {
			continue;
		}

		memset(stream, 0, sizeof(*stream));

		stream->is_used = true;

		return stream;
	}

	return NULL;
}

static void http_stream_release(struct http_client_session *session,
				struct http_client_stream *stream)
{
	if (!PART_OF_ARRAY(session->streams, stream)) {
		return;
	}

	memset(stream, 0, sizeof(*stream));
}

static void http_stream_init(struct http_client_stream *stream,
			     const struct http_client_request *request,
			     int32_t timeout, void *user_data)
{
	stream->request = request;
	stream->user_data = user_data;
	/* A negative timeout means wait forever, represented as the maximum
	 * absolute deadline so that http_stream_wait() never expires.
	 */
	stream->timeout = (timeout < 0) ? INT64_MAX : k_uptime_get() + timeout;
	stream->completed = false;
	stream->result = 0;
}

/* Arm the asynchronous deadline timer for the earliest pending stream
 * deadline, or stop it when no stream is waiting. Must be called with the
 * client lock held.
 */
static void http_client_timeout_reschedule(struct http_client *client)
{
	int64_t earliest = INT64_MAX;
	int64_t remaining;

	ARRAY_FOR_EACH(client->session.streams, i) {
		struct http_client_stream *stream = &client->session.streams[i];

		if (stream->is_used && !stream->completed && stream->timeout < earliest) {
			earliest = stream->timeout;
		}
	}

	if (earliest == INT64_MAX) {
		(void)k_work_cancel_delayable(&client->timeout_work);
		return;
	}

	remaining = MAX(earliest - k_uptime_get(), 0);
	(void)k_work_reschedule_for_queue(&http_client_wq, &client->timeout_work,
					  K_MSEC(remaining));
}

/* Expire asynchronous transactions whose deadline has passed: the stream is
 * reset on the wire and the application is notified, the same way as for a
 * synchronous timeout.
 */
static void http_client_timeout_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct http_client *client = CONTAINER_OF(dwork, struct http_client, timeout_work);
	const struct http_client_ops *http_ops;
	int64_t now;

	k_mutex_lock(&client->lock, K_FOREVER);

	if (client->session.state != HTTP_CLIENT_SESSION_OPEN || !client->session.async) {
		goto out;
	}

	http_ops = http_client_get_ops(client->version);
	now = k_uptime_get();

	ARRAY_FOR_EACH(client->session.streams, i) {
		struct http_client_stream *stream = &client->session.streams[i];

		if (!stream->is_used || stream->completed || stream->timeout > now) {
			continue;
		}

		NET_ERR("Transaction timed out");

		if (http_ops != NULL && http_ops->stream_reset != NULL) {
			(void)http_ops->stream_reset(client, stream);
		}

		/* Delivers the ABORTED callback and releases the stream. */
		http_client_stream_set_complete(client, stream, -ETIMEDOUT);
	}

	http_client_timeout_reschedule(client);

out:
	k_mutex_unlock(&client->lock);
}

/* Tear down the session. Streams still in flight are completed with an error
 * so that their owners are notified and, in synchronous mode, woken up. Peer
 * notifications (RST_STREAM, GOAWAY) are only attempted while the transport is
 * usable. Must be called with the client lock held.
 */
static int http_client_disconnect_locked(struct http_client *client, int reason,
					 bool transport_ok)
{
	const struct http_client_ops *http_ops;
	int stream_result = (reason < 0) ? reason : -ECONNABORTED;
	int ret = 0, ret_close;

	if (client->session.state == HTTP_CLIENT_SESSION_CLOSED) {
		NET_DBG("Client HTTP session already closed");
		return 0;
	}

	http_ops = http_client_get_ops(client->version);
	if (http_ops == NULL) {
		NET_ERR("%s not supported", http_version_txt(client->version));
		return -EINVAL;
	}

	ARRAY_FOR_EACH(client->session.streams, i) {
		struct http_client_stream *stream = &client->session.streams[i];

		if (!stream->is_used) {
			continue;
		}

		/* A completed stream is over on the wire as well, and is left
		 * to the thread that owns it.
		 */
		if (stream->completed) {
			continue;
		}

		if (transport_ok && http_ops->stream_reset != NULL) {
			ret = http_ops->stream_reset(client, stream);
			if (ret != 0) {
				NET_ERR("Failed to reset stream, err %d", ret);
			}
		}

		http_client_stream_set_complete(client, stream, stream_result);
	}

	if (transport_ok && http_ops->session_close != NULL) {
		ret = http_ops->session_close(client, reason);
		if (ret != 0) {
			NET_ERR("Failed to close the session, err %d", ret);
		}
	}

	(void)k_work_cancel_delayable(&client->timeout_work);

	/* Close the transport regardless of the previous results. */
	ret_close = client->transport_ops->close(client);
	if (ret_close != 0) {
		NET_ERR("Failed to disconnect from the server, err %d", ret_close);
		ret = ret_close;
	}

	client->session.state = HTTP_CLIENT_SESSION_CLOSED;

	return ret;
}


/* Public API */

int http_client_init(struct http_client *client, enum http_client_version version,
		     const struct http_client_transport_ops *transport_ops,
		     void *transport_ctx)
{
	if (client == NULL || transport_ops == NULL) {
		return -EINVAL;
	}

	if (transport_ops->init != NULL) {
		transport_ops->init(transport_ctx);
	}

	NET_ASSERT(transport_ops->connect != NULL);
	NET_ASSERT(transport_ops->send != NULL);
	NET_ASSERT(transport_ops->recv != NULL);
	NET_ASSERT(transport_ops->wait != NULL);
	NET_ASSERT(transport_ops->close != NULL);

	memset(client, 0, sizeof(*client));

	client->transport_ops = transport_ops;
	client->transport_ctx = transport_ctx;
	client->version = version;
	client->session.state = HTTP_CLIENT_SESSION_CLOSED;

	k_mutex_init(&client->lock);
	k_condvar_init(&client->tx_cond);
	k_work_init_delayable(&client->timeout_work, http_client_timeout_work);

	return 0;
}

int http_client_connect(struct http_client *client,
			struct http_client_session_config *config)
{
	const struct http_client_ops *http_ops;
	int ret;

	if (client == NULL || config == NULL) {
		return -EINVAL;
	}

	if (config->addrlen == 0 || config->addrlen > sizeof(config->server_addr)) {
		NET_ERR("Invalid server address length %u", config->addrlen);
		return -EINVAL;
	}

	if (client->transport_ops == NULL) {
		NET_ERR("Client transport not initialized");
		return -EINVAL;
	}

	k_mutex_lock(&client->lock, K_FOREVER);

	if (client->session.state != HTTP_CLIENT_SESSION_CLOSED) {
		NET_ERR("Client HTTP session already open");
		ret = -EALREADY;
		goto out;
	}

	http_ops = http_client_get_ops(client->version);
	if (http_ops == NULL) {
		NET_ERR("%s not supported", http_version_txt(client->version));
		ret = -EINVAL;
		goto out;
	}

	http_session_init(&client->session, config);

	if (config->async) {
		/* Only asynchronous sessions need the deadline work queue. */
		http_client_wq_start();
	}

	ret = client->transport_ops->connect(client);
	if (ret != 0) {
		NET_ERR("Failed to connect to the server, err %d", ret);
		goto out;
	}

	ret = http_ops->session_open(client);
	if (ret != 0) {
		NET_ERR("Failed to start a new session, err %d", ret);
		(void)client->transport_ops->close(client);
		goto out;
	}

	client->session.state = HTTP_CLIENT_SESSION_OPEN;

out:
	k_mutex_unlock(&client->lock);

	return ret;
}

static int client_send_payload(struct http_client *client,
			       struct http_client_stream *stream,
			       const struct http_client_request *request,
			       const struct http_client_ops *http_ops)
{
	const uint8_t *payload;
	size_t payload_len;
	size_t offset;
	bool more;
	int ret;

	/* If not payload callback, use pointer provided payload if any */
	if (request->payload_cb == NULL) {
		if (request->payload != NULL && request->payload_len > 0) {
			ret = http_ops->stream_send_data(client, stream, request->payload,
							 request->payload_len, true);
			if (ret != 0 && ret != -EALREADY) {
				return ret;
			}
		}

		return 0;
	}

	/* Otherwise, use the callback until the application reports no more data */
	offset = 0;
	more = true;
	while (more) {
		more = false;
		ret = request->payload_cb(offset, &payload, &payload_len,
					  &more, stream->user_data);
		if (ret != 0) {
			NET_ERR("Application aborted payload send, err %d", ret);
			break;
		}

		if (payload_len > 0 && payload == NULL) {
			NET_ERR("No payload provided by the application");
			ret = -EINVAL;
			break;
		}

		/* An empty final chunk is allowed, and simply terminates the
		 * request payload. An empty chunk with more data pending is not,
		 * as the callback would only be called again with the same
		 * offset.
		 */
		if (payload_len == 0 && more) {
			NET_ERR("Empty payload chunk with more data pending");
			ret = -EINVAL;
			break;
		}

		ret = http_ops->stream_send_data(client, stream, payload,
						 payload_len, !more);
		if (ret == -EALREADY) {
			/* The response is complete, the rest is of no use. */
			ret = 0;
			break;
		}

		if (ret != 0) {
			break;
		}

		offset += payload_len;
	}

	return ret;
}

/* Notify the application that the transaction was aborted with error. A
 * NULL response means no stream was allocated for the request yet.
 */
static void client_transaction_aborted(struct http_client *client,
				       const struct http_client_request *request,
				       struct http_client_response *response,
				       int error, void *user_data)
{
	struct http_client_response empty = { 0 };

	if (request->response_cb == NULL) {
		return;
	}

	if (response == NULL) {
		response = &empty;
	}

	response->error = error;

	(void)request->response_cb(client, request, response,
				   HTTP_CLIENT_TRANSACTION_ABORTED, user_data);
}

struct http_client_stream *http_client_stream_lookup(struct http_client *client,
						     uint64_t stream_id)
{
	ARRAY_FOR_EACH(client->session.streams, i) {
		struct http_client_stream *stream = &client->session.streams[i];

		if (stream->is_used && !stream->completed && stream->stream_id == stream_id) {
			return stream;
		}
	}

	return NULL;
}

static void client_stream_finish(struct http_client *client,
				 const struct http_client_ops *http_ops,
				 struct http_client_stream *stream)
{
	if (http_ops != NULL && http_ops->stream_close != NULL) {
		int ret = http_ops->stream_close(client, stream);

		if (ret != 0) {
			NET_ERR("Failed to close the stream, err %d", ret);
		}
	}

	http_stream_release(&client->session, stream);
}

void http_client_stream_set_complete(struct http_client *client,
				     struct http_client_stream *stream, int result)
{
	/* On an error result, notify the application that the transaction was
	 * aborted. On success the protocol layer has already delivered the
	 * final response chunk via the callback.
	 */
	if (result < 0 && stream->request != NULL) {
		client_transaction_aborted(client, stream->request, &stream->response,
					   result, stream->user_data);
	}

	stream->result = result;
	stream->completed = true;

	/* A thread blocked sending on this session has to re-evaluate. */
	http_client_tx_wakeup(client);

	if (client->session.async && !stream->sending) {
		/* No thread will pick this up, so close and release the stream
		 * here.
		 */
		client_stream_finish(client, http_client_get_ops(client->version), stream);
	}

	/* Otherwise the thread in http_client_send_request() sees the completed
	 * flag and closes and releases the stream itself.
	 */
}

void http_client_tx_wakeup(struct http_client *client)
{
	k_condvar_broadcast(&client->tx_cond);
}

void http_client_session_terminate(struct http_client *client, int reason)
{
	(void)http_client_disconnect_locked(client, reason, true);
}

static int client_process_rx_data(struct http_client *client,
				  const struct http_client_ops *http_ops, bool end_stream)
{
	struct http_client_session *session = &client->session;
	size_t offset = 0;
	int ret;

	/* Runs at least once, so that a stream ending without data reaches
	 * the protocol layer.
	 */
	do {
		ret = http_ops->process_data(client, session->rx_stream, session->rx_buf + offset,
					     session->rx_data_len - offset, end_stream);
		if (ret == -EAGAIN) {
			if (end_stream) {
				/* The stream ended inside a protocol unit, so the
				 * leftover can never be completed. Drop it rather
				 * than keep waiting on that stream.
				 */
				NET_WARN("Stream ended within a protocol unit, %zu bytes dropped",
					 session->rx_data_len - offset);
				offset = session->rx_data_len;
			}

			/* Otherwise wait for more data to be received. */
			break;
		} else if (ret < 0) {
			NET_ERR("Failed to process received data, err %d", ret);
			return ret;
		}

		if (session->state != HTTP_CLIENT_SESSION_OPEN) {
			/* The protocol layer ended the session. */
			session->rx_data_len = 0;
			return 0;
		}

		/* Otherwise ret holds the number of bytes consumed. */
		offset += ret;
		NET_ASSERT(offset <= session->rx_data_len);
	} while (session->rx_data_len - offset > 0);

	/* Consume the processed data, keeping the leftover at the buffer start. */
	if (offset > 0) {
		session->rx_data_len -= offset;
		if (session->rx_data_len > 0) {
			memmove(session->rx_buf, session->rx_buf + offset,
				session->rx_data_len);
		}
	}

	return 0;
}

/* Read whatever the transport has available and process it. On any error the
 * session is torn down, which also notifies the owners of in-flight streams.
 * Must be called with the client lock held.
 */
static void client_receive_locked(struct http_client *client, int status)
{
	const struct http_client_ops *http_ops;
	struct http_client_session *session = &client->session;
	bool transport_ok = true;
	bool end_stream = false;
	size_t free_space;
	int ret;

	http_ops = http_client_get_ops(client->version);
	if (http_ops == NULL || http_ops->process_data == NULL) {
		NET_ERR("%s not supported", http_version_txt(client->version));
		ret = -ENOTSUP;
		goto abort;
	}

	if (status < 0) {
		/* Transport reported an error condition, abort without trying
		 * to read any data.
		 */
		NET_ERR("Transport reception error %d", status);
		ret = status;
		transport_ok = false;
		goto abort;
	}

	free_space = sizeof(session->rx_buf) - session->rx_data_len;
	if (free_space == 0) {
		/* Buffer is full and the implementation still needs more data
		 * to make progress. We cannot grow the buffer, so abort.
		 */
		NET_ERR("RX buffer too small to process incoming data");
		ret = -ENOBUFS;
		goto abort;
	}

	/* Read whatever is available into the free part of the buffer. The
	 * transport recv is always non-blocking. Data left over from a previous
	 * read belongs to one transport stream, so only that stream is read
	 * until the leftover is consumed.
	 */
	if (session->rx_data_len == 0) {
		session->rx_stream = NULL;
	}

	ret = client->transport_ops->recv(client, &session->rx_stream,
					  session->rx_buf + session->rx_data_len, free_space,
					  &end_stream);
	if (ret == -EAGAIN) {
		/* No data currently available, nothing to do. */
		return;
	} else if (ret < 0) {
		NET_ERR("Failed to receive data, err %d", ret);
		transport_ok = false;
		goto abort;
	} else if (ret == 0 && !end_stream) {
		NET_DBG("Connection closed by the peer");
		ret = -ENOTCONN;
		transport_ok = false;
		goto abort;
	}

	session->rx_data_len += ret;

	ret = client_process_rx_data(client, http_ops, end_stream);
	if (ret < 0) {
		goto abort;
	}

	if (session->rx_data_len == sizeof(session->rx_buf)) {
		/* No unit could be processed and the buffer is now full, so no
		 * additional data can ever be received. Abort the connection.
		 */
		NET_ERR("RX buffer too small to process incoming data");
		ret = -ENOBUFS;
		goto abort;
	}

	return;

abort:
	(void)http_client_disconnect_locked(client, ret, transport_ok);
}

void http_client_received_data(struct http_client *client, int status)
{
	if (client == NULL) {
		return;
	}

	k_mutex_lock(&client->lock, K_FOREVER);

	if (client->session.state != HTTP_CLIENT_SESSION_OPEN) {
		NET_DBG("Client HTTP session closed");
	} else {
		client_receive_locked(client, status);
	}

	k_mutex_unlock(&client->lock);
}

/* Time to wait for a stream in one go: what is left until its deadline, capped
 * so that a session closed by another thread is noticed. Not positive once the
 * deadline has passed.
 */
static int64_t client_stream_wait_slice(const struct http_client_stream *stream)
{
	if (stream->timeout == INT64_MAX) {
		return HTTP_WAIT_SLICE_MS;
	}

	return MIN(stream->timeout - k_uptime_get(), HTTP_WAIT_SLICE_MS);
}

/* Receive on the calling thread, waiting for data for at most timeout_ms.
 * The client lock is released while waiting, so that another thread can
 * disconnect the session meanwhile. Must be called with the client lock held.
 */
static void client_sync_receive(struct http_client *client, struct http_client_stream *stream,
				int64_t timeout_ms)
{
	int ret;

	k_mutex_unlock(&client->lock);
	ret = client->transport_ops->wait(client, K_MSEC(timeout_ms));
	k_mutex_lock(&client->lock, K_FOREVER);

	if (stream->completed || ret == -EAGAIN) {
		return;
	}

	if (ret < 0) {
		NET_ERR("Transport wait error %d", ret);
		(void)http_client_disconnect_locked(client, ret, false);
		return;
	}

	client_receive_locked(client, 0);
}

/* Receive on the calling thread until the stream transaction completes or its
 * deadline expires. Must be called with the client lock held.
 */
static int http_stream_wait(struct http_client *client,
			    struct http_client_stream *stream)
{
	while (!stream->completed) {
		int64_t slice = client_stream_wait_slice(stream);

		if (slice <= 0) {
			return -ETIMEDOUT;
		}

		client_sync_receive(client, stream, slice);
	}

	return stream->result;
}

int http_client_stream_wait_tx(struct http_client *client, struct http_client_stream *stream)
{
	int64_t slice = client_stream_wait_slice(stream);

	if (slice <= 0) {
		return -ETIMEDOUT;
	}

	if (client->session.async) {
		/* The receive context makes the progress and signals it. */
		(void)k_condvar_wait(&client->tx_cond, &client->lock, K_MSEC(slice));
	} else {
		client_sync_receive(client, stream, slice);
	}

	if (stream->completed) {
		return (stream->result < 0) ? stream->result : -EALREADY;
	}

	return 0;
}

int http_client_send_request(struct http_client *client,
			     const struct http_client_request *request,
			     int32_t timeout, void *user_data)
{
	const struct http_client_ops *http_ops = NULL;
	struct http_client_stream *stream = NULL;
	bool headers_sent = false;
	bool has_payload;
	int ret;

	if (client == NULL || request == NULL) {
		return -EINVAL;
	}

	if (client->transport_ops == NULL) {
		NET_ERR("Client transport not initialized");
		return -EINVAL;
	}

	k_mutex_lock(&client->lock, K_FOREVER);

	if (client->session.state != HTTP_CLIENT_SESSION_OPEN) {
		NET_ERR("Client HTTP session closed");
		ret = -ENOTCONN;
		goto error;
	}

	http_ops = http_client_get_ops(client->version);
	if (http_ops == NULL) {
		NET_ERR("%s not supported", http_version_txt(client->version));
		ret = -EINVAL;
		goto error;
	}

	stream = http_stream_alloc(&client->session);
	if (stream == NULL) {
		NET_ERR("Failed to allocate new HTTP stream");
		ret = -ENOMEM;
		goto error;
	}

	http_stream_init(stream, request, timeout, user_data);
	stream->sending = true;

	if (http_ops->stream_open != NULL) {
		ret = http_ops->stream_open(client, stream);
		if (ret != 0) {
			NET_ERR("Failed to open a new stream, err %d", ret);
			goto error;
		}
	}

	has_payload = request->payload_cb != NULL ||
		      (request->payload != NULL && request->payload_len > 0);

	ret = http_ops->stream_send_headers(client, stream, request, !has_payload);
	if (ret != 0) {
		NET_ERR("Failed to send headers, err %d", ret);
		goto error;
	}

	headers_sent = true;

	if (has_payload) {
		ret = client_send_payload(client, stream, request, http_ops);
		if (ret != 0) {
			NET_ERR("Failed to send payload, err %d", ret);
			goto error;
		}
	}

	stream->sending = false;

	if (client->session.async) {
		/* In asynchronous mode we return without waiting; the RX path
		 * completes the transaction and releases the stream later. The
		 * response is delivered via the application callback, same as in
		 * synchronous mode.
		 */
		if (stream->completed) {
			/* It already arrived, while the request was being sent. */
			client_stream_finish(client, http_ops, stream);
		} else {
			http_client_timeout_reschedule(client);
		}

		goto out;
	}

	/* In synchronous mode, receive on this thread until the transaction
	 * completes or the deadline expires.
	 */
	ret = http_stream_wait(client, stream);
	if (ret < 0 && !stream->completed) {
		/* Timed out with the stream still open on the wire: tell the
		 * server to drop it and notify the application the same way as
		 * for any other aborted transaction.
		 */
		NET_ERR("Transaction timed out");

		if (http_ops->stream_reset != NULL) {
			(void)http_ops->stream_reset(client, stream);
		}

		client_transaction_aborted(client, request, &stream->response, ret,
					   user_data);
	} else if (ret < 0) {
		NET_ERR("Transaction did not complete, err %d", ret);
	}

	client_stream_finish(client, http_ops, stream);

out:
	k_mutex_unlock(&client->lock);

	return ret;

error:
	if (stream != NULL && stream->completed) {
		/* The receive path ended the transaction while the request was
		 * being sent, and has reported it already.
		 */
		NET_ERR("Transaction did not complete, err %d", ret);
	} else {
		/* Let the server know the stream is abandoned, but only if the
		 * request was already put on the wire. A stream with no headers
		 * sent is still idle, and resetting it would be a protocol
		 * violation.
		 */
		if (headers_sent && http_ops->stream_reset != NULL) {
			(void)http_ops->stream_reset(client, stream);
		}

		client_transaction_aborted(client, request,
					   (stream != NULL) ? &stream->response : NULL, ret,
					   user_data);
	}

	if (stream != NULL) {
		http_stream_release(&client->session, stream);
	}

	k_mutex_unlock(&client->lock);

	return ret;
}

int http_client_disconnect(struct http_client *client)
{
	int ret;

	if (client == NULL) {
		return -EINVAL;
	}

	if (client->transport_ops == NULL) {
		NET_ERR("Client transport not initialized");
		return -EINVAL;
	}

	k_mutex_lock(&client->lock, K_FOREVER);

	ret = http_client_disconnect_locked(client, 0, true);

	k_mutex_unlock(&client->lock);

	return ret;
}

/* Request building, shared by the protocol layers */

static const char *find_header_value(const struct http_client_header *headers,
				     size_t headers_count, const char *name)
{
	if (headers == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < headers_count; i++) {
		if (strcasecmp(headers[i].name, name) == 0) {
			return headers[i].value;
		}
	}

	return NULL;
}

/* Only token characters may make up a field name (RFC 9110, Section 5.6.2),
 * and a pseudo-header name is the protocol's to give.
 */
static bool field_name_is_valid(const char *name)
{
	if (name[0] == '\0' || name[0] == ':') {
		return false;
	}

	for (; *name != '\0'; name++) {
		unsigned char c = (unsigned char)*name;

		if (isalnum(c) == 0 && strchr("!#$%&'*+-.^_`|~", c) == NULL) {
			return false;
		}
	}

	return true;
}

/* A field value carries no control characters other than tab, and neither
 * starts nor ends with whitespace (RFC 9113, Section 8.2.1).
 */
static bool field_value_is_valid(const char *value)
{
	size_t len = strlen(value);

	if (len > 0 && (value[0] == ' ' || value[0] == '\t' || value[len - 1] == ' ' ||
			value[len - 1] == '\t')) {
		return false;
	}

	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)value[i];

		if (c != '\t' && iscntrl(c) != 0) {
			return false;
		}
	}

	return true;
}

/* An authority is a host, possibly bracketed, with an optional port: the
 * characters of those components only, and no userinfo (RFC 3986,
 * Section 3.2; RFC 9113, Section 8.3.1).
 */
static bool authority_is_valid(const char *authority)
{
	if (authority[0] == '\0') {
		return false;
	}

	for (; *authority != '\0'; authority++) {
		unsigned char c = (unsigned char)*authority;

		if (isalnum(c) == 0 && strchr("-._~!$&'()*+,;=:[]%", c) == NULL) {
			return false;
		}
	}

	return true;
}

/* Whether the request target is an absolute URL: a URI scheme (RFC 3986,
 * Section 3.1) followed by an authority. A colon on its own may well be part
 * of a path.
 */
static bool url_has_scheme(const char *url)
{
	if (isalpha((unsigned char)url[0]) == 0) {
		return false;
	}

	for (const char *p = url + 1; *p != '\0'; p++) {
		unsigned char c = (unsigned char)*p;

		if (c == ':') {
			return p[1] == '/' && p[2] == '/';
		}

		if (isalnum(c) == 0 && c != '+' && c != '-' && c != '.') {
			return false;
		}
	}

	return false;
}

/* Format host[:port] for the request. A configured host name takes precedence
 * over the address literal. An IPv6 literal is bracketed (RFC 3986,
 * Section 3.2.2) and the default port of the scheme is omitted.
 */
static int format_authority(struct http_client *client, const char *scheme, char *buf,
			    size_t buflen)
{
	const struct net_sockaddr *addr = (const struct net_sockaddr *)&client->session.server_addr;
	const char *host = client->session.host;
	char addr_str[NET_INET6_ADDRSTRLEN];
	bool bracket = false;
	bool default_port;
	uint16_t port;
	int ret;

	if (addr->sa_family == NET_AF_INET) {
		const struct net_sockaddr_in *sin = net_sin(addr);

		port = sys_be16_to_cpu(sin->sin_port);
		if (net_addr_ntop(NET_AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str)) ==
		    NULL) {
			return -EINVAL;
		}
	} else if (addr->sa_family == NET_AF_INET6) {
		const struct net_sockaddr_in6 *sin6 = net_sin6(addr);

		port = sys_be16_to_cpu(sin6->sin6_port);
		if (net_addr_ntop(NET_AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str)) ==
		    NULL) {
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	if (host == NULL) {
		host = addr_str;
		bracket = (addr->sa_family == NET_AF_INET6);
	}

	default_port = (strcmp(scheme, "https") == 0 && port == 443) ||
		       (strcmp(scheme, "http") == 0 && port == 80);

	if (bracket) {
		ret = default_port ? snprintk(buf, buflen, "[%s]", host) :
				     snprintk(buf, buflen, "[%s]:%u", host, port);
	} else {
		ret = default_port ? snprintk(buf, buflen, "%s", host) :
				     snprintk(buf, buflen, "%s:%u", host, port);
	}

	if (ret < 0 || ret >= buflen) {
		return -ENOBUFS;
	}

	return ret;
}

int http_client_request_line_get(struct http_client *client,
				 const struct http_client_request *req, char *buf, size_t buflen,
				 struct http_client_request_line *line)
{
	const char *authority;
	int ret;

	if (req->url == NULL) {
		return -EINVAL;
	}

	/* A tunnel has no request target and its body is the tunnel itself,
	 * which the request API does not express.
	 */
	if (req->method == HTTP_CONNECT) {
		return -ENOTSUP;
	}

	if (url_has_scheme(req->url)) {
		NET_ERR("Absolute URLs are not supported");
		return -EINVAL;
	}

	line->method = http_method_str(req->method);
	line->scheme = client->transport_ops->secure ? "https" : "http";

	/* An application Host header names the authority; the protocol layer
	 * carries it in its own way (:authority or Host), so the walker skips
	 * the header itself.
	 */
	authority = find_header_value(req->header_fields, req->headers_count, "host");
	if (authority != NULL && !authority_is_valid(authority)) {
		NET_ERR("Invalid Host header value");
		return -EINVAL;
	}

	if (authority == NULL) {
		ret = format_authority(client, line->scheme, buf, buflen);
		if (ret < 0) {
			return ret;
		}

		authority = buf;
		buf += ret + 1;
		buflen -= ret + 1;
	}

	line->authority = authority;
	line->path = req->url;

	/* The asterisk form is only meaningful for OPTIONS (RFC 9110,
	 * Section 7.1).
	 */
	if (req->method == HTTP_OPTIONS && strcmp(req->url, "*") == 0) {
		return 0;
	}

	if (req->url[0] != '/') {
		ret = snprintk(buf, buflen, "/%s", req->url);
		if (ret < 0 || ret >= buflen) {
			return -ENOBUFS;
		}

		line->path = buf;
	}

	return 0;
}

/* Field names are lowercase on the wire: HTTP/2 and HTTP/3 require it and
 * HTTP/1.1 does not care. Only a name with uppercase characters is copied.
 */
static int field_name_lower(const char *name, char *buf, size_t buflen, const char **out)
{
	size_t len = strlen(name);
	size_t i;

	for (i = 0; i < len; i++) {
		if (isupper((unsigned char)name[i]) != 0) {
			break;
		}
	}

	if (i == len) {
		*out = name;
		return 0;
	}

	if (len >= buflen) {
		return -ENOBUFS;
	}

	for (i = 0; i < len; i++) {
		buf[i] = (char)tolower((unsigned char)name[i]);
	}

	buf[len] = '\0';
	*out = buf;

	return 0;
}

/* Application fields the library sends itself or that have no place in the
 * request.
 */
struct field_filter {
	/* The protocol multiplexes the connection, so connection-specific
	 * fields are dropped (RFC 9113, Section 8.2.2).
	 */
	bool connection_specific;
	bool content_type;
	bool content_length;
};

/* Whether an application field is left out of the request. */
static bool field_is_filtered(const struct http_client_header *hdr,
			      const struct field_filter *filter)
{
	static const char *const connection_specific[] = {
		"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade",
	};

	/* Host is carried by the request line. */
	if (strcasecmp(hdr->name, "host") == 0) {
		return true;
	}

	if (filter->content_type && strcasecmp(hdr->name, "content-type") == 0) {
		return true;
	}

	if (filter->content_length && strcasecmp(hdr->name, "content-length") == 0) {
		return true;
	}

	if (!filter->connection_specific) {
		return false;
	}

	for (size_t i = 0; i < ARRAY_SIZE(connection_specific); i++) {
		if (strcasecmp(hdr->name, connection_specific[i]) == 0) {
			return true;
		}
	}

	/* TE may only ask for trailers. */
	if (strcasecmp(hdr->name, "te") == 0 && strcasecmp(hdr->value, "trailers") != 0) {
		return true;
	}

	return false;
}

static bool field_is_valid(const struct http_client_header *hdr)
{
	return hdr->name != NULL && hdr->value != NULL && field_name_is_valid(hdr->name) &&
	       field_value_is_valid(hdr->value);
}

/* Emit the application fields that pass the filter. The fields given up
 * front were validated before anything was sent; those from the callback
 * are validated here.
 */
static int emit_application_fields(const struct http_client_header *headers, size_t count,
				   bool validate, const struct field_filter *filter,
				   char *lower, size_t lower_len, http_client_field_cb_t cb,
				   void *user_data)
{
	int ret;

	if (headers == NULL) {
		return (count == 0) ? 0 : -EINVAL;
	}

	for (size_t i = 0; i < count; i++) {
		const char *name;

		if (validate && !field_is_valid(&headers[i])) {
			NET_ERR("Invalid request header field");
			return -EINVAL;
		}

		if (field_is_filtered(&headers[i], filter)) {
			NET_DBG("Request header %s dropped", headers[i].name);
			continue;
		}

		ret = field_name_lower(headers[i].name, lower, lower_len, &name);
		if (ret < 0) {
			return ret;
		}

		ret = cb(name, headers[i].value, user_data);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

int http_client_request_fields_foreach(struct http_client *client,
				       struct http_client_stream *stream, bool pseudo_headers,
				       http_client_field_cb_t cb, void *user_data)
{
	const struct http_client_request *req = stream->request;
	char buf[HTTP_CLIENT_REQUEST_FIELD_MAX_LEN];
	/* The length is only known up front for a payload passed in one piece. */
	bool content_length_known =
		(req->payload != NULL && req->payload_len > 0 && req->payload_cb == NULL);
	const struct field_filter filter = {
		.connection_specific = pseudo_headers,
		.content_type = (req->content_type_value != NULL),
		.content_length = content_length_known,
	};
	bool more = true;
	int ret;

	/* Nothing goes out before the fields given up front are known to be
	 * valid, so that a request is never left incomplete on the wire.
	 */
	if (req->header_fields == NULL && req->headers_count > 0) {
		return -EINVAL;
	}

	for (size_t i = 0; i < req->headers_count; i++) {
		if (!field_is_valid(&req->header_fields[i])) {
			NET_ERR("Invalid request header field");
			return -EINVAL;
		}
	}

	if (pseudo_headers) {
		struct http_client_request_line line;
		const char *const names[] = { ":method", ":scheme", ":path", ":authority" };
		const char *values[ARRAY_SIZE(names)];

		ret = http_client_request_line_get(client, req, buf, sizeof(buf), &line);
		if (ret < 0) {
			return ret;
		}

		values[0] = line.method;
		values[1] = line.scheme;
		values[2] = line.path;
		values[3] = line.authority;

		for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
			ret = cb(names[i], values[i], user_data);
			if (ret < 0) {
				return ret;
			}
		}
	}

	if (req->content_type_value != NULL) {
		ret = cb("content-type", req->content_type_value, user_data);
		if (ret < 0) {
			return ret;
		}
	}

	if (content_length_known) {
		char content_len[CONTENT_LENGTH_STR_SIZE];

		ret = snprintk(content_len, sizeof(content_len), "%zu", req->payload_len);
		if (ret < 0 || ret >= sizeof(content_len)) {
			return -ENOBUFS;
		}

		ret = cb("content-length", content_len, user_data);
		if (ret < 0) {
			return ret;
		}
	}

	ret = emit_application_fields(req->header_fields, req->headers_count, false, &filter,
				      buf, sizeof(buf), cb, user_data);
	if (ret < 0) {
		return ret;
	}

	if (req->headers_cb == NULL) {
		return 0;
	}

	while (more) {
		const struct http_client_header *headers = NULL;
		size_t count = 0;

		more = false;
		ret = req->headers_cb(&headers, &count, &more, stream->user_data);
		if (ret < 0) {
			return ret;
		}

		ret = emit_application_fields(headers, count, true, &filter, buf, sizeof(buf), cb,
					      user_data);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}
