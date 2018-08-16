//
// Created by tomslabbaert on 4/20/17.
//
#include "uthreads.h"
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <map>
#include <iostream>
#include <queue>
#include <setjmp.h>
#include <unistd.h>
#include <list>
#include <stdlib.h>

#define RUNNING 0
#define BLOCKED 2
#define READY 1

#define TIMER_ERR "System error: Setitimer error"
#define SIGACTION_ERR "System error: Sigaction error"
#define BAD_ALLOC_ERR "System error: bad memory allocation"
#define MAX_THREADS_LIMIT_ERR "Thread library error: Max threads limit exceeded"
#define WRONG_ID_ERR "Thread library error: Can't find a thread with the given ID"
#define MAIN_THREAD_BLOCKING_ERR "Thread library error: Can't block main thread"
#define BAD_INPUT_ERR "Thread library error: Quantum usecs should be Non positive"
#define BAD_SYNC_ERR "Thread library error: Tried to perform bad sync"
#define BLOCKED_THREAD -1
#define TERMINATED_THREAD -2
#define MAIN_THREAD_ID 0

#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
	address_t ret;
	asm volatile("xor    %%fs:0x30,%0\n"
			"rol    $0x11,%0\n"
	: "=g" (ret)
	: "0" (addr));
	return ret;
}

#else
/* code for 32 bit Intel arch */
typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
		"rol    $0x9,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}
#endif
/**
 * thread class to contain the threads adress, id, bool flags and more.
 */
class thread {
public:
	int threadId, state, threadQuantum;
	bool isSync, officiallyBlocked;
	char _stack[STACK_SIZE];
	std::list<int> SyncThreads; // a list of threads sync'd to this one.
	sigjmp_buf env;
	thread(int Id);
	void changeState(int newState);
};

/**
 * thread constructor.
 * @param Id an int, the threads ID.
 */
thread::thread(int Id){
	state = READY;
	threadQuantum = 1;
	threadId = Id;
}

sigset_t set; // a simple signal set
std::map<int, thread*> threadsMap; // a map to contain thread ID as key, and a
										// thread as the maps Value.
// a priority que to contain available ID's to give to new spawned threads.
std::priority_queue<int, std::vector<int>, std::greater<int>> threadIDs;
std::list<thread*> readyThreadQue; // a que for threads ready to be executed.
int totalQuantums, runningThreadID;
struct sigaction sa;
struct itimerval timer;

/**
 * this function receives an int representing a new state.
 * @param newState an int number.
 */
void thread::changeState(int newState)
{
	switch(newState) // switches the threads state accordingly.
	{
		case READY:
			state = READY;
			break;
		case RUNNING:
			state = RUNNING;
			break;
		case BLOCKED:
			state = BLOCKED;
	}
}

/**
 * a scheduler function to manage threads usage
 * @param sig an int to differentiate which process called the function.
 */
void scheduler(int sig)
{
	// resets the timer if the scheduler was called due to a block/terminate
	// action.
	if(sig == TERMINATED_THREAD or sig == BLOCKED_THREAD)
	{
		if (setitimer (ITIMER_VIRTUAL, &timer, NULL)) {
			printf(TIMER_ERR);
		}
	}
	totalQuantums++; // increase the quantum counter.
	// if there is only one thread in the map (and one wasn't just terminated)
	// we increase the threads quantum counter and return (because a jump is not
	// needed).
	if(threadsMap.size() == 1 and sig != TERMINATED_THREAD)
	{
		thread* curr = threadsMap.find(runningThreadID) -> second;
		curr -> threadQuantum++;
		return;
	}
	// if the the thread HAS NOT been terminated.
	if(sig != TERMINATED_THREAD)
	{	// then we find it and save the process using sigsetjmp.
		thread* threadToEnd = threadsMap.find(runningThreadID) -> second;
		int ret_val = sigsetjmp(threadToEnd -> env, 1);
		if (ret_val == 1) { // if we returned here by using siglongjmp
			return;			// then we return in order to return the the
		}					// threads functions.
		if(sig != BLOCKED_THREAD){  //  if the thread HAS NOT been blocked.
			threadToEnd->changeState(READY);	// we switch it to ready
			readyThreadQue.push_back(threadToEnd);	// and add it to the que.
		}
	}
	// we get the next thread in the que and put  it into running.
	thread* threadToRun = readyThreadQue.front();
	readyThreadQue.pop_front();
	threadToRun -> changeState(RUNNING);
	runningThreadID = threadToRun -> threadId;
	threadToRun -> threadQuantum++;
	// we iterate over all threads sync'd to this thread and resume them
	// if needed.
	for(int i = 0; i < threadToRun -> SyncThreads.size(); i++)
	{
		int tid = threadToRun -> SyncThreads.front();
		threadToRun -> SyncThreads.pop_front();
		thread* tempThread = threadsMap.find(tid) -> second;
		tempThread -> isSync = false;
		if(!tempThread->officiallyBlocked) // only resumes to being ready
		{				// if the thread was only sync'd and not blocked aswell.
			tempThread -> changeState(READY);
			readyThreadQue.push_back(tempThread);
		}
	}
	siglongjmp(threadToRun -> env, 1); // jump to the new thread to run.
}

