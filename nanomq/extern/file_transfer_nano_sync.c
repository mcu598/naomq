/*
 * This is an example of how to use the MQTT client library to send a file to
 * the broker using EMQX's the file transfer extension.
 *
 * The EMQX file transfer extension documentation is available here:
 * https://www.emqx.io/docs/en/v5/file-transfer/introduction.html
 *
 * This example reads a file from the file system and publishes it to the
 * broker (the file transfer feature has to be enabled in the EMQX
 * configuration). The user can specify the file path, file id, file name etc
 * as command line parameters. Run the program with the --help flag to see the
 * list of options.
 *
 * Change the DEBUG macro to 1 to see debug messages.
 */

/*
 * TODO: In order to know that the broker accepted the file and the individual
 * messages one has to check the PUBACK reason code. This is not implemented
 * in this example so even if everything seems to work we don't know that the
 * file has been stored by the broker (without checking with e.g., the HTTP
 * API). The PUBACK reason code is a MQTT v5 feature so in order to fix this we
 * would first have to make sure that the client connects with the MQTT v5
 * protocol and then check the PUBACK reason code fore each message. It seems
 * like this could be done by setting a handler with MQTTClient_setPublished()
 * https://www.eclipse.org/paho/files/mqttdoc/MQTTClient/html/_m_q_t_t_client_8h.html#a9f13911351a3de6b1ebdabd4cb4116ba
 * . Unfortunately I had some problem with connecting with MQTT v5 so I have
 * not been able to test this yet. See also:
 * https://github.com/emqx/MQTT-Client-Examples/pull/112#discussion_r1253421492
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/file.h>
#include <errno.h>
#include "nng/mqtt/mqtt_client.h"

#include "nng/nng.h"
#include "nng/supplemental/nanolib/log.h"
#include "nng/supplemental/nanolib/cJSON.h"

#define DEBUG                   1
#define MAX_DELAY_7_DAYS        (1000 * 60 * 60 * 24 * 7)
#define TOPIC_LEN               1024
#define BUF_SIZE                1024 * 10
#define FT_SUB_TOPIC            "file_transfer"
#define FT_RESULT_TOPIC         "file_transfer/result"
//
// Publish a message to the given topic and with the given QoS.
int
client_publish(nng_socket sock, const char *topic, uint8_t *payload, uint32_t payload_len, uint8_t qos, bool verbose)
{
	int rv;

	// create a PUBLISH message
	nng_msg *pubmsg;
	nng_mqtt_msg_alloc(&pubmsg, 0);
	nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_dup(pubmsg, 0);
	nng_mqtt_msg_set_publish_qos(pubmsg, qos);
	nng_mqtt_msg_set_publish_retain(pubmsg, 0);
	nng_mqtt_msg_set_publish_payload(
	    pubmsg, (uint8_t *) payload, payload_len);
	nng_mqtt_msg_set_publish_topic(pubmsg, topic);

	property *plist = mqtt_property_alloc();

	nng_mqtt_msg_set_publish_property(pubmsg, plist);

	log_info("Publishing to '%s' ...\n", topic);
	if ((rv = nng_sendmsg(sock, pubmsg, 0)) != 0) {
		fatal("nng_sendmsg", rv);
	}

	return rv;
}


static int publish_send_result(nng_socket *sock,
							   char *requestid,
							   int success)
{
	int rc;
	char payload[BUF_SIZE];
	char topic[TOPIC_LEN];

	memset(topic, 0, TOPIC_LEN);
	memset(payload, 0, BUF_SIZE);

	rc = snprintf(
			payload,
			BUF_SIZE,
			"{"
			"  \"request-id\": \"%s\","
			"  \"success\": %s,"
			"  \"message\": \"%s\""
			"}",
			requestid,
			success ? "true" : "false",
			"");
	if (rc < 0 || rc >= BUF_SIZE) {
		log_warn("Failed to create payload for initial message\n");
		return -1;
	}

	// Create topic of the form file_transfer/result for result message
	strcpy(topic, FT_RESULT_TOPIC);
	// Publish result message
	if (DEBUG) {
		log_info("Publishing result message to topic %s\n", topic);
		log_info("Payload:\n%s\n", payload);
	}

	rc = client_publish(*sock, topic, payload, strlen(payload), 1, true);
	if (rc != 0) {
		log_warn("Failed to publish result message, return code %d\n", rc);
		return -1;
	}

	return 0;
}

static int publish_initial(const nng_socket *sock,
						   const char *file_id,
						   const char *file_name,
						   const long file_size,
						   const unsigned long expire_time_s_since_epoch,
						   const unsigned long segments_ttl_seconds)
{
	char payload[BUF_SIZE];
	char expire_at_str[128];
	char segments_ttl_str[128];
	char topic[TOPIC_LEN];
	int rc;

	memset(payload, 0, BUF_SIZE);
	memset(expire_at_str, 0, 128);
	memset(segments_ttl_str, 0, 128);
	memset(topic, 0, TOPIC_LEN);

	if (expire_time_s_since_epoch != -1) {
		// No need to check return value since we know the buffer is large enough
		snprintf(expire_at_str,
				128,
				"  \"expire_at\": %ld,\n",
				expire_time_s_since_epoch);
	}

	if (segments_ttl_seconds != -1) {
		// No need to check return value since we know the buffer is large enough
		snprintf(segments_ttl_str,
				128,
				"  \"segments_ttl\": %ld,\n",
				segments_ttl_seconds);
	}

	rc = snprintf(
			payload,
			BUF_SIZE,
			"{"
			"\"name\": \"%s\","
			"\"size\": %ld,"
			"%s"
			"%s"
			"  \"user_data\": {}\n"
			"}",
			file_name,
			file_size,
			expire_at_str,
			segments_ttl_str);
	if (rc < 0 || rc >= BUF_SIZE) {
		log_warn("Failed to create payload for initial message\n");
		return -1;
	}

	// Create topic of the form $file/{file_id}/init for initial message
	rc = snprintf(topic, TOPIC_LEN, "$file/%s/init", file_id);
	if (rc < 0 || rc >= TOPIC_LEN) {
		log_warn("Failed to create topic for initial message\n");
		return -1;
	}

	if (DEBUG) {
		log_info("Publishing initial message to topic %s\n", topic);
		log_info("Payload: %s\n", payload);
	}

	rc = client_publish(*sock, topic, (uint8_t *)payload, (uint32_t)strlen(payload), 1, true);
	if (rc != 0) {
		log_warn("Failed to publish initial message, return code %d\n", rc);
		return -1;
	}

	return 0;
}

static inline int parse_input(cJSON *cjson_objs,
							  cJSON **cjson_filepaths,
							  cJSON **cjson_filenames, cJSON **cjson_fileids,
							  cJSON **cjson_requestid, cJSON **cjson_segmentsize,
							  cJSON **cjson_delete, cJSON **cjson_interval)
{
	*cjson_filepaths = cJSON_GetObjectItem(cjson_objs, "files");
	*cjson_filenames = cJSON_GetObjectItem(cjson_objs, "filenames");
	*cjson_fileids = cJSON_GetObjectItem(cjson_objs, "fileids");
	*cjson_requestid = cJSON_GetObjectItem(cjson_objs, "request_id");
	*cjson_segmentsize = cJSON_GetObjectItem(cjson_objs, "segment-size");
	*cjson_delete = cJSON_GetObjectItem(cjson_objs, "delete");
	*cjson_interval = cJSON_GetObjectItem(cjson_objs, "interval");
	if (*cjson_filepaths == NULL || *cjson_fileids == NULL ||
		*cjson_filenames == NULL || *cjson_requestid == NULL ||
		cJSON_GetArraySize(*cjson_filepaths) == 0 ||
		cJSON_GetArraySize(*cjson_filepaths) != cJSON_GetArraySize(*cjson_fileids) ||
		cJSON_GetArraySize(*cjson_filepaths) != cJSON_GetArraySize(*cjson_filenames)) {
		return -1;
	} 

	return 0;
}

void
delete_delay_cb(void *arg)
{
	char *filename = arg;
	int ret;
	if (filename != NULL) {
		ret = nng_file_delete(filename);
		log_warn("delete_delay_cb: file:%s result: %d\n", filename, ret);
	} else {
		log_warn("filename is NULL and delete failed\n");
	}
	return;
}

static int do_flock(FILE *fp, int op)
{
	int fd;
	int rc;

	fd = fileno(fp);
	if (fd == -1) {
		log_warn("Failed to get file discription\n");
		return -1;
	}

	rc = flock(fd, op);
	if (rc != 0) {
		log_warn("Failed to do lock opration with file: op: %d rc: %d error: %s\n",
													op, rc, strerror(errno));
	}

	return rc;
}

static int publish_file(nng_socket *sock,
						FILE *fp,
						char *file_id,
						long file_size,
						unsigned int chunk_size,
						unsigned int interval)
{
	char payload[BUF_SIZE];
	size_t offset = 0;
	size_t read_bytes = 0;
	char topic[TOPIC_LEN];
	int rc = 0;

	memset(payload, 0, BUF_SIZE);
	memset(topic, 0, TOPIC_LEN);

	// Read binary chunks of max size 1024 bytes and publish them to the broker
	// The chunks are published to the topic of the form $file/{file_id}/{offset}
	// The chunks are read into the payload
	while ((read_bytes = fread(payload, 1, chunk_size, fp)) > 0) {
		rc = snprintf(topic, BUF_SIZE, "$file/%s/%lu", file_id, offset);
		if (rc < 0 || rc >= BUF_SIZE) {
			log_warn("Failed to create topic for file chunk\n");
			return -1;
		}
		if (DEBUG) {
			log_info("Publishing file chunk to topic %s offset %lu\n", topic, offset);
		}
		rc = client_publish(*sock, topic, (uint8_t *)payload, (uint32_t)read_bytes, 1, true);
		if (rc != 0) {
			log_warn("Failed to publish message, return code %d\n", rc);
			return -1;
		}
		nng_msleep(interval);

		memset(payload, 0, BUF_SIZE);
		offset += read_bytes;
		if (offset == file_size) {
			break;
		}
		if (chunk_size > file_size - offset) {
			/* Processing the last chunk */
			chunk_size = file_size - offset;
		}
	}

	return 0;
}

