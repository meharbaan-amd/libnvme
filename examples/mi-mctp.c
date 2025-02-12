// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * This file is part of libnvme.
 * Copyright (c) 2021 Code Construct Pty Ltd.
 *
 * Authors: Jeremy Kerr <jk@codeconstruct.com.au>
 */

/**
 * mi-mctp: open a MI connection over MCTP, and query controller info
 */

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

#include <libnvme-mi.h>

#include <ccan/array_size/array_size.h>
#include <ccan/endian/endian.h>

/* Global NVME MI EndPoint */
nvme_mi_ep_t g_ep;

/* MVNI Payload maximum length is subject to change is needed.*/
#define MVNI_PAYLOAD_LEN_MAX 1024
/* 64 bytes standard NVME command.*/
#define NVME_REQ_CMD_LEN 64
/* 20 bytes standard NVME-MI command response.*/
#define NVME_RESP_CMD_LEN 20

/** MVNI request object for snapper core*/
typedef struct cmd_mvni_req_s {
	/** Command associated with request */
	uint8_t nvme_cmd[NVME_REQ_CMD_LEN];
	/** Payload buffer*/
	uint8_t payload[MVNI_PAYLOAD_LEN_MAX];
	/** Payload length in the message*/
	uint16_t payload_len;
} cmd_mvni_req_t;

/** MVNI request object for snapper core*/
typedef struct cmd_mvni_resp_s {
	/** Command associated with request */
	uint8_t nvme_cmd[NVME_RESP_CMD_LEN];
	/** Payload buffer*/
	uint8_t payload[MVNI_PAYLOAD_LEN_MAX];
	/** Return code recieved in response */
	uint16_t rc;
	/** Payload length in the message*/
	uint16_t payload_len;
} cmd_mvni_resp_t;

/* Utility function to dump buffer in hex format*/
void print_hex(char* str, const void *ptr, size_t size) {
	printf("%s:\n", str);
	const uint8_t *byte = (const uint8_t *)ptr;
	for (size_t i = 0; i < size; i++) {
		printf("%02X ", byte[i]);
	}
	printf("\n");
}

int do_admin_raw_breck(nvme_mi_ep_t ep, cmd_mvni_req_t* req_data, cmd_mvni_resp_t* resp_data);

#define INTERPOSER_CONF_RESERVED_BYTES	49
#define MVNI_SOCKET_PORT 8080

/* Function to handle each client in a separate thread */
void *handle_client(void *client_sock_ptr) {
	int client_sock = *(int *)client_sock_ptr;
	/* Free dynamically allocated memory */
	free(client_sock_ptr);

	ssize_t bytes_received;
	cmd_mvni_req_t req_data;
	cmd_mvni_resp_t resp_data;

        /* A handshake structure, used only once for each connection */
	typedef struct {
		int id;
		char message[50];
	} DataPacket;

	DataPacket packet;
	/* Receive data from client */
	bytes_received = recv(client_sock, &packet, sizeof(DataPacket), 0);

	if (bytes_received > 0) {
		printf("Received from client: ID=%d, Message=%s\n", packet.id, packet.message);
		/* Modify the response */
		packet.id += 1;
		strcpy(packet.message, "Acknowledged by master");
		/* Send response back to client */
		if (send(client_sock, &packet, sizeof(DataPacket), 0) < 0) {
			perror("Send failed");
			goto error;
		}
	} else if (bytes_received == 0) {
		/* Client disconnected */
		printf("Client disconnected.\n");
		goto error;
	} else {
		perror("Receive failed");
		goto error;
	}

	while (1) {
		memset(&req_data, 0, sizeof(cmd_mvni_req_t));
		memset(&resp_data, 0, sizeof(cmd_mvni_resp_t));
		ssize_t bytes_received = recv(client_sock, &req_data, sizeof(cmd_mvni_req_t), 0);

		if (bytes_received>0) {
			printf("Received from master.\n");
			
			if (!do_admin_raw_breck(g_ep, &req_data, &resp_data))
			{
				if (send(client_sock, &resp_data, sizeof(cmd_mvni_resp_t), 0) < 0)
					perror("Send failed");
			}
			else
			{
				printf("command processing failed on cpp.!!!\n");
				resp_data.rc = -1;
				if (send(client_sock, &resp_data, sizeof(cmd_mvni_resp_t), 0) < 0)
					perror("Send failed");
			}
		}
		else if (bytes_received ==0){
			printf("Connection closed by master.\n");
			break;
		}
		else
		{
			perror("Receive Failed\n");
			break;
		}
	}

error:
	close(client_sock);
	pthread_exit(NULL);
}

/* Server socket waits for new connection requests */
int cpp_server_start()
{
	struct sockaddr_in server_addr, client_addr;
	socklen_t addrlen = sizeof(server_addr);
	int opt = 1;
	int server_sock = 0, *client_sock_ptr;;

	printf("CPP server starting..... on PORT=%d.\n", MVNI_SOCKET_PORT);
	/* Create socket */
	if( (server_sock = socket(AF_INET, SOCK_STREAM, 0)) == 0 ) {
		printf("Socket failed\n");
		goto err;
	}

	/* Set SO_REUSEADDR to avoid "Address already in use" error */
	if( setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))
			== -1 ) {
		printf("setsockopt failed\n");
		goto err;
	}

	/* Bind socket */
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(MVNI_SOCKET_PORT);
	if( bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0 ) {
		printf("Bind failed\n");
		goto err;
	}

	/* Listen for connections */
	if( listen(server_sock, 10) < 0 ) {
		printf("Listen failed\n");
		goto err;
	}
	printf("Waiting for connection on CPP Server.....\n");
	while(1)
	{
		/* Allocate memory for new client socket */
		client_sock_ptr = malloc(sizeof(int));
		if (!client_sock_ptr) {
			perror("Memory allocation failed\n");
			goto err;
		}
		*client_sock_ptr = accept(server_sock, (struct sockaddr*)&client_addr, &addrlen);
		if (*client_sock_ptr == -1) {
			perror("Accept failed\n");
			free(client_sock_ptr);
			goto err;
		}
		printf("New Client connected !\n");
		// Create a new thread for each client
		pthread_t client_thread;
		if (pthread_create(&client_thread, NULL, handle_client, client_sock_ptr) != 0) {
			perror("Thread creation failed\n");
			free(client_sock_ptr);
			goto err;
		}
		/* Detach thread to clean up resources automatically */
		pthread_detach(client_thread);
	}

err:
	close(server_sock);
	return 0;
}

