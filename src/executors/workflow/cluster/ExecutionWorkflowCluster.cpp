/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019-2020 Barcelona Supercomputing Center (BSC)
*/

#include "ExecutionWorkflowCluster.hpp"
#include "tasks/Task.hpp"

#include "lowlevel/SpinLock.hpp"
#include <ClusterManager.hpp>
#include <ClusterServicesPolling.hpp>
#include <ClusterServicesTask.hpp>
#include <DataAccess.hpp>
#include <DataAccessRegistration.hpp>
#include <DataTransfer.hpp>
#include <Directory.hpp>
#include <InstrumentLogMessage.hpp>
#include <TaskOffloading.hpp>
#include "InstrumentCluster.hpp"

namespace ExecutionWorkflow {

	void ClusterDataLinkStep::linkRegion(
		DataAccess const *access,
		bool read,
		bool write,
		TaskOffloading::SatisfiabilityInfoMap &satisfiabilityMap
	) {
		assert(access != nullptr);
		assert(_task->getClusterContext() != nullptr);
		const DataAccessRegion &region = access->getAccessRegion();
		const MemoryPlace *location = access->getLocation();
		const WriteID writeID = access->getWriteID();

		bool deleteStep = false;
		// This function is occasionally called after creating the
		// ClusterDataLinkStep (whose constructor sets the data link
		// step) but before starting the ClusterDataLinkStep. A lock
		// is required because both functions manipulate _bytesToLink 
		// and delete the workflow when it reaches zero.
		{
			std::lock_guard<SpinLock> guard(_lock);
			assert(_targetMemoryPlace != nullptr);

			int locationIndex;
			if (location == nullptr) {
				// The location is only nullptr when write satisfiability
				// is propagated before read satisfiability, which happens
				// very rarely. In this case, we send -1 as the location
				// index.
				assert(write);
				assert(!read);
				locationIndex = -1; // means nullptr
			} else {
				if (!location->isDirectoryMemoryPlace()
					&& location->getType() != nanos6_cluster_device) {
					location = ClusterManager::getCurrentMemoryNode();
				}
				locationIndex = location->getIndex();
			}

			// namespacePredecessor is in principle irrelevant, because it only matters when the
			// task is created, not when a satisfiability message is sent (which is what is
			// happening now). Nevertheless, this only happens when propagation does not happen
			// in the namespace; so send the value nullptr.
			ClusterNode *destNode = _task->getClusterContext()->getRemoteNode();

			satisfiabilityMap[destNode].push_back(
				TaskOffloading::SatisfiabilityInfo(
					region, locationIndex,
					read, write,
					writeID, _task)
			);

			size_t linkedBytes = region.getSize();

			//! We need to account for linking both read and write satisfiability
			if (read && write) {
				linkedBytes *= 2;
			}
			_bytesToLink -= linkedBytes;
			if (_started && _bytesToLink  == 0) {
				deleteStep = true;
			}
		}
		if (deleteStep) {

			/*
			 * If two tasks A and B are offloaded to the same namespace,
			 * and A has an in dependency, then at the remote side, read
			 * satisfiability is propagated from A to B both via the
			 * offloader's dependency system and via remote propagation in
			 * the remote namespace (this is the normal optimization from a
			 * namespace). So task B receives read satisfiability twice.
			 * This is harmless.  TODO: It would be good to add an
			 * assertion to make sure that read satisfiability arrives
			 * twice only in the circumstance described here, but we can no
			 * longer check what type of dependency the access of task A
			 * had (in fact the access may already have been deleted). This
			 * info would have to be added to the UpdateOperation.
			 *
			 * This unfortunately messes up the counting of linked bytes.
			 * The master should be able to figure out when this happens
			 * and update its own _bytesToLink value accordingly. Or
			 * maybe there is a way to avoid this happening.
			 */
			// delete this;
		}
	}

