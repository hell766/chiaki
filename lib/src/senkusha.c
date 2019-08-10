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

#include <chiaki/senkusha.h>
#include <chiaki/session.h>
#include <chiaki/random.h>
#include <chiaki/time.h>

#include <string.h>
#include <assert.h>

#include "utils.h"
#include "pb_utils.h"

#include <takion.pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <pb.h>
#include <chiaki/takion.h>


#define SENKUSHA_PORT 9297

#define EXPECT_TIMEOUT_MS 5000

#define SENKUSHA_PING_COUNT_DEFAULT 10
#define EXPECT_PONG_TIMEOUT_MS 1000

typedef enum {
	STATE_IDLE,
	STATE_TAKION_CONNECT,
	STATE_EXPECT_BANG,
	STATE_EXPECT_DATA_ACK,
	STATE_EXPECT_PONG,
	STATE_EXPECT_MTU
} SenkushaState;

static ChiakiErrorCode senkusha_run_ping_test(ChiakiSenkusha *senkusha, uint16_t ping_test_index, uint16_t ping_count);
static ChiakiErrorCode senkusha_run_mtu_in_test(ChiakiSenkusha *senkusha, uint32_t min, uint32_t max, uint32_t retries, uint64_t timeout_ms, uint32_t *mtu);
static void senkusha_takion_cb(ChiakiTakionEvent *event, void *user);
static void senkusha_takion_data(ChiakiSenkusha *senkusha, ChiakiTakionMessageDataType data_type, uint8_t *buf, size_t buf_size);
static void senkusha_takion_data_ack(ChiakiSenkusha *senkusha, ChiakiSeqNum32 seq_num);
static void senkusha_takion_av(ChiakiSenkusha *senkusha, ChiakiTakionAVPacket *packet);
static ChiakiErrorCode senkusha_send_big(ChiakiSenkusha *senkusha);
static ChiakiErrorCode senkusha_send_disconnect(ChiakiSenkusha *senkusha);
static ChiakiErrorCode senkusha_send_echo_command(ChiakiSenkusha *senkusha, bool enable);
static ChiakiErrorCode senkusha_send_mtu_command(ChiakiSenkusha *senkusha, tkproto_SenkushaMtuCommand *command);
static ChiakiErrorCode senkusha_send_client_mtu_command(ChiakiSenkusha *senkusha, tkproto_SenkushaClientMtuCommand *command);
static ChiakiErrorCode senkusha_send_data_wait_for_ack(ChiakiSenkusha *senkusha, uint8_t *buf, size_t buf_size);

