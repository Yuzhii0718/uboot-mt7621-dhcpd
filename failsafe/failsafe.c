/* SPDX-License-Identifier:	GPL-2.0 */
/*
 * Copyright (C) 2019 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 *
 */

#include <common.h>
#include <command.h>
#include <image.h>
#include <linux/libfdt.h>
#include <malloc.h>
#include <net/tcp.h>
#include <net/httpd.h>
#include <net/mtk_dhcpd.h>
#include <u-boot/md5.h>

#include "fs.h"

static u32 upload_data_id;
static const void *upload_data;
static size_t upload_size;
static int upgrade_success;

static void not_found_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

enum failsafe_fw_type {
	FAILSAFE_FW_FIRMWARE,
	FAILSAFE_FW_UBOOT,
	FAILSAFE_FW_INITRAMFS,
};

static enum failsafe_fw_type fw_type;

extern const char version_string[];

extern int write_firmware_failsafe(size_t data_addr, uint32_t data_size);
extern int write_bootloader_failsafe(size_t data_addr, uint32_t data_size);
extern int write_uboot_failsafe(size_t data_addr, uint32_t data_size);

static int output_plain_file(struct httpd_response *response,
	const char *filename)
{
	const struct fs_desc *file;
	int ret = 0;

	file = fs_find_file(filename);

	response->status = HTTP_RESP_STD;

	if (file) {
		response->data = file->data;
		response->size = file->size;
	} else {
		response->data = "Error: file not found";
		response->size = strlen(response->data);
		ret = 1;
	}

	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "text/html";

	return ret;
}

static void index_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW)
		output_plain_file(response, "index.html");
}

static void version_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status != HTTP_CB_NEW)
		return;

	response->status = HTTP_RESP_STD;
	response->data = version_string;
	response->size = strlen(response->data);

	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "text/plain";
}

static void html_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status != HTTP_CB_NEW)
		return;

	if (output_plain_file(response, request->urih->uri + 1))
		not_found_handler(status, request, response);
}

static void upload_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	static char md5_str[33] = "";
	static char resp[128];
	struct httpd_form_value *fw;
	u8 md5_sum[16];
	int i;

	static char hexchars[] = "0123456789abcdef";

	if (status != HTTP_CB_NEW)
		return;

	response->status = HTTP_RESP_STD;
	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "text/plain";

	fw = httpd_request_find_value(request, "firmware");
	if (fw) {
		fw_type = FAILSAFE_FW_FIRMWARE;
		goto done;
	}

	fw = httpd_request_find_value(request, "uboot");
	if (!fw)
		fw = httpd_request_find_value(request, "u-boot");
	if (fw) {
		fw_type = FAILSAFE_FW_UBOOT;
		goto done;
	}

	fw = httpd_request_find_value(request, "initramfs");
	if (fw) {
		int fdt_ret;
		bool is_uimage;
		const u8 *b;

		fw_type = FAILSAFE_FW_INITRAMFS;
		/*
		 * Accept both FIT (FDT header) and legacy uImage.
		 * OpenWrt ramips/mt7621 initramfs images are commonly legacy uImage.
		 */
		fdt_ret = fdt_check_header(fw->data);
		is_uimage = fw->size >= sizeof(image_header_t) &&
			image_check_magic((const image_header_t *)fw->data);
		if (fdt_ret && !is_uimage) {
			b = (const u8 *)fw->data;
			printf("failsafe: initramfs invalid image: size=%zu, fdt=%d, first4=%02x%02x%02x%02x\n",
			       fw->size, fdt_ret, b[0], b[1], b[2], b[3]);
			goto fail;
		}
		goto done;
	}

fail:
	response->data = "fail";
	response->size = strlen(response->data);
	return;