static void show_port_pcie(struct nvme_mi_read_port_info *port)
{
	printf("    PCIe max payload: 0x%x\n", 0x80 << port->pcie.mps);
	printf("    PCIe link speeds: 0x%02x\n", port->pcie.sls);
	printf("    PCIe current speed: 0x%02x\n", port->pcie.cls);
	printf("    PCIe max link width: 0x%02x\n", port->pcie.mlw);
	printf("    PCIe neg link width: 0x%02x\n", port->pcie.nlw);
	printf("    PCIe port: 0x%02x\n", port->pcie.pn);
}

static void show_port_smbus(struct nvme_mi_read_port_info *port)
{
	printf("    SMBus address: 0x%02x\n", port->smb.vpd_addr);
	printf("    VPD access freq: 0x%02x\n", port->smb.mvpd_freq);
	printf("    MCTP address: 0x%02x\n", port->smb.mme_addr);
	printf("    MCTP access freq: 0x%02x\n", port->smb.mme_freq);
	printf("    NVMe basic management: %s\n",
	       (port->smb.nvmebm & 0x1) ? "enabled" : "disabled");
}

static struct {
	int typeid;
	const char *name;
	void (*fn)(struct nvme_mi_read_port_info *);
} port_types[] = {
	{ 0x00, "inactive", NULL },
	{ 0x01, "PCIe", show_port_pcie },
	{ 0x02, "SMBus", show_port_smbus },
};

static int show_port(nvme_mi_ep_t ep, int portid)
{
	void (*show_fn)(struct nvme_mi_read_port_info *);
	struct nvme_mi_read_port_info port;
	const char *typestr;
	int rc;

	rc = nvme_mi_mi_read_mi_data_port(ep, portid, &port);
	if (rc)
		return rc;

	if (port.portt < ARRAY_SIZE(port_types)) {
		show_fn = port_types[port.portt].fn;
		typestr = port_types[port.portt].name;
	} else {
		show_fn = NULL;
		typestr = "INVALID";
	}

	printf("  port %d\n", portid);
	printf("    type %s[%d]\n", typestr, port.portt);
	printf("    MCTP MTU: %d\n", port.mmctptus);
	printf("    MEB size: %d\n", port.meb);

	if (show_fn)
		show_fn(&port);

	return 0;
}

int do_info(nvme_mi_ep_t ep)
{
	struct nvme_mi_nvm_ss_health_status ss_health;
	struct nvme_mi_read_nvm_ss_info ss_info;
	int i, rc;

	rc = nvme_mi_mi_read_mi_data_subsys(ep, &ss_info);
	if (rc) {
		warn("can't perform Read MI Data operation");
		return -1;
	}

	printf("NVMe MI subsys info:\n");
	printf(" num ports: %d\n", ss_info.nump + 1);
	printf(" major ver: %d\n", ss_info.mjr);
	printf(" minor ver: %d\n", ss_info.mnr);

	printf("NVMe MI port info:\n");
	for (i = 0; i <= ss_info.nump; i++)
		show_port(ep, i);

	rc = nvme_mi_mi_subsystem_health_status_poll(ep, true, &ss_health);
	if (rc)
		err(EXIT_FAILURE, "can't perform Health Status Poll operation");

	printf("NVMe MI subsys health:\n");
	printf(" subsystem status:  0x%x\n", ss_health.nss);
	printf(" smart warnings:    0x%x\n", ss_health.sw);
	printf(" composite temp:    %d\n", ss_health.ctemp);
	printf(" drive life used:   %d%%\n", ss_health.pdlu);
	printf(" controller status: 0x%04x\n", le16_to_cpu(ss_health.ccs));

	return 0;
}

static int show_ctrl(nvme_mi_ep_t ep, uint16_t ctrl_id)
{
	struct nvme_mi_read_ctrl_info ctrl;
	int rc;

	rc = nvme_mi_mi_read_mi_data_ctrl(ep, ctrl_id, &ctrl);
	if (rc)
		return rc;

	printf("  Controller id: %d\n", ctrl_id);
	printf("    port id: %d\n", ctrl.portid);
	if (ctrl.prii & 0x1) {
		uint16_t bdfn = le16_to_cpu(ctrl.pri);
		printf("    PCIe routing valid\n");
		printf("    PCIe bus: 0x%02x\n", bdfn >> 8);
		printf("    PCIe dev: 0x%02x\n", bdfn >> 3 & 0x1f);
		printf("    PCIe fn : 0x%02x\n", bdfn & 0x7);
	} else {
		printf("    PCIe routing invalid\n");
	}
	printf("    PCI vendor: %04x\n", le16_to_cpu(ctrl.vid));
	printf("    PCI device: %04x\n", le16_to_cpu(ctrl.did));
	printf("    PCI subsys vendor: %04x\n", le16_to_cpu(ctrl.ssvid));
	printf("    PCI subsys device: %04x\n", le16_to_cpu(ctrl.ssvid));

	return 0;
}

static int do_controllers(nvme_mi_ep_t ep)
{
	struct nvme_ctrl_list ctrl_list;
	int rc, i;

	rc = nvme_mi_mi_read_mi_data_ctrl_list(ep, 0, &ctrl_list);
	if (rc) {
		warnx("Can't perform Controller List operation");
		return rc;
	}

	printf("NVMe controller list:\n");
	for (i = 0; i < le16_to_cpu(ctrl_list.num); i++) {
		uint16_t id = le16_to_cpu(ctrl_list.identifier[i]);
		show_ctrl(ep, id);
	}
	return 0;
}

static const char *__copy_id_str(const void *field, size_t size,
				 char *buf, size_t buf_size)
{
	assert(size < buf_size);
	strncpy(buf, field, size);
	buf[size] = '\0';
	return buf;
}

#define copy_id_str(f,b) __copy_id_str(f, sizeof(f), b, sizeof(b))

