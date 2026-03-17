/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project uboot-mt7621-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Internal interfaces for Failsafe Web UI modules
 */
#ifndef __FAILSAFE_INTERNAL_H
#define __FAILSAFE_INTERNAL_H

#include <net/httpd.h>

void sysinfo_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

void backupinfo_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

void backup_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response);

#endif /* __FAILSAFE_INTERNAL_H */
