/* SPDX-License-Identifier:	GPL-2.0 */
/*
 * Copyright (C) 2019 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 *
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <image.h>
#include <linux/libfdt.h>
#include <linux/ctype.h>
#include <malloc.h>
#include <net/tcp.h>
#include <net/httpd.h>
#include <net/mtk_dhcpd.h>
#include <u-boot/md5.h>
#include <asm/global_data.h>
#ifdef CONFIG_MTD
#include <linux/mtd/mtd.h>
#include <spi_flash.h>
#ifdef CONFIG_MTD_NAND
#include <linux/mtd/rawnand.h>
#include <nand.h>
#endif
#endif
#ifdef CONFIG_CMD_MTDPARTS
#include <jffs2/load_kernel.h>
#endif

#include "fs.h"

DECLARE_GLOBAL_DATA_PTR;

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
	FAILSAFE_FW_FACTORY,
};

static enum failsafe_fw_type fw_type;

extern const char version_string[];

extern int write_firmware_failsafe(size_t data_addr, uint32_t data_size);
extern int write_bootloader_failsafe(size_t data_addr, uint32_t data_size);
extern int write_uboot_failsafe(size_t data_addr, uint32_t data_size);
extern int write_factory_failsafe(size_t data_addr, uint32_t data_size);

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

	fw = httpd_request_find_value(request, "factory");
	if (fw) {
		fw_type = FAILSAFE_FW_FACTORY;
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

static size_t json_escape(char *dst, size_t dst_sz, const char *src)
{
	size_t di = 0;
	const unsigned char *s = (const unsigned char *)src;

	if (!dst || !dst_sz)
		return 0;

	if (!src) {
		dst[0] = '\0';
		return 0;
	}

	while (*s && di + 2 < dst_sz) {
		unsigned char c = *s++;

		if (c == '"' || c == '\\') {
			if (di + 2 >= dst_sz)
				break;
			dst[di++] = '\\';
			dst[di++] = (char)c;
			continue;
		}

		if (c < 0x20) {
			/* Replace control chars with a space */
			dst[di++] = ' ';
			continue;
		}

		dst[di++] = (char)c;
	}

	dst[di] = '\0';
	return di;
}

static const char *failsafe_get_mtdparts(void)
{
	const char *s;

	s = env_get("mtdparts");
	if (s && s[0])
		return s;

#ifdef CONFIG_MTDPARTS_DEFAULT
	s = CONFIG_MTDPARTS_DEFAULT;
	if (s && !strncmp(s, "mtdparts=", 9))
		return s + 9;
	if (s)
		return s;
#endif

	return "";
}

static const char *failsafe_get_mtdids(void)
{
	const char *s;

	s = env_get("mtdids");
	if (s && s[0])
		return s;

#ifdef CONFIG_MTDIDS_DEFAULT
	s = CONFIG_MTDIDS_DEFAULT;
	if (s)
		return s;
#endif

	return "";
}

#ifdef CONFIG_CMD_MTDPARTS
static int failsafe_parse_dev_id(const char *id, u8 *type, u8 *num)
{
	char *end;
	ulong n;

	if (!id || !id[0] || !type || !num)
		return -EINVAL;

	if (!strncmp(id, "nor", 3)) {
		*type = MTD_DEV_TYPE_NOR;
		id += 3;
	} else if (!strncmp(id, "nand", 4)) {
		*type = MTD_DEV_TYPE_NAND;
		id += 4;
	} else if (!strncmp(id, "onenand", 7)) {
		*type = MTD_DEV_TYPE_ONENAND;
		id += 7;
	} else if (!strncmp(id, "nmbm", 4)) {
		*type = MTD_DEV_TYPE_NMBM;
		id += 4;
	} else {
		return -EINVAL;
	}

	n = simple_strtoul(id, &end, 10);
	if (end == id)
		return -EINVAL;

	if (n > 255)
		return -ERANGE;

	*num = (u8)n;
	return 0;
}

static int failsafe_get_mtdparts_dev_id(char *out, size_t out_sz)
{
	const char *mtdparts = failsafe_get_mtdparts();
	size_t i = 0;

	if (!out || !out_sz)
		return -EINVAL;

	out[0] = '\0';

	if (!mtdparts || !mtdparts[0])
		return -ENOENT;

	/* mtdparts format in env/config: "<dev>:..." or "<dev>:...;<dev2>:..." */
	while (mtdparts[i] && mtdparts[i] != ':' && mtdparts[i] != ';' && i + 1 < out_sz) {
		out[i] = mtdparts[i];
		i++;
	}
	out[i] = '\0';

	return out[0] ? 0 : -EINVAL;
}

static int failsafe_map_alias_to_dev_id(const char *alias, char *out, size_t out_sz)
{
	const char *mtdids = failsafe_get_mtdids();
	const char *p;

	if (!alias || !alias[0] || !out || !out_sz)
		return -EINVAL;

	out[0] = '\0';

	if (!mtdids || !mtdids[0])
		return -ENOENT;

	p = mtdids;
	while (*p) {
		const char *eq, *comma;
		size_t klen, vlen;
		char key[32], val[64];

		comma = strchr(p, ',');
		eq = strchr(p, '=');
		if (!eq || (comma && eq > comma)) {
			break;
		}

		klen = (size_t)(eq - p);
		if (klen >= sizeof(key))
			klen = sizeof(key) - 1;
		memcpy(key, p, klen);
		key[klen] = '\0';

		vlen = comma ? (size_t)(comma - (eq + 1)) : strlen(eq + 1);
		if (vlen >= sizeof(val))
			vlen = sizeof(val) - 1;
		memcpy(val, eq + 1, vlen);
		val[vlen] = '\0';

		if (!strcmp(val, alias)) {
			strlcpy(out, key, out_sz);
			return 0;
		}

		if (!comma)
			break;
		p = comma + 1;
	}

	return -ENOENT;
}