CHIAKI_EXPORT ChiakiErrorCode chiaki_senkusha_init(ChiakiSenkusha *senkusha, ChiakiSession *session)
{
	senkusha->session = session;
	senkusha->log = session->log;

	ChiakiErrorCode err = chiaki_mutex_init(&senkusha->state_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;

	err = chiaki_cond_init(&senkusha->state_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_state_mutex;

	senkusha->state = STATE_IDLE;
	senkusha->state_finished = false;
	senkusha->state_failed = false;
	senkusha->should_stop = false;
	senkusha->data_ack_seq_num_expected = 0;
	senkusha->ping_tag = 0;
	senkusha->pong_time_us = 0;

	return CHIAKI_ERR_SUCCESS;

error_state_mutex:
	chiaki_mutex_fini(&senkusha->state_mutex);
error:
	return err;
}

CHIAKI_EXPORT void chiaki_senkusha_fini(ChiakiSenkusha *senkusha)
{
	chiaki_cond_fini(&senkusha->state_cond);
	chiaki_mutex_fini(&senkusha->state_mutex);
}

static bool state_finished_cond_check(void *user)
{
	ChiakiSenkusha *senkusha = user;
	return senkusha->state_finished || senkusha->should_stop;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_senkusha_run(ChiakiSenkusha *senkusha)
{
	ChiakiSession *session = senkusha->session;
	ChiakiErrorCode err;

	err = chiaki_mutex_lock(&senkusha->state_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);

#define QUIT(quit_label) do { \
		chiaki_mutex_unlock(&senkusha->state_mutex); \
		goto quit_label; \
	} while(0)

	if(senkusha->should_stop)
	{
		err = CHIAKI_ERR_CANCELED;
		goto quit;
	}

	ChiakiTakionConnectInfo takion_info;
	takion_info.log = senkusha->log;
	takion_info.sa_len = session->connect_info.host_addrinfo_selected->ai_addrlen;
	takion_info.sa = malloc(takion_info.sa_len);
	if(!takion_info.sa)
	{
		err = CHIAKI_ERR_MEMORY;
		QUIT(quit);
	}

	memcpy(takion_info.sa, session->connect_info.host_addrinfo_selected->ai_addr, takion_info.sa_len);
	err = set_port(takion_info.sa, htons(SENKUSHA_PORT));
	assert(err == CHIAKI_ERR_SUCCESS);

	takion_info.enable_crypt = false;
	takion_info.protocol_version = 7;

	takion_info.cb = senkusha_takion_cb;
	takion_info.cb_user = senkusha;

	senkusha->state = STATE_TAKION_CONNECT;
	senkusha->state_finished = false;
	senkusha->state_failed = false;

	err = chiaki_takion_connect(&senkusha->takion, &takion_info);
	free(takion_info.sa);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(session->log, "Senkusha connect failed");
		QUIT(quit);
	}

	err = chiaki_cond_timedwait_pred(&senkusha->state_cond, &senkusha->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, senkusha);
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);
	if(!senkusha->state_finished)
	{
		if(err == CHIAKI_ERR_TIMEOUT)
			CHIAKI_LOGE(session->log, "Senkusha connect timeout");

		if(senkusha->should_stop)
			err = CHIAKI_ERR_CANCELED;
		else
			CHIAKI_LOGE(session->log, "Senkusha Takion connect failed");

		QUIT(quit_takion);
	}

	CHIAKI_LOGI(session->log, "Senkusha sending big");

	senkusha->state = STATE_EXPECT_BANG;
	senkusha->state_finished = false;
	senkusha->state_failed = false;
	err = senkusha_send_big(senkusha);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(session->log, "Senkusha failed to send big");
		QUIT(quit_takion);
	}

	err = chiaki_cond_timedwait_pred(&senkusha->state_cond, &senkusha->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, senkusha);
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);

	if(!senkusha->state_finished)
	{
		if(err == CHIAKI_ERR_TIMEOUT)
			CHIAKI_LOGE(session->log, "Senkusha bang receive timeout");

		if(senkusha->should_stop)
			err = CHIAKI_ERR_CANCELED;
		else
			CHIAKI_LOGE(session->log, "Senkusha didn't receive bang");

		QUIT(quit_takion);
	}

	CHIAKI_LOGI(session->log, "Senkusha successfully received bang");

	err = senkusha_run_ping_test(senkusha, 0, SENKUSHA_PING_COUNT_DEFAULT);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha Ping Test failed");
		goto disconnect;
	}

	// TODO: timeout should be measured rtt * 5
	uint32_t mtu_in;
	err = senkusha_run_mtu_in_test(senkusha, 576, 1454, 3, 200, &mtu_in);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha MTU test failed");
		goto disconnect;
	}

disconnect:
	CHIAKI_LOGI(session->log, "Senkusha is disconnecting");

	senkusha_send_disconnect(senkusha);
	chiaki_mutex_unlock(&senkusha->state_mutex);

quit_takion:
	chiaki_takion_close(&senkusha->takion);
	CHIAKI_LOGI(session->log, "Senkusha closed takion");
quit:
	return err;
}