static int publish_fin(nng_socket *sock,
					   char *file_id,
					   long file_size)
{
	int rc = 0;
	char topic[TOPIC_LEN];

	memset(topic, 0, TOPIC_LEN);

	// Send final message to the topic $file/{file_id}/fin/{file_size} with an empty payload
	rc = snprintf(topic, BUF_SIZE, "$file/%s/fin/%ld", file_id, file_size);
	if (rc < 0 || rc >= BUF_SIZE) {
		log_warn("Failed to create topic for final message\n");
		return -1;
	}
	if (DEBUG) {
		log_info("Publishing final message to topic %s\n", topic);
	}

	rc = client_publish(*sock, topic, (uint8_t *)"", (uint32_t)0, 1, true);
	if (rc != 0) {
		log_warn("Failed to publish message, return code %d\n", rc);
		return -1;
	}

	return 0;
}

int send_file(nng_socket *sock,
			  char *file_path,
			  char *file_id,
			  char *file_name,
			  unsigned int chunk_size,
			  unsigned int interval,
			  unsigned long expire_time_s_since_epoch,
			  unsigned long segments_ttl_seconds) {
	FILE *fp;
	int rc = 0;
	bool isLock = true;
	long file_size;

	// Payload's length is depend on BUF_SIZE, BUF_SIZE is 10240 now.
	if (chunk_size > 10240 || chunk_size == 0) {
		chunk_size = 10240;
	}

	fp = fopen(file_path, "rb");
	if (fp == NULL) {
		log_warn("Failed to open file %s\n", file_path);
		return -1;
	}

	rc = do_flock(fp, LOCK_SH);
	if (rc != 0) {
		isLock = false;
		log_warn("Failed to lock file. Still send file without a file lock...\n");
	}

	// Get file size
	fseek(fp, 0L, SEEK_END);
	file_size = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	// Create payload for initial message 
	rc = publish_initial(sock,
						 file_id, file_name, file_size,
						 expire_time_s_since_epoch,
						 segments_ttl_seconds);

	if (rc) {
		fclose(fp);
		return -1;
	}
	
	rc = publish_file(sock,
					  fp, file_id, file_size,
					  chunk_size, interval);
	if (rc) {
		fclose(fp);
		return -1;
	}

	// Check if we reached the end of the file
	if (feof(fp)) {
		if (DEBUG) {
			log_info("Reached end of file\n");
		}
	} else {
		if (DEBUG) {
			log_warn("Failed to reach end of file errno: %d\n", errno);
		}
	}

	if (isLock) {
		rc = do_flock(fp, LOCK_UN);
		if (rc != 0) {
			isLock = false;
			log_warn("Failed to unlock file\n");
		}
	}

	fclose(fp);

	rc = publish_fin(sock, file_id, file_size);
	if (rc) {
		return -1;
	}

	return 0;
}