static struct mtd_device *failsafe_find_mtd_device(void)
{
	struct mtd_device *dev;
	char id[64];
	char mapped[32];
	u8 type, num;

	if (failsafe_get_mtdparts_dev_id(id, sizeof(id)))
		goto fallback;

	/* Try direct "nmbm0/nand0/nor0" first */
	if (!failsafe_parse_dev_id(id, &type, &num)) {
		dev = device_find(type, num);
		if (dev)
			return dev;
	}

	/* Try mapping alias (e.g. "raspi") via mtdids (e.g. "nor0=raspi") */
	if (!failsafe_map_alias_to_dev_id(id, mapped, sizeof(mapped)) &&
	    !failsafe_parse_dev_id(mapped, &type, &num)) {
		dev = device_find(type, num);
		if (dev)
			return dev;
	}

fallback:
	dev = device_find(MTD_DEV_TYPE_NMBM, 0);
	if (dev)
		return dev;
	dev = device_find(MTD_DEV_TYPE_NAND, 0);
	if (dev)
		return dev;
	dev = device_find(MTD_DEV_TYPE_NOR, 0);
	if (dev)
		return dev;
	dev = device_find(MTD_DEV_TYPE_ONENAND, 0);
	return dev;
}
#endif /* CONFIG_CMD_MTDPARTS */

static void sysinfo_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = 4096;
	const char *board_model;
	const char *board_name;
	const char *board_compat;
	const char *mtdparts;
	const char *mtdids;
	char esc_board_model[256];
	char esc_board_name[256];
	char esc_board_compat[256];
	char esc_mtdparts[1024];
	char esc_mtdids[512];
	u64 ram_size = 0;
#ifdef CONFIG_MTD
	struct mtd_info *mtd = NULL;
	char master_name[32];
	char esc_master_name[64];
	char esc_flash_model[128];
	char flash_model[128];
	char esc_raw_name[64];
	char esc_raw_model[128];
	u64 raw_size = 0;
	u64 flash_size = 0;
	u32 erasesize = 0, writesize = 0;
	u32 flash_type = 0;
#endif
	const void *fdt;
	int l;
	const char *dt_model = NULL;
	const char *dt_compat = NULL;

	(void)request;

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	buf = malloc(left);
	if (!buf) {
		response->status = HTTP_RESP_STD;
		response->data = "{}";
		response->size = strlen(response->data);
		response->info.code = 500;
		response->info.connection_close = 1;
		response->info.content_type = "application/json";
		return;
	}

	fdt = gd ? gd->fdt_blob : NULL;
	if (fdt) {
		dt_model = fdt_getprop(fdt, 0, "model", &l);
		dt_compat = fdt_getprop(fdt, 0, "compatible", &l);
		if (dt_compat && l > 0) {
			/* compatible is NUL-separated list; take the first string */
			board_compat = dt_compat;
		} else {
			board_compat = "";
		}
	} else {
		board_compat = "";
	}

	/*
	 * Prefer user-provided model/name (CI/build/customize injected) over DT,
	 * since many reference DTS files are generic.
	 */
	board_model = CONFIG_WEBUI_FAILSAFE_BOARD_MODEL;
	if (!board_model || !board_model[0])
		board_model = env_get("model");
	if (!board_model || !board_model[0])
		board_model = dt_model;
	if (!board_model)
		board_model = "";

	board_name = CONFIG_WEBUI_FAILSAFE_BOARD_NAME;
	if (!board_name || !board_name[0])
		board_name = env_get("board_name");
	if (!board_name || !board_name[0])
		board_name = env_get("board");
	if (!board_name)
		board_name = "";

	mtdparts = failsafe_get_mtdparts();
	mtdids = failsafe_get_mtdids();

	if (gd)
		ram_size = (u64)gd->ram_size;

	json_escape(esc_board_model, sizeof(esc_board_model), board_model);
	json_escape(esc_board_name, sizeof(esc_board_name), board_name);
	json_escape(esc_board_compat, sizeof(esc_board_compat), board_compat);
	json_escape(esc_mtdparts, sizeof(esc_mtdparts), mtdparts);
	json_escape(esc_mtdids, sizeof(esc_mtdids), mtdids);