done:
	upload_data_id = upload_id;
	upload_data = fw->data;
	upload_size = fw->size;

	md5((u8 *)fw->data, fw->size, md5_sum);
	for (i = 0; i < 16; i++) {
		u8 hex = (md5_sum[i] >> 4) & 0xf;
		md5_str[i * 2] = hexchars[hex];
		hex = md5_sum[i] & 0xf;
		md5_str[i * 2 + 1] = hexchars[hex];
	}
	md5_str[32] = '\0';

	snprintf(resp, sizeof(resp), "%zu %s", fw->size, md5_str);
	response->data = resp;
	response->size = strlen(resp);
}

static void result_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	int ret = -1;

	if (status == HTTP_CB_NEW) {
		if (upload_data_id == upload_id) {
			switch (fw_type) {
			case FAILSAFE_FW_INITRAMFS:
				ret = 0;
				break;
			case FAILSAFE_FW_UBOOT:
				ret = write_uboot_failsafe((size_t)upload_data,
					upload_size);
				break;
			case FAILSAFE_FW_FIRMWARE:
			default:
				ret = write_firmware_failsafe((size_t)upload_data,
					upload_size);
				break;
			}
		}

		/* invalidate upload identifier */
		upload_data_id = rand();

		upgrade_success = !ret;

		response->status = HTTP_RESP_STD;
		response->info.code = 200;
		response->info.connection_close = 1;
		response->info.content_type = "text/plain";
		response->data = upgrade_success ? "success" : "failed";
		response->size = strlen(response->data);

		return;
	}

	if (status == HTTP_CB_CLOSED) {
		if (upgrade_success)
			tcp_close_all_conn();
	}
}

static void style_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		output_plain_file(response, "style.css");
		response->info.content_type = "text/css";
	}
}

static void js_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		output_plain_file(response, "main.js");
		response->info.content_type = "text/javascript";
	}
}

static void not_found_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		output_plain_file(response, "404.html");
		response->info.code = 404;
	}
}

int start_web_failsafe(void)
{
	struct httpd_instance *inst;

	inst = httpd_find_instance(80);
	if (inst)
		httpd_free_instance(inst);

	inst = httpd_create_instance(80);
	if (!inst) {
		printf("Error: failed to create HTTP instance on port 80\n");
		return -1;
	}

	httpd_register_uri_handler(inst, "/", &index_handler, NULL);
	httpd_register_uri_handler(inst, "/cgi-bin/luci", &index_handler, NULL);
	httpd_register_uri_handler(inst, "/cgi-bin/luci/", &index_handler, NULL);

	httpd_register_uri_handler(inst, "/upload", &upload_handler, NULL);
	httpd_register_uri_handler(inst, "/result", &result_handler, NULL);
	httpd_register_uri_handler(inst, "/version", &version_handler, NULL);

	httpd_register_uri_handler(inst, "/main.js", &js_handler, NULL);
	httpd_register_uri_handler(inst, "/style.css", &style_handler, NULL);

	httpd_register_uri_handler(inst, "/booting.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/fail.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/flashing.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/initramfs.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/uboot.html", &html_handler, NULL);

	httpd_register_uri_handler(inst, "", &not_found_handler, NULL);

	if (IS_ENABLED(CONFIG_MTK_DHCPD))
		mtk_dhcpd_start();

	net_loop(TCP);

	if (IS_ENABLED(CONFIG_MTK_DHCPD))
		mtk_dhcpd_stop();

	return 0;
}

static int do_httpd(cmd_tbl_t *cmdtp, int flag, int argc,
	char *const argv[])
{
	int ret;

	printf("\nWeb failsafe UI started\n");
	
	ret = start_web_failsafe();

	if (upgrade_success) {
		if (fw_type == FAILSAFE_FW_INITRAMFS) {
			char cmd[64];

			/* initramfs is expected to be a FIT image */
			snprintf(cmd, sizeof(cmd), "bootm 0x%lx", (ulong)upload_data);
			run_command(cmd, 0);
		} else {
			do_reset(NULL, 0, 0, NULL);
		}
	}

	return ret;
}

U_BOOT_CMD(httpd, 1, 0, do_httpd,
	"Start failsafe HTTP server", ""
);