int do_identify(nvme_mi_ep_t ep, int argc, char **argv)
{
	struct nvme_identify_args id_args = { 0 };
	struct nvme_mi_ctrl *ctrl;
	struct nvme_id_ctrl id;
	uint16_t ctrl_id;
	char buf[41];
	bool partial;
	int rc, tmp;

	if (argc < 2) {
		fprintf(stderr, "no controller ID specified\n");
		return -1;
	}

	tmp = atoi(argv[1]);
	if (tmp < 0 || tmp > 0xffff) {
		fprintf(stderr, "invalid controller ID\n");
		return -1;
	}

	ctrl_id = tmp & 0xffff;

	partial = argc > 2 && !strcmp(argv[2], "--partial");

	ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
	if (!ctrl) {
		warn("can't create controller");
		return -1;
	}

	id_args.data = &id;
	id_args.args_size = sizeof(id_args);
	id_args.cns = NVME_IDENTIFY_CNS_CTRL;
	id_args.nsid = NVME_NSID_NONE;
	id_args.cntid = 0;
	id_args.csi = NVME_CSI_NVM;

	/* for this example code, we can either do a full or partial identify;
	 * since we're only printing the fields before the 'rab' member,
	 * these will be equivalent, aside from the size of the MI
	 * response.
	 */
	if (partial) {
		rc = nvme_mi_admin_identify_partial(ctrl, &id_args, 0,
					    offsetof(struct nvme_id_ctrl, rab));
	} else {
		rc = nvme_mi_admin_identify(ctrl, &id_args);
	}

	if (rc) {
		warn("can't perform Admin Identify command");
		return -1;
	}

	printf("NVMe Controller %d identify\n", ctrl_id);
	printf(" PCI vendor: %04x\n", le16_to_cpu(id.vid));
	printf(" PCI subsys vendor: %04x\n", le16_to_cpu(id.ssvid));
	printf(" Serial number: %s\n", copy_id_str(id.sn, buf));
	printf(" Model number: %s\n", copy_id_str(id.mn, buf));
	printf(" Firmware rev: %s\n", copy_id_str(id.fr, buf));

	return 0;
}

int do_control_primitive(nvme_mi_ep_t ep, int argc, char **argv)
{
	int rc = 0;
	char *action = NULL;
	uint8_t opcode = 0xff;
	uint16_t cpsr;

	static const struct {
		const char *name;
		uint8_t opcode;
	} control_actions[] = {
		{ "pause", nvme_mi_control_opcode_pause },
		{ "resume", nvme_mi_control_opcode_resume },
		{ "abort", nvme_mi_control_opcode_abort },
		{ "get-state", nvme_mi_control_opcode_get_state },
		{ "replay", nvme_mi_control_opcode_replay },
	};

	static const char * const slot_state[] = {
		"Idle",
		"Receive",
		"Process",
		"Transmit",
	};

	static const char * const cpas_state[] = {
		"Command aborted after processing completed or no command to abort",
		"Command aborted before processing began",
		"Command processing partially completed",
		"Reserved",
	};

	if (argc < 2) {
		fprintf(stderr, "no opcode specified\n");
		return -1;
	}

	action = argv[1];

	for (int i = 0; i < ARRAY_SIZE(control_actions); i++) {
		if (!strcmp(action, control_actions[i].name)) {
			opcode = control_actions[i].opcode;
			break;
		}
	}

	if (opcode == 0xff) {
		fprintf(stderr, "invalid action specified: %s\n", action);
		return -1;
	}

	rc = nvme_mi_control(ep, opcode, 0, &cpsr); /* cpsp reserved in example */
	if (rc) {
		warn("can't perform primitive control command");
		return -1;
	}

	printf("NVMe control primitive\n");
	switch (opcode) {
	case nvme_mi_control_opcode_pause:
		printf(" Pause : cspr is %#x\n", cpsr);
		printf("  Pause Flag Status Slot 0: %s\n", (cpsr & (1 << 0)) ? "Yes" : "No");
		printf("  Pause Flag Status Slot 1: %s\n", (cpsr & (1 << 1)) ? "Yes" : "No");
		break;
	case nvme_mi_control_opcode_resume:
		printf(" Resume : cspr is %#x\n", cpsr);
		break;
	case nvme_mi_control_opcode_abort:
		printf(" Abort : cspr is %#x\n", cpsr);
		printf("  Command Aborted Status: %s\n", cpas_state[cpsr & 0x3]);
		break;
	case nvme_mi_control_opcode_get_state:
		printf(" Get State : cspr is %#x\n", cpsr);
		printf("  Slot Command Servicing State: %s\n", slot_state[cpsr & 0x3]);
		printf("  Bad Message Integrity Check: %s\n", (cpsr & (1 << 4)) ? "Yes" : "No");
		printf("  Timeout Waiting for a Packet: %s\n", (cpsr & (1 << 5)) ? "Yes" : "No");
		printf("  Unsupported Transmission Unit: %s\n", (cpsr & (1 << 6)) ? "Yes" : "No");
		printf("  Bad Header Version: %s\n", (cpsr & (1 << 7)) ? "Yes" : "No");
		printf("  Unknown Destination ID: %s\n", (cpsr & (1 << 8)) ? "Yes" : "No");
		printf("  Incorrect Transmission Unit: %s\n", (cpsr & (1 << 9)) ? "Yes" : "No");
		printf("  Unexpected Middle or End of Packet: %s\n", (cpsr & (1 << 10)) ? "Yes" : "No");
		printf("  Out-of-Sequence Packet Sequence Number: %s\n", (cpsr & (1 << 11)) ? "Yes" : "No");
		printf("  Bad, Unexpected, or Expired Message Tag: %s\n", (cpsr & (1 << 12)) ? "Yes" : "No");
		printf("  Bad Packet or Other Physical Layer: %s\n", (cpsr & (1 << 13)) ? "Yes" : "No");
		printf("  NVM Subsystem Reset Occurred: %s\n", (cpsr & (1 << 14)) ? "Yes" : "No");
		printf("  Pause Flag: %s\n", (cpsr & (1 << 15)) ? "Yes" : "No");
		break;
	case nvme_mi_control_opcode_replay:
		printf(" Replay : cspr is %#x\n", cpsr);
		break;
	default:
		/* unreachable */
		break;
	}

	return 0;
}

void fhexdump(FILE *fp, const unsigned char *buf, int len)
{
	const int row_len = 16;
	int i, j;

	for (i = 0; i < len; i += row_len) {
		char hbuf[row_len * strlen("00 ") + 1];
		char cbuf[row_len + strlen("|") + 1];

		for (j = 0; (j < row_len) && ((i+j) < len); j++) {
			unsigned char c = buf[i + j];

			sprintf(hbuf + j * 3, "%02x ", c);

			if (!isprint(c))
				c = '.';

			sprintf(cbuf + j, "%c", c);
		}

		strcat(cbuf, "|");

		fprintf(fp, "%08x  %*s |%s\n", i,
				0 - (int)sizeof(hbuf) + 1, hbuf, cbuf);
	}
}

void hexdump(const unsigned char *buf, int len)
{
	fhexdump(stdout, buf, len);
}

