#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>



/* This is specific to Intel Pentium! */
#ifndef SYSTEM_PAGE_SIZE
#define SYSTEM_PAGE_SIZE  (1<<12)
#endif

/* The memory allocated for the PTCB must be a multiple of SYSTEM_PAGE_SIZE */
#define PTCB_SIZE   (((sizeof(PTCB)+SYSTEM_PAGE_SIZE-1)/SYSTEM_PAGE_SIZE)*SYSTEM_PAGE_SIZE)

void free_ptcb(void* ptr, size_t size)
{
  free(ptr);
}

void* allocate_ptcb(size_t size)
{
  void* ptr = aligned_alloc(SYSTEM_PAGE_SIZE, size);
  CHECK((ptr==NULL)?-1:0);
  return ptr;
}

rlnode* rlist_find_Tid_t(rlnode* List, Tid_t key, rlnode* fail)
{
	rlnode* i= List->next;
	while(i!=List) {
		if((i->ptcb->owned_thread->tid) == key){
			return i;
		}
		else
			i = i->next;
	}
	return fail;
}


void sys_ThreadExit(int exitval){
Mutex_Lock(&kernel_mutex);
CURTHREAD->state=EXITED;
CURTHREAD->detached=1;
CURTHREAD->exitval=exitval;
CURTHREAD->owner_ptcb->exitval=exitval;
CURTHREAD->owner_ptcb->exited=1;

rlist_remove(&CURTHREAD->owner_ptcb->access_node);

Cond_Broadcast (&CURTHREAD->owner_ptcb->cv_ptcb); 
free_ptcb(CURTHREAD->owner_ptcb,PTCB_SIZE);

sleep_releasing(EXITED,&kernel_mutex,SCHED_USER,0);
}


//Override spawn_thread().
void start_extra_thread(){
	int exitval; 
	Task call=CURTHREAD->owner_ptcb->task;
	int argl=CURTHREAD->owner_ptcb->argl;
	void *args= CURTHREAD->owner_ptcb->args;


	exitval= call(argl,args);
	sys_ThreadExit(exitval);
}

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
	//Allocate space for the ptcb.
	Mutex_Lock(&kernel_mutex);

	PTCB* my_ptcb=(PTCB*) xmalloc(sizeof(PTCB));

	//If there is no more room.
	if(my_ptcb==NULL){
		fprintf(stderr,"Memory allocation error!");
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}

	if(task==NULL) {
		fprintf(stderr, "%s\n", "The task is invalid!" );
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}
	
	//put ptcb in pcb's nodes.
	rlnode * newnode =rlnode_init(&my_ptcb->access_node,my_ptcb);
	rlist_push_front(&CURPROC->ptcb,newnode);
	
	//set some variables.
	my_ptcb->owner_process = CURPROC;	
	my_ptcb->detached=0;
	my_ptcb->exitval=-1;
	my_ptcb->exited=0;
	my_ptcb->joinFlag=0;

	//Set those variables.
	my_ptcb->task=task;
	my_ptcb->argl=argl;
	my_ptcb->args= args;
	my_ptcb->cv_ptcb=COND_INIT;

	// spawn and set new thread.
	spawn_thread(CURPROC,start_extra_thread,my_ptcb);		
	wakeup(my_ptcb->owned_thread);
	Mutex_Unlock(&kernel_mutex);
	return (Tid_t) my_ptcb->owned_thread;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
    return (Tid_t) CURTHREAD;
}

int sys_ThreadJoin(Tid_t tid_arg, int* exitval_arg){
	
	Mutex_Lock(&kernel_mutex);
	if(tid_arg == (Tid_t)CURTHREAD){
		fprintf(stderr,"Tid given is the current thread's.\n");
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}
	PCB* owner_proc_ = CURTHREAD->owner_pcb;
	//PTCB* owner_ptcb_ = CURTHREAD->owner_ptcb;
	//Find the thread we want to join.

	if(CURTHREAD->JoinFlag){
		fprintf(stderr,"Thread has already joined. Now returning.\n");
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}

	rlnode* traverse = rlist_find_Tid_t(&(owner_proc_->ptcb),tid_arg,NULL);
	// fprintf(stderr, "I got node %lu\n",(traverse->ptcb->owned_thread->tid));

	if(traverse==NULL){
		fprintf(stderr,"Thread not found. Returning.\n");
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}

	if(traverse->ptcb->detached){
		fprintf(stderr,"Thread asked to join is detached. Returning.\n");	
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}
	
	// while(traverse->ptcb->owned_thread->state!=EXITED){
	while(traverse->ptcb->exited!=1){
		fprintf(stderr, "%s\n", "Waiting in join..." );
		Cond_Wait(&kernel_mutex,&(traverse->ptcb->cv_ptcb)); //this isn't called .
		// Cond_TimedWait(&kernel_mutex,&(traverse->ptcb->cv_ptcb),50); //this isn't called .
	}
	*exitval_arg = traverse->ptcb->exitval;
	CURTHREAD->JoinFlag = 1;
	Mutex_Unlock(&kernel_mutex);
	return 0;

}


/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid_arg){
	Mutex_Lock(&kernel_mutex);
	
	PCB* owner_proc_ = CURTHREAD->owner_pcb;
	PTCB* owner_ptcb_ = CURTHREAD->owner_ptcb;
	
	if(tid_arg == (Tid_t)CURTHREAD){
		if(CURTHREAD->detached){
			fprintf(stderr,"Thread is already detached. Returning...\n");
			Cond_Broadcast(&owner_ptcb_->cv_ptcb);
			Mutex_Unlock(&kernel_mutex);			
			return 0;
		}
		CURTHREAD->detached = 1;
		owner_ptcb_->detached = 1;
		fprintf(stderr,"Thread has become detached. Returning...\n");
		Cond_Broadcast(&owner_ptcb_->cv_ptcb);
		Mutex_Unlock(&kernel_mutex);
		return 0;
	}
	
	// PCB* owner_proc = CURTHREAD->owner_pcb;
	rlnode_ptr traverse = rlist_find_Tid_t(&(owner_proc_->ptcb),tid_arg,NULL);
	
	if(traverse==NULL){
		fprintf(stderr,"Thread is not in this process. Returning.\n");
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}	

	if(traverse->ptcb->detached){
		fprintf(stderr,"Thread is already detached. Returning...\n");
		Cond_Broadcast(&(owner_ptcb_->cv_ptcb));
		Mutex_Unlock(&kernel_mutex);
		return 0;
	}

	if(traverse->ptcb->exitval==-1){
		fprintf(stderr,"Thread has exited. Returning...\n");
		Cond_Broadcast(&(owner_ptcb_->cv_ptcb));
		Mutex_Unlock(&kernel_mutex);
		return -1;
	}	

	traverse->ptcb->owned_thread->detached = 1;
	traverse->ptcb->detached = 1;
	fprintf(stderr,"Thread found. Detach operation successfully done. Returning...\n");
	Cond_Broadcast(&(owner_ptcb_->cv_ptcb));
	Mutex_Unlock(&kernel_mutex);
	return 0;
}