	void ClusterDataLinkStep::start()
	{
		bool deleteStep = false;
		{
			// Get a lock (see comment in ClusterDataLinkStep::linkRegion).
			std::lock_guard<SpinLock> guard(_lock);
			assert(_targetMemoryPlace != nullptr);

			int location = -1;
			if (_read || _write) {
				assert(_sourceMemoryPlace != nullptr);
				location = _sourceMemoryPlace->getIndex();
			}
			Instrument::logMessage(
					Instrument::ThreadInstrumentationContext::getCurrent(),
					"ClusterDataLinkStep for MessageTaskNew. ",
					"Current location of ", _region,
					" Node:", location
			);

			//! The current node is the source node. We just propagate
			//! the info we 've gathered
			assert(_successors.size() == 1);
			ClusterExecutionStep *execStep = dynamic_cast<ClusterExecutionStep *>(_successors[0]);
			assert(execStep != nullptr); // This asserts that the next step is the execution step

			// assert(_read || _write);

			// This assert is to prevent future errors when working with maleability.
			// For now the index and the comIndex are the same. A more complete implementation
			// Will have a pointer in ClusterMemoryNode to the ClusterNode and will get the
			// Commindex from there with getCommIndex.
			// assert(_sourceMemoryPlace->getIndex() == _sourceMemoryPlace->getCommIndex());
			execStep->addDataLink(
				location, _region,
				_writeID,
				_read, _write,
				(void *)_namespacePredecessor
			);

			const size_t linkedBytes = _region.getSize();
			//! If at the moment of offloading the access is not both
			//! read and write satisfied, then the info will be linked
			//! later on. In this case, we just account for the bytes that
			//! we link now, the Step will be deleted when all the bytes
			//! are linked through linkRegion method invocation
			if (_read && _write) {
				deleteStep = true;
			} else {
				_bytesToLink -= linkedBytes;
				_started = true;
			}

			// Release successors before releasing the lock (otherwise
			// ClusterDataLinkStep::linkRegion may delete this step first).
			releaseSuccessors();
		}

		if (deleteStep) {
			// TODO: See comment in linkRegion
			delete this;
		}
	}

	ClusterDataCopyStep::ClusterDataCopyStep(
		MemoryPlace const *sourceMemoryPlace,
		MemoryPlace const *targetMemoryPlace,
		DataAccessRegion const &region,
		Task *task,
		WriteID writeID,
		bool isTaskwait,
		bool isWeak,
		bool needsTransfer,
		bool registerLocation
	) : Step(),
		_sourceMemoryPlace(sourceMemoryPlace),
		_targetMemoryPlace(targetMemoryPlace),
		_fullRegion(region),
		_regionsFragments(),
		_task(task),
		_writeID(writeID),
		_isTaskwait(isTaskwait),
		_isWeak(isWeak),
		_needsTransfer(needsTransfer),
		_registerLocation(registerLocation)
	{
#ifndef NDEBUG
		assert(ClusterManager::getCurrentMemoryNode() == _targetMemoryPlace);
		assert(_targetMemoryPlace->getType() == nanos6_cluster_device);

		assert(_sourceMemoryPlace != _targetMemoryPlace);

		if (_registerLocation) {
			assert(!_needsTransfer);
		}

		if (_sourceMemoryPlace->isDirectoryMemoryPlace()) {
			assert(!_needsTransfer);
		}

		if (_needsTransfer) {
			assert(_sourceMemoryPlace->getType() == nanos6_cluster_device);
		}
#endif

		// We fragment the transfers here.
		// TODO: If this affects performance, we can do the fragmentation on demand.
		// So only when the fragmentation is going to take place.
		_nFragments = ClusterManager::getMPIFragments(region);

		char *start = (char *)region.getStartAddress();

		for (size_t i = 0; start < region.getEndAddress(); ++i) {

			assert(i < _nFragments);
			// Note: this calculation still works when the maximum message size is SIZE_MAX.
			char *end = (char *) region.getEndAddress();
			if ((size_t)(end-start) > ClusterManager::getMessageMaxSize()) {
				end = start + ClusterManager::getMessageMaxSize();
			}

			_regionsFragments.push_back(DataAccessRegion(start, end));

			start = end;
		}

		_postcallback = [&]() {
			if (--_nFragments == 0) {
				//! If this data copy is performed for a taskwait we don't need to update the
				//! location here.
				DataAccessRegistration::updateTaskDataAccessLocation(
					_task,
					_fullRegion,
					_targetMemoryPlace,
					_isTaskwait
				);

				WriteIDManager::registerWriteIDasLocal(_writeID, _fullRegion);
				this->releaseSuccessors();
				delete this;
			}
		};
	}


