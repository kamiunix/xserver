/* 
 * File: sws.c
 * Author: Alex Brodsky
 * Purpose: This file contains the implementation of a simple web server.
 *          It consists of two functions: main() which contains the main 
 *          loop accept client connections, and serve_client(), which
 *          processes each client request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h> 
#include <semaphore.h> 

#include "network.h"
#include "datastruct.h"

#define MAX_HTTP_SIZE 8192                 /* size of buffer to allocate */
#define MLFB_FIRST 8192
#define MLFB_SECOND 65536

/* struct to hold cli arguments passed to threads */
struct args {
	struct linkedlist* list;
	int port;
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;


/* This function takes a file handle to a client, reads in the request, 
 *    parses the request, and sends back the requested file.  If the
 *    request is improper or the file is not available, the appropriate
 *    error is sent back.
 * Parameters: 
 *             fd : the file descriptor to the client connection
 * Returns: None
 */
static void check_client( struct client* client ) {
	static char *buffer;                              /* request buffer */
	char *req = NULL;                                 /* ptr to req file */
	char *brk;                                        /* state used by strtok */
	char *tmp;                                        /* error checking ptr */
	int len;                                          /* length of data read */

	if( !buffer ) {                                   /* 1st time, alloc buffer */
		buffer = malloc( MAX_HTTP_SIZE );
		if( !buffer ) {                                 /* error check */
			perror( "Error while allocating memory" );
			abort();
		}
	}

	memset( buffer, 0, MAX_HTTP_SIZE );
	if( read( client->fd, buffer, MAX_HTTP_SIZE ) <= 0 ) {    /* read req from client */
		perror( "Error while reading request" );
		abort();
	} 

	/* standard requests are of the form
	 *   GET /foo/bar/qux.html HTTP/1.1
	 * We want the second token (the file path).
	 */
	tmp = strtok_r( buffer, " ", &brk );              /* parse request */
	if( tmp && !strcmp( "GET", tmp ) ) {
		req = strtok_r( NULL, " ", &brk );
	}

	if( !req ) {                                      /* is req valid? */
		len = sprintf( buffer, "HTTP/1.1 400 Bad request\n\n" );
		write( client->fd, buffer, len );                       /* if not, send err */
	} else {                                          /* if so, open file */
		req++;                                          /* skip leading / */
		client->fin = fopen( req, "r" );                        /* open file */
		if( !client->fin ) {                                    /* check if successful */
			len = sprintf( buffer, "HTTP/1.1 404 File not found\n\n" );  
			write( client->fd, buffer, len );                     /* if not, send err */
			printf("404 first write: %s\n",buffer);
		} else {                                        /* if so, send file */
			/* if so, send file */
			len = sprintf( buffer, "HTTP/1.1 200 OK\n\n" );/* send success code */
			write( client->fd, buffer, len );

			//check size of file and rewind fin
			fseek(client->fin,0,SEEK_END);
			len = ftell(client->fin);
			rewind(client->fin);
			client->rem = len;
		}
	}
}

static int serve_client( struct client* client, int mss ) {
  static char *buffer;                              /* request buffer */
  int len;                                          /* length of data read */
  int n;                                            /* amount to send */

  if( !buffer ) {                                   /* 1st time, alloc buffer */
    buffer = malloc( MAX_HTTP_SIZE );
    if( !buffer ) {                                 /* error check */
      perror( "error allocating memory" );
      abort();
    }
  }

  n = mss;                                     /* compute send amount */
  if( !n ) {                                         /* if 0, we're done */
    return 0;
  } else if( client->rem && ( client->rem < n ) ) {        /* if there is limit */
    n = client->rem;                                    /* send upto the limit */
  }
  client->rem = client->rem - n;
  client->pos = client->pos + n;				/* remember send size */

  do {                                              /* loop, read & send file */
    len = n < MAX_HTTP_SIZE ? n : MAX_HTTP_SIZE;    /* how much to read */
    len = fread( buffer, 1, len, client->fin );         /* read file chunk */
    if( len < 1 ) {                                 /* check for errors */
      perror( "error reading file" );
      return 0;
    } else if( len > 0 ) {                          /* if none, send chunk */
      len = write( client->fd, buffer, len );
      if( len < 1 ) {                               /* check for errors */
        perror( "error writing to client" );
        return 0;
      }
      n -= len;
    }
  } while( ( n > 0 ) && ( len == MAX_HTTP_SIZE ) );  /* the last chunk < 8192 */
  
  if (client->rem == 0) {
	  close(client->fd);
	  fclose(client->fin);
  }

  return 1;
}

/* loop function to receive clients */
void *get_clients( void* vargs) {
	struct args *args = (struct args*) vargs;
	
	network_init( args->port );                             /* init network module */

	int fd;
	int flag = 1;
	struct client *client;
	for( ;; ) {                                       /* main request loop */
		network_wait();                                 /* wait for clients */

		for( fd = network_open(); fd >= 0; fd = network_open() ) { /* get clients */
			if (flag) {
				printf("got client\n");
				client = (struct client*) malloc(sizeof(struct client));
				initClient(client);
				client->fd = fd;
				check_client(client);  /* process each client's request */
				flag = 0;
			}

			//lock critical section
			pthread_mutex_lock(&lock);
			insertFirst(args->list, client);
			flag = 1;
			pthread_mutex_unlock(&lock);
			//unlock critical section
		}
	}
}

/* loop function to process clients using SJF */
void *proc_sjf( void* list ) {
	printf("Commencing SJF scheduling\n");
	int flag = 0;
	for( ;; ) {                                       /* main SJF loop */
		while(length(list) > 0) {

			//lock critical section
			pthread_mutex_lock(&lock);
			sort((struct linkedlist*) list);				/* sort to find shortest job */
			struct client *client = deleteFirst((struct linkedlist*) list);
			flag = 1;
			pthread_mutex_unlock(&lock);
			//unlock critical section

			//send file to client
			if (flag) {
				int size = client->rem;
				serve_client(client, client->rem);
				printf("%p sent %d bytes of file to\n",&client ,size);
				flag = 0;
			}
		}
	}
}

/* loop function to process clients using RR */
void *proc_rr( void* list ) {
	printf("Commencing RR scheduling\n");
	int flag = 0;
	int quantum = 8192; 
 	
	for ( ;; ) {
		
		while(length(list) > 0) {
			//lock critical section
			pthread_mutex_lock(&lock);
			
			struct client *client = deleteFirst((struct linkedlist*) list);
			//read from file in quantum
						
			flag = 1;
			pthread_mutex_unlock(&lock);
			//unlock critical section

			//send file to client
			if (flag) {
				
				if(client->rem < quantum){					
					serve_client(client, client->rem);
				}
				else{
					serve_client(client, quantum);
					insertLast((struct linkedlist*) list,client);
				}
				flag = 0;
			}
		}
	}
}

/* loop function to process clients using MLFB */
void *proc_mlfb( void* list ) {
	printf("Commencing MLFB scheduling\n");
	for ( ;; ) {
		//do stuff
	}
}

/* This function is where the program starts running.
 *    The function first parses its command line parameters to determine port #
 *    Then, it initializes, the network and enters the main loop.
 *    The main loop waits for a client (1 or more to connect, and then processes
 *    all clients by calling the seve_client() function for each one.
 * Parameters: 
 *             argc : number of command line parameters (including program name
 *             argv : array of pointers to command line parameters
 * Returns: an integer status code, 0 for success, something else for error.
 */
int main( int argc, char **argv ) {
	char* scheduler;
	int port;
	int threads;
	if (argc < 2) {
		port = 38080;
		scheduler = "SJF";
		threads = 1;
	}
	else if (argc < 3) {
		port = (int) strtol(argv[1], (char**)NULL,10);
		scheduler = "SJF";
		threads = 1;
	}
	else if (argc < 4) {
		port = (int) strtol(argv[1], (char**)NULL,10);
		char *scheduler_list[3];
		scheduler_list[0] = "SJF";
		scheduler_list[1] = "RR";
		scheduler_list[2] = "MLFB";
		for(int i=0;i<3;i++) {
			if (strcmp(argv[2],scheduler_list[i]) == 0) {
				scheduler = argv[2];
			}
		}
		threads = 1;
	}
	else {
		port = (int) strtol(argv[1], (char**)NULL,10);
		char *scheduler_list[3];
		scheduler_list[0] = "SJF";
		scheduler_list[1] = "RR";
		scheduler_list[2] = "MLFB";
		for(int i=0;i<3;i++) {
			if (strcmp(argv[2],scheduler_list[i]) == 0) {
				scheduler = argv[2];
			}
		}
		threads = (int) strtol(argv[3], (char**)NULL,10);
	}

	printf("port: %d scheduler: %s threads: %d\n",port,scheduler,threads);

	if (scheduler == NULL) {
		printf("Unrecognized scheduling algorithm\n Choices are : SJF RR MLFB\n");
		exit(1);
	}

	struct linkedlist *list = (struct linkedlist*) malloc(sizeof(struct linkedlist));
	initList(list);

	struct args *args = (struct args*) malloc(sizeof(struct linkedlist));
	args->port = port;                                    /* server port # */
	args->list = list;

	/* threads to receive requests and send file data */
	pthread_t get_reqs;
	pthread_t *send_files = (pthread_t*)malloc(sizeof(pthread_t) * threads); 

	/* create request parsing thread */
	pthread_create(&get_reqs, NULL, get_clients, (void*) args);

	/* create SJF thread */
	if (strcmp(scheduler, "SJF") == 0) {
		for (int i=0; i<threads; i++) {
			pthread_create(&send_files[i], NULL, proc_sjf, (void*) list);
		}
	}
	/* create RR thread */
	else if (strcmp(scheduler, "RR") == 0) {
		for (int i=0; i<threads; i++) {
			pthread_create(&send_files[i], NULL, proc_rr, (void*) list);
		}
	}
	/* create MLFB thread */
	else {
		for (int i=0; i<threads; i++) {
			pthread_create(&send_files[i], NULL, proc_mlfb, (void*) list);
		}
	}

	/* join threads*/
	pthread_join(get_reqs, NULL);
	for (int i=0; i<threads;i++) {
		pthread_join(send_files[i], NULL);
	}
}

