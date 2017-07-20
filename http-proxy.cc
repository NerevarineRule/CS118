/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <string>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <algorithm>
#include <pthread.h>
#include <fcntl.h>
#include <map>
#include <time.h>
#include <assert.h>

#include "http-request.h"
#include "http-response.h"

#define MAX_BUF_SIZE 2048
#define MAX_THREAD 20
#define PROXY_PORT "14886"
#define REMOTE_SERVER "24894"
#define REMOTE_SERVER2 "34889"

using namespace std;
int thread_count;
typedef map<string,string> webCache;

//pthread structure to use

typedef struct
{
 pthread_mutex_t* mutex; //mutex to block other threads and ensure atomic operation
 int cfd; //client file descriptor
 webCache* wcache; //all threads will share the same cache pointed by this field
 const char* sport; //server port number

} proxy_thread_t;

//function to determine whether the requested file has expired or not 
bool isExpired(string exp_date)
{
	string e_date;
	string day2,mon2,year2,h2,m2,s2;
	string nmon2;
	long int datenum,edatenum;

	//inputted expiration date is in HTTP date format. Below line converts
	//HTTP date format into our own string format, which will make our comparison
	//process more convenient	
	//using our own time calculation method, since mktime is sometimes unstable
	day2 = exp_date.substr(5,2);
	mon2 = exp_date.substr(8,3);
	year2 = exp_date.substr(11,5);
	h2 = exp_date.substr(17,2);
	m2 = exp_date.substr(20,2);
	s2 = exp_date.substr(23,2);

	//converting from month name to month value	
	if (mon2 == "Jan")
	{nmon2 = "01";}
	else if (mon2 == "Feb")
	{nmon2 = "02";}
	else if (mon2 == "Mar")
	{nmon2 = "03";}
	else if (mon2 == "Apr")
	{nmon2 = "04";}
	else if (mon2 == "May")
	{nmon2 = "05";}
	else if (mon2 == "Jun")
	{nmon2 = "06";}
	else if (mon2 == "Jul")
	{nmon2 = "07";}
	else if (mon2 == "Aug")
	{nmon2 = "08";}
	else if (mon2 == "Sep")
	{nmon2 = "09";}
	else if (mon2 == "Oct")
	{nmon2 = "10";}
	else if (mon2 == "Nov")
	{nmon2 = "11";}
	else if (mon2 == "Dec")
	{nmon2 = "12";}
	
	//adding NULL terminator just in case
	e_date = year2+nmon2+day2+h2+m2+s2+'\0';
	

	time_t now = time(0); //obtain current local time
	char ltime[15];
	string loctime;
	//then convert local time from time_t to string representation
	if (strftime(ltime,sizeof(ltime),"%Y%m%d%H%M%S",localtime(&now)) == 0)
	{ assert(0);}
	loctime.append(ltime);

	//convert string to long int
	datenum = atol(loctime.c_str());
	edatenum = atol(e_date.c_str());
	
//	printf("local time: %ld expire time: %ld \n",datenum,edatenum); 	
	
	//compare local time with expiration time.
	if (datenum > edatenum)
	{ return true;}
	else
	{ return false;}
}


//accepting incoming connection to the proxy
int socketAccept(int fd_temp)
{
   int socketID;
   struct sockaddr_in req_addr; //address of requesting peer
   socklen_t addr_len = sizeof(req_addr); // size of requesting address
   socketID = accept(fd_temp,(struct sockaddr *) &req_addr, &addr_len);
   if (socketID == -1 && (errno == EINTR || errno == EAGAIN)) //if invalid socket ID or no data available right now, try again later
   { return -1;} 
	
 
   return socketID;

}

