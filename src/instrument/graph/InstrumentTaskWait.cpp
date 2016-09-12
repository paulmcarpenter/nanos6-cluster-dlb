#include "InstrumentTaskWait.hpp"
#include "InstrumentGraph.hpp"

#include <InstrumentTaskExecution.hpp>

#include "executors/threads/CPU.hpp"
#include "executors/threads/WorkerThread.hpp"

#include <cassert>


namespace Instrument {
	using namespace Graph;
	
	
	void enterTaskWait(task_id_t taskId, char const *invocationSource)
	{
		std::lock_guard<SpinLock> guard(_graphLock);
		WorkerThread *currentThread = WorkerThread::getCurrentWorkerThread();
		assert(currentThread != nullptr);
		
		CPU *cpu = (CPU *) currentThread->getComputePlace();
		assert(cpu != nullptr);
		
		assert(_threadToId.find(currentThread) != _threadToId.end());
		thread_id_t threadId = _threadToId[currentThread];
		
		taskwait_id_t taskwaitId = _nextTaskwaitId++;
		
		taskwait_t *taskwait = new taskwait_t(taskwaitId, invocationSource);
		enter_taskwait_step_t *enterTaskwaitStep = new enter_taskwait_step_t(cpu->_virtualCPUId, threadId, taskwaitId, taskId);
		
		task_info_t &taskInfo = _taskToInfoMap[taskId];
		
		// Attempt to recover some memory
		if (!taskInfo._phaseList.empty()) {
			phase_t *lastPhase = taskInfo._phaseList.back();
			task_group_t *taskGroup = dynamic_cast<task_group_t *>(lastPhase);
			if (taskGroup != 0) {
				taskGroup->_dependencyInfoMap.clear();
			}
		}
		
		taskInfo._phaseList.push_back(taskwait);
		
		_executionSequence.push_back(enterTaskwaitStep);
	}
	
	
	void exitTaskWait(task_id_t taskId)
	{
		std::lock_guard<SpinLock> guard(_graphLock);
		WorkerThread *thread = WorkerThread::getCurrentWorkerThread();
		assert(thread != nullptr);
		
		assert(_threadToId.find(thread) != _threadToId.end());
		thread_id_t threadId = _threadToId[thread];
		
		CPU *cpu = (CPU *) thread->getComputePlace();
		assert(cpu != nullptr);
		
		task_info_t &taskInfo = _taskToInfoMap[taskId];
		
		assert(!taskInfo._phaseList.empty());
		phase_t *taskwaitPhase = taskInfo._phaseList.back();
		taskwait_t *taskwait = dynamic_cast<taskwait_t *> (taskwaitPhase);
		assert(taskwait != nullptr);
		taskwait_id_t taskwaitId = taskwait->_taskwaitId;
		
		exit_taskwait_step_t *exitTaskwaitStep = new exit_taskwait_step_t(cpu->_virtualCPUId, threadId, taskwaitId, taskId);
		_executionSequence.push_back(exitTaskwaitStep);
		
		// Instead of calling to Instrument::returnToTask we later on reuse the exitTaskwaitStep to also reactivate the task
	}
	
}
