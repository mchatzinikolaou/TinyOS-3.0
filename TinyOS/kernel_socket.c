#include "tinyos.h"
#include "util.h"
#include "kernel_dev.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_cc.h"

#ifndef MAX_FILES
#define MAX_FILES MAX_PROC
#endif

#define MAX_SOCKETS MAX_FILES

Mutex connect_mutex=MUTEX_INIT;
Mutex accept_mutex= MUTEX_INIT;

typedef struct listener_variables{
	CondVar conn_cv;
	rlnode req_queue;
	short accepted;	//an eksypiretei idi enan peer, auto einai 0.
}listener_vars;

typedef enum socket_type_e{
	LISTENER,
	PEER,
	UNBOUND,
	BOUND
}socket_type;

typedef struct peer_variables{
	int dummy;
}peer_vars;

typedef struct socket_control_block{
	port_t 	port;
	pipe_t	transmitter;
	pipe_t 	receiver;
	rlnode  wait_node;
	
	//This is the pipe system we use to communicate accross sockets.
	// pipe_t transiever; //transmitter/receiver.
	union{
		listener_vars* l_vars;
		peer_vars* p_vars;
	};
	Fid_t 	socket_id;
	socket_type type;
}SCB;


/*This will be the port descriptor
	Every port has only one listener socket and some other possible sockets
	connected to it.*/

typedef struct port_control_block{
	unsigned int socket_count;
	SCB* SOCKETS[MAX_SOCKETS]; //change to rlist.
	SCB* listener_socket;
	short listener_available;
}port_cb;

//Port table.
port_cb* PORTS[MAX_PORT];

//Singleton constructor.
static short ports_initialized[MAX_PORT]={0};


int socket_write(void* this,const char* string,uint count){	
	return -1;
}

int socket_read(void* this,const char* string,uint count){
	
	return -1;
}

int close_listener(port_t port,SCB* socket){
	PORTS[port]->listener_socket=NULL;
	PORTS[port]->listener_available=0;
	return 0;
}

int socket_close(void* this){
	// kernel_lock();
     	SCB* socket = (SCB*) this;

     	fprintf(stderr, "%s %d\n","Closing socket with id : " ,socket->socket_id);

     	if(socket==NULL){
     		fprintf(stderr, "%s\n", "Close call to unavailable socket." );
     		kernel_unlock();
     		return -1;
     	}

     	int result=-1;
     	switch(socket->type){

     		case LISTENER:
     			fprintf(stderr, "%s\n", "Correctly called listener." );
     			result=close_listener(socket->port,socket);
     			break;
     		default:
     			fprintf(stderr, "%s\n","Socket type not recognized." );
     			result=-1;
     	}
     	// kernel_unlock();
	return result;
}

static file_ops  socket_ops ={
	.Open=NULL,
	.Read=socket_read,
	.Write=socket_write,
	.Close=socket_close
};

void initialize_port(port_t port){

	if(!ports_initialized[port]){
		fprintf(stderr, "%s %d\n","Initializing port", port );
		PORTS[port]= (port_cb*) xmalloc(sizeof(port_cb));
		PORTS[port]->socket_count=0;			
		PORTS[port]->listener_available=0;
		PORTS[port]->listener_socket=NULL;
		for(int j=0;j<MAX_SOCKETS;j++){
			PORTS[port]->SOCKETS[j]=NULL;
		}
	}
	ports_initialized[port]=1;
}

SCB* getSCB(Fid_t sock){
	
	FCB* socket_fcb = get_fcb(sock);

	if(socket_fcb==NULL){
	     	fprintf(stderr,"Invalid fid. Returning..\n");
	     	return NULL;
    	}
    	 SCB* socket = (SCB*)socket_fcb->streamobj;

     	if(socket==NULL){
	     	fprintf(stderr, "%s\n", "Socket unavailable" );
     	}

     	return socket;
}