//listening on the proxy port
int serverListen(const char* port)
{

 	struct addrinfo addrInfo,*addr_ptr;
	int sock_fd;
	int yes = 1;
	memset(&addrInfo,0,sizeof(addrInfo));
	addrInfo.ai_family = AF_UNSPEC; //using unspecified address family (either IPv4 or IPv6)
	addrInfo.ai_flags = AI_PASSIVE; //intends to use the returned socket address structure in a call to the bind function
 	addrInfo.ai_socktype = SOCK_STREAM; //SOCK_STREAM type socket provides sequenced,reliable,two-way connection 
	
	//return an addrinfo structure that can be used in a call to bind/connect
	int addr_status = getaddrinfo(NULL,port,&addrInfo,&addr_ptr);
	if (addr_status != 0)
	{ return -1;}

	struct addrinfo *ptr = addr_ptr;
	while (ptr != NULL)
	{
		//obtain socket file descriptor
		sock_fd = socket(addr_ptr->ai_family,addr_ptr->ai_socktype,addr_ptr->ai_protocol);
		if (sock_fd < 0) //if socket file descriptor is not available, move to the next available address
		{ 
			ptr = ptr->ai_next;
			continue;
		}
		
		//set socket option
		//SOL_SOCKET: using SOL_SOCKET for options that are protocol independent
		//SO_REUSEADDR: indicates that the rules used in validating addresses supplied in a bind() call should allow reuse of local addresses
		int opt_status = setsockopt(sock_fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(int));
		if (opt_status == -1)
		{ exit(1);}
	
		//assigns address (addr_ptr->ai_addr) to the socket referred to by sock_fd
		int bind_status = bind(sock_fd,addr_ptr->ai_addr,addr_ptr->ai_addrlen);
		if (bind_status != 0)
		{ 
			close(sock_fd);
		  	ptr = ptr->ai_next;
		  	continue;
		}
		break;
	
	}
	//if ptr is NULL at this point, we failed to bind
	if (ptr == NULL)
	{ return -1;}

	//listen on the socket. we can allow at most 100 connections according to the spec
	if (listen(sock_fd,100) == -1)
	{ exit(1);}

	return sock_fd;
}
int connectToServer(const char* hostname,const char* port)
{
 	struct addrinfo addrInfo,*addr_ptr;
	int sock_fd;
	
	memset(&addrInfo,0,sizeof(addrInfo));
	addrInfo.ai_family = AF_UNSPEC; //using unspecified address family (either IPv4 or IPv6)
 	addrInfo.ai_socktype = SOCK_STREAM; //SOCK_STREAM type socket provides sequenced,two-way,reliable connection
	
	//return an addrinfo structure that can be used in a call to bind/connect
	int addr_status = getaddrinfo(hostname,port,&addrInfo,&addr_ptr);
	if (addr_status != 0)
	{ return -1;}
	
	struct addrinfo *ptr = addr_ptr;
	while (ptr != NULL)
	{
		//obtain socket file descriptor
		sock_fd = socket(addr_ptr->ai_family,addr_ptr->ai_socktype,addr_ptr->ai_protocol);
		if (sock_fd < 0)
		{
			ptr = ptr->ai_next;
			continue;
		}
		
		//connect the socket to the specified address (ptr->ai_addr)
		if (connect(sock_fd,ptr->ai_addr,ptr->ai_addrlen) < 0)
		{
			 ptr = ptr->ai_next;
			 close(sock_fd);
		 	 continue;
		}
		break;
	}
	
	//if ptr is NULL, this means that we could not connect. 
	if (ptr == NULL)
	{ return -1;}
	
	//now that we have connected, addr_ptr is not needed. free its space
	freeaddrinfo(addr_ptr);
	return sock_fd;

}

/*
int getResponse(int socket_id,string &resp)
{
	int nbytes;
	while (1)
	{
		char temp;	
		nbytes = recv(socket_id,&temp,1,0);
		if (memmem(resp.c_str(),resp.length(),"\r\n\r\n",4) != NULL)
		{
			resp.append(&temp,nbytes);
			HttpResponse temp_resp;
			temp_resp.ParseResponse(resp.c_str(),resp.length());
			string rsize = temp_resp.FindHeader("Content-Length");
			int sizeInt = atoi(rsize.c_str());
			char *temp_buf2 = new char[sizeInt];
			nbytes = recv(socket_id,temp_buf2,sizeInt,0);
			resp.append(temp_buf2,nbytes);
			free(temp_buf2);
			return 0;

		}

		if (nbytes < 0)
		{
			perror("The following receiving error occured");
			return -1;
		}
		
		if (nbytes == 0)
		{ return -1;}
		
		resp.append(&temp,nbytes);
	}
	return -1;
}
*/

