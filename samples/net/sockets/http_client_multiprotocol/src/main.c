/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_http_client_sample, LOG_LEVEL_DBG);

#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/client_multiprotocol.h>

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)

#define CA_CERTIFICATE_TAG 1
#define TLS_PEER_HOSTNAME "localhost"

/* This is the same cert as what is found in net-tools/https-cert.pem file
 */
static const unsigned char ca_certificate[] = {
#include "https-cert.der.inc"
};

static const sec_tag_t sec_tag_list[] = {
	CA_CERTIFICATE_TAG,
};

#endif /* CONFIG_NET_SOCKETS_SOCKOPT_TLS */

#define HTTP_PORT "8080"
#define HTTPS_PORT "4443"

#if defined(CONFIG_NET_CONFIG_PEER_IPV6_ADDR)
#define SERVER_ADDR6  CONFIG_NET_CONFIG_PEER_IPV6_ADDR
#else
#define SERVER_ADDR6 ""
#endif

#if defined(CONFIG_NET_CONFIG_PEER_IPV4_ADDR)
#define SERVER_ADDR4  CONFIG_NET_CONFIG_PEER_IPV4_ADDR
#else
#define SERVER_ADDR4 ""
#endif

#define REQUEST_TIMEOUT_MS 5000

static const uint8_t put_payload[] = "{\"dummy\":\"payload\"}";

static int response_cb(struct http_client *client,
		       const struct http_client_request *request,
		       struct http_client_response *response,
		       enum http_client_transaction_status status,
		       void *user_data)
{
	const char *name = user_data;

	if (status == HTTP_CLIENT_TRANSACTION_ABORTED) {
		LOG_ERR("%s: transaction aborted, err %d", name, response->error);
		return 0;
	}

	/* Headers are reported once, in a callback carrying no body data. */
	if (response->header_count > 0) {
		LOG_INF("%s: status %u", name, response->status_code);
		if (response->content_len_present) {
			LOG_INF("%s: content length %zu", name, response->content_len);
		}

		for (size_t i = 0; i < response->header_count; i++) {
			LOG_INF("  header: %s: %s", response->headers[i].name,
				response->headers[i].value);
		}
	}

	if (response->body_len > 0) {
		LOG_INF("%s: %zu body bytes", name, response->body_len);
	}

	if (status == HTTP_CLIENT_RESPONSE_DATA_FINAL) {
		LOG_INF("%s: complete", name);
	}

	return 0;
}


int main(void)
{
#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	struct http_client_transport_tls_ctx ctx = {
		.config = {
			.sec_tag_list = sec_tag_list,
			.sec_tag_count = ARRAY_SIZE(sec_tag_list),
			.hostname = TLS_PEER_HOSTNAME,
		},
	};
	const char *port = HTTPS_PORT;
#else
	struct http_client_transport_tcp_ctx ctx = { 0 };
	const char *port = HTTP_PORT;
#endif
	struct http_client client;
	struct http_client_session_config cfg = { 0 };
	struct http_client_request request = { 0 };
	struct http_client_header request_headers[] = { { "accept", "*/*" } };
	struct zsock_addrinfo *res;
	int ret;

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	ret = tls_credential_add(CA_CERTIFICATE_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate, sizeof(ca_certificate));
	if (ret < 0) {
		LOG_ERR("Failed to register CA certificate, err: %d", ret);
		return ret;
	}
#endif

	ret = zsock_getaddrinfo(SERVER_ADDR4, port, NULL, &res);
	if (ret != 0) {
		LOG_ERR("Failed to resolve server address, err: %d", ret);
		return ret;
	}

	memcpy(&cfg.server_addr, res->ai_addr, res->ai_addrlen);
	cfg.addrlen = res->ai_addrlen;
#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	cfg.host = TLS_PEER_HOSTNAME;
#endif

	zsock_freeaddrinfo(res);

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	ret = http_client_init_tls(&client, HTTP_CLIENT_VERSION_2, &ctx);
#else
	ret = http_client_init_tcp(&client, HTTP_CLIENT_VERSION_2, &ctx);
#endif
	if (ret != 0) {
		LOG_ERR("Failed to initialize HTTP client, err: %d", ret);
		return ret;
	}

	ret = http_client_connect(&client, &cfg);
	if (ret != 0) {
		LOG_ERR("Failed to connect to the HTTP server, err: %d", ret);
		return ret;
	}

	request.method = HTTP_GET;
	request.url = "index.html";
	request.response_cb = response_cb;
	request.header_fields = request_headers;
	request.headers_count = 1;

	ret = http_client_send_request(&client, &request, REQUEST_TIMEOUT_MS, "IPv4 GET");
	if (ret != 0) {
		LOG_ERR("Failed to send HTTP request, err: %d", ret);
	}

	request.method = HTTP_PUT;
	request.url = "dummy.html";
	request.content_type_value = "application/json";
	request.payload = put_payload;
	request.payload_len = sizeof(put_payload) - 1;

	ret = http_client_send_request(&client, &request, REQUEST_TIMEOUT_MS, "IPv4 PUT");
	if (ret != 0) {
		LOG_ERR("Failed to send HTTP request, err: %d", ret);
	}

	ret = http_client_disconnect(&client);
	if (ret != 0) {
		LOG_ERR("Failed to disconnect from the HTTP server, err: %d", ret);
	}

	return 0;
}
