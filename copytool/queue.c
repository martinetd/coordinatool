/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <search.h>

#include "coordinatool.h"

void hsm_action_queues_init(struct state *state,
			    struct hsm_action_queues *queues) {
	CDS_INIT_LIST_HEAD(&queues->waiting_restore);
	CDS_INIT_LIST_HEAD(&queues->waiting_archive);
	CDS_INIT_LIST_HEAD(&queues->waiting_remove);
	queues->actions_tree = NULL;
	queues->state = state;
}

static int tree_compare(const void *a, const void *b) {
	__u64 va = *(__u64*)a, vb = *(__u64*)b;

	if (va < vb)
		return -1;
	else if (va == vb)
		return 0;
	return 1;
}

void queue_node_free(struct hsm_action_node *node) {
	if (!tdelete(&node->hai.hai_cookie, &node->queues->actions_tree,
		     tree_compare))
		abort();
	free(node);
}

struct hsm_action_queues *hsm_action_queues_get(struct state *state,
						unsigned int archive_id,
						unsigned long long flags,
						const char *fsname) {
	if (archive_id == ARCHIVE_ID_UNINIT) {
		LOG_ERROR(-EINVAL, "No archive_id given");
		return NULL;
	}

	if (state->queues.archive_id == ARCHIVE_ID_UNINIT) {
		if (!fsname) {
			LOG_ERROR(-EINVAL, "Tried to get queues for %d before it was set",
				  archive_id);
			return NULL;
		}
		// XXX we only support one archive_id, first one we sees
		// determines what others should be.
		state->queues.archive_id = archive_id;
		state->queues.fsname = strdup(fsname);
		state->queues.hal_flags = flags;
	} else {
		if (state->queues.archive_id != archive_id) {
			LOG_ERROR(-EINVAL, "received action list for archive id %d but already seen %d, ignoring these",
				  archive_id, state->queues.archive_id);
			return NULL;
		}
		if (fsname) {
			// XXX strcmp fsname?
			if (state->queues.hal_flags != flags) {
				LOG_ERROR(-EINVAL, "received action list with different flags (got %llx, expected %llx); keeping current flags anyway",
					  flags, state->queues.hal_flags);
				return NULL;
			}
		}
	}

	return &state->queues;
}

struct hsm_action_node *hsm_action_search_queue(struct hsm_action_queues *queues,
						unsigned long cookie,
						bool pop) {
	void *key = tfind(&cookie, &queues->actions_tree, tree_compare);

	if (!key)
		return NULL;

	key = *(void **)key;
	if (pop)
		tdelete(&cookie, &queues->actions_tree, tree_compare);

	struct hsm_action_item *hai =
		caa_container_of(key, struct hsm_action_item, hai_cookie);
	return caa_container_of(hai, struct hsm_action_node, hai);
}

int hsm_action_requeue(struct hsm_action_node *node) {
	struct cds_list_head *head;
	struct hsm_action_queues *queues = node->queues;

	switch (node->hai.hai_action) {
	case HSMA_RESTORE:
		head = &queues->waiting_restore;
		break;
	case HSMA_ARCHIVE:
		head = &queues->waiting_archive;
		break;
	case HSMA_REMOVE:
		head = &queues->waiting_remove;
		break;
	default:
		queue_node_free(node);
		return -EINVAL;
	}

	struct cds_list_head *n;
	cds_list_for_each(n, &queues->state->waiting_clients) {
		struct client *client = caa_container_of(n, struct client,
							 node_waiting);
		if (protocol_reply_recv_single(client, queues, node) == 0) {
			cds_list_del_init(n);
			return 1;
		}
	}

	cds_list_add_tail(&node->node, head);
	return 1;
}

int hsm_action_enqueue(struct hsm_action_queues *queues,
		       struct hsm_action_item *hai) {
	struct hsm_action_node *node;
	__u64 **tree_key;

	if (hai->hai_action == HSMA_CANCEL) {
		// XXX tfind + remove from waiting queue or signal client
		return 0;
	}
	switch (hai->hai_action) {
	case HSMA_RESTORE:
	case HSMA_ARCHIVE:
	case HSMA_REMOVE:
		break;
	default:
		return -EINVAL;
	}

	node = malloc(sizeof(struct hsm_action_node) +
		      hai->hai_len - sizeof(struct hsm_action_item));
	if (!node) {
		abort();
	}
	memcpy(&node->hai, hai, hai->hai_len);
	node->queues = queues;

	tree_key = tsearch(&node->hai.hai_cookie, &queues->actions_tree,
			   tree_compare);
	if (!tree_key)
		abort();
	if (*tree_key != &node->hai.hai_cookie) {
		/* duplicate */
		free(node);
		return 0;
	}

	return hsm_action_requeue(node);
}

struct hsm_action_node *hsm_action_dequeue(struct hsm_action_queues *queues,
					   enum hsm_copytool_action action) {
	struct cds_list_head *node = NULL;

	switch (action) {
	case HSMA_RESTORE:
		if (cds_list_empty(&queues->waiting_restore))
			return NULL;
		node = queues->waiting_restore.next;
		break;
	case HSMA_ARCHIVE:
		if (cds_list_empty(&queues->waiting_archive))
			return NULL;
		node = queues->waiting_archive.next;
		break;
	case HSMA_REMOVE:
		if (cds_list_empty(&queues->waiting_remove))
			return NULL;
		node = queues->waiting_remove.next;
		break;
	default:
		LOG_ERROR(-EINVAL, "requested to dequeue %s",
			  ct_action2str(action));
		return NULL;
	}

	cds_list_del(node);
	return caa_container_of(node, struct hsm_action_node, node);
}