//function to obtain response from the server specified by socket_id
//HTTP response will be stored in resp
//using select to timeout to prevent recv blocking
int getResponse2(int socket_fd,string& resp)
{
	//set socket in non-blocking mode
	fcntl(socket_fd,F_SETFL,O_NONBLOCK);
	int nbytes,sel;
	while (1)
	{
		//socket is used for reading. 
		//no writing necessary, therefore writing file descriptor set field is set NULL in select
		struct timeval wait; 
		wait.tv_sec = 2; //timeout in second
		wait.tv_usec = 0; //timeout in microsecond
		fd_set readfds; //read file descriptor set
		FD_ZERO(&readfds); //initialize read file descriptor set in every iteration
		FD_SET(socket_fd,&readfds); //and readd the socket file descriptor back to read file descriptor set
		FD_SET(0,&readfds); //include standard input in the read file descriptor set as well

		sel = select(socket_fd+1,&readfds,NULL,NULL,&wait);
		char temp;	
		if (sel < 0)
		{ continue;}
		
		else if (sel == 0) //select timeout. return 
		{
			return 1;
		}
		//if socket_id is part of the set
		if (FD_ISSET(socket_fd,&readfds))
		{
			
			FD_CLR(socket_fd,&readfds); //remove a given file descriptor from the set
			nbytes = recv(socket_fd,&temp,1,0); //reading in one byte at a time from the server
			if (nbytes < 0) //error has occurred
			{
				perror("The following receiving error occured");
				return -1;
			}
		
			if (nbytes == 0) //if no more byte is available at the server side, return 0
			{return 0;}
			
			resp.append(&temp,1); //and append it to the response string
		
		}
		
		
	
	}
	return -1;
}