#ifdef CONFIG_MTD
	mtd = NULL;
	master_name[0] = '\0';
	#ifdef CONFIG_CMD_MTDPARTS
	if (mtdparts_init() == 0) {
		struct mtd_device *dev = failsafe_find_mtd_device();
		if (dev && dev->id)
			snprintf(master_name, sizeof(master_name), "%s%d",
				 MTD_DEV_TYPE(dev->id->type), dev->id->num);
	}
	#endif
	if (!master_name[0]) {
		/* fallback */
		strlcpy(master_name, "nmbm0", sizeof(master_name));
	}

	mtd = get_mtd_device_nm(master_name);
	if (!IS_ERR(mtd)) {
		int master_is_nand = !strncmp(master_name, "nand", 4);
		int master_is_nmbm = !strncmp(master_name, "nmbm", 4);

		/* raw device defaults */
		json_escape(esc_raw_name, sizeof(esc_raw_name), "");
		json_escape(esc_raw_model, sizeof(esc_raw_model), "");
		raw_size = 0;

		/* Expose the logical MTD device name (e.g. nmbm0/nand0/nor0) */
		json_escape(esc_master_name, sizeof(esc_master_name), master_name);
		flash_model[0] = '\0';
		if (mtd->type == MTD_NORFLASH) {
			struct spi_flash *sf = (struct spi_flash *)mtd->priv;
			if (sf && sf->name)
				strlcpy(flash_model, sf->name, sizeof(flash_model));
		}
#ifdef CONFIG_MTD_NAND
		/*
		 * Raw NAND: prefer ONFI manufacturer/model when available.
		 * Avoid mtd_to_nand() on wrappers (e.g. NMBM) by checking name prefix.
		 */
		if (!flash_model[0] && master_is_nand && nand_mtd_to_devnum(mtd) >= 0) {
			struct nand_chip *chip = mtd_to_nand(mtd);
			if (chip && !memcmp(chip->onfi_params.sig, "ONFI", 4)) {
				char manuf[13];
				char model[21];
				size_t i, end;

				memset(manuf, 0, sizeof(manuf));
				memset(model, 0, sizeof(model));
				end = sizeof(chip->onfi_params.manufacturer);
				while (end && (chip->onfi_params.manufacturer[end - 1] == ' ' ||
						chip->onfi_params.manufacturer[end - 1] == '\0'))
					end--;
				for (i = 0; i < end && i + 1 < sizeof(manuf); i++)
					manuf[i] = chip->onfi_params.manufacturer[i];
				manuf[i] = '\0';

				end = sizeof(chip->onfi_params.model);
				while (end && (chip->onfi_params.model[end - 1] == ' ' ||
						chip->onfi_params.model[end - 1] == '\0'))
					end--;
				for (i = 0; i < end && i + 1 < sizeof(model); i++)
					model[i] = chip->onfi_params.model[i];
				model[i] = '\0';

				if (manuf[0] && model[0])
					snprintf(flash_model, sizeof(flash_model), "%s %s", manuf, model);
				else if (model[0])
					strlcpy(flash_model, model, sizeof(flash_model));
				else if (manuf[0])
					strlcpy(flash_model, manuf, sizeof(flash_model));
			}
			/* Fallback to NAND ID table name if ONFI isn't present */
			if (!flash_model[0] && mtd->name && mtd->name[0])
				strlcpy(flash_model, mtd->name, sizeof(flash_model));
		}

		/*
		 * NMBM (bad block management wrapper) usually sits on top of raw NAND.
		 * Always try to expose raw NAND info for UI (raw 128MiB vs nmbm 120MiB).
		 */
		if (master_is_nmbm) {
			struct mtd_info *lower;
			struct nand_chip *chip;
			u64 logical_size = (u64)mtd->size;

			/*
			 * Prefer a device literally named "nand0" when present.
			 * Some old trees/platforms may register the raw NAND under a different name,
			 * so fall back to scanning all MTD devices.
			 */
			lower = get_mtd_device_nm("nand0");
			if (!IS_ERR(lower) && nand_mtd_to_devnum(lower) >= 0) {
				/* ok */
			} else {
				if (!IS_ERR(lower))
					put_mtd_device(lower);
				lower = NULL;

				/* Find the largest NAND MTD device as the best raw-NAND candidate */
				{
					int idx;
					u64 best_size = 0;
					char best_name[64];
					char best_model[128];

					best_name[0] = '\0';
					best_model[0] = '\0';

					for (idx = 0; idx < MAX_MTD_DEVICES; idx++) {
						struct mtd_info *cand;
						char cand_model[128];

						cand = get_mtd_device(NULL, idx);
						if (IS_ERR(cand))
							continue;

						/*
						 * Old trees may abuse mtd->type for MTD_DEV_TYPE_*,
						 * so do not rely on mtd_type_is_nand().
						 */
						if (nand_mtd_to_devnum(cand) < 0 &&
						    !(cand->name && !strncmp(cand->name, "nand", 4))) {
							put_mtd_device(cand);
							continue;
						}

						/* Skip the logical NMBM device itself */
						if (cand->name && !strncmp(cand->name, "nmbm", 4)) {
							put_mtd_device(cand);
							continue;
						}

						/* Prefer a NAND larger than the logical (nmbm) device */
						if ((u64)cand->size <= logical_size) {
							put_mtd_device(cand);
							continue;
						}

						cand_model[0] = '\0';
						chip = mtd_to_nand(cand);
						if (chip && !memcmp(chip->onfi_params.sig, "ONFI", 4)) {
							char manuf[13];
							char model[21];
							size_t i, end;

							memset(manuf, 0, sizeof(manuf));
							memset(model, 0, sizeof(model));
							end = sizeof(chip->onfi_params.manufacturer);
							while (end && (chip->onfi_params.manufacturer[end - 1] == ' ' ||
									chip->onfi_params.manufacturer[end - 1] == '\0'))
								end--;
							for (i = 0; i < end && i + 1 < sizeof(manuf); i++)
								manuf[i] = chip->onfi_params.manufacturer[i];
							manuf[i] = '\0';

							end = sizeof(chip->onfi_params.model);
							while (end && (chip->onfi_params.model[end - 1] == ' ' ||
									chip->onfi_params.model[end - 1] == '\0'))
								end--;
							for (i = 0; i < end && i + 1 < sizeof(model); i++)
								model[i] = chip->onfi_params.model[i];
							model[i] = '\0';

							if (manuf[0] && model[0])
								snprintf(cand_model, sizeof(cand_model), "%s %s", manuf, model);
							else if (model[0])
								strlcpy(cand_model, model, sizeof(cand_model));
							else if (manuf[0])
								strlcpy(cand_model, manuf, sizeof(cand_model));
						}

						if (!cand_model[0] && cand->name && cand->name[0])
							strlcpy(cand_model, cand->name, sizeof(cand_model));

						if ((u64)cand->size > best_size) {
							best_size = (u64)cand->size;
							strlcpy(best_name, cand->name ? cand->name : "", sizeof(best_name));
							strlcpy(best_model, cand_model, sizeof(best_model));
						}

						put_mtd_device(cand);
					}

					if (best_size) {
						/* Use the best candidate as raw NAND info */
						json_escape(esc_raw_name, sizeof(esc_raw_name), best_name);
						json_escape(esc_raw_model, sizeof(esc_raw_model), best_model);
						raw_size = best_size;
					}
				}
			}

			if (lower && !IS_ERR(lower) && nand_mtd_to_devnum(lower) >= 0) {
				chip = mtd_to_nand(lower);
				if (chip && !memcmp(chip->onfi_params.sig, "ONFI", 4)) {
					char manuf[13];
					char model[21];
					size_t i, end;

					memset(manuf, 0, sizeof(manuf));
					memset(model, 0, sizeof(model));
					end = sizeof(chip->onfi_params.manufacturer);
					while (end && (chip->onfi_params.manufacturer[end - 1] == ' ' ||
							chip->onfi_params.manufacturer[end - 1] == '\0'))
						end--;
					for (i = 0; i < end && i + 1 < sizeof(manuf); i++)
						manuf[i] = chip->onfi_params.manufacturer[i];
					manuf[i] = '\0';

					end = sizeof(chip->onfi_params.model);
					while (end && (chip->onfi_params.model[end - 1] == ' ' ||
							chip->onfi_params.model[end - 1] == '\0'))
						end--;
					for (i = 0; i < end && i + 1 < sizeof(model); i++)
						model[i] = chip->onfi_params.model[i];
					model[i] = '\0';

					if (manuf[0] && model[0])
						snprintf(flash_model, sizeof(flash_model), "%s %s", manuf, model);
					else if (model[0])
						strlcpy(flash_model, model, sizeof(flash_model));
					else if (manuf[0])
						strlcpy(flash_model, manuf, sizeof(flash_model));
				}
				if (!flash_model[0] && lower->name && lower->name[0])
					strlcpy(flash_model, lower->name, sizeof(flash_model));

				/* also expose raw NAND info for UI (128MiB raw vs 120MiB nmbm, etc.) */
				json_escape(esc_raw_name, sizeof(esc_raw_name), lower->name ? lower->name : "nand");
				raw_size = (u64)lower->size;
				{
					char raw_model[128];
					raw_model[0] = '\0';
					if (chip && !memcmp(chip->onfi_params.sig, "ONFI", 4)) {
						char manuf[13];
						char model[21];
						size_t i, end;

						memset(manuf, 0, sizeof(manuf));
						memset(model, 0, sizeof(model));
						end = sizeof(chip->onfi_params.manufacturer);
						while (end && (chip->onfi_params.manufacturer[end - 1] == ' ' ||
								chip->onfi_params.manufacturer[end - 1] == '\0'))
							end--;
						for (i = 0; i < end && i + 1 < sizeof(manuf); i++)
							manuf[i] = chip->onfi_params.manufacturer[i];
						manuf[i] = '\0';

						end = sizeof(chip->onfi_params.model);
						while (end && (chip->onfi_params.model[end - 1] == ' ' ||
								chip->onfi_params.model[end - 1] == '\0'))
							end--;
						for (i = 0; i < end && i + 1 < sizeof(model); i++)
							model[i] = chip->onfi_params.model[i];
						model[i] = '\0';

						if (manuf[0] && model[0])
							snprintf(raw_model, sizeof(raw_model), "%s %s", manuf, model);
						else if (model[0])
							strlcpy(raw_model, model, sizeof(raw_model));
						else if (manuf[0])
							strlcpy(raw_model, manuf, sizeof(raw_model));
					}
					if (!raw_model[0] && lower->name && lower->name[0])
						strlcpy(raw_model, lower->name, sizeof(raw_model));
					json_escape(esc_raw_model, sizeof(esc_raw_model), raw_model);
				}
				put_mtd_device(lower);
			}
		}
#endif

		/*
		 * When using NMBM, the logical device is usually named "nmbm0" and the
		 * underlying raw NAND is exposed as "nand0".
		 *
		 * Some builds may have CONFIG_MTD enabled but CONFIG_MTD_NAND disabled,
		 * in which case the ONFI/nand helper logic above is not compiled.
		 * Still, we should expose raw NAND capacity for UI purposes.
		 */
		if (master_is_nmbm && !raw_size) {
			u64 logical_size = (u64)mtd->size;
			struct mtd_info *lower;

			/* Prefer a device literally named "nand0" when present */
			lower = get_mtd_device_nm("nand0");
			if (!IS_ERR(lower)) {
				if ((u64)lower->size > logical_size) {
					json_escape(esc_raw_name, sizeof(esc_raw_name),
						lower->name ? lower->name : "nand0");
					json_escape(esc_raw_model, sizeof(esc_raw_model),
						lower->name ? lower->name : "nand0");
					raw_size = (u64)lower->size;
				}
				put_mtd_device(lower);
			}

			/* Fallback: scan all MTD devices and pick the largest "nand*" */
			if (!raw_size) {
				int idx;
				u64 best_size = 0;
				char best_name[64];

				best_name[0] = '\0';

				for (idx = 0; idx < MAX_MTD_DEVICES; idx++) {
					struct mtd_info *cand;

					cand = get_mtd_device(NULL, idx);
					if (IS_ERR(cand))
						continue;

					if (!cand->name || !cand->name[0]) {
						put_mtd_device(cand);
						continue;
					}

					/* Skip the logical NMBM device itself */
					if (!strncmp(cand->name, "nmbm", 4)) {
						put_mtd_device(cand);
						continue;
					}

					/* We only care about raw NAND candidates */
					if (strncmp(cand->name, "nand", 4)) {
						put_mtd_device(cand);
						continue;
					}

					/* Prefer a NAND larger than the logical (nmbm) device */
					if ((u64)cand->size <= logical_size) {
						put_mtd_device(cand);
						continue;
					}

					if ((u64)cand->size > best_size) {
						best_size = (u64)cand->size;
						strlcpy(best_name, cand->name, sizeof(best_name));
					}

					put_mtd_device(cand);
				}

				if (best_size) {
					json_escape(esc_raw_name, sizeof(esc_raw_name), best_name);
					json_escape(esc_raw_model, sizeof(esc_raw_model), best_name);
					raw_size = best_size;
				}
			}
		}

		/* Last resort: use whatever name the master MTD exposes */
		if (!flash_model[0] && mtd->name && mtd->name[0])
			strlcpy(flash_model, mtd->name, sizeof(flash_model));
		json_escape(esc_flash_model, sizeof(esc_flash_model), flash_model);
		flash_size = (u64)mtd->size;
		erasesize = mtd->erasesize;
		writesize = mtd->writesize;
		flash_type = mtd->type;
		put_mtd_device(mtd);
	} else {
		json_escape(esc_master_name, sizeof(esc_master_name), "");
		json_escape(esc_flash_model, sizeof(esc_flash_model), "");
		json_escape(esc_raw_name, sizeof(esc_raw_name), "");
		json_escape(esc_raw_model, sizeof(esc_raw_model), "");
		raw_size = 0;
	}