SCB* acquire_SCB(port_t port,int* success_flag){

	
	SCB* new_socket=NULL;

		
	//If the sockets are full, return null.
 	if(PORTS[port]->socket_count==MAX_FILES){
 		fprintf(stderr, "%s\n", "Run out of sockets");
 		return NULL;
 	}

 	//initialize values for the new socket.
		
 	new_socket= (SCB*) xmalloc(sizeof(SCB));
	new_socket->port=port;

	if(port!=NOPORT)
		new_socket->type=BOUND;
	else
		new_socket->type=UNBOUND;
		
	//reserve fcb
	*success_flag=FCB_reserve(1,&(new_socket->socket_id),(CURPROC->FIDT));
	if(*success_flag==0){
		fprintf(stderr, "%s\n", "Maximum files reached. returning..." );
		return NULL;	
	} 


	FCB* socket_fcb=get_fcb(new_socket->socket_id);
	fprintf(stderr, "%s%lu %s %d\n","Creating socket on addr: ", socket_fcb,"Socket id",new_socket->socket_id  );
	
	socket_fcb->streamobj=new_socket;
	socket_fcb->streamfunc=&socket_ops;
	rlnode_init(&socket_fcb->freelist_node, &socket_fcb);
	rlnode_init(&new_socket->wait_node,&new_socket);

	//Find the first available null spot. Serial search.
	int i=0;
	while(PORTS[port]->SOCKETS[i]!=NULL){
		i++;
	}

	PORTS[port]->SOCKETS[i]=new_socket;
	PORTS[port]->socket_count++;

	return new_socket;
}

//This connects the transievers of both sockets.
void connect_pipes(SCB* socket1,SCB* socket2){
	
	//Create the two pipes.
/*	pipe_t pipe1,pipe2;

	//Keep the control blocks for reference.
	PPCB* ppcb1;
	PPCB* ppcb2;
	ppcb1=createPipe();
	ppcb2=createPipe();

	socket1->transmitter=pipe1;
	socket2->receiver=pipe1;

	socket1->receiver=pipe2;
	socket2->transmitter=pipe2;
	
*/


}

Fid_t sys_Socket(port_t port){

	if(port<0 || port>MAX_PORT){
		fprintf(stderr, "%s\n","Invalid port!" );
		return NOFILE;
	}
	
	initialize_port(port);
	int FT_not_full=0;


	SCB* new_socket=acquire_SCB(port,&FT_not_full);	

	if(new_socket==NULL||FT_not_full==0){
		return NOFILE;
	}else {
		return new_socket->socket_id;
	}
}

int sys_Listen(Fid_t sock){

     SCB* socket = getSCB(sock);

     if(socket==NULL){
     	fprintf(stderr, "%s\n", "Bad fid" );
     	return -1;
     }

     fprintf(stderr, "Trying to make listener on Socket id: %d\n",socket->socket_id );	
     if(socket->type==UNBOUND){
     	fprintf(stderr,"Socket is unbound. sys_Listen returning...\n");
     	return -1;
     }

     if((socket->type==LISTENER) || (socket->type==PEER)){
     	fprintf(stderr,"Requested socket is already peer or listener. Returning...\n");
     	return -1;	
     }

     if((PORTS[socket->port]->listener_socket)!=NULL){
     	fprintf(stderr,"This port already has a listener socket. Returning...\n");
     	return -1;
     }

     //Error conditions exhausted. Continuing...

     socket->l_vars = (listener_vars*) xmalloc(sizeof(listener_vars));
     socket->l_vars->conn_cv = COND_INIT;
     socket->type = LISTENER;

     rlnode_init(&socket->l_vars->req_queue,NULL); // Ayth h grammh petaei segmentation.


     PORTS[socket->port]->listener_socket = socket;
     PORTS[socket->port]->listener_available = 1; 
     socket->l_vars->accepted=0;
     //Handle  PORTS[socket->port]->SOCKETS
     for(int i=0;i<MAX_FILES;i++){
     	if(PORTS[socket->port]->SOCKETS[i]==socket){
     		PORTS[socket->port]->SOCKETS[i]=NULL;
     		break;	
     	}
     }
     return 0;
}		