//establishing HTTP connection
int http_connection(int fd,pthread_mutex_t *mutex,webCache* cache,const char* sport)
{
	string response,http_buffer,error,host_addr,version,connection_type,hostname,pathname,mod_date,res_buffer;
	char* req_buffer;
	size_t req_length;
	int socket_id;
	//optain http request data format.
	//http data format ends with two sequences of \r\n	

	//atomically increment thread number
	pthread_mutex_lock(mutex);
	thread_count++;
	pthread_mutex_unlock(mutex);
		
	while (memmem(http_buffer.c_str(),http_buffer.length(),"\r\n\r\n",4) == NULL)
	{
		
		//receive data and append it to the string.
		char temp_buf[MAX_BUF_SIZE];
		if (recv(fd,temp_buf,sizeof(temp_buf),0) < 0)
		{return -1;}
	
		http_buffer.append(temp_buf);

	}


	//obtain http request
	HttpRequest http_req;
	try
	{ http_req.ParseRequest(http_buffer.c_str(),http_buffer.length());}
	catch (ParseException ex)
	{ 
	 	if (strcmp(ex.what(),"Request is not GET") == 0) //request is not get
		{ error = "HTTP/1.1 501 Not Implemented\r\n\r\n";}
		else  //if request is GET and receives a parse exception then it is a bad request.
		{ error = "HTTP/1.1 400 Bad Request\r\n\r\n";}

		if (send(fd,error.c_str(),error.length(),0) == -1)
		{ perror("The following send error occured\n");}
	}
	
	
	//resolving the name
	hostname = http_req.GetHost(); //get host name
        pathname = hostname + http_req.GetPath(); //get full path to the file		
	webCache::iterator it = cache->find(pathname); //find if path is stored in the cache
	
	if (it != cache->end()) //cache contains the response
	{
		//printf("data has been cached before!\n");

		//extract expiration date from cached response
		HttpResponse tempResp;
		tempResp.ParseResponse(it->second.c_str(),it->second.length());
		string exp_date = tempResp.FindHeader("Expires"); //find expiration date		
		mod_date = tempResp.FindHeader("Last-Modified"); //find last modified date
	
		//check whether file has expired or not
		if (isExpired(exp_date)) //the file has been modified!
		{
			//printf("\n\nthe file has been modified!\n\n");
			
			http_req.AddHeader("If-Modified-Since",mod_date); //add a new field to the header
			//If-Modified_Since date corresponds to the Last-Modified date from the cached response
		
			//format request
			req_length = http_req.GetTotalLength();
			req_buffer = (char*)malloc(req_length);
			http_req.FormatRequest(req_buffer); 
		

			//connect to the server
			socket_id = connectToServer(hostname.c_str(),REMOTE_SERVER);
			
			//forward the newly formatted request to the host	
			if (send(socket_id,req_buffer,req_length,0) == -1)
			{
				perror("The following send error occured");
				close(socket_id); //close open file descriptor
				free(req_buffer); //deallocate dynamically allocated variable
				return -1;	
			}	

			//obtain response
			if (getResponse2(socket_id,response) < 0) //cannot obtain response
			{
				close(socket_id);
				free(req_buffer);
				exit(1);
			}		
			
			//parse response and see if status is Not Modified or not		
			HttpResponse http_res;
			http_res.ParseResponse(response.c_str(),response.length());
			string statusMsg = http_res.GetStatusMsg();	
		
			if (!statusMsg.compare("Not Modified")) //if response is not modified, send cached HTTP response to the client
			{
				//printf("\n\n\ncached element has not expired yet.\n\n\n");
				response = it->second;
			}

			//otherwise, store the newly obtained HTTP response in the cache
			else
			{
				//printf("\n\n\ncached element has expired. fetching latest version!\n\n\n"); 
				pthread_mutex_lock(mutex);
				cache->insert(pair<string,string>(pathname,response));
				pthread_mutex_unlock(mutex);
			}

			//then send the latest response to the client
			if (send(fd,response.c_str(),response.length(),0) == -1)
			{
				perror("The following send error occured");
				return -1;
			}

			//printf("latest response: \n %s \n",response.c_str());	

				
			string connection_status = http_req.FindHeader("Connection");
			if (connection_status.compare("close") == 0) //if HTTP request requests closing the connection 
			{
				//close both server and client connection
				//printf("file descriptor closed\n");
				close(socket_id);
				close(fd);
				return 0;
			}
			
			else //if no close request is found, keep client connection open 
			{
				
				//but close server connection
				close(socket_id);
				return http_connection(fd,mutex,cache,sport);
			}		
			
		}
		
		else //the file has not been modified. simply use cached response
		{
			//notice that we did not need to establish connection between the proxy and the server
			//since we already have cached response
			
			//printf("\n\n The file has not been modified!\n\n");
			
			response = it->second; //use cached response
			if (send(fd,response.c_str(),response.length(),0) == -1)
			{
				perror("The following send error occured");
				return -1;
			}

			//printf("cached response: \n %s \n",response.c_str());	

			string connection_status = http_req.FindHeader("Connection");
			if (connection_status.compare("close") == 0) //if HTTP request requests closing the connection 
			{
				//close client connection
				//printf("file descriptor closed\n");
				close(fd);
				return 0;
			}
					
			
			else
			{
				//if not, don't close client connection and start from the beginning of the function to
				//process incoming requests on the persistent connection		
				return http_connection(fd,mutex,cache,sport);
			}	
			
		
		} 
	
	}
	else //cache does not contain the response. need to send the request to the server 
	{ 
		//format request
		try
		{req_length = http_req.GetTotalLength();}
		catch (ParseException ex)
		{
			if (!strcmp(ex.what(),"Only GET method is supported"))
			{error = "HTTP/1.1 501 Not Implemented\r\n\r\n";}
		}
		req_buffer = (char*)malloc(req_length);
		http_req.FormatRequest(req_buffer);	

		//establish connection between proxy and the server
		socket_id = connectToServer(hostname.c_str(),REMOTE_SERVER);

		if (send(socket_id,req_buffer,req_length,0) == -1)
		{
			//if send error occurs, close server and client connection
			perror("The following send error occured");
			close(socket_id); 
			close(fd);
			free(req_buffer); 
			return -1;	
		}	
		
		//obtain response from the server
		if (getResponse2(socket_id,response) < 0) //cannot obtain response
		{
			//if response cannot be obtained, close server and client connection and exit
			close(socket_id);
			close(fd);
			free(req_buffer);
			exit(1);
		}

		
		//store the HTTP response in the cache and increment thread count
		pthread_mutex_lock(mutex);
		cache->insert(pair<string,string>(pathname,response));
		pthread_mutex_unlock(mutex);

		//send response back to the client
		if (send(fd,response.c_str(),response.length(),0) == -1) //if send error occurs
		{
			//close server and client connection
			close(socket_id);
			close(fd);
			perror("The following send error occured");
			free(req_buffer);
			return -1;
		}		
 	
		//check Connection header and see if it is "close"
		string connection_status = http_req.FindHeader("Connection");
		if (connection_status.compare("close") == 0) //if client requests closing the connection 
		{
			//close client connection
			//printf("file descriptor closed\n");
			close(fd);
			return 0;
		}
		
	
		//waiting for couple seconds before closing the connection between the proxy and the server
		//so that subsequent transmission between the proxy and the server can reuse the connection	
		fd_set tstat;
		struct timeval ttime;
		int vset;
		while (1)
		{
			ttime.tv_sec = ttime.tv_usec = 2;
			FD_ZERO(&tstat);
			FD_SET(socket_id,&tstat);
			vset = select(socket_id+1,&tstat,NULL,NULL,&ttime);
			
			if (vset < 0)
			{ continue;}
			else if (vset == 0)
			{ break;}
		}
		close(socket_id);
		return 0;

	}

}

