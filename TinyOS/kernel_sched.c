
#include <assert.h>
#include <sys/mman.h>

#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_sched.h"
#include "kernel_proc.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif


/*
   The thread layout.
  --------------------

  On the x86 (Pentium) architecture, the stack grows upward. Therefore, we
  can allocate the TCB at the top of the memory block used as the stack.

  +-------------+
  |       TCB     |
  +-------------+
  |                  |
  |    stack      | 
  |                  |
  |        ^       |
  |         |        |
  +-------------+
  | first frame |
  +-------------+

  Advantages: (a) unified memory area for stack and TCB (b) stack overrun will
  crash own thread, before it affects other threads (which may make debugging
  easier).

  Disadvantages: The stack cannot grow unless we move the whole TCB. Of course,
  we do not support stack growth anyway!
 */


/* 
  A counter for active threads. By "active", we mean 'existing', 
  with the exception of idle threads (they don't count).
*/
#define STARV_LIM 3

static volatile int starv_flag = 0;
volatile unsigned int active_threads = 0;
Mutex active_threads_spinlock = MUTEX_INIT;
Mutex starv_flag_spinlock = MUTEX_INIT;

/* This is specific to Intel Pentium! */
#define SYSTEM_PAGE_SIZE  (1<<12)

/* The memory allocated for the TCB must be a multiple of SYSTEM_PAGE_SIZE */
#define THREAD_TCB_SIZE   (((sizeof(TCB)+SYSTEM_PAGE_SIZE-1)/SYSTEM_PAGE_SIZE)*SYSTEM_PAGE_SIZE)

#define THREAD_SIZE  (THREAD_TCB_SIZE+THREAD_STACK_SIZE)

//levels of multi-level feedback queue.
#define PRIORITIES 10

//#define MMAPPED_THREAD_MEM 
#ifdef MMAPPED_THREAD_MEM 


/*
  Use mmap to allocate a thread. A more detailed implementation can allocate a
  "sentinel page", and change access to PROT_NONE, so that a stack overflow
  is detected as seg.fault.
 */
void free_thread(void* ptr, size_t size)
{
  CHECK(munmap(ptr, size));
}

void* allocate_thread(size_t size)
{
  void* ptr = mmap(NULL, size, 
      PROT_READ|PROT_WRITE|PROT_EXEC,  
      MAP_ANONYMOUS  | MAP_PRIVATE 
      , -1,0);
  
  CHECK((ptr==MAP_FAILED)?-1:0);

  return ptr;
}
#else
/*
  Use malloc to allocate a thread. This is probably faster than  mmap, but cannot
  be made easily to 'detect' stack overflow.
 */
void free_thread(void* ptr, size_t size)
{
  free(ptr);
}

void* allocate_thread(size_t size)
{
  void* ptr = aligned_alloc(SYSTEM_PAGE_SIZE, size);
  CHECK((ptr==NULL)?-1:0);
  return ptr;
}
#endif



/*
  This is the function that is used to start normal threads.
*/

void gain(int preempt); /* forward */

static void thread_start()
{
  gain(1);
  CURTHREAD->thread_func(); // executes the thread. 

  /* We are not supposed to get here! */
  assert(0);
}


/*
  Initialize and return a new TCB
*/

#define INITIAL_PRIORITY PRIORITIES/2


TCB* spawn_thread(PCB* pcb, void (*func)(),PTCB* ptcb)
{

  /* The allocated thread size must be a multiple of page size */
  TCB* tcb = (TCB*) allocate_thread(THREAD_SIZE);
  tcb->state = INIT;
  /* Set the owner */
  tcb->owner_pcb = pcb;
  tcb->JoinFlag = 0;
  

//easy way to allocate thread id. Could have also used a variable like "spawned threads" and increment by one.
  // tcb->tid=(Tid_t) &tcb; 
    /* Initialize the other attributes */
  tcb->type = NORMAL_THREAD;
  
  tcb->phase = CTX_CLEAN;
  tcb->thread_func = func;
  tcb->wakeup_time = NO_TIMEOUT;
  tcb->sched_priority=INITIAL_PRIORITY;
  tcb->waited_for=0;


  rlnode_init(& tcb->sched_node, tcb);  /* Intrusive list node */


  /* Compute the stack segment address and size */
  void* sp = ((void*)tcb) + THREAD_TCB_SIZE;

  if(ptcb!=NULL){
  ptcb->owned_thread=tcb;
  ptcb->thread_id=(Tid_t) tcb;
  
    //Set thread relations.
    tcb->owner_ptcb=ptcb;
  } 

    tcb->tid=(Tid_t) tcb;

  // fprintf(stderr, "Curr thread = %lu. Calling thread %lu\n", (Tid_t) CURTHREAD, tcb->tid );


  /* Init the context */
  cpu_initialize_context(& tcb->context, sp, THREAD_STACK_SIZE, thread_start);

 


#ifndef NVALGRIND
  tcb->valgrind_stack_id = 
    VALGRIND_STACK_REGISTER(sp, sp+THREAD_STACK_SIZE);
#endif

  /* increase the count of active threads */
  Mutex_Lock(&active_threads_spinlock);
  active_threads++;
  Mutex_Unlock(&active_threads_spinlock);
 
  return tcb;
}