static void
disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	int reason = 0;
	// get disconnect reason
	nng_pipe_get_int(p, NNG_OPT_MQTT_DISCONNECT_REASON, &reason);
	log_warn("%s: disconnected! RC [%d] \n", __FUNCTION__, reason);
}

static void
connect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	int reason;
	// get connect reason
	nng_pipe_get_int(p, NNG_OPT_MQTT_CONNECT_REASON, &reason);
	log_info("%s: connected! RC [%d] \n", __FUNCTION__, reason);
}

//
// Connect to the given address.
int
client_connect(nng_socket *sock, const char *url)
{
	nng_dialer dialer;
	int        rv;

	if ((rv = nng_mqttv5_client_open(sock)) != 0) {
		fatal("nng_socket", rv);
	}

	if ((rv = nng_dialer_create(&dialer, *sock, url)) != 0) {
		fatal("nng_dialer_create", rv);
	}

	// create a CONNECT message
	/* CONNECT */
	nng_msg *connmsg;
	nng_mqtt_msg_alloc(&connmsg, 0);
	nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(connmsg, 5);
	nng_mqtt_msg_set_connect_keep_alive(connmsg, 600);
	nng_mqtt_msg_set_connect_clean_session(connmsg, true);

	property * p = mqtt_property_alloc();
	nng_mqtt_msg_set_connect_property(connmsg, p);
	property *will_prop = mqtt_property_alloc();
	nng_mqtt_msg_set_connect_will_property(connmsg, will_prop);

	nng_mqtt_set_connect_cb(*sock, connect_cb, sock);
	nng_mqtt_set_disconnect_cb(*sock, disconnect_cb, connmsg);

	nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg);

	log_info("Connecting to server ... url: %s\n", url);
	/* connect as sync mode */
	rv = nng_dialer_start(dialer, 0);
	while (rv != 0) {
		log_warn("Connect to %s failed, retry in 10s....\n", url);
		nng_msleep(10 * 1000);
		rv = nng_dialer_start(dialer, 0);
	}

	log_info("Connecting to server finished rv: %d ...\n", rv);

	return (0);
}