/**
 * creates a signal set for future use, contains only the SIGVTALRM signal.
 */
void createSignalSet()
{
	sigemptyset(&set);
	sigaddset(&set, SIGVTALRM);
}

/**
 * the function to be binded with the timer signal
 * @param signum
 */
void timer_handler (int signum)
{
	scheduler(1); //calls the scheduler.
}

/**
 * creates a timer and binds it with the "Scheduler" function.
 * @param quantum_usecs the amount of time wanted between scheduler calls.
 */
void createTimer(int quantum_usecs){
	// Install timer_handler as the signal handler for SIGVTALRM.
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = &timer_handler;

	if (sigaction(SIGVTALRM, &sa ,NULL) < 0) {
		printf(SIGACTION_ERR);
	}

	timer.it_value.tv_sec = 0;		// first time interval, seconds part
	timer.it_value.tv_usec = quantum_usecs;		// first time interval,
												// microseconds part
	timer.it_interval.tv_sec = 0;	// following time intervals, seconds part
	timer.it_interval.tv_usec = quantum_usecs;	// following time intervals,
													// microseconds part
	// following time intervals, microseconds part
	// Start a virtual timer. It counts down whenever this process is executing.
	if (setitimer (ITIMER_VIRTUAL, &timer, NULL)) {
		printf(TIMER_ERR);
	}
}

/**
 * this function creates the main thread
 */
void createMainThread()
{
	try
	{ // we create the main thread with all the wanted settings.
		thread* newThread = new thread(MAIN_THREAD_ID);
		newThread -> changeState(RUNNING);
		newThread -> threadQuantum = 1;
		runningThreadID = MAIN_THREAD_ID;
		newThread -> isSync = false;
		newThread -> officiallyBlocked = false;
		threadsMap.insert(std::pair<int, thread*>(MAIN_THREAD_ID, newThread));
		sigsetjmp(newThread -> env, 1);
	}
	catch(std::bad_alloc& ba) // catch bad memory allocation try.
	{
		fprintf(stderr, BAD_ALLOC_ERR);
		exit(1);
	}
}

/**
 * Description: This function initializes the thread library.
 * You may assume that this function is called before any other thread library
 * function, and that it is called exactly once. The input to the function is
 * the length of a quantum in micro-seconds. It is an error to call this
 * function with non-positive quantum_usecs.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs) {
	if(quantum_usecs <= 0) // if the input is invalid return fail.
	{
		fprintf(stderr, BAD_INPUT_ERR);
		return -1;
	}
	totalQuantums = 1; // we start the quantum count at 1.
	for(int i = 1; i < MAX_THREAD_NUM; i++) // we create a min heap to
	{								// act as an available ID pool and we push
		threadIDs.push(i);			// the wanted ID's into it.
	}
	createSignalSet();    // creating a signal set to later be blocked by our
						// function containing only the SIGVTALRM signal.
	createMainThread();  // creating the main thread (id 0).
	createTimer(quantum_usecs); // creating a timer to send a signal every X
							// quantum secs to the wanted function.
	return 0;
}


/**
 * Description: This function creates a new thread, whose entry point is the
 * function f with the signature void f(void). The thread is added to the end
 * of the READY threads list. The uthread_spawn function should fail if it
 * would cause the number of concurrent threads to exceed the limit
 * (MAX_THREAD_NUM). Each thread should be allocated with a stack of size
 * STACK_SIZE bytes.
 * Return value: On success, return the ID of the created thread.
 * On failure, return -1.
*/
int uthread_spawn(void (*f)(void)) {
	sigprocmask(SIG_BLOCK, &set, NULL);
	if(threadsMap.size() > MAX_THREAD_NUM - 1) // if the amount of threads
	{							// exceeds the max amount we return fail.
		fprintf(stderr, MAX_THREADS_LIMIT_ERR);
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		return -1;
	}
	int threadId = threadIDs.top(); // else we get the lowest ID available in
	threadIDs.pop();				// the ID pool.
	try
	{ // we create the new thread object with all the default settings needed.
		thread* newThread = new thread(threadId);
		newThread -> changeState(READY);
		newThread -> threadQuantum = 0;
		newThread -> isSync = false;
		newThread -> officiallyBlocked = false;
		address_t sp, pc;
		sp = (address_t) newThread -> _stack + STACK_SIZE - sizeof(address_t);
		pc = (address_t)f;
		sigsetjmp(newThread -> env, 1);
		((newThread -> env )->__jmpbuf)[JB_SP] = translate_address(sp);
		((newThread -> env )->__jmpbuf)[JB_PC] = translate_address(pc);
		sigemptyset(&(newThread -> env )->__saved_mask);
		threadsMap.insert(std::pair<int, thread*>(threadId, newThread));
		readyThreadQue.push_back(newThread);
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		return threadId;
	}
	catch(std::bad_alloc& ba) // error if we were not able to allocate memory
	{							// for the thread.
		fprintf(stderr, BAD_ALLOC_ERR);
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		exit(1);
	}
}

