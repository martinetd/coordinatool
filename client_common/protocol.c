/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client_common.h"


static int protocol_write_lock(json_t *request, const struct ct_state *state,
			int flags) {
	// XXX lock
	return protocol_write(request, state->socket_fd, flags);
}

int protocol_request_status(const struct ct_state *state) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss}", "command", "status");
	if (!request) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not pack status request");
		return rc;
	}

	LOG_INFO("Sending status request to %d", state->socket_fd);
	if (protocol_write_lock(request, state, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write status request");
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_recv(const struct ct_state *state) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss,si,si,si,si,si}",
			    "command", "recv",
			    "max_archive", state->config.max_archive,
			    "max_restore", state->config.max_restore,
			    "max_remove", state->config.max_remove,
			    "max_bytes", state->config.hsm_action_list_size,
			    "archive_id", state->config.archive_id);
	if (!request) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not pack recv request");
		return rc;
	}
	LOG_INFO("Sending recv request to %d", state->socket_fd);
	if (protocol_write_lock(request, state, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write recv request");
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_done(const struct ct_state *state, uint32_t archive_id,
			  uint64_t cookie, int status) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss,si,si,si}",
			    "command", "done",
			    "archive_id", archive_id,
			    "cookie", cookie,
			    "status", status);
	if (!request) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not pack recv request");
		return rc;
	}
	LOG_INFO("Sending done request to %d", state->socket_fd);
	if (protocol_write_lock(request, state, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write done request");
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_queue(const struct ct_state *state,
			   uint32_t archive_id, uint64_t flags,
			   json_t *hai_list) {
	int rc;

	json_t *hal = json_object();
	if (!hal)
		abort();
	if ((rc = protocol_setjson_int(hal, "hal_version", HAL_VERSION)) ||
	    (rc = protocol_setjson_int(hal, "hal_count", json_array_size(hai_list))) ||
	    (rc = protocol_setjson_int(hal, "hal_archive_id", archive_id)) ||
	    (rc = protocol_setjson_int(hal, "hal_flags", flags)) ||
	    (rc = protocol_setjson_str(hal, "hal_fsname", state->fsname))) {
		LOG_ERROR(rc, "Could not fill hsm action list");
		json_decref(hal);
		return rc;
	}
	if ((rc = protocol_setjson(hal, "list", hai_list)))
		return rc;

	json_t *request = json_object();
	if (!request)
		abort();

	if ((rc = protocol_setjson(request, "hsm_action_list", hal)) ||
	    (rc = protocol_setjson_str(request, "command", "queue")))
		goto out_free;

	LOG_INFO("Sending queue request to %d", state->socket_fd);
	if (protocol_write_lock(request, state, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write queue request");
		goto out_free;
	}
out_free:
	json_decref(request);
	return rc;
}