static int process_msg(nng_socket *sock, nng_msg *msg, bool verbose)
{
		uint32_t topic_len = 0;
		uint32_t payload_len = 0;
		const char *topic = nng_mqtt_msg_get_publish_topic(msg, &topic_len);
		char *payload = nng_mqtt_msg_get_publish_payload(msg, &payload_len);

	    log_info("Receive \'%.*s\' from \'%.*s\'\n", payload_len, payload, topic_len, topic);

		cJSON *cjson_objs = cJSON_Parse(payload);
		if (cjson_objs == NULL) {
			log_warn("Parse json failed\n");
			nng_msg_free(msg);
			return -1;
		} else {
			int result;
			cJSON *cjson_filepaths;
			cJSON *cjson_filenames;
			cJSON *cjson_fileids;
			cJSON *cjson_requestid;
			cJSON *cjson_segmentsize;
			cJSON *cjson_delete;
			cJSON *cjson_interval;
			result = parse_input(cjson_objs, &cjson_filepaths,
								 &cjson_filenames, &cjson_fileids,
								 &cjson_requestid, &cjson_segmentsize,
								 &cjson_delete, &cjson_interval);
			if (result) {
				log_warn("INPUT JSON INVALID!\n");
				nng_msg_free(msg);
				return -1;
			} else {
				int fileCount = cJSON_GetArraySize(cjson_filepaths);
				if (DEBUG) {
					log_info("Input Json: request-id: %s segment-size: %u interval: %u\n",
													cjson_requestid->valuestring,
													cjson_segmentsize == NULL ? 0U : cjson_segmentsize->valueint,
													cjson_interval == NULL ? 0U : cjson_interval->valueint);
				}
				result = 0;;
				for (int i = 0; i < fileCount; i++) {
					cJSON *pathEle = cJSON_GetArrayItem(cjson_filepaths, i);
					cJSON *idEle = cJSON_GetArrayItem(cjson_fileids, i);
					cJSON *nameEle = cJSON_GetArrayItem(cjson_filenames, i);
					log_info("Sending file: filepath: %s fileid: %s filename: %s\n",
												pathEle->valuestring,
												idEle->valuestring,
												nameEle->valuestring);
					// Send file
					result = send_file(sock,
									   pathEle->valuestring,
									   idEle->valuestring,
									   nameEle->valuestring,
									   cjson_segmentsize == NULL ? 0U : cjson_segmentsize->valueint,
									   cjson_interval == NULL ? 0U : cjson_interval->valueint,
									   -1,
									   -1);
					log_info("Send file file_id: %s %s\n", idEle->valuestring,
										!result ? "success" : "fail");
					/* fail */
					if (result) {
						break;
					} else {
						if (cjson_delete != NULL && cjson_delete->valueint >= 0) {
							if (cjson_delete->valueint == 0) {
								int ret;
								ret = nng_file_delete(pathEle->valuestring);
								log_info("Delete imediately: file:%s result: %d\n", pathEle->valuestring, ret);
							} else {
								nng_aio *a;
								char *filename;
								filename = nng_alloc(strlen(pathEle->valuestring) + 1);
								if (filename == NULL) {
									log_warn("Alloc filename failed continue...\n");
									continue;
								}
								strcpy(filename, pathEle->valuestring);

								/* Delete after 7 days at the latest */
								int delay = cjson_delete->valueint * 1000;
								if (delay > MAX_DELAY_7_DAYS) {
									delay = MAX_DELAY_7_DAYS;
								}
								nng_aio_alloc(&a, delete_delay_cb, filename);
								nng_sleep_aio(delay, a);
								log_warn("Send file finished: Will delete %s in %d milliseconds\n",
																				pathEle->valuestring,
																				delay);
							}
						} else {
							log_info("Send file finished will not delete: %s\n", pathEle->valuestring);
						}
					}
				}
				result = publish_send_result(sock, cjson_requestid->valuestring, !result);
				if (DEBUG) {
					log_info("Send file request-id: %s transfer result: %s\n",
										cjson_requestid->valuestring,
										!result ? "success" : "fail");
				}
			}
		}

		nng_msg_free(msg);
		return 0;
}