#endif

	len += snprintf(buf + len, left - len, "{");
	len += snprintf(buf + len, left - len,
		"\"board\":{\"model\":\"%s\",\"name\":\"%s\",\"compatible\":\"%s\"},",
		esc_board_model, esc_board_name, esc_board_compat);
	len += snprintf(buf + len, left - len,
		"\"ram\":{\"size\":%llu}",
		(unsigned long long)ram_size);
	len += snprintf(buf + len, left - len,
		",\"mtd\":{\"ids\":\"%s\",\"parts\":\"%s\"}",
		esc_mtdids, esc_mtdparts);
#ifdef CONFIG_MTD
	len += snprintf(buf + len, left - len,
		",\"flash\":{\"name\":\"%s\",\"model\":\"%s\",\"size\":%llu,\"erasesize\":%u,\"writesize\":%u,\"type\":%u",
		esc_master_name, esc_flash_model,
		(unsigned long long)flash_size,
		erasesize, writesize, flash_type);
	if (raw_size) {
		len += snprintf(buf + len, left - len,
			",\"raw\":{\"name\":\"%s\",\"model\":\"%s\",\"size\":%llu}",
			esc_raw_name, esc_raw_model,
			(unsigned long long)raw_size);
	}
	len += snprintf(buf + len, left - len, "}");
