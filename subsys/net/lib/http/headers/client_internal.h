/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HTTP_CLIENT_INTERNAL_H_
#define HTTP_CLIENT_INTERNAL_H_

#include <zephyr/net/http/client_multiprotocol.h>

/* Internal stream API */

struct http_client_ops {
	int (*session_open)(struct http_client *client);
	int (*session_close)(struct http_client *client, int reason);
	int (*stream_open)(struct http_client *client, struct http_client_stream *stream);
	int (*stream_close)(struct http_client *client, struct http_client_stream *stream);
	int (*stream_reset)(struct http_client *client, struct http_client_stream *stream);
	int (*stream_send_headers)(struct http_client *client, struct http_client_stream *stream,
				   const struct http_client_request *req, bool end_stream);
	int (*stream_send_data)(struct http_client *client, struct http_client_stream *stream,
				const uint8_t *data, size_t len, bool end_stream);
	/* Process received data. transport_stream is the transport stream the
	 * data came from, NULL on a single-stream transport, and end_stream is
	 * set when no more data follows the len bytes on that stream; it is
	 * then called even with len 0. Returns the number of bytes consumed,
	 * -EAGAIN when more data is needed (not with end_stream set, as none
	 * will come), or a negative errno on error.
	 */
	int (*process_data)(struct http_client *client, void *transport_stream, uint8_t *data,
			    size_t len, bool end_stream);
};

#if defined(CONFIG_HTTP_CLIENT_VERSION_2)
const struct http_client_ops *http_client_get_http2_ops(void);
#else
static inline const struct http_client_ops *http_client_get_http2_ops(void) { return NULL; }
#endif

/* Look up an active stream by its protocol stream identifier. A completed
 * stream is not found, even while it waits to be released. Returns NULL if no
 * matching stream exists. Must be called with the client lock held.
 */
struct http_client_stream *http_client_stream_lookup(struct http_client *client,
						     uint64_t stream_id);

/* Notify the client that a stream transaction has completed, either because the
 * full response was received (result == 0) or because it was aborted
 * (result < 0). Must be called with the client lock held (e. g. from a
 * process_data() implementation). On an error result the application is
 * notified via its response callback. In asynchronous mode the stream is closed
 * and released here, unless its request is still being sent; otherwise the
 * thread in http_client_send_request() does that.
 */
void http_client_stream_set_complete(struct http_client *client,
				     struct http_client_stream *stream, int result);

/* Let the receive path make progress while a request cannot be sent further,
 * e. g. for lack of flow control credit. Receives on the calling thread in
 * synchronous mode, waits for http_client_tx_wakeup() in asynchronous mode. The
 * wait is bounded, so the caller re-checks its condition and calls again.
 * Must be called with the client lock held, which is released while waiting.
 *
 * Returns 0 when the caller should re-check, -ETIMEDOUT when the stream
 * deadline has passed, -EALREADY when the transaction completed successfully
 * in the meantime, or the negative result it was aborted with.
 */
int http_client_stream_wait_tx(struct http_client *client, struct http_client_stream *stream);

/* Wake threads waiting in http_client_stream_wait_tx(). Must be called with
 * the client lock held.
 */
void http_client_tx_wakeup(struct http_client *client);

/* Request building, shared by the protocol layers. */

/* Longest field name, path or authority the client handles. */
#define HTTP_CLIENT_REQUEST_FIELD_MAX_LEN 256U

/* The request line in protocol-neutral form: what HTTP/2 and HTTP/3 send as
 * pseudo-headers and HTTP/1.1 as its request line and Host header. The
 * strings point into the request, the session or the buffer given to
 * http_client_request_line_get().
 */
struct http_client_request_line {
	const char *method;
	const char *scheme;
	const char *authority;
	const char *path;
};

/* Fill in the request line of req. buf backs the authority, host[:port] from
 * the application Host header or the server address, and the path, which
 * always starts with a slash.
 */
int http_client_request_line_get(struct http_client *client,
				 const struct http_client_request *req, char *buf, size_t buflen,
				 struct http_client_request_line *line);

/* Receives one header field. A negative return stops the walk with that
 * error. The strings are only valid during the call.
 */
typedef int (*http_client_field_cb_t)(const char *name, const char *value, void *user_data);

/* Walk the header fields of the stream's request in wire order: the request
 * line as :method, :scheme, :path and :authority when pseudo_headers is set,
 * the fields the library adds (content-type, and content-length when the
 * payload length is known up front), then the application fields including
 * those from headers_cb. Names are lowercase. Fields the library sends
 * itself are left out: Host, as the request line carries the authority, an
 * application Content-Type when the request sets content_type_value, and
 * Content-Length when the library adds one. With pseudo_headers set the
 * connection-specific fields of HTTP/1.1 are dropped as well. A field whose
 * name is not a token or starts with a colon, or whose value carries control
 * characters other than tab or leading or trailing whitespace, fails with
 * -EINVAL; the fields given up front are checked before anything is emitted.
 * The value of an application Host header must be a valid authority.
 */
int http_client_request_fields_foreach(struct http_client *client,
				       struct http_client_stream *stream, bool pseudo_headers,
				       http_client_field_cb_t cb, void *user_data);

/* End the session from the protocol layer, e. g. once the peer has shut the
 * connection down: remaining streams are aborted, the peer is notified and the
 * transport closed. A reason of 0 stands for a regular shutdown. Must be called
 * with the client lock held.
 */
void http_client_session_terminate(struct http_client *client, int reason);

#endif /* HTTP_CLIENT_INTERNAL_H_ */