int do_get_log_page(nvme_mi_ep_t ep, int argc, char **argv)
{
	struct nvme_get_log_args args = { 0 };
	struct nvme_mi_ctrl *ctrl;
	uint8_t buf[512];
	uint16_t ctrl_id;
	int rc, tmp;

	if (argc < 2) {
		fprintf(stderr, "no controller ID specified\n");
		return -1;
	}

	tmp = atoi(argv[1]);
	if (tmp < 0 || tmp > 0xffff) {
		fprintf(stderr, "invalid controller ID\n");
		return -1;
	}

	ctrl_id = tmp & 0xffff;

	args.args_size = sizeof(args);
	args.log = buf;
	args.len = sizeof(buf);

	if (argc > 2) {
		tmp = atoi(argv[2]);
		args.lid = tmp & 0xff;
	} else {
		args.lid = 0x1;
	}

	ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
	if (!ctrl) {
		warn("can't create controller");
		return -1;
	}

	rc = nvme_mi_admin_get_log(ctrl, &args);
	if (rc) {
		warn("can't perform Get Log page command");
		return -1;
	}

	printf("Get log page (log id = 0x%02x) data:\n", args.lid);
	hexdump(buf, args.len);

	return 0;
}

int do_admin_raw(nvme_mi_ep_t ep, int argc, char **argv)
{
	struct nvme_mi_admin_req_hdr req;
	struct nvme_mi_admin_resp_hdr *resp;
	struct nvme_mi_ctrl *ctrl;
	size_t resp_data_len;
	unsigned long tmp;
	uint8_t buf[512];
	uint16_t ctrl_id;
	uint8_t opcode;
	__le32 *cdw;
	int i, rc;

	if (argc < 2) {
		fprintf(stderr, "no controller ID specified\n");
		return -1;
	}

	if (argc < 3) {
		fprintf(stderr, "no opcode specified\n");
		return -1;
	}

	tmp = atoi(argv[1]);
	if (tmp > 0xffff) {
		fprintf(stderr, "invalid controller ID\n");
		return -1;
	}
	ctrl_id = tmp & 0xffff;

	tmp = atoi(argv[2]);
	if (tmp > 0xff) {
		fprintf(stderr, "invalid opcode\n");
		return -1;
	}
	opcode = tmp & 0xff;

	memset(&req, 0, sizeof(req));
	req.opcode = opcode;
	req.ctrl_id = cpu_to_le16(ctrl_id);

	/* The cdw10 - cdw16 fields are contiguous in req; set from argv. */
	cdw = (void *)&req + offsetof(typeof(req), cdw10);
	for (i = 0; i < 6; i++) {
		if (argc >= 4 + i)
			tmp = strtoul(argv[3 + i], NULL, 0);
		else
			tmp = 0;
		*cdw = cpu_to_le32(tmp & 0xffffffff);
		cdw++;
	}

	printf("Admin request:\n");
	printf(" opcode: 0x%02x\n", req.opcode);
	printf(" ctrl:   0x%04x\n", le16_to_cpu(req.ctrl_id));
	printf(" cdw10:   0x%08x\n", le32_to_cpu(req.cdw10));
	printf(" cdw11:   0x%08x\n", le32_to_cpu(req.cdw11));
	printf(" cdw12:   0x%08x\n", le32_to_cpu(req.cdw12));
	printf(" cdw13:   0x%08x\n", le32_to_cpu(req.cdw13));
	printf(" cdw14:   0x%08x\n", le32_to_cpu(req.cdw14));
	printf(" cdw15:   0x%08x\n", le32_to_cpu(req.cdw15));
	printf(" raw:\n");
	hexdump((void *)&req, sizeof(req));

	memset(buf, 0, sizeof(buf));
	resp = (void *)buf;

	ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
	if (!ctrl) {
		warn("can't create controller");
		return -1;
	}

	resp_data_len = sizeof(buf) - sizeof(*resp);

	rc = nvme_mi_admin_xfer(ctrl, &req, 0, resp, 0, &resp_data_len);
	if (rc) {
		warn("nvme_admin_xfer failed: %d", rc);
		return -1;
	}

	printf("Admin response:\n");
	printf(" Status: 0x%02x\n", resp->status);
	printf(" cdw0:   0x%08x\n", le32_to_cpu(resp->cdw0));
	printf(" cdw1:   0x%08x\n", le32_to_cpu(resp->cdw1));
	printf(" cdw3:   0x%08x\n", le32_to_cpu(resp->cdw3));
	printf(" data [%zd bytes]\n", resp_data_len);

	hexdump(buf + sizeof(*resp), resp_data_len);
	return 0;
}

/* Breck interposer structure. Refer MVNI spec for more info*/
typedef struct interposer_config_s {
	uint8_t upstream_pci_ports;
	uint8_t config_num_pssds;
	uint16_t total_pssd_capacity;
	uint16_t config_pssd_capacity;
	uint8_t oob_support;
	uint8_t lba_formats;
	uint8_t propagate_reset_events_to_pssd;
	uint8_t pssd_fill_event_threshold;
	uint8_t log_persistence_time;
	uint8_t overprovisioning_factor;
	uint8_t lm_support_on_pc;
	uint8_t max_pssd_recovery_time;
	uint8_t oob_interfaces_roles;
	uint8_t rsvd[INTERPOSER_CONF_RESERVED_BYTES];
} interposer_config_t;

void print_interposer_config(interposer_config_t *ip_cfg)
{
	printf("Interposer configuration:\n");
	printf(" upstream_pci_ports: 0x%x\n", ip_cfg->upstream_pci_ports);
	printf(" config_num_pssds: 0x%x\n", ip_cfg->config_num_pssds);
	printf(" total_pssd_capacity: 0x%x\n", ip_cfg->total_pssd_capacity);
	printf(" config_pssd_capacity: 0x%x\n", ip_cfg->config_pssd_capacity);
	printf(" oob_support: 0x%x\n", ip_cfg->oob_support);
	printf(" lba_formats: 0x%x\n", ip_cfg->lba_formats);
	printf(" propagate_reset_events_to_pssd: 0x%x\n", ip_cfg->propagate_reset_events_to_pssd);
	printf(" pssd_fill_event_threshold: 0x%x\n", ip_cfg->pssd_fill_event_threshold);
	printf(" log_persistence_time: 0x%x\n", ip_cfg->log_persistence_time);
	printf(" overprovisioning_factor: 0x%x\n", ip_cfg->overprovisioning_factor);
	printf(" lm_support_on_pc: 0x%x\n", ip_cfg->lm_support_on_pc);
	printf(" max_pssd_recovery_time: 0x%x\n", ip_cfg->max_pssd_recovery_time);
	printf(" oob_interfaces_roles: 0x%x\n", ip_cfg->oob_interfaces_roles);
}