/**
 * Description: This function terminates the thread with ID tid and deletes
 * it from all relevant control structures. All the resources allocated by
 * the library for this thread should be released. If no thread with ID tid
 * exists it is considered as an error. Terminating the main thread
 * (tid == 0) will result in the termination of the entire process using
 * exit(0) [after releasing the assigned library memory].
 * Return value: The function returns 0 if the thread was successfully
 * terminated and -1 otherwise. If a thread terminates itself or the main
 * thread is terminated, the function does not return.
*/
int uthread_terminate(int tid) {
    sigprocmask(SIG_BLOCK, &set, NULL);
	if(tid == MAIN_THREAD_ID) //if we terminate the main thread we delete all the other
	{			// existing threads.
		for(int i = 1; i < threadsMap.size() + 1; i++) // iterate over all the
		{												// threads.
			thread* temp = threadsMap.find(i) -> second;
			delete(temp);  // delete each object.

		}
		threadsMap.clear(); // finally we clear all the DB's we used in the
		readyThreadQue.clear();								// program.
		threadIDs = std::priority_queue<int, std::vector<int>,
				std::greater<int>>();
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		exit(0);
	}
	// if its not the main thread we look for it.
	thread* threadToTerminate = threadsMap.find(tid) -> second;
	if(!threadToTerminate) // return -1 if not found.
	{
		fprintf(stderr, WRONG_ID_ERR);
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		return -1;
	}
	if(threadToTerminate -> state == READY) // if its currently in a ready
	{										// state we remove it from the que.
		readyThreadQue.remove(threadToTerminate);
	}
	// we iterate over all the threads that are sync'd to the thread
	// that's about to be terminates and we release their sync block.
	for(int i = 0; i < threadToTerminate -> SyncThreads.size(); i++)
	{
		int tempId = (threadToTerminate -> SyncThreads).front();
		(threadToTerminate -> SyncThreads).pop_front();
		thread* tempThread = threadsMap.find(tempId) -> second;
		if(tempThread)
		{
			tempThread -> isSync = false;
			if(!tempThread -> officiallyBlocked)
			{
				tempThread -> changeState(READY);
				readyThreadQue.push_back(tempThread);
			}
		}
	}
	// we add the thread ID back to the open thread ID pool.
	threadIDs.push(tid);
	threadsMap.erase(tid); // and erase it from the map.
	delete(threadToTerminate); // delete the thread object.
	if(tid == runningThreadID) // also if the thread was currently running
	{						// we call the scheduler.
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		scheduler(TERMINATED_THREAD);
	}
	else
	{
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		return 0;
	}

}