static ChiakiErrorCode senkusha_run_ping_test(ChiakiSenkusha *senkusha, uint16_t ping_test_index, uint16_t ping_count)
{
	CHIAKI_LOGI(senkusha->log, "Senkusha Ping Test with count %u starting", (unsigned int)ping_count);

	ChiakiErrorCode err = senkusha_send_echo_command(senkusha, true);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha Ping Test failed because sending echo command (true) failed");
		return err;
	}

	CHIAKI_LOGI(senkusha->log, "Senkusha enabled echo");

	for(uint16_t ping_index=0; ping_index<ping_count; ping_index++)
	{
		CHIAKI_LOGI(senkusha->log, "Senkusha sending Ping %u of test index %u", (unsigned int)ping_index, (unsigned int)ping_test_index);

		ChiakiTakionAVPacket av_packet = { 0 };
		av_packet.codec = 0xff;
		av_packet.is_video = false;
		av_packet.frame_index = ping_test_index;
		av_packet.unit_index = ping_index;
		av_packet.units_in_frame_total = 0x800; // or 0

		uint8_t data[0x224];
		memset(data, 0, sizeof(data));

		size_t header_size;
		err = chiaki_takion_v7_av_packet_format_header(data, sizeof(data), &header_size, &av_packet);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(senkusha->log, "Senkusha failed to format AV Header");
			return err;
		}

		uint32_t tag = 0x1337; //chiaki_random_32();
		*((uint32_t *)(data + header_size + 4)) = htonl(tag);

		senkusha->state = STATE_EXPECT_PONG;
		senkusha->state_finished = false;
		senkusha->state_failed = false;
		senkusha->ping_test_index = ping_test_index;
		senkusha->ping_index = ping_index;
		senkusha->ping_tag = tag;

		uint64_t time_start_us = chiaki_time_now_monotonic_us();

		err = chiaki_takion_send_raw(&senkusha->takion, data, sizeof(data));
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(senkusha->log, "Senkusha failed to send ping");
			return err;
		}

		err = chiaki_cond_timedwait_pred(&senkusha->state_cond, &senkusha->state_mutex, EXPECT_PONG_TIMEOUT_MS, state_finished_cond_check, senkusha);
		assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);

		if(!senkusha->state_finished)
		{
			if(err == CHIAKI_ERR_TIMEOUT)
				CHIAKI_LOGE(senkusha->log, "Senkusha pong receive timeout");

			if(senkusha->should_stop)
				return CHIAKI_ERR_CANCELED;
			else
				CHIAKI_LOGE(senkusha->log, "Senkusha failed to receive pong");

			continue;
		}

		uint64_t delta_us = senkusha->pong_time_us - time_start_us;
		CHIAKI_LOGI(senkusha->log, "Senkusha received Pong, RTT = %.3f ms", (float)delta_us * 0.001f);
	}

	err = senkusha_send_echo_command(senkusha, false);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha Ping Test failed because sending echo command (false) failed");
		return err;
	}

	CHIAKI_LOGI(senkusha->log, "Senkusha disabled echo");

	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode senkusha_run_mtu_in_test(ChiakiSenkusha *senkusha, uint32_t min, uint32_t max, uint32_t retries, uint64_t timeout_ms, uint32_t *mtu)
{
	CHIAKI_LOGI(senkusha->log, "Senkusha starting MTU test with min %u, max %u, retries %u, timeout %llu ms",
			(unsigned int)min, (unsigned int)max, (unsigned int)retries, (unsigned long long)timeout_ms);

	uint32_t cur = max;
	uint32_t request_id = 0;
	while(max > min)
	{
		bool success = false;
		for(uint32_t attempt=0; attempt<retries; attempt++)
		{
			senkusha->state = STATE_EXPECT_MTU;
			senkusha->state_finished = false;
			senkusha->mtu_id = ++request_id;

			tkproto_SenkushaMtuCommand mtu_cmd;
			mtu_cmd.id = request_id;
			mtu_cmd.mtu_req = cur;
			mtu_cmd.num = 1;
			ChiakiErrorCode err = senkusha_send_mtu_command(senkusha, &mtu_cmd);
			if(err != CHIAKI_ERR_SUCCESS)
			{
				CHIAKI_LOGE(senkusha->log, "Senkusha failed to send MTU command");
				return err;
			}

			CHIAKI_LOGI(senkusha->log, "Senkusha MTU request %u (min %u, max %u), id %u, attempt %u",
					(unsigned int)cur, (unsigned int)min, (unsigned int)max, (unsigned int)request_id, (unsigned int)attempt);

			err = chiaki_cond_timedwait_pred(&senkusha->state_cond, &senkusha->state_mutex, timeout_ms, state_finished_cond_check, senkusha);
			assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);

			if(!senkusha->state_finished)
			{
				if(err == CHIAKI_ERR_TIMEOUT)
				{
					CHIAKI_LOGI(senkusha->log, "Senkusha MTU %u timeout", (unsigned int)cur);
					continue;
				}

				if(senkusha->should_stop)
					return CHIAKI_ERR_CANCELED;
				else
					CHIAKI_LOGE(senkusha->log, "Senkusha failed to receive MTU response");
			}

			CHIAKI_LOGI(senkusha->log, "Senkusha MTU %u success", (unsigned int)cur);
			success = true;
			break;
		}

		if(success)
			min = cur + 1;
		else
			max = cur - 1;
		cur = min + (max - min) / 2;
	}

	CHIAKI_LOGI(senkusha->log, "Senkusha determined MTU %u", (unsigned int)max);
	*mtu = max;

	/*tkproto_SenkushaClientMtuCommand client_mtu_cmd;
	client_mtu_cmd.id = 2;
	client_mtu_cmd.state = false;
	client_mtu_cmd.mtu_req = 1454;
	client_mtu_cmd.has_mtu_down = true;
	client_mtu_cmd.mtu_down = 1454;
	ChiakiErrorCode err = senkusha_send_client_mtu_command(senkusha, &client_mtu_cmd);
	CHIAKI_LOGD(senkusha->log, "MTU result: %d\n", err);*/

	return CHIAKI_ERR_SUCCESS;
}