#endif
	len += snprintf(buf + len, left - len, "}");

	response->status = HTTP_RESP_STD;
	response->data = buf;
	response->size = strlen(buf);
	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
	response->session_data = buf;
}

struct reboot_session {
	int dummy;
};

static void reboot_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	struct reboot_session *st;

	(void)request;

	if (status == HTTP_CB_NEW) {
		st = calloc(1, sizeof(*st));
		if (!st) {
			response->status = HTTP_RESP_STD;
			response->data = "error";
			response->size = strlen(response->data);
			response->info.code = 500;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		response->session_data = st;
		response->status = HTTP_RESP_STD;
		response->data = "rebooting";
		response->size = strlen(response->data);
		response->info.code = 200;
		response->info.connection_close = 1;
		response->info.content_type = "text/plain";
		return;
	}

	if (status == HTTP_CB_CLOSED) {
		st = response->session_data;
		free(st);

		/* Ensure current HTTP session fully closes before reset */
		tcp_close_all_conn();
		do_reset(NULL, 0, 0, NULL);
	}
}

static void backupinfo_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
#if !defined(CONFIG_MTD) || !defined(CONFIG_CMD_MTDPARTS)
	(void)request;

	if (status != HTTP_CB_NEW)
		return;

	response->status = HTTP_RESP_STD;
	response->data = "{\"mmc\":{\"present\":false},\"mtd\":{\"present\":false,\"parts\":[]}}\n";
	response->size = strlen(response->data);
	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
	return;