/* CPP Supported commands*/
typedef enum breck_nvme_cmd_opcodes_e {
	BRECK_NVME_CMD_CREATE_CHILD_CTRLR = 0xD1,
	BRECK_NVME_CMD_CREATE_NAMESPACE = 0xD0,
	BRECK_NVME_CMD_ATTACH_NAMESPACE = 0xD4,
	BRECK_NVME_CMD_DETACH_NAMESPACE = 0xD8,
	BRECK_NVME_CMD_DELETE_NAMESPACE = 0xE0,
	BRECK_NVME_CMD_SET_FEATURES = 0x09,
	BRECK_NVME_CMD_GET_FEATURES = 0x0A,
	BRECK_NVME_CMD_GET_LOG_PAGE = 0x02,
	BRECK_NVME_CMD_RESET_INTERPOSER_CONFIG = 0xE8,
	/*Add other commands support here*/
} breck_nvme_cmd_opcodes_t;

/* CPP Supported features */
typedef enum breck_set_feat_id_e {
	BRECK_NVME_SET_FEAT_INTERPOSER_CONFIG = 0xD1,
	BRECK_NVME_SET_FEAT_INVALID = 0xFF,
	/*Add other set feature identifiers here*/
} breck_set_feat_id_t;

static inline const char*
feature_str(breck_set_feat_id_t type)
{
#define A2STR(_tag)                                                            \
  case BRECK_NVME_SET_FEAT_##_tag:                                                \
    return #_tag;
  switch( type ) {
    A2STR(INTERPOSER_CONFIG);
    A2STR(INVALID);
  }
#undef A2STR
  return "<unknown>";
}

static inline const char*
nvme_cmd_str(breck_nvme_cmd_opcodes_t type)
{
#define A2STR(_tag)                                                            \
  case BRECK_NVME_CMD_##_tag:                                                \
    return #_tag;
  switch( type ) {
    A2STR(SET_FEATURES);
    A2STR(GET_FEATURES);
    A2STR(RESET_INTERPOSER_CONFIG);
    A2STR(CREATE_CHILD_CTRLR);
    A2STR(CREATE_NAMESPACE);
    A2STR(ATTACH_NAMESPACE);
    A2STR(DETACH_NAMESPACE);
    A2STR(DELETE_NAMESPACE);
    A2STR(GET_LOG_PAGE);
  }
#undef A2STR
  return "<unknown>";
}

void dump_req(const char *cmd, struct nvme_mi_admin_req_hdr *req, const unsigned char *buf, size_t req_data_len)
{
	printf("\n\n------ %s ------\n", cmd);

	printf("Admin request:\n");
	printf(" opcode:  0x%02x\n", req->opcode);
	printf(" flags:   0x%02x\n", req->flags);
	printf(" ctrl:    0x%04x\n", le16_to_cpu(req->ctrl_id));
	printf(" cdw1:    0x%08x\n", le32_to_cpu(req->cdw1));
	printf(" cdw2:    0x%08x\n", le32_to_cpu(req->cdw2));
	printf(" cdw3:    0x%08x\n", le32_to_cpu(req->cdw3));
	printf(" cdw4:    0x%08x\n", le32_to_cpu(req->cdw4));
	printf(" cdw5:    0x%08x\n", le32_to_cpu(req->cdw5));
	printf(" d_off:   0x%08x\n", le32_to_cpu(req->doff));
	printf(" d_len:   0x%08x\n", le32_to_cpu(req->dlen));
	printf(" cdw10:   0x%08x\n", le32_to_cpu(req->cdw10));
	printf(" cdw11:   0x%08x\n", le32_to_cpu(req->cdw11));
	printf(" cdw12:   0x%08x\n", le32_to_cpu(req->cdw12));
	printf(" cdw13:   0x%08x\n", le32_to_cpu(req->cdw13));
	printf(" cdw14:   0x%08x\n", le32_to_cpu(req->cdw14));
	printf(" cdw15:   0x%08x\n", le32_to_cpu(req->cdw15));

        printf(" data [%zd bytes]\n", req_data_len);
        hexdump(buf + sizeof(*req), req_data_len);
}

void dump_resp(const char *cmd,  struct nvme_mi_admin_resp_hdr *resp, const unsigned char *buf, size_t resp_data_len)
{
        printf("\nAdmin response:\n");
        printf(" MI message header:\n");
        printf("  type:  0x%02x\n", resp->hdr.type);
        printf("  nmp:   0x%02x\n", resp->hdr.nmp);
        printf("  meb:   0x%02x\n", resp->hdr.meb);
        printf("  rsvd:  0x%02x\n", resp->hdr.rsvd0);
        printf(" Status: 0x%02x\n", resp->status);
        if (resp->status == 0x4) { /* Invalid parameter error */
                printf(" PEL:");
                printf(" 0x%02x 0x%02x 0x%02x\n",
                                resp->rsvd0[2], resp->rsvd0[1], resp->rsvd0[0]);
        }

        printf(" cdw0:   0x%08x\n", le32_to_cpu(resp->cdw0));
        printf(" cdw1:   0x%08x\n", le32_to_cpu(resp->cdw1));
        printf(" cdw3:   0x%08x\n", le32_to_cpu(resp->cdw3));

	printf(" data [%zd bytes]\n", resp_data_len);
	hexdump(buf + sizeof(*resp), resp_data_len);
}

int do_set_features_breck(nvme_mi_ep_t ep, cmd_mvni_req_t* req_data, cmd_mvni_resp_t* resp_data)
{
	struct nvme_mi_admin_resp_hdr *resp;
	struct nvme_mi_admin_req_hdr *req;
	uint8_t resp_buf[512] = {0};
	uint8_t req_buf[512] = {0};
	struct nvme_mi_ctrl *ctrl;
	size_t resp_data_len = 0;
	size_t req_data_len;
	uint16_t ctrl_id;
	uint8_t fid;
	int rc;
	ctrl_id = 0; /* Management Controller ID */

	req = (struct nvme_mi_admin_req_hdr *)req_buf;
	resp = (struct nvme_mi_admin_resp_hdr *)resp_buf;

	memcpy(&req->opcode, req_data->nvme_cmd, sizeof(struct nvme_mi_admin_req_hdr) + req_data->payload_len);
	req_data_len = req_data->payload_len;
	req->ctrl_id = cpu_to_le16(ctrl_id);

	fid = NVME_GET(req->cdw10, FEATURES_CDW10_FID);
	dump_req(nvme_cmd_str(BRECK_NVME_CMD_SET_FEATURES), req, req_buf, req_data_len);

	switch (fid) {
		case BRECK_NVME_SET_FEAT_INTERPOSER_CONFIG: /* Interposer Configuration */
			print_interposer_config((interposer_config_t*)req_data->payload);
			resp_data_len = 0;
			break;
		default:
			printf("Feature id not supported\n");
			return -1;
	}

	ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
	if (!ctrl) {
		warn("can't create controller");
		return -1;
	}

	printf(" req_data_len: %lu\n", req_data_len);
	rc = nvme_mi_admin_xfer(ctrl, req, req_data_len, resp, 0, &resp_data_len);
	if (rc) {
		warn("nvme_admin_xfer failed: %d", rc);
		return -1;
	}
	memcpy(resp_data, resp_buf, sizeof(struct nvme_mi_admin_resp_hdr) + resp_data_len);
	resp_data->payload_len = resp_data_len;

	dump_resp(nvme_cmd_str(BRECK_NVME_CMD_SET_FEATURES), resp, resp_buf, resp_data_len);

	return 0;
}

