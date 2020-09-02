/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2020 Barcelona Supercomputing Center (BSC)
*/

#ifndef CLUSTER_HYBRID_INTERFACE_FILE_HPP
#define CLUSTER_HYBRID_INTERFACE_FILE_HPP

#include <fstream>
#include "ClusterHybridInterface.hpp"

class ClusterHybridInterfaceFile : public ClusterHybridInterface {

	private:
		struct timespec _prevTime;
		const char *_directory;
		const char *_allocFileThisApprank;
		std::ofstream _utilizationFile;

		static void readTime(struct timespec *pt)
		{
			int rc = clock_gettime(CLOCK_MONOTONIC, pt);
			FatalErrorHandler::failIf(rc != 0,
									  "Error reading time: ",
									  strerror(errno));
		}

		// Return value is whether the allocation changed
		bool updateNumbersOfCores(void);

		void appendUtilization(float timestamp, float busy_cores);

	public:
		ClusterHybridInterfaceFile();

		~ClusterHybridInterfaceFile();

		void initialize(int externalRank,
						int apprankNum,
						int internalRank,
						int nodeNum,
						int indexThisNode);

		void poll(void);

		// // Send the utilization
		// void sendUtilization(float ncores);
};

//! Register ClusterHybridInterfaceFile with the object factory
namespace
{
	ClusterHybridInterface *createInterfaceFile() { return new ClusterHybridInterfaceFile; }

	const bool __attribute__((unused))_registered_file_hyb =
		REGISTER_HYBIF_CLASS("hybrid-file-interface", createInterfaceFile);
}

#endif /* CLUSTER_HYBRID_INTERFACE_FILE_HPP */