	bool ClusterDataCopyStep::requiresDataFetch()
	{
		assert(ClusterManager::getCurrentMemoryNode() == _targetMemoryPlace);
		assert(_targetMemoryPlace->getType() == nanos6_cluster_device);
		// TODO: If this condition never trigers then the _writeID member can be removed. from this
		// class.

		bool lateWriteID = false;
		if (_needsTransfer && WriteIDManager::checkWriteIDLocal(_writeID, _fullRegion)) {
			// Second chance: now it is found by write ID, so in the end just register the
			// location.
			lateWriteID = true;
			Instrument::dataFetch(Instrument::LateWriteID, _fullRegion);
		}

		if (_registerLocation || lateWriteID) {
			//! This access doesn't need a transfer. But we need to update the task location
			//! to match the target node.
			assert(!_needsTransfer || lateWriteID);

			DataAccessRegistration::updateTaskDataAccessLocation(
				_task,
				_fullRegion,
				_targetMemoryPlace,
				_isTaskwait
			);
		}

		if (!_needsTransfer || lateWriteID) {
			releaseSuccessors();
			delete this;
			return false;
		}

		assert(_sourceMemoryPlace->getType() == nanos6_cluster_device);
		assert(_sourceMemoryPlace != _targetMemoryPlace);


		// Now check pending data transfers because the same data transfer
		// (or one fully containing it) may already be pending. An example
		// would be when several tasks with an "in" dependency on the same
		// data region are offloaded at a similar time.
		bool handled = ClusterPollingServices::PendingQueue<DataTransfer>::checkPendingQueue(

			// This lambda is called for all pending data transfers (with the lock taken)
			[&](DataTransfer *dtPending) {

				// Check whether the pending data transfer has the same target
				// (this node) and that it fully contains the current region.
				// Note: it is important to check that the target matches
				// because outgoing and incoming data transfers are held in the
				// same queue.  It is possible for an outgoing message transfer
				// to still be in the queue because of the race condition
				// between (a) remote task completion and triggering incoming
				// data fetches and (b) completing the outgoing data transfer.

				const DataAccessRegion pendingRegion = dtPending->getDataAccessRegion();
				const MemoryPlace *pendingTarget = dtPending->getTarget();
				assert(pendingTarget->getType() == nanos6_cluster_device);

				if (pendingTarget->getIndex() == _targetMemoryPlace->getIndex()
					&& _fullRegion.fullyContainedIn(pendingRegion)) {

					// Yes, the pending data transfer contains this region: so add a callback
					// for this task
					Instrument::dataFetch(Instrument::FoundInPending, _fullRegion);
					dtPending->addCompletionCallback(
						[&]() {
							//! If this data copy is performed for a taskwait we
							//! don't need to update the location here.
							DataAccessRegistration::updateTaskDataAccessLocation(
								_task,
								_fullRegion,
								_targetMemoryPlace,
								_isTaskwait
							);
							this->releaseSuccessors();
							delete this;
						});
					// Done, so return true: do not check any more pending transfers and
					// also return true to the caller
					return true;
				}
				// Not a match: continue checking pending data transfers
				return false;
			}
		);

		if (!handled) {
			Instrument::dataFetch(Instrument::FetchRequired, _fullRegion);
		}
		return (handled == false);
	}

};