int do_get_features_breck(nvme_mi_ep_t ep, cmd_mvni_req_t* req_data, cmd_mvni_resp_t* resp_data)
{
	struct nvme_mi_admin_resp_hdr *resp;
	struct nvme_mi_admin_req_hdr *req;
	uint8_t resp_buf[512] = {0};
	uint8_t req_buf[512] = {0};
	struct nvme_mi_ctrl *ctrl;
	size_t resp_data_len = 0;
	size_t req_data_len;
	uint16_t ctrl_id;
	uint8_t fid;
	int rc;
	ctrl_id = 0; /* Management Controller */

	req = (struct nvme_mi_admin_req_hdr *)req_buf;
	resp = (struct nvme_mi_admin_resp_hdr *)resp_buf;

	memcpy(&req->opcode, req_data->nvme_cmd, sizeof(struct nvme_mi_admin_req_hdr) + req_data->payload_len);
	req_data_len = req_data->payload_len;
	req->ctrl_id = cpu_to_le16(ctrl_id);
	fid = NVME_GET(req->cdw10, FEATURES_CDW10_FID);

	dump_req(nvme_cmd_str(BRECK_NVME_CMD_GET_FEATURES), req, req_buf, req_data_len);

	switch (fid) {
		case BRECK_NVME_SET_FEAT_INTERPOSER_CONFIG: /* Interposer Configuration */
			resp_data_len = sizeof(interposer_config_t);
			break;
		default:
			printf("Feature id not supported\n");
			return -1;
	}

	ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
	if (!ctrl) {
		warn("can't create controller");
		return -1;
	}

	printf(" req_data_len: %lu\n", req_data_len);

	rc = nvme_mi_admin_xfer(ctrl, req, req_data_len, resp, 0, &resp_data_len);
	if (rc) {
		warn("nvme_admin_xfer failed: %d", rc);
		return -1;
	}

	memcpy(resp_data, resp_buf, sizeof(struct nvme_mi_admin_resp_hdr) + resp_data_len);
	resp_data->payload_len = resp_data_len;
	
	dump_resp(nvme_cmd_str(BRECK_NVME_CMD_GET_FEATURES), resp, resp_buf, resp_data_len);
        
	if( fid == BRECK_NVME_SET_FEAT_INTERPOSER_CONFIG )
		print_interposer_config((interposer_config_t *)(resp_buf + sizeof(*resp)));
	
	return 0;
}

int do_reset_ip_cfg_breck(nvme_mi_ep_t ep, cmd_mvni_req_t* req_data, cmd_mvni_resp_t* resp_data)
{
        struct nvme_mi_admin_resp_hdr *resp;
        struct nvme_mi_admin_req_hdr *req;
        uint8_t resp_buf[512] = {0};
        uint8_t req_buf[512] = {0};
        struct nvme_mi_ctrl *ctrl;
        size_t resp_data_len = 0;
        size_t req_data_len;
        uint16_t ctrl_id;
        int rc;
        ctrl_id = 0; /* Management Controller */

        req = (struct nvme_mi_admin_req_hdr *)req_buf;
        resp = (struct nvme_mi_admin_resp_hdr *)resp_buf;

        memcpy(&req->opcode, req_data->nvme_cmd, sizeof(struct nvme_mi_admin_req_hdr) + req_data->payload_len);
        req_data_len = req_data->payload_len;
        req->ctrl_id = cpu_to_le16(ctrl_id);

	dump_req(nvme_cmd_str(BRECK_NVME_CMD_RESET_INTERPOSER_CONFIG), req, req_buf, req_data_len);

        ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
        if (!ctrl) {
                warn("can't create controller");
                return -1;
        }

        printf(" req_data_len: %lu\n", req_data_len);

        rc = nvme_mi_admin_xfer(ctrl, req, req_data_len, resp, 0, &resp_data_len);
        if (rc) {
                warn("nvme_admin_xfer failed: %d", rc);
                return -1;
        }

        memcpy(resp_data, resp_buf, sizeof(struct nvme_mi_admin_resp_hdr) + resp_data_len);
        resp_data->payload_len = resp_data_len;
	dump_resp(nvme_cmd_str(BRECK_NVME_CMD_RESET_INTERPOSER_CONFIG), resp, resp_buf, resp_data_len);

        return 0;
}

int do_create_child_ctrlr_breck(nvme_mi_ep_t ep, cmd_mvni_req_t* req_data, cmd_mvni_resp_t* resp_data)
{
        struct nvme_mi_admin_resp_hdr *resp;
        struct nvme_mi_admin_req_hdr *req;
        uint8_t resp_buf[512] = {0};
        uint8_t req_buf[512] = {0};
        struct nvme_mi_ctrl *ctrl;
        size_t resp_data_len = 0;
        size_t req_data_len;
        uint16_t ctrl_id;
        int rc;
        ctrl_id = 0; /* Management Controller */

        req = (struct nvme_mi_admin_req_hdr *)req_buf;
        resp = (struct nvme_mi_admin_resp_hdr *)resp_buf;

        memcpy(&req->opcode, req_data->nvme_cmd, sizeof(struct nvme_mi_admin_req_hdr) + req_data->payload_len);
        req_data_len = req_data->payload_len;
        req->ctrl_id = cpu_to_le16(ctrl_id);

	dump_req(nvme_cmd_str(BRECK_NVME_CMD_CREATE_CHILD_CTRLR), req, req_buf, req_data_len);

        ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
        if (!ctrl) {
                warn("can't create controller");
                return -1;
        }

        printf(" req_data_len: %lu\n", req_data_len);

        rc = nvme_mi_admin_xfer(ctrl, req, req_data_len, resp, 0, &resp_data_len);
        if (rc) {
                warn("nvme_admin_xfer failed: %d", rc);
                return -1;
        }

        memcpy(resp_data, resp_buf, sizeof(struct nvme_mi_admin_resp_hdr) + resp_data_len);
        resp_data->payload_len = resp_data_len;
	dump_resp(nvme_cmd_str(BRECK_NVME_CMD_CREATE_CHILD_CTRLR), resp, resp_buf, resp_data_len);

        return 0;
}

