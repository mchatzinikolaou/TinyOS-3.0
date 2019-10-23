
#include "tinyos.h"
#include "util.h"
#include "kernel_dev.h"
#include "kernel_proc.h"
#include "kernel_streams.h"

#define PIPE_SIZE   16 *1024

void* pipe_open(pipe_t* pipe){
	
	return NULL;
}


// int pipe_read(PPCB* ppcb,char* string,int count){
int pipe_read(void* this,char* string,uint count){


	PPCB* ppcb = (PPCB*)this;

	uint counted=0;

	while(counted!=count){

		//While there are more characters to read.
		if(ppcb->char_count>0){

			//Read next byte.
			string[counted] = ppcb->buffer[ppcb->r_pointer];

			//Empty buffer space.
			ppcb->buffer[ppcb->r_pointer] = '\0';

			//If the read pointer overflows, reset to 0 (start).
			ppcb->r_pointer++;
			if(ppcb->r_pointer>=ppcb->capacity)
				ppcb->r_pointer = 0;

			ppcb->char_count--;
			counted++;

			if(ppcb->char_count<PIPE_SIZE){
				Cond_Broadcast(&(ppcb->full));
			}
		
		}else{
			
			if(ppcb->write_close==1){
				fprintf(stderr, "%s\n", "The write end has closed. Returning." );
				return counted;
			}
			fprintf(stderr,"Waiting for a writer...\nChar count : %d\n",counted);
			int wait_status=Cond_TimedWait(&(ppcb->pipe_mutex),&(ppcb->empty),50);
			if(wait_status==0) return counted;
		}
			
	}
	fprintf(stderr,"SUCCESS on read.\n");
	return counted;
}


int pipe_write(void* this,const char* string,uint count){


	PPCB* ppcb = (PPCB*)this;

	
	/*This variable counts the bytes that have been input into the buffer.
		If by the end it's different from the amount we wanted to input,
		a failure value is returned.
		*/

	if(ppcb->read_close==1){
		fprintf(stderr, "%s\n","Write unavailable, read end is closed. " );
		return -1;
	}

	uint counted=0;
	while(counted!=count){

		if(ppcb->read_close==1){
			fprintf(stderr, "%s\n","Write unavailable, read end is closed. " );
			return counted; //maybe return count?
		}

		if(ppcb->char_count<ppcb->capacity){
		
			ppcb->buffer[ppcb->w_pointer]=string[counted];

			ppcb->w_pointer++;

			//Reset write pointer.
			if(ppcb->w_pointer>=ppcb->capacity)
			ppcb->w_pointer=0;

			ppcb->char_count++;
			counted++;

			//broadcast CV_non_empty.
			Cond_Broadcast(&ppcb->empty);

		}else{
			fprintf(stderr, "%s\n", "Buffer full, waiting for reads to empty it.");
			if(ppcb->read_close==1){
				fprintf(stderr, "%s\n","Read end is closed. " );
				return counted;
			}
			Cond_TimedWait(&(ppcb->pipe_mutex),&(ppcb->full),50); //wait for 10 sec , else timout.
		}
	}
	return counted;
}

int write_close(void* this){
	
	PPCB* ppcb=(PPCB*) this;
	ppcb->write_close=1;
	Cond_Broadcast(&ppcb->empty);
	if(ppcb->read_close==1){
		
		
		// free(ppcb);
		return 0;
	}
	return -1;
}


int read_close(void* this){

	PPCB* ppcb=(PPCB*) this;
	ppcb->read_close=1;
	Cond_Broadcast(&ppcb->full);
	if(ppcb->write_close==1){
		
		// free(ppcb);
		return 0;
	}

	return -1;
}

//Invalid OPs
int invalid_write(Fid_t fd, char *buf, unsigned int size){
	fprintf(stderr, "%s\n", "Invalid Write" );
	return -1;
}
int invalid_read(Fid_t fd, const char *buf, unsigned int size){
	fprintf(stderr, "%s\n", "Invalid Read" );
	return -1;
}



static file_ops writer_ops={
	// .Open=invalid_open,
	// .Read=invalid_read,
	.Write=pipe_write,
	.Close=write_close
};

static file_ops reader_ops={
	// .Open=invalid_open,
	// .Write=invalid_write,
	.Read=pipe_read,
	.Close=read_close
};





PPCB* createPipe(/*pipe_t* pipe*/){
//Avoid mem issues.
	void* temp= xmalloc(PIPE_SIZE);

	//Declare pipe.
	PPCB* new_pipe=xmalloc(sizeof(PPCB));
	
	//Initialize pipe.
	new_pipe->buffer 	 = temp;

	new_pipe->capacity	 = PIPE_SIZE;
	new_pipe->char_count	= 0;
	new_pipe->r_pointer	= 0;
	new_pipe->w_pointer	= 0;

	new_pipe->full		 = COND_INIT;
	new_pipe->empty	 = COND_INIT;
	new_pipe->pipe_mutex  = MUTEX_INIT;


	new_pipe->read_close=0;
	new_pipe->write_close=0;



	return new_pipe;
}



//Maybe use mutex.

int sys_Pipe(pipe_t* pipe)
{
	

	PPCB* new_pipe=createPipe(/*pipe*/);


	//Allocate FIDs.

	Fid_t pipe_fids[2]; 
	
	//Initialize to -1 to check if all went good despite not being detected in FCB_Reserve.
	pipe_fids[0] = -1;
	pipe_fids[1] = -1;

	int reserve_flag = FCB_reserve(2,pipe_fids,(CURPROC->FIDT));

	if(pipe_fids[0]==-1 || pipe_fids[1]==-1){
		fprintf(stderr, "Did not aquire valid index for pipe files.\n");
		return -1;
	}

	if(reserve_flag==0){
		fprintf(stderr,"Available FCBs not found in the current process. Returning...\n");
		return -1;
	}

	pipe->read = pipe_fids[0];
	pipe->write = pipe_fids[1]; 
	new_pipe->end_files = pipe;


	fprintf(stderr, "Pipe locations : %d , %d \n",pipe->read,pipe->write );
	//Set FCBs.

	FCB* files[2];

	files[0]=get_fcb(pipe->read);
	files[1]=get_fcb(pipe->write);

	for(int i=0;i<2;i++){
		// files[i]=get_fcb(pipe_fids[i]);
		files[i]->streamobj=new_pipe;
		rlnode_init(&files[i]->freelist_node, &files[i]);
	}

	
	files[0]->streamfunc=&reader_ops;
	files[1]->streamfunc=&writer_ops;

	return 0;
}
