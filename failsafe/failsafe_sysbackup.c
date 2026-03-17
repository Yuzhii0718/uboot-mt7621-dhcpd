/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project uboot-mt7621-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe HTTP handlers for system backup and info display.
 */

#include <common.h>
#include <errno.h>
#include <linux/libfdt.h>
#include <linux/ctype.h>
#include <malloc.h>
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

#include "failsafe_internal.h"

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_MTD_NAND
static void failsafe_trim_ascii(char *dst, size_t dst_sz, const u8 *src, size_t src_sz)
{
	size_t i, end;

	if (!dst || !dst_sz)
		return;

	dst[0] = '\0';
	if (!src || !src_sz)
		return;

	end = src_sz;
	while (end && (src[end - 1] == ' ' || src[end - 1] == '\0'))
		end--;

	for (i = 0; i < end && i + 1 < dst_sz; i++)
		dst[i] = (char)src[i];
	dst[i] = '\0';
}

static void failsafe_get_nand_model(struct nand_chip *chip, char *out, size_t out_sz)
{
	char manuf[13];
	char model[21];

	if (!out || !out_sz)
		return;

	out[0] = '\0';

	if (!chip || memcmp(chip->onfi_params.sig, "ONFI", 4))
		return;

	failsafe_trim_ascii(manuf, sizeof(manuf),
		(const u8 *)chip->onfi_params.manufacturer,
		sizeof(chip->onfi_params.manufacturer));
	failsafe_trim_ascii(model, sizeof(model),
		(const u8 *)chip->onfi_params.model,
		sizeof(chip->onfi_params.model));

	if (manuf[0] && model[0])
		snprintf(out, out_sz, "%s %s", manuf, model);
	else if (model[0])
		strlcpy(out, model, out_sz);
	else if (manuf[0])
		strlcpy(out, manuf, out_sz);
}
#endif

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
	if (s && s[0]) {
		if (!strncmp(s, "mtdparts=", 9))
			return s + 9;
		return s;
	}

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
	if (s && s[0]) {
		if (!strncmp(s, "mtdids=", 7))
			return s + 7;
		return s;
	}

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
		if (!eq || (comma && eq > comma))
			break;

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

void sysinfo_handler(enum httpd_uri_handler_status status,
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
		if (dt_compat && l > 0)
			board_compat = dt_compat;
		else
			board_compat = "";
	} else {
		board_compat = "";
	}

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
		static const char *names[] = { "nmbm0", "nand0", "nor0", "onenand0" };
		size_t i;

		for (i = 0; i < ARRAY_SIZE(names); i++) {
			struct mtd_info *probe = get_mtd_device_nm(names[i]);

			if (IS_ERR(probe))
				continue;

			strlcpy(master_name, names[i], sizeof(master_name));
			put_mtd_device(probe);
			break;
		}
	}

	mtd = get_mtd_device_nm(master_name);
	if (!IS_ERR(mtd)) {
		int master_is_nmbm = !strncmp(master_name, "nmbm", 4);

		json_escape(esc_raw_name, sizeof(esc_raw_name), "");
		json_escape(esc_raw_model, sizeof(esc_raw_model), "");
		raw_size = 0;

		json_escape(esc_master_name, sizeof(esc_master_name), master_name);
		flash_model[0] = '\0';
		if (mtd->type == MTD_NORFLASH) {
			struct spi_flash *sf = (struct spi_flash *)mtd->priv;
			if (sf && sf->name)
				strlcpy(flash_model, sf->name, sizeof(flash_model));
		}
#ifdef CONFIG_MTD_NAND
		if (!flash_model[0] && !strncmp(master_name, "nand", 4) &&
		    nand_mtd_to_devnum(mtd) >= 0) {
			struct nand_chip *chip = mtd_to_nand(mtd);

			failsafe_get_nand_model(chip, flash_model, sizeof(flash_model));
			if (!flash_model[0] && mtd->name && mtd->name[0])
				strlcpy(flash_model, mtd->name, sizeof(flash_model));
		}

		if (master_is_nmbm) {
			struct mtd_info *lower;
			struct nand_chip *chip;
			u64 logical_size = (u64)mtd->size;

			lower = get_mtd_device_nm("nand0");
			if (!IS_ERR(lower) && nand_mtd_to_devnum(lower) >= 0) {
				/* ok */
			} else {
				if (!IS_ERR(lower))
					put_mtd_device(lower);
				lower = NULL;

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

						if (nand_mtd_to_devnum(cand) < 0 &&
						    !(cand->name && !strncmp(cand->name, "nand", 4))) {
							put_mtd_device(cand);
							continue;
						}

						if (cand->name && !strncmp(cand->name, "nmbm", 4)) {
							put_mtd_device(cand);
							continue;
						}

						if ((u64)cand->size <= logical_size) {
							put_mtd_device(cand);
							continue;
						}

						cand_model[0] = '\0';
						chip = mtd_to_nand(cand);
						failsafe_get_nand_model(chip, cand_model, sizeof(cand_model));

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
						json_escape(esc_raw_name, sizeof(esc_raw_name), best_name);
						json_escape(esc_raw_model, sizeof(esc_raw_model), best_model);
						raw_size = best_size;
					}
				}
			}

			if (lower && !IS_ERR(lower) && nand_mtd_to_devnum(lower) >= 0) {
				chip = mtd_to_nand(lower);
				failsafe_get_nand_model(chip, flash_model, sizeof(flash_model));
				if (!flash_model[0] && lower->name && lower->name[0])
					strlcpy(flash_model, lower->name, sizeof(flash_model));

				json_escape(esc_raw_name, sizeof(esc_raw_name), lower->name ? lower->name : "nand");
				raw_size = (u64)lower->size;
				{
					char raw_model[128];
					raw_model[0] = '\0';
					failsafe_get_nand_model(chip, raw_model, sizeof(raw_model));
					if (!raw_model[0] && lower->name && lower->name[0])
						strlcpy(raw_model, lower->name, sizeof(raw_model));
					json_escape(esc_raw_model, sizeof(esc_raw_model), raw_model);
				}
				put_mtd_device(lower);
			}
		}
