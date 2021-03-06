/*
 * YAMC - Yet Another MQTT Client library
 *
 * yamc_runner_socket.c - Quick and dirty Unix TCP socket wrapper. Allows YAMC testing on PC.
 *
 * Author: Michal Lower <keton22@gmail.com>
 *
 * All rights reserved 2017
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>

#include "yamc.h"
#include "yamc_port.h"

#include <yamc_debug_pkt_handler.h> //example user defined packet handlers, dump everything to console

//if true server has disconnected, exit
volatile uint8_t exit_now = false;

//server socket handle
static int server_socket = -1;

//read_sock_thr thread handle
static pthread_t rx_tid;

static yamc_instance_t yamc_instance;

//timeout timer settings
#define YAMC_TIMEOUT_S		30	//seconds
#define YAMC_TIMEOUT_NS		0	//nanoseconds

//timeout timer handle
static timer_t timeout_timer;

//timeout timer signal handler
static void timeout_handler(sigval_t sigval)
{
	YAMC_UNUSED_PARAMETER(sigval);

	YAMC_ERROR_PRINTF("Timeout!\n");
	fflush(stderr);

	exit(-1);

}

//start/prolong timeout timer wrapper
static void timeout_pat(void)
{
	struct itimerspec its;
	memset(&its, 0, sizeof(struct itimerspec));
	its.it_value.tv_sec=YAMC_TIMEOUT_S;
	its.it_value.tv_nsec=YAMC_TIMEOUT_NS;

	int err_code=timer_settime(timeout_timer,0,&its,NULL);
	if(err_code<0)
	{
		YAMC_ERROR_PRINTF("Timer error\n");
		exit(-1);
	}
}

//stop timeout timer
static void timeout_stop(void)
{
	struct itimerspec its;
	memset(&its, 0, sizeof(struct itimerspec));

	int err_code=timer_settime(timeout_timer,0,&its,NULL);
	if(err_code<0)
	{
		YAMC_ERROR_PRINTF("Timer error\n");
		exit(-1);
	}
}

//timeout timer setup
static void setup_timer(void)
{
	int err_code=-1;

	struct sigevent sev;

	memset(&sev, 0, sizeof(struct sigevent));

	sev.sigev_signo=SIGRTMIN;					//signal type
	sev.sigev_notify=SIGEV_THREAD;				//call timeout handler as if it was starting a new thread
	sev.sigev_notify_function=timeout_handler;	//set timeout handler


	if ((err_code=timer_create(CLOCK_REALTIME, &sev, &timeout_timer))<0)
	{
		YAMC_ERROR_PRINTF("ERROR setting up timeout timer: %s\n", strerror(err_code));
		exit(0);
	}

}

//write to socket wrapper
static int socket_write_buff(uint8_t * buff, uint32_t len)
{
	int n = write(server_socket, buff, len);
	if (n < 0)
		YAMC_ERROR_PRINTF("ERROR writing to socket:%s\n", strerror(n));
	return n;
}

static void disconnect_handler(void)
{
	YAMC_ERROR_PRINTF("yamc requested to drop connection!\n");
	exit(-1);
}

//receive data from socket thread
static void *read_sock_thr(void* p_ctx)
{
	YAMC_UNUSED_PARAMETER(p_ctx);

	//buffer for incoming data
	uint8_t rx_buff[10];

	//how many bytes were received in single read operation or read() error code
	int rx_bytes = 0;

	do
	{
		//zero rx buffer
		memset(rx_buff, 0, sizeof(rx_buff));

		rx_bytes = read(server_socket, rx_buff, sizeof(rx_buff));

		//there was error code thrown by read()
		if (rx_bytes < 0)
		{
			YAMC_ERROR_PRINTF("TCP read() error: %s\n", strerror(rx_bytes));
			exit_now=true;
			pthread_exit(&rx_bytes);
		}

		//process buffer here
		if(rx_bytes > 0) yamc_parse_buff(&yamc_instance, rx_buff, rx_bytes);

	} while (rx_bytes > 0);

	exit_now=true;
	return NULL;
}

//connect socket to specified host and port
static void setup_socket(int* p_socket, char *hostname, int portno)
{
	//server socket address struct
	struct sockaddr_in serv_addr;

	//host data struct pointer
	struct hostent *server;

	int err_code = 0;

	memset(&serv_addr, 0, sizeof(serv_addr));

	//create socket
	*p_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (*p_socket < 0)
		YAMC_ERROR_PRINTF("ERROR opening socket\n");

	//perform host name lookup
	server = gethostbyname(hostname);
	if (server == NULL)
	{
		YAMC_ERROR_PRINTF("ERROR, no such host\n");
		exit(0);
	}

	//prepare data for connect()

	//this is Internet (TCP) connection
	serv_addr.sin_family = AF_INET;

	//copy connection IP address
	memcpy(server->h_addr, &serv_addr.sin_addr.s_addr, server->h_length);

	//set connection port, convert number format to network byte order
	serv_addr.sin_port = htons(portno);

	//connect
	if ((err_code = connect(*p_socket, (struct sockaddr *) &serv_addr,
			sizeof(serv_addr))) < 0)
	{
		YAMC_ERROR_PRINTF("ERROR connecting: %s\n", strerror(err_code));
		exit(0);
	}
}

int main(int argc, char *argv[])
{
	int portno;

	if (argc < 3)
	{
		YAMC_ERROR_PRINTF("usage %s hostname port\n", argv[0]);
		exit(0);
	}

	//setup timeout timer
	setup_timer();

	portno = atoi(argv[2]);

	//setup socket and connect to server
	setup_socket(&server_socket, argv[1], portno);

	//Create receive data thread
	YAMC_DEBUG_PRINTF("Connected launching rx thread...\n");

	//show startup messages on console
	fflush(stdout);

	yamc_handler_cfg_t handler_cfg={
			.disconnect=disconnect_handler,
			.write=socket_write_buff,
			.timeout_pat=timeout_pat,
			.timeout_stop=timeout_stop,
			.pkt_handler=yamc_debug_pkt_handler_main
	};

	yamc_init(&yamc_instance, &handler_cfg);

	//enable pkt_handler for following packet types
	yamc_instance.parser_enables.CONNACK=true;
	yamc_instance.parser_enables.PUBLISH=true;
	yamc_instance.parser_enables.PUBACK=true;
	yamc_instance.parser_enables.PINGRESP=true;
	yamc_instance.parser_enables.SUBACK=true;

	//create thread
	pthread_create(&rx_tid, NULL, read_sock_thr, NULL);

	//process sending data to socket here...


	//send MQTT connect packet
	uint8_t connect_msg[]={
			//fixed header
			0x10,						//packet type + flags
			16,							//remaining length

			//variable header

			0,4,'M','Q','T','T',		//protocol name
			4,							//protocol level
			//connect flags
			2, 							//clean session
			//keep alive
			0, 30, 						//30 s keepalive

			//payload
			0,4,'T','e','s','t' 		//client Id

	};
	socket_write_buff(connect_msg, sizeof(connect_msg));

	//subscribe to topic 'test/#'
	uint8_t subscribe_msg[]={
			0x82,							//packet type and QoS flag
			11,								//remaining length
			0,11,							//packet identifier
			0,6,'t','e','s','t','/','#',	//topic to subscribe to
			0x01							//requested QoS
	};
	socket_write_buff(subscribe_msg, sizeof(subscribe_msg));

	//repeatedly send ping request to keep connection alive
	while(!exit_now)
	{
		uint8_t pingreq_msg[]={
				//fixed header only

				12 << 4,	//packet type
				0			//remaining length

		};

		socket_write_buff(pingreq_msg, sizeof(pingreq_msg));

		sleep(25);
	}

	//cleanup
	close(server_socket);
	exit(0);
}
