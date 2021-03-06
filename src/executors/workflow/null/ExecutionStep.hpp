/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019 Barcelona Supercomputing Center (BSC)
*/

#ifndef EXECUTION_STEP_HPP
#define EXECUTION_STEP_HPP

#include <DataAccessRegion.hpp>
#include "dependencies/DataAccessType.hpp"

struct DataAccess;
class MemoryPlace;
typedef size_t WriteID;

namespace ExecutionWorkflow {

	class Step {
	public:
		Step()
		{
		}

		~Step()
		{
		}

		inline void addPredecessor()
		{
		}

		inline void addSuccessor(__attribute__((unused))Step *step)
		{
		}

		inline bool release()
		{
			return true;
		}

		inline void releaseSuccessors()
		{
		}

		inline bool ready() const
		{
			return true;
		}

		inline void start()
		{
		}
	};

	class DataLinkStep : public Step {
	public:
		DataLinkStep(__attribute__((unused))DataAccess const *access)
			: Step()
		{
		}

		inline void linkRegion(
			__attribute__((unused))DataAccessRegion const &region,
			__attribute__((unused))MemoryPlace const *location,
			__attribute__((unused))WriteID const writeID,
			__attribute__((unused))bool read,
			__attribute__((unused))bool write
		) {
		}
	};

	class DataReleaseStep : public Step {
	public:
		DataReleaseStep(__attribute__((unused))DataAccess const *access)
			: Step()
		{
		}

		inline void releaseRegion(
			__attribute__((unused))DataAccessRegion const &region,
			__attribute__((unused))WriteID writeID,
			__attribute__((unused))MemoryPlace const *location
		) {
		}

		inline bool checkDataRelease(__attribute__((unused))DataAccess const *access)
		{
			return false;
		}
	};
}

#endif /* EXECUTION_STEP_HPP */