static void senkusha_takion_cb(ChiakiTakionEvent *event, void *user)
{
	ChiakiSenkusha *senkusha = user;
	switch(event->type)
	{
		case CHIAKI_TAKION_EVENT_TYPE_CONNECTED:
		case CHIAKI_TAKION_EVENT_TYPE_DISCONNECT:
			chiaki_mutex_lock(&senkusha->state_mutex);
			if(senkusha->state == STATE_TAKION_CONNECT)
			{
				senkusha->state_finished = event->type == CHIAKI_TAKION_EVENT_TYPE_CONNECTED;
				senkusha->state_failed = event->type == CHIAKI_TAKION_EVENT_TYPE_DISCONNECT;
				chiaki_cond_signal(&senkusha->state_cond);
			}
			chiaki_mutex_unlock(&senkusha->state_mutex);
			break;
		case CHIAKI_TAKION_EVENT_TYPE_DATA:
			senkusha_takion_data(senkusha, event->data.data_type, event->data.buf, event->data.buf_size);
			break;
		case CHIAKI_TAKION_EVENT_TYPE_DATA_ACK:
			senkusha_takion_data_ack(senkusha, event->data_ack.seq_num);
			break;
		case CHIAKI_TAKION_EVENT_TYPE_AV:
			senkusha_takion_av(senkusha, event->av);
			break;
		default:
			break;
	}
}

static void senkusha_takion_data(ChiakiSenkusha *senkusha, ChiakiTakionMessageDataType data_type, uint8_t *buf, size_t buf_size)
{
	if(data_type != CHIAKI_TAKION_MESSAGE_DATA_TYPE_PROTOBUF)
		return;

	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!r)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha failed to decode data protobuf");
		return;
	}

	chiaki_mutex_lock(&senkusha->state_mutex);
	if(senkusha->state == STATE_EXPECT_BANG)
	{
		if(msg.type != tkproto_TakionMessage_PayloadType_BANG || !msg.has_bang_payload)
		{
			CHIAKI_LOGE(senkusha->log, "Senkusha expected bang payload but received something else");
		}
		else
		{
			senkusha->state_finished = true;
			chiaki_cond_signal(&senkusha->state_cond);
		}
	}
	chiaki_mutex_unlock(&senkusha->state_mutex);
}

