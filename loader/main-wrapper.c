#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "main-wrapper.h"
#include "api/nanos6/bootstrap.h"
#include "api/nanos6/library-mode.h"
#include "api/nanos6/runtime-info.h"
#include "api/nanos6/taskwait.h"


main_function_t *_nanos6_loader_wrapped_main = 0;


typedef struct {
	pthread_mutex_t _mutex;
	pthread_cond_t _cond;
	int _signaled;
} condition_variable_t;


typedef struct {
	int argc;
	char **argv;
	char **envp;
	int returnCode;
} main_task_args_block_t;


static void main_task_wrapper(void *argsBlock)
{
	main_task_args_block_t *realArgsBlock = (main_task_args_block_t *) argsBlock;
	
	assert(_nanos6_loader_wrapped_main != NULL);
	assert(realArgsBlock != NULL);
	
	realArgsBlock->returnCode = _nanos6_loader_wrapped_main(
		realArgsBlock->argc,
		realArgsBlock->argv,
		realArgsBlock->envp
	);
	
	nanos_taskwait("NanosLoader Bootstrap code");
	
	char *reportPrefix = getenv("NANOS6_REPORT_PREFIX");
	if (reportPrefix != NULL) {
		for (void *it = nanos6_runtime_info_begin(); it != nanos6_runtime_info_end(); it = nanos6_runtime_info_advance(it)) {
			if (reportPrefix[0] != '\0') {
				printf("%s\t", reportPrefix);
			}
			
			nanos6_runtime_info_entry_t entry;
			nanos6_runtime_info_get(it, &entry);
			
			switch (entry.type) {
				case nanos6_integer_runtime_info_entry:
					printf("long\t");
					break;
				case nanos6_real_runtime_info_entry:
					printf("double\t");
					break;
				case nanos6_text_runtime_info_entry:
					printf("string\t");
					break;
			}
			
			printf("%s\t", entry.name);
			
			switch (entry.type) {
				case nanos6_integer_runtime_info_entry:
					printf("%li\t", entry.integer);
					break;
				case nanos6_real_runtime_info_entry:
					printf("%f\t", entry.real);
					break;
				case nanos6_text_runtime_info_entry:
					printf("%s\t", entry.text);
					break;
			}
			
			printf("%s\t%s\n", entry.units, entry.description);
		}
	}
}


static void main_completion_callback(void *args)
{
	condition_variable_t *condVar = (condition_variable_t *) args;
	assert(condVar != NULL);
	
	pthread_mutex_lock(&condVar->_mutex);
	condVar->_signaled = 1;
	pthread_cond_signal(&condVar->_cond);
	pthread_mutex_unlock(&condVar->_mutex);
}


int _nanos6_loader_main(int argc, char **argv, char **envp) {
	// First half of the initialization
	nanos_preinit();
	
	condition_variable_t condVar = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
	
	// Spawn the main task
	main_task_args_block_t argsBlock = { argc, argv, envp, 0 };
	nanos_spawn_function(main_task_wrapper, &argsBlock, main_completion_callback, &condVar, "main");
	
	// Second half of the initialization
	nanos_init();
	
	// Wait for the completion callback
	pthread_mutex_lock(&condVar._mutex);
	while (condVar._signaled == 0) {
		pthread_cond_wait(&condVar._cond, &condVar._mutex);
	}
	pthread_mutex_unlock(&condVar._mutex);
	
	// Terminate
	nanos_shutdown();
	
	return argsBlock.returnCode;
}