void start_listening(nng_socket *sock)
{
	int rv;

	nng_mqtt_topic_qos subscriptions[] = {
		{
		    .qos   = 1,
		    .topic = { 
				.buf    = (uint8_t *)FT_SUB_TOPIC,
		        .length = strlen(FT_SUB_TOPIC), 
			},
			.nolocal         = 1,
			.rap             = 1,
			.retain_handling = 0,
		},
	};

	rv = nng_mqtt_subscribe(*sock, subscriptions, 1, NULL);
	log_info("Start receiving loop:\n");

	/* dead loop? */
	while (true) {
		nng_msg *msg;
		log_info("Start recvmsg:\n");
		if ((rv = nng_recvmsg(*sock, &msg, 0)) != 0) {
			fatal("nng_recvmsg", rv);
			continue;
		}

		log_info("recvmsg return rv: %d type: %d\n", rv, nng_mqtt_msg_get_packet_type(msg));
		if (nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_PUBLISH) {
			rv = process_msg(sock, msg, true);
			if (rv) {
				log_warn("something wrong occured when process msg\n");
			}
		}
	}

	return;
}

int file_transfer(int argc, char *argv[]) {
	int rc;
	nng_socket sock;
	char *host = "127.0.0.1";
	int port = 1883;

	if (DEBUG) {
		log_info("host: %s\n", host);
		log_info("port: %d\n", port);
	}
	// Construct address string from host and port
	char address[2048];
	rc = snprintf(address, 2048, "mqtt-tcp://%s:%d", host, port);
	if (rc < 0 || rc >= 2048) {
		log_warn("Failed to construct address string\n");
		log_warn("Something wrong occurred. File transfer thread exiting...\n");
		return -1;
	}

	client_connect(&sock, address);

	if (DEBUG) {
		log_info("Connected to MQTT Broker!\n");
	}
	
	(void) start_listening(&sock);

	return -1;
}