/*
  This is called with tcb->state_spinlock locked !
 */
void release_TCB(TCB* tcb)
{
#ifndef NVALGRIND
  VALGRIND_STACK_DEREGISTER(tcb->valgrind_stack_id);    
#endif

  free_thread(tcb, THREAD_SIZE);

  Mutex_Lock(&active_threads_spinlock);
  active_threads--;
  Mutex_Unlock(&active_threads_spinlock);
}


/*
 *
 * Scheduler
 *
 */


/*
 *  Note: the scheduler routines are all in the non-preemptive domain.
 */


/* Core control blocks */
CCB cctx[MAX_CORES];


/*
  The scheduler queue is implemented as a doubly linked list. The
  head and tail of this list are stored in SCHED.
	
  Also, the scheduler contains a linked list of all the sleeping
  threads with a timeout.

  Both of these structures are protected by @c sched_spinlock.
*/



// rlnode SCHED;                         /* The scheduler queue */
rlnode MFQ[PRIORITIES];         /* The NEW - extra spicy may I add - scheduler queue */   
rlnode TIMEOUT_LIST;				  /* The list of threads with a timeout */
Mutex sched_spinlock = MUTEX_INIT;    /* spinlock for scheduler queue */

/*---------------------------------------------------------------------

                    Multilevel feedback queue

-----------------------------------------------------------------------*/

/*Insert TCB into its place in MFQ*/

static void mfq_add(TCB* tcb,enum SCHED_CAUSE cause){
  //Select the priority of the queue to insert the tcb at.
 // lower values of the "sched_priority" variable mean higher priority.

  switch(cause)
  {
  		case SCHED_QUANTUM  :
                //reset wait cycles, since we are about to change priority.
               tcb->waited_for=0;
               Mutex_Lock(&starv_flag_spinlock);
               starv_flag = 0;
               Mutex_Unlock(&starv_flag_spinlock);

               //decrease priority
              if (tcb->sched_priority+1<=PRIORITIES-1){
                 // fprintf(stderr, "QUANTUM Changing priority : #%d Priority %d  , new priority %d \n",get_pid(tcb->owner_pcb), tcb->sched_priority,tcb->sched_priority-1);
  
                 tcb->sched_priority++;
               }
               break;

        case SCHED_IO :
    
              //reset wait cycles, since we are about to change priority.
              tcb->waited_for=0;
  			  
 			  Mutex_Lock(&starv_flag_spinlock);
              starv_flag = 0;
              Mutex_Unlock(&starv_flag_spinlock);	

             //increase priority
             if (tcb->sched_priority-1>=0){
              // fprintf(stderr, "I/O Changing priority : #%d Priority %d  , new priority %d \n",get_pid(tcb->owner_pcb), tcb->sched_priority,tcb->sched_priority-1);
  
                tcb->sched_priority--;
             }

              break;

        case SCHED_MUTEX:

        		tcb->waited_for = 0;
        		Mutex_Lock(&starv_flag_spinlock);
        		starv_flag++;
        		Mutex_Unlock(&starv_flag_spinlock);
        		if(starv_flag>=STARV_LIM){
        			int j;
        			for(j=1;j<PRIORITIES;j++){
        				while(!is_rlist_empty(&MFQ[j])){
        					rlist_push_back(&MFQ[0],rlist_pop_front(&MFQ[j]));
        				}
        			}	
        		}
        		break;

  default:    tcb->waited_for++;
  			  Mutex_Lock(&starv_flag_spinlock);
              starv_flag = 0;
              Mutex_Unlock(&starv_flag_spinlock);
                 break;
}

  // If you wait for a long time, Mrs Perkins...
  // Your membership to the Continental has been, by thine own hand ...revoked.
  if(tcb->waited_for>= MAX_PRIORITY_STAY){

    tcb->waited_for=0;
    if (tcb->sched_priority-1>=0){
        tcb->sched_priority--;
        // if(tcb->sched_priority==0)       
        // fprintf(stderr, "Process number %d reached top priority\n", get_pid(CURTHREAD->owner_pcb) );

    }
  }

  //Put the node back in it's rightful place.
  rlist_push_back(&MFQ[tcb->sched_priority],& tcb->sched_node);

  /* Restart possibly halted cores */
  cpu_core_restart_one();
}

