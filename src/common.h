/*
 * Copyright (C) 2016 Inteno Broadband Technology AB
 *
 * This software is the confidential and proprietary information of the
 * Inteno Broadband Technology AB. You shall not disclose such Confidential
 * Information and shall use it only in accordance with the terms of the
 * license agreement you entered into with the Inteno Broadband Technology AB
 *
 * All rights reserved.
 *
 * Author: Denis Osvald <denis.osvald@sartura.hr>
 *
 */
#pragma once
#include <stddef.h>

#include <libubus.h>

struct prog_context {
	struct uloop_fd **ufds;
	size_t num_ufds;

	struct uloop_timeout utimer;

	struct lws_context *lws_ctx;

	struct ubus_context *ubus_ctx;

	struct origin *origin_list;

	const char *www_path;
};

struct origin {
	struct list_head list;
	char *url;
};
