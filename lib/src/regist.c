/*
 * This file is part of Chiaki.
 *
 * Chiaki is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chiaki is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Chiaki.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "utils.h"

#include <chiaki/regist.h>
#include <chiaki/rpcrypt.h>
#include <chiaki/http.h>
#include <chiaki/random.h>
#include <chiaki/time.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <netdb.h>

#define REGIST_PORT 9295

#define SEARCH_REQUEST_SLEEP_MS 100
#define REGIST_SEARCH_TIMEOUT_MS 3000
#define REGIST_REPONSE_TIMEOUT_MS 3000

static void *regist_thread_func(void *user);
static ChiakiErrorCode regist_search(ChiakiRegist *regist, struct addrinfo *addrinfos);
static int regist_connect(ChiakiRegist *regist, struct addrinfo *addrinfos, int protocol);
static ChiakiErrorCode regist_recv_response(ChiakiRegist *regist, ChiakiRegisteredHost *host, int sock, ChiakiRPCrypt *rpcrypt);
static ChiakiErrorCode regist_parse_response_payload(ChiakiRegist *regist, ChiakiRegisteredHost *host, char *buf, size_t buf_size);

CHIAKI_EXPORT ChiakiErrorCode chiaki_regist_start(ChiakiRegist *regist, ChiakiLog *log, const ChiakiRegistInfo *info, ChiakiRegistCb cb, void *cb_user)
{
	regist->log = log;
	regist->info = *info;
	regist->info.host = strdup(regist->info.host);
	if(!regist->info.host)
		return CHIAKI_ERR_MEMORY;
	regist->info.psn_id = strdup(regist->info.psn_id);
	if(!regist->info.psn_id)
		goto error_host;

	regist->cb = cb;
	regist->cb_user = cb_user;

	ChiakiErrorCode err = chiaki_stop_pipe_init(&regist->stop_pipe);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_psn_id;

	err = chiaki_thread_create(&regist->thread, regist_thread_func, regist);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_stop_pipe;

	return CHIAKI_ERR_SUCCESS;

error_stop_pipe:
	chiaki_stop_pipe_fini(&regist->stop_pipe);
error_psn_id:
	free(regist->info.psn_id);
error_host:
	free(regist->info.host);
	return err;
}

CHIAKI_EXPORT void chiaki_regist_fini(ChiakiRegist *regist)
{
	chiaki_thread_join(&regist->thread, NULL);
	chiaki_stop_pipe_fini(&regist->stop_pipe);
	free(regist->info.host);
}

CHIAKI_EXPORT void chiaki_regist_stop(ChiakiRegist *regist)
{
	chiaki_stop_pipe_stop(&regist->stop_pipe);
}

static void regist_event_simple(ChiakiRegist *regist, ChiakiRegistEventType type)
{
	ChiakiRegistEvent event = { 0 };
	event.type = type;
	regist->cb(&event, regist->cb_user);
}

static const char * const request_fmt =
	"POST /sce/rp/regist HTTP/1.1\r\n"
	"HOST: 10.0.2.15\r\n" // random lol
	"User-Agent: remoteplay Windows\r\n"
	"Connection: close\r\n"
	"Content-Length: %llu\r\n\r\n";

static const char * const request_inner_fmt =
	"Client-Type: Windows\r\n"
	"Np-Online-Id: %s\r\n";

CHIAKI_EXPORT ChiakiErrorCode chiaki_regist_request_payload_format(uint8_t *buf, size_t *buf_size, ChiakiRPCrypt *crypt, const char *psn_id)
{
	size_t buf_size_val = *buf_size;
	static const size_t inner_header_off = 0x1e0;
	if(buf_size_val < inner_header_off)
		return CHIAKI_ERR_BUF_TOO_SMALL;
	memset(buf, 'A', inner_header_off);
	chiaki_rpcrypt_aeropause(buf + 0x11c, crypt->ambassador);
	int inner_header_size = snprintf((char *)buf + inner_header_off, buf_size_val - inner_header_off, request_inner_fmt, psn_id);
	if(inner_header_size < 0 || inner_header_size >= buf_size_val - inner_header_off)
		return CHIAKI_ERR_BUF_TOO_SMALL;
	ChiakiErrorCode err = chiaki_rpcrypt_encrypt(crypt, 0, buf + inner_header_off, buf + inner_header_off, inner_header_size);
	*buf_size = inner_header_off + inner_header_size;
	return err;
}

static void *regist_thread_func(void *user)
{
	ChiakiRegist *regist = user;

	bool canceled = false;
	bool success = false;

	uint8_t ambassador[CHIAKI_RPCRYPT_KEY_SIZE];
	ChiakiErrorCode err = chiaki_random_bytes_crypt(ambassador, sizeof(ambassador));
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to generate random nonce");
		goto fail;
	}

	ChiakiRPCrypt crypt;
	chiaki_rpcrypt_init_regist(&crypt, ambassador, regist->info.pin);

	uint8_t payload[0x400];
	size_t payload_size = sizeof(payload);
	err = chiaki_regist_request_payload_format(payload, &payload_size, &crypt, regist->info.psn_id);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to format payload");
		goto fail;
	}

	char request_header[0x100];
	int request_header_size = snprintf(request_header, sizeof(request_header), request_fmt, (unsigned long long)payload_size);
	if(request_header_size >= sizeof(request_header))
	{
		CHIAKI_LOGE(regist->log, "Regist failed to format request");
		goto fail;
	}

	struct addrinfo *addrinfos;
	int r = getaddrinfo(regist->info.host, NULL, NULL, &addrinfos);
	if(r != 0)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to getaddrinfo on %s", regist->info.host);
		goto fail;
	}

	err = regist_search(regist, addrinfos); // TODO: get addr from response
	if(err != CHIAKI_ERR_SUCCESS)
	{
		if(err == CHIAKI_ERR_CANCELED)
			canceled = true;
		else
			CHIAKI_LOGE(regist->log, "Regist search failed");
		goto fail_addrinfos;
	}

	err = chiaki_stop_pipe_sleep(&regist->stop_pipe, SEARCH_REQUEST_SLEEP_MS); // PS4 doesn't accept requests immediately
	if(err != CHIAKI_ERR_TIMEOUT)
	{
		canceled = true;
		goto fail_addrinfos;
	}

	int sock = regist_connect(regist, addrinfos, IPPROTO_TCP);
	if(sock < 0)
	{
		CHIAKI_LOGE(regist->log, "Regist eventually failed to connect for request");
		goto fail_addrinfos;
	}

	CHIAKI_LOGI(regist->log, "Regist connected to %s, sending request", regist->info.host);

	int s = send(sock, request_header, request_header_size, 0);
	if(s < 0)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to send request header: %s", strerror(errno));
		goto fail_socket;
	}

	s = send(sock, payload, payload_size, 0);
	if(s < 0)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to send payload: %s", strerror(errno));
		goto fail_socket;
	}

	CHIAKI_LOGI(regist->log, "Regist waiting for response");

	ChiakiRegisteredHost host;
	err = regist_recv_response(regist, &host, sock, &crypt);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		if(err == CHIAKI_ERR_CANCELED)
			canceled = true;
		else
			CHIAKI_LOGE(regist->log, "Regist eventually failed");
		goto fail_socket;
	}

	CHIAKI_LOGI(regist->log, "Regist successfully received response");

	success = true;

fail_socket:
	close(sock);
fail_addrinfos:
	freeaddrinfo(addrinfos);
fail:
	if(canceled)
	{
		CHIAKI_LOGI(regist->log, "Regist canceled");
		regist_event_simple(regist, CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED);
	}
	else if(success)
	{
		ChiakiRegistEvent event = { 0 };
		event.type = CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS;
		event.registered_host = &host;
		regist->cb(&event, regist->cb_user);
	}
	else
		regist_event_simple(regist, CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED);
	return NULL;
}

static ChiakiErrorCode regist_search(ChiakiRegist *regist, struct addrinfo *addrinfos)
{
	CHIAKI_LOGI(regist->log, "Regist starting search");
	int sock = regist_connect(regist, addrinfos, IPPROTO_UDP);
	if(sock < 0)
	{
		CHIAKI_LOGE(regist->log, "Regist eventually failed to connect for search");
		return CHIAKI_ERR_NETWORK;
	}

	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;

	CHIAKI_LOGI(regist->log, "Regist sending search packet");
	int r = send(sock, "SRC2", 4, 0);
	if(r < 0)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to send search: %s", strerror(errno));
		goto done;
	}

	uint64_t timeout_abs_ms = chiaki_time_now_monotonic_ms() + REGIST_SEARCH_TIMEOUT_MS;
	while(true)
	{
		uint64_t now_ms = chiaki_time_now_monotonic_ms();
		if(now_ms > timeout_abs_ms)
			err = CHIAKI_ERR_TIMEOUT;
		else
			err = chiaki_stop_pipe_select_single(&regist->stop_pipe, sock, timeout_abs_ms - now_ms);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			if(err == CHIAKI_ERR_TIMEOUT)
				CHIAKI_LOGE(regist->log, "Regist timed out waiting for search response");
			break;
		}

		uint8_t buf[0x100];
		struct sockaddr recv_addr;
		socklen_t recv_addr_size;
		recv_addr_size = sizeof(recv_addr);
		ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, &recv_addr, &recv_addr_size);
		if(n <= 0)
		{
			if(n < 0)
				CHIAKI_LOGE(regist->log, "Regist failed to receive search response: %s", strerror(errno));
			else
				CHIAKI_LOGE(regist->log, "Regist failed to receive search response");
			err = CHIAKI_ERR_NETWORK;
			goto done;
		}

		CHIAKI_LOGV(regist->log, "Regist received packet:");
		chiaki_log_hexdump(regist->log, CHIAKI_LOG_VERBOSE, buf, n);

		if(n >= 4 && memcmp(buf, "RES2", 4) == 0)
		{
			CHIAKI_LOGI(regist->log, "Regist received search response"); // TODO: print from <addr>
			break;
		}
	}

done:
	close(sock);
	return err;
}

static int regist_connect(ChiakiRegist *regist, struct addrinfo *addrinfos, int protocol)
{
	int sock = -1;
	for(struct addrinfo *ai=addrinfos; ai; ai=ai->ai_next)
	{
		if(ai->ai_protocol != protocol)
			continue;

		if(ai->ai_addr->sa_family != AF_INET) // TODO: support IPv6
			continue;

		struct sockaddr *sa = malloc(ai->ai_addrlen);
		if(!sa)
			continue;
		memcpy(sa, ai->ai_addr, ai->ai_addrlen);

		set_port(sa, htons(REGIST_PORT));

		sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if(sock < 0)
		{
			free(sa);
			continue;
		}
		int r = connect(sock, sa, ai->ai_addrlen);
		if(r < 0)
		{
			int errsv = errno;
			CHIAKI_LOGE(regist->log, "Regist connect failed: %s", strerror(errsv));
			close(sock);
			sock = -1;
			free(sa);
			continue;
		}
		free(sa);
		break;
	}

	return sock;
}

static ChiakiErrorCode regist_recv_response(ChiakiRegist *regist, ChiakiRegisteredHost *host, int sock, ChiakiRPCrypt *rpcrypt)
{
	uint8_t buf[0x200];
	size_t buf_filled_size;
	size_t header_size;
	ChiakiErrorCode err = chiaki_recv_http_header(sock, (char *)buf, sizeof(buf), &header_size, &buf_filled_size, &regist->stop_pipe, REGIST_REPONSE_TIMEOUT_MS);
	if(err == CHIAKI_ERR_CANCELED)
		return err;
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to receive response HTTP header");
		return err;
	}

	CHIAKI_LOGV(regist->log, "Regist response HTTP header:");
	chiaki_log_hexdump(regist->log, CHIAKI_LOG_VERBOSE, buf, header_size);

	ChiakiHttpResponse http_response;
	err = chiaki_http_response_parse(&http_response, (char *)buf, header_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to pare response HTTP header");
		return err;
	}

	if(http_response.code != 200)
	{
		CHIAKI_LOGE(regist->log, "Regist received HTTP code %d", http_response.code);
		chiaki_http_response_fini(&http_response);
		return CHIAKI_ERR_UNKNOWN;
	}

	size_t content_size = 0;
	for(ChiakiHttpHeader *header=http_response.headers; header; header=header->next)
	{
		if(strcmp(header->key, "Content-Length") == 0)
		{
			content_size = (size_t)strtoull(header->value, NULL, 0);
		}
	}

	chiaki_http_response_fini(&http_response);

	if(!content_size)
	{
		CHIAKI_LOGE(regist->log, "Regist response does not contain or contains invalid Content-Length");
		return CHIAKI_ERR_INVALID_RESPONSE;
	}

	if(content_size + header_size > sizeof(buf))
	{
		CHIAKI_LOGE(regist->log, "Regist response content too big");
		return CHIAKI_ERR_BUF_TOO_SMALL;
	}

	while(buf_filled_size < content_size + header_size)
	{
		err = chiaki_stop_pipe_select_single(&regist->stop_pipe, sock, REGIST_REPONSE_TIMEOUT_MS);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			if(err == CHIAKI_ERR_TIMEOUT)
				CHIAKI_LOGE(regist->log, "Regist timed out receiving response content");
			return err;
		}

		ssize_t received = recv(sock, buf + buf_filled_size, (content_size + header_size) - buf_filled_size, 0);
		if(received <= 0)
		{
			CHIAKI_LOGE(regist->log, "Regist failed to receive response content");
			return CHIAKI_ERR_NETWORK;
		}
		buf_filled_size += received;
	}

	uint8_t *payload = buf + header_size;
	size_t payload_size = buf_filled_size - header_size;
	chiaki_rpcrypt_decrypt(rpcrypt, 0, payload, payload, payload_size);

	CHIAKI_LOGI(regist->log, "Regist response payload (decrypted):");
	chiaki_log_hexdump(regist->log, CHIAKI_LOG_VERBOSE, payload, payload_size);

	err = regist_parse_response_payload(regist, host, (char *)payload, payload_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to parse response payload");
		return err;
	}

	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode regist_parse_response_payload(ChiakiRegist *regist, ChiakiRegisteredHost *host, char *buf, size_t buf_size)
{
	ChiakiHttpHeader *headers;
	ChiakiErrorCode err = chiaki_http_header_parse(&headers, buf, buf_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(regist->log, "Regist failed to parse response payload HTTP header");
		return err;
	}

	memset(host, 0, sizeof(*host));

	bool mac_found = false;
	bool regist_key_found = false;
	bool key_found = false;

	for(ChiakiHttpHeader *header=headers; header; header=header->next)
	{
#define COPY_STRING(name, key_str) \
		if(strcmp(header->key, key_str) == 0) \
		{ \
			size_t len = strlen(header->value); \
			if(len >= sizeof(host->name)) \
			{ \
				CHIAKI_LOGE(regist->log, "Regist value for " key_str " in response is too long"); \
				continue; \
			} \
			memcpy(host->name, header->value, len); \
			host->name[len] = 0; \
			continue; \
		}
		COPY_STRING(ap_ssid, "AP-Ssid")
		COPY_STRING(ap_bssid, "AP-Bssid")
		COPY_STRING(ap_key, "AP-Key")
		COPY_STRING(ap_name, "AP-Name")
		COPY_STRING(ps4_nickname, "PS4-Nickname")
#undef COPY_STRING

		if(strcmp(header->key, "PS4-RegistKey") == 0)
		{
			memset(host->rp_regist_key, 0, sizeof(host->rp_regist_key));
			size_t buf_size = sizeof(host->rp_regist_key);
			err = parse_hex((uint8_t *)host->rp_regist_key, &buf_size, header->value, strlen(header->value));
			if(err != CHIAKI_ERR_SUCCESS)
			{
				CHIAKI_LOGE(regist->log, "Regist received invalid RegistKey in response");
				memset(host->rp_regist_key, 0, sizeof(host->rp_regist_key));
			}
			else
			{
				regist_key_found = true;
			}
		}
		else if(strcmp(header->key, "RP-KeyType") == 0)
		{
			host->rp_key_type = (uint32_t)strtoul(header->value, NULL, 0);
		}
		else if(strcmp(header->key, "RP-Key") == 0)
		{
			size_t buf_size = sizeof(host->rp_key);
			err = parse_hex((uint8_t *)host->rp_key, &buf_size, header->value, strlen(header->value));
			if(err != CHIAKI_ERR_SUCCESS || buf_size != sizeof(host->rp_key))
			{
				CHIAKI_LOGE(regist->log, "Regist received invalid key in response");
				memset(host->rp_key, 0, sizeof(host->rp_key));
			}
			else
			{
				key_found = true;
			}
		}
		else if(strcmp(header->key, "PS4-Mac") == 0)
		{
			size_t buf_size = sizeof(host->ps4_mac);
			err = parse_hex((uint8_t *)host->ps4_mac, &buf_size, header->value, strlen(header->value));
			if(err != CHIAKI_ERR_SUCCESS || buf_size != sizeof(host->ps4_mac))
			{
				CHIAKI_LOGE(regist->log, "Regist received invalid MAC Address in response");
				memset(host->ps4_mac, 0, sizeof(host->ps4_mac));
			}
			else
			{
				mac_found = true;
			}
		}
		else
		{
			CHIAKI_LOGI(regist->log, "Regist received unknown key %s in response payload", header->key);
		}
	}

	chiaki_http_header_free(headers);

	if(!regist_key_found)
	{
		CHIAKI_LOGE(regist->log, "Regist response is missing RegistKey (or it was invalid)");
		return CHIAKI_ERR_INVALID_RESPONSE;
	}

	if(!key_found)
	{
		CHIAKI_LOGE(regist->log, "Regist response is missing key (or it was invalid)");
		return CHIAKI_ERR_INVALID_RESPONSE;
	}

	if(!mac_found)
	{
		CHIAKI_LOGE(regist->log, "Regist response is missing MAC Adress (or it was invalid)");
		return CHIAKI_ERR_INVALID_RESPONSE;
	}

	return CHIAKI_ERR_SUCCESS;
}