int do_admin_raw_breck(nvme_mi_ep_t ep, cmd_mvni_req_t* req_data, cmd_mvni_resp_t* resp_data)
{
	struct nvme_mi_admin_req_hdr req;
	memcpy(&req.opcode, req_data->nvme_cmd, sizeof(uint8_t));
	switch(req.opcode)
	{
		case BRECK_NVME_CMD_SET_FEATURES:
			do_set_features_breck(ep, req_data, resp_data);
			break;
		case BRECK_NVME_CMD_GET_FEATURES:
			do_get_features_breck(ep, req_data, resp_data);
			break;
		case BRECK_NVME_CMD_RESET_INTERPOSER_CONFIG:
			do_reset_ip_cfg_breck(ep, req_data, resp_data);
			break;
		case BRECK_NVME_CMD_CREATE_CHILD_CTRLR:
			do_create_child_ctrlr_breck(ep, req_data, resp_data);
			break;
		default:
			printf("Breck cmd opcode not supported: 0x%02x.", req.opcode);
			return 0;
	}

        return 0;
}

static struct {
	uint8_t id;
	const char *name;
} sec_protos[] = {
	{ 0x00, "Security protocol information" },
	{ 0xea, "NVMe" },
	{ 0xec, "JEDEC Universal Flash Storage" },
	{ 0xed, "SDCard TrustedFlash Security" },
	{ 0xee, "IEEE 1667" },
	{ 0xef, "ATA Device Server Password Security" },
};

static const char *sec_proto_description(uint8_t id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sec_protos); i++) {
		if (sec_protos[i].id == id)
			return sec_protos[i].name;
	}

	if (id >= 0xf0)
		return "Vendor specific";

	return "unknown";
}

int do_security_info(nvme_mi_ep_t ep, int argc, char **argv)
{
	struct nvme_security_receive_args args = { 0 };
	nvme_mi_ctrl_t ctrl;
	int i, rc, n_proto;
	unsigned long tmp;
	uint16_t ctrl_id;
	struct {
		uint8_t		rsvd[6];
		uint16_t	len;
		uint8_t		protocols[256];
	} proto_info;

	if (argc != 2) {
		fprintf(stderr, "no controller ID specified\n");
		return -1;
	}

	tmp = atoi(argv[1]);
	if (tmp > 0xffff) {
		fprintf(stderr, "invalid controller ID\n");
		return -1;
	}

	ctrl_id = tmp & 0xffff;

	ctrl = nvme_mi_init_ctrl(ep, ctrl_id);
	if (!ctrl) {
		warn("can't create controller");
		return -1;
	}

	/* protocol 0x00, spsp 0x0000: retrieve supported protocols */
	args.args_size = sizeof(args);
	args.data = &proto_info;
	args.data_len = sizeof(proto_info);

	rc = nvme_mi_admin_security_recv(ctrl, &args);
	if (rc) {
		warnx("can't perform Security Receive command: rc %d", rc);
		return -1;
	}

	if (args.data_len < 6) {
		warnx("Short response in security receive command (%d bytes)",
		      args.data_len);
		return -1;
	}

	n_proto = be16_to_cpu(proto_info.len);
	if (args.data_len < 6 + n_proto) {
		warnx("Short response in security receive command (%d bytes), "
		      "for %d protocols", args.data_len, n_proto);
		return -1;
	}

	printf("Supported protocols:\n");
	for (i = 0; i < n_proto; i++) {
		uint8_t id = proto_info.protocols[i];
		printf("  0x%02x: %s\n", id, sec_proto_description(id));
	}

	return 0;
}

struct {
	enum nvme_mi_config_smbus_freq id;
	const char *str;
} smbus_freqs[] = {
	{ NVME_MI_CONFIG_SMBUS_FREQ_100kHz, "100k" },
	{ NVME_MI_CONFIG_SMBUS_FREQ_400kHz, "400k" },
	{ NVME_MI_CONFIG_SMBUS_FREQ_1MHz,   "1M" },
};

static const char *smbus_freq_str(enum nvme_mi_config_smbus_freq freq)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(smbus_freqs); i++) {
		if (smbus_freqs[i].id == freq)
			return smbus_freqs[i].str;
	}

	return NULL;
}

static int smbus_freq_val(const char *str, enum nvme_mi_config_smbus_freq *freq)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(smbus_freqs); i++) {
		if (!strcmp(smbus_freqs[i].str, str)) {
			*freq = smbus_freqs[i].id;
			return 0;
		}
	}

	return -1;
}

int do_config_get(nvme_mi_ep_t ep, int argc, char **argv)
{
	enum nvme_mi_config_smbus_freq freq;
	uint16_t mtu;
	uint8_t port;
	int rc;

	if (argc > 1)
		port = atoi(argv[1]) & 0xff;
	else
		port = 0;

	rc = nvme_mi_mi_config_get_smbus_freq(ep, port, &freq);
	if (rc) {
		warn("can't query SMBus freq for port %d\n", port);
	} else {
		const char *fstr = smbus_freq_str(freq);
		printf("SMBus access frequency (port %d): %s [0x%x]\n", port,
		       fstr ?: "unknown", freq);
	}

	rc = nvme_mi_mi_config_get_mctp_mtu(ep, port, &mtu);
	if (rc)
		warn("can't query MCTP MTU for port %d\n", port);
	else
		printf("MCTP MTU (port %d): %d\n", port, mtu);

	return 0;
}

int do_config_set(nvme_mi_ep_t ep, int argc, char **argv)
{
	const char *name, *val;
	uint8_t port;
	int rc;

	if (argc != 4) {
		fprintf(stderr, "config set requires <port> <type> <val>\n");
		return -1;
	}

	port = atoi(argv[1]) & 0xff;
	name = argv[2];
	val = argv[3];

	if (!strcmp(name, "freq")) {
		enum nvme_mi_config_smbus_freq freq;
		rc = smbus_freq_val(val, &freq);
		if (rc) {
			fprintf(stderr, "unknown SMBus freq %s. "
				"Try 100k, 400k or 1M\n", val);
			return -1;
		}
		rc = nvme_mi_mi_config_set_smbus_freq(ep, port, freq);

	} else if (!strcmp(name, "mtu")) {
		uint16_t mtu;
		mtu = atoi(val) & 0xffff;
		/* controllers should reject this, but prevent the potential
		 * footgun of disabling futher comunication with the device
		 */
		if (mtu < 64) {
			fprintf(stderr, "MTU value too small\n");
			return -1;
		}
		rc = nvme_mi_mi_config_set_mctp_mtu(ep, port, mtu);

	} else {
		fprintf(stderr, "Invalid configuration '%s', "
			"try freq or mtu\n", name);
		return -1;
	}

	if (rc)
		fprintf(stderr, "config set failed with status %d\n", rc);

	return rc;
}