#else
	char *buf;
	int len = 0;
	int left = 8192;
	struct mtd_device *dev = NULL;
	struct list_head *lh;
	int first = 1;
	char esc_mtdparts[1024];

	(void)request;

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	buf = malloc(left);
	if (!buf) {
		response->status = HTTP_RESP_STD;
		response->data = "{}";
		response->size = strlen(response->data);
		response->info.code = 500;
		response->info.connection_close = 1;
		response->info.content_type = "application/json";
		return;
	}

	if (mtdparts_init())
		dev = NULL;
	else
		dev = failsafe_find_mtd_device();

	len += snprintf(buf + len, left - len, "{");
	len += snprintf(buf + len, left - len, "\"mmc\":{\"present\":false},");
	len += snprintf(buf + len, left - len, "\"mtd\":{");

	if (!dev) {
		len += snprintf(buf + len, left - len, "\"present\":false,\"parts\":[]");
		len += snprintf(buf + len, left - len, "}}\n");

		response->status = HTTP_RESP_STD;
		response->data = buf;
		response->size = strlen(buf);
		response->info.code = 200;
		response->info.connection_close = 1;
		response->info.content_type = "application/json";
		response->session_data = buf;
		return;
	}

	len += snprintf(buf + len, left - len, "\"present\":true,");
	json_escape(esc_mtdparts, sizeof(esc_mtdparts), failsafe_get_mtdparts());
	len += snprintf(buf + len, left - len, "\"mtdparts\":\"%s\",", esc_mtdparts);

	/*
	 * Expose full-disk candidates (raw NAND vs NMBM, etc.).
	 * Old tree may not have mtd->parent, so we explicitly probe well-known device names.
	 */
	len += snprintf(buf + len, left - len, "\"devices\":[");
	{
		static const char *names[] = { "nand0", "nmbm0", "nor0", "onenand0" };
		int first_dev = 1;
		size_t ni;

		for (ni = 0; ni < ARRAY_SIZE(names) && len < left - 128; ni++) {
			struct mtd_info *m;
			char esc_name[64];
			const char *nm = names[ni];

			m = get_mtd_device_nm(nm);
			if (IS_ERR(m))
				continue;

			json_escape(esc_name, sizeof(esc_name), nm);
			len += snprintf(buf + len, left - len,
				"%s{\"name\":\"%s\",\"size\":%llu,\"type\":%u}",
				first_dev ? "" : ",",
				esc_name,
				(unsigned long long)m->size,
				(unsigned)m->type);
			first_dev = 0;
			put_mtd_device(m);
		}
	}
	len += snprintf(buf + len, left - len, "],");

	len += snprintf(buf + len, left - len, "\"parts\":[");
	list_for_each(lh, &dev->parts) {
		struct part_info *p = list_entry(lh, struct part_info, link);
		char esc_name[128];

		if (!p || !p->name)
			continue;

		json_escape(esc_name, sizeof(esc_name), p->name);

		if (!first)
			len += snprintf(buf + len, left - len, ",");
		first = 0;

		len += snprintf(buf + len, left - len,
			"{\"name\":\"%s\",\"offset\":%llu,\"size\":%llu}",
			esc_name,
			(unsigned long long)p->offset,
			(unsigned long long)p->size);
		if (len + 128 >= left)
			break;
	}
	len += snprintf(buf + len, left - len, "]}");
	len += snprintf(buf + len, left - len, "}\n");

	response->status = HTTP_RESP_STD;
	response->data = buf;
	response->size = strlen(buf);
	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
	response->session_data = buf;
#endif
}

static void str_sanitize_component(char *s)
{
	char *p;

	if (!s)
		return;

	for (p = s; *p; p++) {
		unsigned char c = *p;

		if (isalnum(c) || c == '-' || c == '_' || c == '.')
			continue;

		*p = '_';
	}
}

static int parse_u64_len(const char *s, u64 *out)
{
	char *end;
	unsigned long long v;

	if (!s || !*s || !out)
		return -EINVAL;

	v = simple_strtoull(s, &end, 0);
	if (end == s)
		return -EINVAL;

	while (*end == ' ' || *end == '\t')
		end++;

	if (!*end) {
		*out = (u64)v;
		return 0;
	}

	if (!strcasecmp(end, "k") || !strcasecmp(end, "kb") ||
	    !strcasecmp(end, "kib")) {
		*out = (u64)v * 1024ULL;
		return 0;
	}

	return -EINVAL;
}

enum backup_phase {
	BACKUP_PHASE_HDR = 0,
	BACKUP_PHASE_DATA,
};

struct backup_session {
	enum backup_phase phase;
	struct mtd_info *mtd;
	u64 start;
	u64 end;
	u64 cur;
	u64 total;
	char hdr[512];
	int hdr_len;
	u8 *buf;
	size_t buf_size;
};

static void backup_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
#ifndef CONFIG_MTD
	if (status != HTTP_CB_NEW)
		return;

	response->status = HTTP_RESP_STD;
	response->data = "backup not supported";
	response->size = strlen(response->data);
	response->info.code = 503;
	response->info.connection_close = 1;
	response->info.content_type = "text/plain";
	return;