/**
 * Description: This function blocks the thread with ID tid. The thread may
 * be resumed later using uthread_resume. If no thread with ID tid exists it
 * is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision
 * should be made. Blocking a thread in BLOCKED state has no
 * effect and is not considered as an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_block(int tid) {
	sigprocmask(SIG_BLOCK, &set, NULL);
	if(tid == MAIN_THREAD_ID) // cant block the main thread.
	{
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		fprintf(stderr, MAIN_THREAD_BLOCKING_ERR);
		return -1;
	}
	thread* threadToBlock = threadsMap.find(tid) -> second;
	if(!threadToBlock) // if there is no thread with th given ID returns -1.
	{
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		fprintf(stderr, WRONG_ID_ERR);
		return -1;
	}
	if(threadToBlock -> state == READY) //if the block is currently ready we
	{									// remove it from the thread que.
		readyThreadQue.remove(threadToBlock);
	}
	else if(threadToBlock -> state == RUNNING)//if the thread is currently
	{  							// running we block it and  call the scheduler.
		threadToBlock -> changeState(BLOCKED);
		scheduler(-1);
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		return 0;
	}
	threadToBlock -> officiallyBlocked = true;
	threadToBlock -> changeState(BLOCKED); // otherwise just blocks the thread.
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	return 0;
}


/**
 * Description: This function resumes a blocked thread with ID tid and moves
 * it to the READY state. Resuming a thread in a RUNNING or READY state
 * has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered as an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid) {
	sigprocmask(SIG_BLOCK, &set, NULL);
	thread* threadToUnblock = threadsMap.find(tid) -> second;
	if(!threadToUnblock) // if there is no thread with th given ID returns -1.
	{
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		fprintf(stderr, WRONG_ID_ERR);
		return -1;
	}

	if(threadToUnblock -> state == RUNNING) // no effect if running.
	{
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		return 0;
	}
	threadToUnblock -> officiallyBlocked = false;//else unblocks the block flag.
	if(!threadToUnblock -> isSync){ // only resumes the thread if it is not
									// waiting for a different thread.
		threadToUnblock -> changeState(READY);
		readyThreadQue.push_back(threadToUnblock);
	}
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	return 0;
}


/**
 * Description: This function blocks the RUNNING thread until thread with
 * ID tid will move to RUNNING state (i.e.right after the next time that
 * thread tid will stop running, the calling thread will be resumed
 * automatically). If thread with ID tid will be terminated before RUNNING
 * again, the calling thread should move to READY state right after thread
 * tid is terminated (i.e. it won’t be blocked forever). It is considered
 * as an error if no thread with ID tid exists or if the main thread (tid==0)
 * calls this function. Immediately after the RUNNING thread transitions to
 * the BLOCKED state a scheduling decision should be made.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_sync(int tid) {
    sigprocmask(SIG_BLOCK, &set, NULL);
	// returns -1 for an illegal sync.
	if(runningThreadID == MAIN_THREAD_ID or tid == runningThreadID)
	{
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		fprintf(stderr, BAD_SYNC_ERR);
		return -1;
	}
	thread* threadToSync = threadsMap.find(tid) -> second;
	//if there is no thread with th given ID returns -1.
	if(!threadToSync)
	{
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		fprintf(stderr, WRONG_ID_ERR);
		return -1;
	}
	// otherwise syncs the thread to the wanted one, and blocks it.
	thread* runningThread = threadsMap.find(runningThreadID) -> second;
	runningThread -> isSync = true;
	threadToSync -> SyncThreads.push_back(runningThreadID);
	threadToSync -> SyncThreads.unique();
	runningThread -> changeState(BLOCKED);
	scheduler(-1); // calls the scheduler to schedule a new thread to run.
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	return 0;
}


/**
 * Description: This function returns the number of quantums the thread with
 * ID tid was in RUNNING state. On the first time a thread runs, the function
 * should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state
 * when this function is called, include also the current quantum). If no
 * thread with ID tid exists it is considered as an error.
 * Return value: On success, return the number of quantums of the thread with ID
 * tid. On failure, return -1.
*/
int uthread_get_quantums(int tid) {
	sigprocmask(SIG_BLOCK, &set, NULL);
    if (threadsMap.find(tid) == threadsMap.end()) {
        // not found
        sigprocmask(SIG_UNBLOCK, &set, NULL);
		fprintf(stderr, WRONG_ID_ERR);
        return -1;
    } else {
        // found
        sigprocmask(SIG_UNBLOCK, &set, NULL);
        return threadsMap.at(tid)->threadQuantum;
    }
}


/**
 * Description: This function returns the total number of quantums that were
 * started since the library was initialized, including the current quantum.
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number
 * should be increased by 1.
 * Return value: The total number of quantums.
*/
int uthread_get_total_quantums() {
	return totalQuantums;
}

/**
 * Description: This function returns the thread ID of the calling thread.
 * Return value: The ID of the calling thread.
*/
int uthread_get_tid() {
	return runningThreadID;
}