static void mfq_make_ready(TCB* tcb,enum SCHED_CAUSE cause){
      assert(tcb->state == STOPPED || tcb->state == INIT);

      /* Possibly remove from TIMEOUT_LIST */
        if(tcb->wakeup_time != NO_TIMEOUT) {
          /* tcb is in TIMEOUT_LIST, fix it */
          assert(tcb->sched_node.next != &(tcb->sched_node) && tcb->state == STOPPED);
          rlist_remove(& tcb->sched_node);
          tcb->wakeup_time = NO_TIMEOUT;
        }

        /* Mark as ready */
          tcb->state = READY;

          /* Possibly add to the scheduler queue */
          if(tcb->phase == CTX_CLEAN) 
            mfq_add(tcb,cause);

}

static TCB* mfq_select(enum SCHED_CAUSE cause){

  TimerDuration curtime = bios_clock();
while(! is_rlist_empty(&TIMEOUT_LIST)) {
      
             TCB* tcb = TIMEOUT_LIST.next->tcb;
      
              if(tcb->wakeup_time > curtime)
        break;
              mfq_make_ready(tcb,cause);
  }

//Check if it is empty. If it's empty, select the highest non-empty Q.
  int i=0;
  while(is_rlist_empty(&MFQ[i])){
    i++;
    if(i==(PRIORITIES-1)) break;
  }

  rlnode * sel = rlist_pop_front(& MFQ[i]);
  return sel->tcb; 

}

/*
  Initialize our new scheduler.
*/
void initialize_MFQ(){
  int i;
  for(i=0;i<PRIORITIES;i++){
    rlnode_init(&MFQ[i],NULL);
  }
}

/* Interrupt handler for ALARM */
void yield_handler()
{
  yield(SCHED_QUANTUM);
}

/* Interrupt handle for inter-core interrupts */
void ici_handler() 
{
  /* noop for now... */
}


/*
  Possibly add TCB to the scheduler timeout list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_register_timeout(TCB* tcb, TimerDuration timeout)
{
  if(timeout!=NO_TIMEOUT){

  	/* set the wakeup time */
  	TimerDuration curtime = bios_clock(); //sync with curr. system time.
  	tcb->wakeup_time = (timeout==NO_TIMEOUT) ? NO_TIMEOUT : curtime+timeout;

  	/* add to the TIMEOUT_LIST in sorted order */
  	rlnode* n = TIMEOUT_LIST.next;
  	for( ; n!=&TIMEOUT_LIST; n=n->next)  //O(n). Not bad.
  		/* skip earlier entries */
  		if(tcb->wakeup_time < n->tcb->wakeup_time) break;

  	/* insert before n */
	rl_splice(n->prev, & tcb->sched_node);
  }
}


/*
  Make the process ready. 
 */
int wakeup(TCB* tcb)
{
	int ret = 0;

	/* Preemption off */
	int oldpre = preempt_off;

	/* To touch tcb->state, we must get the spinlock. */
	Mutex_Lock(& sched_spinlock);

	if(tcb->state==STOPPED || tcb->state==INIT) {
		// sched_make_ready(tcb);
              mfq_make_ready(tcb,SCHED_INIT);
		ret = 1;		
	}


	Mutex_Unlock(& sched_spinlock);

	/* Restore preemption state */
	if(oldpre) preempt_on;

	return ret;
}


/*
  Atomically put the current process to sleep, after unlocking mx.
 */
void sleep_releasing(Thread_state state, Mutex* mx, enum SCHED_CAUSE cause, TimerDuration timeout)
{
  //  if( get_pid(CURTHREAD->owner_pcb) !=0)
  // fprintf(stderr, "Thread %d sleeping. \n", CURTHREAD);

  assert(state==STOPPED || state==EXITED);

  TCB* tcb = CURTHREAD;
  
  /* 
    The tcb->state_spinlock guarantees atomic sleep-and-release.
    But, to access it safely, we need to go into the non-preemptive
    domain.
   */
  int preempt = preempt_off;
  Mutex_Lock(& sched_spinlock);

  /* mark the thread as stopped or exited */
  tcb->state = state;

  /* register the timeout (if any) for the sleeping thread */
  if(state!=EXITED) 
  	sched_register_timeout(tcb, timeout);

  /* Release mx */
  if(mx!=NULL) Mutex_Unlock(mx);

  /* Release the scheduler spinlock before calling yield() !!! */
  Mutex_Unlock(& sched_spinlock);

  yield(cause);

  /* Restore preemption state */
  if(preempt) preempt_on;
}


/* This function is the entry point to the scheduler's context switching */