#else
	struct backup_session *st;
	struct httpd_form_value *mode, *target, *start, *end;
	const char *tgt;
	const char *part;
	char filename[128];
	size_t want;
	size_t retlen = 0;
	int ret;

	if (status == HTTP_CB_NEW) {
		mode = httpd_request_find_value(request, "mode");
		target = httpd_request_find_value(request, "target");

		if (!mode || !mode->data) {
			response->status = HTTP_RESP_STD;
			response->data = "bad request";
			response->size = strlen(response->data);
			response->info.code = 400;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		if (!target || !target->data) {
			response->status = HTTP_RESP_STD;
			response->data = "bad request";
			response->size = strlen(response->data);
			response->info.code = 400;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		tgt = target->data;
		/* Full-disk via explicit device name */
		if (!strncmp(tgt, "mtddev:", 7)) {
			const char *devname = tgt + 7;

			if (!devname[0]) {
				response->status = HTTP_RESP_STD;
				response->data = "bad request";
				response->size = strlen(response->data);
				response->info.code = 400;
				response->info.connection_close = 1;
				response->info.content_type = "text/plain";
				return;
			}

			st = calloc(1, sizeof(*st));
			if (!st) {
				response->status = HTTP_RESP_STD;
				response->data = "oom";
				response->size = strlen(response->data);
				response->info.code = 500;
				response->info.connection_close = 1;
				response->info.content_type = "text/plain";
				return;
			}

			st->buf_size = 4096;
			st->buf = malloc(st->buf_size);
			if (!st->buf) {
				free(st);
				response->status = HTTP_RESP_STD;
				response->data = "oom";
				response->size = strlen(response->data);
				response->info.code = 500;
				response->info.connection_close = 1;
				response->info.content_type = "text/plain";
				return;
			}

			st->mtd = get_mtd_device_nm(devname);
			if (IS_ERR(st->mtd)) {
				free(st->buf);
				free(st);
				response->status = HTTP_RESP_STD;
				response->data = "mtd not found";
				response->size = strlen(response->data);
				response->info.code = 404;
				response->info.connection_close = 1;
				response->info.content_type = "text/plain";
				return;
			}

			st->start = 0;
			st->end = st->mtd->size;

			if (!strcmp(mode->data, "range")) {
				u64 rs = 0, re = 0;
				start = httpd_request_find_value(request, "start");
				end = httpd_request_find_value(request, "end");
				if (!start || !start->data || !end || !end->data ||
				    parse_u64_len(start->data, &rs) ||
				    parse_u64_len(end->data, &re) ||
				    re <= rs || re > st->mtd->size) {
					put_mtd_device(st->mtd);
					free(st->buf);
					free(st);
					response->status = HTTP_RESP_STD;
					response->data = "bad range";
					response->size = strlen(response->data);
					response->info.code = 400;
					response->info.connection_close = 1;
					response->info.content_type = "text/plain";
					return;
				}
				st->start = rs;
				st->end = re;
			}

			st->cur = st->start;
			st->total = st->end - st->start;
			st->phase = BACKUP_PHASE_HDR;

			snprintf(filename, sizeof(filename), "backup_%s_0x%llx-0x%llx.bin",
				 devname,
				 (unsigned long long)st->start,
				 (unsigned long long)st->end);
			str_sanitize_component(filename);

			st->hdr_len = snprintf(st->hdr, sizeof(st->hdr),
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: application/octet-stream\r\n"
				"Content-Length: %llu\r\n"
				"Content-Disposition: attachment; filename=\"%s\"\r\n"
				"Connection: close\r\n"
				"\r\n",
				(unsigned long long)st->total,
				filename);

			response->session_data = st;
			response->status = HTTP_RESP_CUSTOM;
			response->data = st->hdr;
			response->size = st->hdr_len;
			return;
		}

		if (strncmp(tgt, "mtd:", 4)) {
			response->status = HTTP_RESP_STD;
			response->data = "unsupported target";
			response->size = strlen(response->data);
			response->info.code = 400;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		part = tgt + 4;
		if (!part[0]) {
			response->status = HTTP_RESP_STD;
			response->data = "no part";
			response->size = strlen(response->data);
			response->info.code = 400;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		st = calloc(1, sizeof(*st));
		if (!st) {
			response->status = HTTP_RESP_STD;
			response->data = "oom";
			response->size = strlen(response->data);
			response->info.code = 500;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		st->buf_size = 4096;
		st->buf = malloc(st->buf_size);
		if (!st->buf) {
			free(st);
			response->status = HTTP_RESP_STD;
			response->data = "oom";
			response->size = strlen(response->data);
			response->info.code = 500;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		/* Prefer mtdparts lookup to support nmbm/nand/nor + alias ids */
#ifdef CONFIG_CMD_MTDPARTS
		{
			struct mtd_device *pdev;
			struct part_info *pinfo;
			u8 pnum;
			char master_name[32];
			u64 part_off = 0, part_size = 0;
			u64 rel_start = 0, rel_end = 0;

			if (mtdparts_init() == 0 &&
			    find_dev_and_part(part, &pdev, &pnum, &pinfo) == 0 &&
			    pdev && pdev->id && pinfo) {
				snprintf(master_name, sizeof(master_name), "%s%d",
					 MTD_DEV_TYPE(pdev->id->type), pdev->id->num);
				st->mtd = get_mtd_device_nm(master_name);
				if (IS_ERR(st->mtd))
					st->mtd = NULL;

				part_off = pinfo->offset;
				part_size = pinfo->size;
			} else {
				master_name[0] = '\0';
				st->mtd = NULL;
			}

			if (st->mtd) {
				st->start = part_off;
				st->end = part_off + part_size;

				if (!strcmp(mode->data, "range")) {
					start = httpd_request_find_value(request, "start");
					end = httpd_request_find_value(request, "end");
					if (!start || !start->data || !end || !end->data ||
					    parse_u64_len(start->data, &rel_start) ||
					    parse_u64_len(end->data, &rel_end) ||
					    rel_end <= rel_start || rel_end > part_size) {
						put_mtd_device(st->mtd);
						free(st->buf);
						free(st);
						response->status = HTTP_RESP_STD;
						response->data = "bad range";
						response->size = strlen(response->data);
						response->info.code = 400;
						response->info.connection_close = 1;
						response->info.content_type = "text/plain";
						return;
					}
					st->start = part_off + rel_start;
					st->end = part_off + rel_end;
				}

				st->cur = st->start;
				st->total = st->end - st->start;
				st->phase = BACKUP_PHASE_HDR;

				snprintf(filename, sizeof(filename), "backup_%s_0x%llx-0x%llx.bin",
					 part,
					 (unsigned long long)(st->start - part_off),
					 (unsigned long long)(st->end - part_off));
				str_sanitize_component(filename);

				st->hdr_len = snprintf(st->hdr, sizeof(st->hdr),
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/octet-stream\r\n"
					"Content-Length: %llu\r\n"
					"Content-Disposition: attachment; filename=\"%s\"\r\n"
					"Connection: close\r\n"
					"\r\n",
					(unsigned long long)st->total,
					filename);

				response->session_data = st;
				response->status = HTTP_RESP_CUSTOM;
				response->data = st->hdr;
				response->size = st->hdr_len;
				return;
			}
		}
#endif

		/* Fallback: old behavior (may fail on platforms without partition mtd_info) */
		st->mtd = get_mtd_device_nm(part);
		if (IS_ERR(st->mtd)) {
			free(st->buf);
			free(st);
			response->status = HTTP_RESP_STD;
			response->data = "mtd not found";
			response->size = strlen(response->data);
			response->info.code = 404;
			response->info.connection_close = 1;
			response->info.content_type = "text/plain";
			return;
		}

		st->start = 0;
		st->end = st->mtd->size;

		if (!strcmp(mode->data, "range")) {
			start = httpd_request_find_value(request, "start");
			end = httpd_request_find_value(request, "end");
			if (!start || !start->data || !end || !end->data ||
			    parse_u64_len(start->data, &st->start) ||
			    parse_u64_len(end->data, &st->end) ||
			    st->end <= st->start || st->end > st->mtd->size) {
				put_mtd_device(st->mtd);
				free(st->buf);
				free(st);
				response->status = HTTP_RESP_STD;
				response->data = "bad range";
				response->size = strlen(response->data);
				response->info.code = 400;
				response->info.connection_close = 1;
				response->info.content_type = "text/plain";
				return;
			}
		}

		st->cur = st->start;
		st->total = st->end - st->start;
		st->phase = BACKUP_PHASE_HDR;

		snprintf(filename, sizeof(filename), "backup_%s_0x%llx-0x%llx.bin",
			 part,
			 (unsigned long long)st->start,
			 (unsigned long long)st->end);
		str_sanitize_component(filename);

		st->hdr_len = snprintf(st->hdr, sizeof(st->hdr),
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/octet-stream\r\n"
			"Content-Length: %llu\r\n"
			"Content-Disposition: attachment; filename=\"%s\"\r\n"
			"Connection: close\r\n"
			"\r\n",
			(unsigned long long)st->total,
			filename);

		response->session_data = st;
		response->status = HTTP_RESP_CUSTOM;
		response->data = st->hdr;
		response->size = st->hdr_len;
		return;
	}

	if (status == HTTP_CB_RESPONDING) {
		st = response->session_data;
		if (!st)
			return;

		if (st->phase == BACKUP_PHASE_HDR)
			st->phase = BACKUP_PHASE_DATA;

		if (st->cur >= st->end) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		want = st->buf_size;
		if (want > (size_t)(st->end - st->cur))
			want = (size_t)(st->end - st->cur);

		ret = mtd_read(st->mtd, st->cur, want, &retlen, st->buf);
		if (ret || !retlen) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		st->cur += retlen;
		response->status = HTTP_RESP_CUSTOM;
		response->data = (const char *)st->buf;
		response->size = retlen;
		return;
	}

	if (status == HTTP_CB_CLOSED) {
		st = response->session_data;
		if (!st)
			return;

		if (st->mtd && !IS_ERR(st->mtd))
			put_mtd_device(st->mtd);

		free(st->buf);
		free(st);
		response->session_data = NULL;
	}
#endif
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
			case FAILSAFE_FW_FACTORY:
				ret = write_factory_failsafe((size_t)upload_data,
					upload_size);
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
	httpd_register_uri_handler(inst, "/sysinfo", &sysinfo_handler, NULL);
	httpd_register_uri_handler(inst, "/backupinfo", &backupinfo_handler, NULL);
	httpd_register_uri_handler(inst, "/backup", &backup_handler, NULL);
	httpd_register_uri_handler(inst, "/reboot", &reboot_handler, NULL);

	httpd_register_uri_handler(inst, "/main.js", &js_handler, NULL);
	httpd_register_uri_handler(inst, "/style.css", &style_handler, NULL);

	httpd_register_uri_handler(inst, "/booting.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/fail.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/flashing.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/factory.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/initramfs.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/uboot.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/backup.html", &html_handler, NULL);
	httpd_register_uri_handler(inst, "/reboot.html", &html_handler, NULL);

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