enum action {
	ACTION_INFO,
	ACTION_CONTROLLERS,
	ACTION_IDENTIFY,
	ACTION_GET_LOG_PAGE,
	ACTION_ADMIN_RAW,
	ACTION_ADMIN_RAW_BRECK,
	ACTION_SECURITY_INFO,
	ACTION_CONFIG_GET,
	ACTION_CONFIG_SET,
	ACTION_CONTROL_PRIMITIVE,
};

static int do_action_endpoint(enum action action, nvme_mi_ep_t ep, int argc, char** argv)
{
	int rc;

	switch (action) {
	case ACTION_INFO:
		rc = do_info(ep);
		break;
	case ACTION_CONTROLLERS:
		rc = do_controllers(ep);
		break;
	case ACTION_IDENTIFY:
		rc = do_identify(ep, argc, argv);
		break;
	case ACTION_GET_LOG_PAGE:
		rc = do_get_log_page(ep, argc, argv);
		break;
	case ACTION_ADMIN_RAW:
		rc = do_admin_raw(ep, argc, argv);
		break;
	case ACTION_ADMIN_RAW_BRECK:
		/* Do nothing, its handled seperately */
                rc = 0;
		break;
	case ACTION_SECURITY_INFO:
		rc = do_security_info(ep, argc, argv);
		break;
	case ACTION_CONFIG_GET:
		rc = do_config_get(ep, argc, argv);
		break;
	case ACTION_CONFIG_SET:
		rc = do_config_set(ep, argc, argv);
		break;
	case ACTION_CONTROL_PRIMITIVE:
		rc = do_control_primitive(ep, argc, argv);
		break;
	default:
		/* This shouldn't be possible, as we should be covering all
		 * of the enum action options above. Hoever, keep the compilers
		 * happy and fail gracefully. */
		fprintf(stderr, "invalid action %d?\n", action);
		rc = -1;
	}
	return rc;
}

int main(int argc, char **argv)
{
	enum action action;
	nvme_root_t root;
	nvme_mi_ep_t ep;
	bool dbus = false, usage = true;
	uint8_t eid = 0;
	int rc = 0, net = 0;
        char *third_argv = argv[3];

	if (argc >= 2 && strcmp(argv[1], "dbus") == 0) {
		usage = false;
		dbus= true;
		argv += 1;
		argc -= 1;
	} else if (argc >= 3) {
		usage = false;
		net = atoi(argv[1]);
		eid = atoi(argv[2]) & 0xff;
		argv += 2;
		argc -= 2;
	}

	if (usage) {
		fprintf(stderr,
			"usage: %s <net> <eid> [action] [action args]\n"
			"       %s 'dbus'      [action] [action args]\n",
			argv[0], argv[0]);
		fprintf(stderr, "where action is:\n"
			"  info\n"
			"  controllers\n"
			"  identify <controller-id> [--partial]\n"
			"  get-log-page <controller-id> [<log-id>]\n"
			"  admin <controller-id> <opcode> [<cdw10>, <cdw11>, ...]\n"
			"  security-info <controller-id>\n"
			"  get-config [port]\n"
			"  set-config <port> <type> <val>\n"
			"  control-primitive [action(abort|pause|resume|get-state|replay)]\n"
			"\n"
			"  'dbus' target will query D-Bus for known MCTP endpoints\n"
			);
		return EXIT_FAILURE;
	}

	if (argc == 1) {
		action = ACTION_INFO;
	} else {
		char *action_str = argv[1];
		argc--;
		argv++;

		if (!strcmp(action_str, "info")) {
			action = ACTION_INFO;
		} else if (!strcmp(action_str, "controllers")) {
			action = ACTION_CONTROLLERS;
		} else if (!strcmp(action_str, "identify")) {
			action = ACTION_IDENTIFY;
		} else if (!strcmp(action_str, "get-log-page")) {
			action = ACTION_GET_LOG_PAGE;
		} else if (!strcmp(action_str, "admin")) {
			action = ACTION_ADMIN_RAW;
		} else if (!strcmp(action_str, "cpp-server")) {
			action = ACTION_ADMIN_RAW_BRECK;
		} else if (!strcmp(action_str, "security-info")) {
			action = ACTION_SECURITY_INFO;
		} else if (!strcmp(action_str, "get-config")) {
			action = ACTION_CONFIG_GET;
		} else if (!strcmp(action_str, "set-config")) {
			action = ACTION_CONFIG_SET;
		} else if (!strcmp(action_str, "control-primitive")) {
			action = ACTION_CONTROL_PRIMITIVE;
		} else {
			fprintf(stderr, "invalid action '%s'\n", action_str);
			return EXIT_FAILURE;
		}
	}
	if (dbus) {
		nvme_root_t root;
		int i = 0;

		root = nvme_mi_scan_mctp();
		if (!root)
			errx(EXIT_FAILURE, "can't scan D-Bus entries");

		nvme_mi_for_each_endpoint(root, ep) i++;
		printf("Found %d endpoints in D-Bus:\n", i);
		nvme_mi_for_each_endpoint(root, ep) {
			char *desc = nvme_mi_endpoint_desc(ep);
			printf("%s\n", desc);
			rc = do_action_endpoint(action, ep, argc, argv);
			printf("---\n");
			free(desc);
		}
		nvme_mi_free_root(root);
	} else {
		root = nvme_mi_create_root(stderr, DEFAULT_LOGLEVEL);
		if (!root)
			err(EXIT_FAILURE, "can't create NVMe root");

		ep = nvme_mi_open_mctp(root, net, eid);
		if (!ep)
			errx(EXIT_FAILURE, "can't open MCTP endpoint %d:%d", net, eid);
                /* mi-mctp 1 9 cpp-server  --> example launch command*/
                if (strcmp(third_argv, "cpp-server") == 0) {
			g_ep = ep;
			rc = cpp_server_start();
		}
		else
			rc = do_action_endpoint(action, ep, argc, argv);

		rc = do_action_endpoint(action, ep, argc, argv);
		nvme_mi_close(ep);
		nvme_mi_free_root(root);
	}

	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}