Fid_t sys_Accept(Fid_t lsock){
	
	fprintf(stderr, "%s\n","Calling accept." );
	SCB* listener = getSCB(lsock);
	if(listener->type!=LISTENER){
		fprintf(stderr, "%d %s\n",listener->socket_id, "socket is not a listener." );
          		return NOFILE;
	}

	//exoume lock mexri na ginei available, diladi na teleiwsei to nima pou ekteleitai idi.
	//molis ksypnisei
	while(is_rlist_empty(&listener->l_vars->req_queue)){
		// Cond_Wait(&connect_mutex,&listener->l_vars->conn_cv); //Block until a request arrives.
	}
	Cond_Signal(&listener->l_vars->conn_cv);
	

	//return a new socket id.


	return NOFILE;
}

int sys_Connect(Fid_t sock, port_t port, timeout_t timeout){

	fprintf(stderr, "%s\n","Calling connect." );
	if(port<=0 || port>MAX_PORT){
		fprintf(stderr, "%s\n","Illegal port!" );
		return -1;
	}

	//get listener on port.
	if(!PORTS[port]->listener_available){
		fprintf(stderr, "%s\n", "Listner unavailable, returning..." );
		return -1;
	}

	SCB* listener=PORTS[port]->listener_socket;


	if(listener==NULL|| listener->type!=LISTENER){
		fprintf(stderr, "%s\n", "Wtf this shouldn't happen. Somebody call 911." );
		return -1;
	}

	SCB* socket = getSCB(sock);
	if(socket==NULL){
		fprintf(stderr, "%s\n","Socket not initialized." );
		 return -1;
	}


	//All ready, signal connection.
	fprintf(stderr, "%s\n","<" );
	rlist_push_back(&listener->l_vars->req_queue,&socket->wait_node);	//An petaksei segfault, den exei ginei init sto wait_node.
	fprintf(stderr, "%s\n",">" );

	//If the timeout is negative, we assume inf. timeout.
	if(timeout>=0){
		int successful_wait = Cond_TimedWait(&connect_mutex,&listener->l_vars->conn_cv,timeout); 
		rlist_remove(&socket->wait_node);
		if(successful_wait==0){
			fprintf(stderr, "%s\n","Connection request Timed out" );
			return -1;
		}
	//Inf. timeout
	}else{ 

		Cond_Wait(&connect_mutex,&listener->l_vars->conn_cv);
		rlist_remove(&socket->wait_node);
	}

	//do stuff.


	return -1;
}

int sys_ShutDown(Fid_t sock, shutdown_mode how){

	return -1;
}

/*// This function could be used if we want to dynamically allocate sockets ???
int bind_socket(port_t port,SCB* socket){
	initialize_port(port);

	if(PORTS[port]->socket_count+1>=MAX_FILES){
		fprintf(stderr, "%s %d\n","Maximum sockets reached on port ." ,port);
		return -1;
	}
	for(int i=0;i<MAX_FILES;i++){
		if(PORTS[i]->SOCKETS[i]!=NULL){
			PORTS[i]->SOCKETS[i]=socket;
			fprintf(stderr, "%s\n","Socket added." );
			PORTS[port]->socket_count++;
			socket->type=BOUND;
			return 0;
		}
	}
	//Shouldn't reach here.
	return -1;
}*/


/*//Set pipes.
	PPCB* transmitter = createPipe(&new_socket->transmitter);
	
	new_socket->transiever[0][0]->streamobj= transmitter;
	new_socket->transiever[0][0]->streamfunc= &reader_ops;
	new_socket->transiever[0][1]->streamobj= transmitter;
	new_socket->transiever[0][1]->streamfunc= &writer_ops;
	

	PPCB* receiver	=  createPipe(&new_socket->receiver);
	new_socket->transiever[1][0]->streamobj  = receiver;
	new_socket->transiever[1][0]->streamfunc= &reader_ops;
	new_socket->transiever[1][1]->streamobj  = receiver;
	new_socket->transiever[1][1]->streamfunc= &writer_ops;*/