static void senkusha_takion_data_ack(ChiakiSenkusha *senkusha, ChiakiSeqNum32 seq_num)
{
	chiaki_mutex_lock(&senkusha->state_mutex);
	if(senkusha->state == STATE_EXPECT_DATA_ACK && senkusha->data_ack_seq_num_expected == seq_num)
	{
		senkusha->state_finished = true;
		chiaki_mutex_unlock(&senkusha->state_mutex);
		chiaki_cond_signal(&senkusha->state_cond);
	}
	else
		chiaki_mutex_unlock(&senkusha->state_mutex);
}

static void senkusha_takion_av(ChiakiSenkusha *senkusha, ChiakiTakionAVPacket *packet)
{
	uint64_t time_us = chiaki_time_now_monotonic_us();

	ChiakiErrorCode err = chiaki_mutex_lock(&senkusha->state_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);

	if(senkusha->state == STATE_EXPECT_PONG)
	{
		if(packet->frame_index != senkusha->ping_test_index
			|| packet->unit_index != senkusha->ping_index
			|| packet->data_size < 8)
		{
			CHIAKI_LOGW(senkusha->log, "Senkusha received invalid Pong %u/%u, size: %#llx",
					(unsigned int)packet->frame_index, (unsigned int)packet->unit_index, (unsigned long long)packet->data_size);
			goto beach;
		}

		uint32_t tag = ntohl(*((uint32_t *)(packet->data + 4)));
		if(tag != senkusha->ping_tag)
		{
			CHIAKI_LOGW(senkusha->log, "Senkusha received Pong with invalid tag");
			goto beach;
		}

		senkusha->pong_time_us = time_us;
		senkusha->state_finished = true;
		chiaki_mutex_unlock(&senkusha->state_mutex);
		chiaki_cond_signal(&senkusha->state_cond);
		return;
	}
	else if(senkusha->state == STATE_EXPECT_MTU)
	{
		//CHIAKI_LOGD(senkusha->log, "Senkusha received av while expecting mtu");
		//chiaki_log_hexdump(senkusha->log, CHIAKI_LOG_DEBUG, packet->data, packet->data_size);
		//CHIAKI_LOGD(senkusha->log, "packet index: %u, frame index: %u, unit index: %u, units in frame: %u", packet->packet_index, packet->frame_index, packet->unit_index, packet->units_in_frame_total);

		if(!packet->is_video
			|| packet->frame_index != senkusha->mtu_id)
		{
			CHIAKI_LOGW(senkusha->log, "Senkusha received invalid MTU response %u, size: %#llx, is video: %d",
					(unsigned int)packet->frame_index, (unsigned long long)packet->data_size, packet->is_video ? 1 : 0);
			goto beach;
		}

		senkusha->state_finished = true;
		chiaki_mutex_unlock(&senkusha->state_mutex);
		chiaki_cond_signal(&senkusha->state_cond);
		return;
	}

beach:
	chiaki_mutex_unlock(&senkusha->state_mutex);
}

static ChiakiErrorCode senkusha_send_big(ChiakiSenkusha *senkusha)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_BIG;
	msg.has_big_payload = true;
	msg.big_payload.client_version = 7;
	msg.big_payload.session_key.arg = "";
	msg.big_payload.session_key.funcs.encode = chiaki_pb_encode_string;
	msg.big_payload.launch_spec.arg = "";
	msg.big_payload.launch_spec.funcs.encode = chiaki_pb_encode_string;
	msg.big_payload.encrypted_key.arg = "";
	msg.big_payload.encrypted_key.funcs.encode = chiaki_pb_encode_string;

	uint8_t buf[12];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha big protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	ChiakiErrorCode err = chiaki_takion_send_message_data(&senkusha->takion, 1, 1, buf, buf_size, NULL);

	return err;
}

