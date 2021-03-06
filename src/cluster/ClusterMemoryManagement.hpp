/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019 Barcelona Supercomputing Center (BSC)
*/

#ifndef CLUSTER_MEMORY_MANAGEMENT_HPP
#define CLUSTER_MEMORY_MANAGEMENT_HPP

#include <nanos6/cluster.h>

class MessageDmalloc;
class MessageDfree;

namespace ClusterMemoryManagement {

	void registerDmalloc(
		DataAccessRegion const &region,
		nanos6_data_distribution_t policy,
		size_t nrDimensions,
		size_t *dimensions,
		Task *task);

	bool unregisterDmalloc(DataAccessRegion const &region);

	void redistributeDmallocs(void);

	void handleDmallocMessage(const MessageDmalloc *msg, Task *task);

	void *dmalloc(
		size_t size,
		nanos6_data_distribution_t policy,
		size_t numDimensions,
		size_t *dimensions
	);

	void handleDfreeMessage(const MessageDfree *msg);

	void dfree(void *ptr, size_t size);

	void *lmalloc(size_t size);

	void lfree(void *ptr, size_t size);
}

#endif /* CLUSTER_MEMORY_MANAGEMENT_HPP */