#endif

		if (master_is_nmbm && !raw_size) {
			u64 logical_size = (u64)mtd->size;
			struct mtd_info *lower;

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

					if (!strncmp(cand->name, "nmbm", 4)) {
						put_mtd_device(cand);
						continue;
					}

					if (strncmp(cand->name, "nand", 4)) {
						put_mtd_device(cand);
						continue;
					}

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

void backupinfo_handler(enum httpd_uri_handler_status status,
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

static void backup_set_std_response(struct httpd_response *response,
	int code, const char *msg)
{
	const char *text = msg ? msg : "error";

	response->status = HTTP_RESP_STD;
	response->data = text;
	response->size = strlen(text);
	response->info.code = code;
	response->info.connection_close = 1;
	response->info.content_type = "text/plain";
}

static struct backup_session *backup_alloc_session(void)
{
	struct backup_session *st;

	st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	st->buf_size = 4096;
	st->buf = malloc(st->buf_size);
	if (!st->buf) {
		free(st);
		return NULL;
	}

	return st;
}

static void backup_free_session(struct backup_session *st)
{
	if (!st)
		return;

	if (st->mtd && !IS_ERR(st->mtd))
		put_mtd_device(st->mtd);

	free(st->buf);
	free(st);
}

void backup_handler(enum httpd_uri_handler_status status,
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
			backup_set_std_response(response, 400, "bad request");
			return;
		}

		if (!target || !target->data) {
			backup_set_std_response(response, 400, "bad request");
			return;
		}

		tgt = target->data;
		if (!strncmp(tgt, "mtddev:", 7)) {
			const char *devname = tgt + 7;

			if (!devname[0]) {
				backup_set_std_response(response, 400, "bad request");
				return;
			}

			st = backup_alloc_session();
			if (!st) {
				backup_set_std_response(response, 500, "oom");
				return;
			}

			st->mtd = get_mtd_device_nm(devname);
			if (IS_ERR(st->mtd)) {
				backup_free_session(st);
				backup_set_std_response(response, 404, "mtd not found");
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
					backup_free_session(st);
					backup_set_std_response(response, 400, "bad range");
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
			backup_set_std_response(response, 400, "unsupported target");
			return;
		}

		part = tgt + 4;
		if (!part[0]) {
			backup_set_std_response(response, 400, "no part");
			return;
		}

		st = backup_alloc_session();
		if (!st) {
			backup_set_std_response(response, 500, "oom");
			return;
		}

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
						backup_free_session(st);
						backup_set_std_response(response, 400, "bad range");
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

		st->mtd = get_mtd_device_nm(part);
		if (IS_ERR(st->mtd)) {
			backup_free_session(st);
			backup_set_std_response(response, 404, "mtd not found");
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
				backup_free_session(st);
				backup_set_std_response(response, 400, "bad range");
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

		backup_free_session(st);
		response->session_data = NULL;
	}
#endif
}