static ChiakiErrorCode senkusha_send_disconnect(ChiakiSenkusha *senkusha)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_DISCONNECT;
	msg.has_disconnect_payload = true;
	msg.disconnect_payload.reason.arg = "Client Disconnecting";
	msg.disconnect_payload.reason.funcs.encode = chiaki_pb_encode_string;

	uint8_t buf[26];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha disconnect protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	ChiakiErrorCode err = chiaki_takion_send_message_data(&senkusha->takion, 1, 1, buf, buf_size, NULL);

	return err;
}

static ChiakiErrorCode senkusha_send_echo_command(ChiakiSenkusha *senkusha, bool enable)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_SENKUSHA;
	msg.has_senkusha_payload = true;
	msg.senkusha_payload.command = tkproto_SenkushaPayload_Command_ECHO_COMMAND;
	msg.senkusha_payload.has_echo_command = true;
	msg.senkusha_payload.echo_command.state = enable;

	uint8_t buf[0x10];
	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha echo command protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	return senkusha_send_data_wait_for_ack(senkusha, buf, stream.bytes_written);
}

static ChiakiErrorCode senkusha_send_mtu_command(ChiakiSenkusha *senkusha, tkproto_SenkushaMtuCommand *command)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_SENKUSHA;
	msg.has_senkusha_payload = true;
	msg.senkusha_payload.command = tkproto_SenkushaPayload_Command_MTU_COMMAND;
	msg.senkusha_payload.has_mtu_command = true;
	memcpy(&msg.senkusha_payload.mtu_command, command, sizeof(msg.senkusha_payload.mtu_command));

	uint8_t buf[0x20];
	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha mtu command protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	return chiaki_takion_send_message_data(&senkusha->takion, 1, 8, buf, stream.bytes_written, NULL);
}

static ChiakiErrorCode senkusha_send_client_mtu_command(ChiakiSenkusha *senkusha, tkproto_SenkushaClientMtuCommand *command)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_SENKUSHA;
	msg.has_senkusha_payload = true;
	msg.senkusha_payload.command = tkproto_SenkushaPayload_Command_CLIENT_MTU_COMMAND;
	msg.senkusha_payload.has_client_mtu_command = true;
	memcpy(&msg.senkusha_payload.client_mtu_command, command, sizeof(msg.senkusha_payload.client_mtu_command));

	uint8_t buf[0x20];
	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha client mtu command protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	return senkusha_send_data_wait_for_ack(senkusha, buf, stream.bytes_written);
}

static ChiakiErrorCode senkusha_send_data_wait_for_ack(ChiakiSenkusha *senkusha, uint8_t *buf, size_t buf_size)
{
	senkusha->state = STATE_EXPECT_DATA_ACK;
	senkusha->state_finished = false;
	senkusha->state_failed = false;
	ChiakiErrorCode err = chiaki_takion_send_message_data(&senkusha->takion, 1, 8, buf, buf_size, &senkusha->data_ack_seq_num_expected);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(senkusha->log, "Senkusha failed to send echo command");
		return err;
	}

	err = chiaki_cond_timedwait_pred(&senkusha->state_cond, &senkusha->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, senkusha);
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);

	if(!senkusha->state_finished)
	{
		if(err == CHIAKI_ERR_TIMEOUT)
			CHIAKI_LOGE(senkusha->log, "Senkusha data ack for echo command receive timeout");

		if(senkusha->should_stop)
			err = CHIAKI_ERR_CANCELED;
		else
			CHIAKI_LOGE(senkusha->log, "Senkusha failed to receive data ack for echo command");
	}

	return err;
}