void yield(enum SCHED_CAUSE cause)
{ 

 // if( get_pid(CURTHREAD->owner_pcb) !=0) 
 //  fprintf(stderr, "Thread %d yielding. \n", CURTHREAD);

  /* Reset the timer, so that we are not interrupted by ALARM */
  bios_cancel_timer();

  /* We must stop preemption but save it! */
  int preempt = preempt_off;

  TCB* current = CURTHREAD;  /* Make a local copy of current process, for speed */

  int current_ready = 0;

  Mutex_Lock(& sched_spinlock);
  switch(current->state)
  {
    case RUNNING:
      current->state = READY;
    case READY: /* We were awakened before we managed to sleep! */
      current_ready = 1;
      break;

    case STOPPED:
    case EXITED:
      break; 

    default:
      fprintf(stderr, "BAD STATE for current thread %p in yield: %d\n", current, current->state);
      assert(0);  /* It should not be READY or EXITED ! */
  }

  /* Get next */
  TCB* next= mfq_select(cause);



  /* Maybe there was nothing ready in the scheduler queue ? */
  if(next==NULL) {
    if(current_ready)
      next = current;
    else
      next = & CURCORE.idle_thread;
  }

  /* ok, link the current and next TCB, for the gain phase */
  current->next = next;
  next->prev = current;
  
  Mutex_Unlock(& sched_spinlock);



  /* Switch contexts */
  if(current!=next) {
    CURTHREAD = next;
    cpu_swap_context( & current->context , & next->context ); //CK says there's some wait here.
  }


  /* This is where we get after we are switched back on! A long time 
     may have passed. Start a new timeslice... 
   */
  gain(preempt);
}


/*
  This function must be called at the beginning of each new timeslice.
  This is done mostly from inside yield(). 
  However, for threads that are executed for the first time, this 
  has to happen in thread_start.

  The 'preempt' argument determines whether preemption is turned on
  in the new timeslice. When returning to threads in the non-preemptive
  domain (e.g., waiting at some driver), we need to not turn preemption
  on!
*/

void gain(int preempt)
{
//  if( get_pid(CURTHREAD->owner_pcb) !=0)
// fprintf(stderr, "Thread %u gaining control. \n", CURTHREAD);


  Mutex_Lock(& sched_spinlock);

  /* Mark current state */
  TCB* current = CURTHREAD; 
  TCB* prev = current->prev;

  current->state = RUNNING;
  current->phase = CTX_DIRTY;

  if(current != prev) {
  	/* Take care of the previous thread */
    prev->phase = CTX_CLEAN;
    switch(prev->state) 
    {
      case READY:
        if(prev->type != IDLE_THREAD)  mfq_add(prev,SCHED_IDLE);//sched_queue_add(prev);
        break;
      case EXITED:
      	release_TCB(prev);
        break;
      case STOPPED:
        break;
      default:
        assert(0);  /* prev->state should not be INIT or RUNNING ! */
    }
  }

  Mutex_Unlock(& sched_spinlock);

  /* Reset preemption as needed */
  if(preempt) preempt_on;

  /* Set a 1-quantum alarm */
  bios_set_timer(QUANTUM);
}

//We begin and end at this thread. When it exits, we simply restart the cores. == boot.
static void idle_thread()
{
  /* When we first start the idle thread */
  yield(SCHED_IDLE);

  /* We come here whenever we cannot find a ready thread for OUR core */
  while(active_threads>0) {
    cpu_core_halt();
    yield(SCHED_IDLE);
  }
/*Other threads may still exist up to this point , in other cores*/


  /* If the idle thread exits here, we are leaving the scheduler! */
  bios_cancel_timer();
  cpu_core_restart_all();
}

/*
  Initialize the scheduler queue
 */
void initialize_scheduler()
{
  initialize_MFQ();
  // rlnode_init(&SCHED,NULL);
  rlnode_init(&TIMEOUT_LIST, NULL);
}


void run_scheduler()
{

  CCB * curcore = & CURCORE;


  /* Initialize current CCB */
  curcore->id = cpu_core_id;

  curcore->current_thread = & curcore->idle_thread;  
  /*Idle thread initialization. Doesn't change again.*/
  curcore->idle_thread.owner_pcb = get_pcb(0);
  curcore->idle_thread.type = IDLE_THREAD;
  curcore->idle_thread.state = RUNNING;
  curcore->idle_thread.phase = CTX_DIRTY;
  curcore->idle_thread.wakeup_time = NO_TIMEOUT;
  curcore->idle_thread.sched_priority=PRIORITIES-1;
  curcore->idle_thread.waited_for=0;
  rlnode_init(& curcore->idle_thread.sched_node, & curcore->idle_thread);

  /* Initialize interrupt handler */
  cpu_interrupt_handler(ALARM, yield_handler);
  cpu_interrupt_handler(ICI, ici_handler);

  /* Run idle thread */
  preempt_on;
  idle_thread();

  /* Finished scheduling */
  assert(CURTHREAD == &CURCORE.idle_thread);
  cpu_interrupt_handler(ALARM, NULL);
  cpu_interrupt_handler(ICI, NULL);
}