//thread function. threads will invoke http_connection via this thread function
void* thread_http_connection(void* thread_param)
{
       proxy_thread_t* temp = (proxy_thread_t *) thread_param;
       http_connection(temp->cfd,temp->mutex,temp->wcache,temp->sport);
       free(thread_param);
       return NULL;
}

int main (int argc, char *argv[])
{
  int socket_fd = serverListen(PROXY_PORT); 
  if (socket_fd < 0)
  {
	exit(1);
  }

  //initialize cache and mutexes 
  webCache local_cache; 
  thread_count = 0;
  pthread_mutex_t temp_mutex;
  pthread_mutex_init(&temp_mutex,NULL);
 
  int cfd;
  fcntl(socket_fd,F_SETFL,O_NONBLOCK); //set socket file descriptor to non_blocking option
  int sel;
  fd_set waitset;
  struct timeval tval;
  //use select to timeout after a certain amount of inactivity 
  while(1)
  {
	FD_ZERO(&waitset);
	FD_SET(socket_fd,&waitset);
	tval.tv_sec = tval.tv_usec = 20; //if there is no incoming connection for 20 seconds, time out.
	sel = select(socket_fd+1,&waitset,NULL,NULL,&tval);		

	if (sel == 0) //timeout after 20 seconds of inactivity 
	{ return 0;}

	if (sel < 0)
	{continue;}
	
	if (FD_ISSET(socket_fd,&waitset)) {
	cfd = socketAccept(socket_fd); //accept the client connection and obtain the client file descriptor
	if (cfd < 0) //if connection cannot be accepted at this point, try again later 
	{continue;}
	
	if (thread_count >= MAX_THREAD)
	{
		printf("cannot support more than 20 connections!\n");
	//	thread_count--;
		continue;
	}

	//initializing thread parameters
	proxy_thread_t *proxy_thread = (proxy_thread_t*)malloc(sizeof(proxy_thread_t));
	proxy_thread->wcache = &local_cache; //point to the shared cache		
	proxy_thread->cfd = cfd; //client file descriptor	
	proxy_thread->mutex = &temp_mutex; //mutex for atomic cache storage operation
	proxy_thread->sport = REMOTE_SERVER; //server port
	pthread_t tid; //thread id
	//printf("thread count = %d \n",thread_count);
	pthread_create(&tid,NULL,thread_http_connection,(void*)proxy_thread);
	thread_count--;
	pthread_detach(tid);
	}

  }	
  return 0;
}


