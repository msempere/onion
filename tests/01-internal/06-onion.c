/*
	Onion HTTP server library
	Copyright (C) 2010 David Moreno Montero

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
	*/

#include <onion/log.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <curl/curl.h>
#include <errno.h>

#include <onion/onion.h>

#include "../ctest.h"
#include <pthread.h>
#include <netdb.h>

const int MAX_TIME=500;

int processed;
pthread_mutex_t processed_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile char okexit=0;
onion *o;
FILE *null_file=NULL;

typedef struct{
  float wait_s;
  float wait_t;
  int n_requests;
  char close_at_n;
}params_t;

onion_response_codes process_request(void *_, onion_request *req, onion_response *res){
  pthread_mutex_lock(&processed_mutex);
  processed++;
  pthread_mutex_unlock(&processed_mutex);
  onion_response_write0(res, "Done");
  
	FAIL_IF_NOT_EQUAL_STR(onion_request_get_client_description(req),"127.0.0.1");
	
  return OCS_PROCESSED;
}

int connect_to(const char *addr, const char *port){
  struct addrinfo hints;
  struct addrinfo *server;
  
  memset(&hints,0, sizeof(struct addrinfo));
  hints.ai_socktype=SOCK_STREAM;
  hints.ai_family=AF_UNSPEC;
  hints.ai_flags=AI_PASSIVE|AI_NUMERICSERV;

  if (getaddrinfo(addr,port,&hints,&server)<0){
    ONION_ERROR("Error getting server info");
    return -1;
  }
  int fd=socket(server->ai_family, server->ai_socktype | SOCK_CLOEXEC, server->ai_protocol);
  
  if (connect(fd, server->ai_addr, server->ai_addrlen)==-1){
    close(fd);
    fd=-1;
    ONION_ERROR("Error connecting to server %s:%s",addr,port);
  }
  
  freeaddrinfo(server);
  
  
  return fd;
}

int curl_get(const char *url){
	CURL *curl;
	curl = curl_easy_init();
  if (!null_file)
    null_file=fopen("/dev/null","w");
  FAIL_IF(null_file == NULL);
  FAIL_IF_NOT(curl_easy_setopt(curl, CURLOPT_URL, url)==CURLE_OK);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  FAIL_IF_NOT(curl_easy_setopt(curl, CURLOPT_WRITEDATA, null_file)==CURLE_OK);
  CURLcode res=curl_easy_perform(curl);
  FAIL_IF_NOT_EQUAL((int)res,0);
	if (res!=0){
		ONION_ERROR("%s",curl_easy_strerror(res));
	}
  long int http_code;
  res=curl_easy_getinfo(curl, CURLINFO_HTTP_CODE, &http_code);
  FAIL_IF_NOT_EQUAL((int)res,0);
  char buffer[1024]; size_t l;
  curl_easy_recv(curl,buffer,sizeof(buffer),&l);
  FAIL_IF_NOT_EQUAL_INT((int)http_code, HTTP_OK);
	curl_easy_cleanup(curl);
  
  return http_code;
}

int curl_get_to_fail(const char *url){
	CURL *curl;
	curl = curl_easy_init();
  if (!null_file)
    null_file=fopen("/dev/null","w");
  FAIL_IF(null_file == NULL);
  FAIL_IF_NOT(curl_easy_setopt(curl, CURLOPT_URL, url)==CURLE_OK);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  FAIL_IF_NOT(curl_easy_setopt(curl, CURLOPT_WRITEDATA, null_file)==CURLE_OK);
  CURLcode res=curl_easy_perform(curl);
  FAIL_IF_EQUAL((int)res,0);
  long int http_code;
  res=curl_easy_getinfo(curl, CURLINFO_HTTP_CODE, &http_code);
  FAIL_IF_NOT_EQUAL((int)res,0);
  char buffer[1024]; size_t l;
  curl_easy_recv(curl,buffer,sizeof(buffer),&l);
  FAIL_IF_EQUAL_INT((int)http_code, HTTP_OK);
	curl_easy_cleanup(curl);
  
  return http_code;
}

void *do_requests(params_t *t){
  ONION_DEBUG("Do %d petitions",t->n_requests);
  int i;
  usleep(t->wait_s*1000000);
  for(i=0;i<t->n_requests;i++){
    curl_get("http://localhost:8080/");
    usleep(t->wait_t*1000000);
  }
  if (t->close_at_n==1){
    usleep(t->wait_s*1000000);
    onion_listen_stop(o);
  }
  
  return NULL;
}

void *watchdog(void *_){
  sleep(MAX_TIME);
  if (!okexit){
    ONION_ERROR("Error! Waiting too long, server must have deadlocked!");
    exit(1);
  }
  return NULL;
}

void do_listen(){
  onion_listen(o);
}

void do_petition_set_threaded(float wait_s, float wait_c, int nrequests, char close, int nthreads){
  ONION_DEBUG("Using %d threads, %d petitions per thread",nthreads,nrequests);
  processed=0;
  
  params_t params;
  params.wait_s=wait_s;
  params.wait_t=wait_c;
  params.n_requests=nrequests;
  params.close_at_n=close;
  
  pthread_t *thread=malloc(sizeof(pthread_t*)*nthreads);
  pthread_t listen_thread;
  int i;
  pthread_create(&listen_thread, NULL, (void*)do_listen, NULL);
  for (i=0;i<nthreads;i++){
    pthread_create(&thread[i], NULL, (void*)do_requests, &params);
  }
  for (i=0;i<nthreads;i++){
    pthread_join(thread[i], NULL);
  }
  free(thread);
  if (close==2){
    usleep(wait_s*1000000);
    onion_listen_stop(o);
  }
  pthread_join(listen_thread, NULL);
  
  
  FAIL_IF_NOT_EQUAL_INT(params.n_requests * nthreads, processed);
}

void do_petition_set(float wait_s, float wait_c, int n_requests, char close){
  do_petition_set_threaded(wait_s, wait_c, n_requests, close, 1);
}

void t01_server_one(){
  INIT_LOCAL();
  
  o=onion_new(O_ONE);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set(1,0.1,1,0);
  onion_free(o);
  
  o=onion_new(O_ONE_LOOP);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set(1,0.1,1,1);
  onion_free(o);
  
  o=onion_new(O_ONE_LOOP);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set(1,0.001,100,1);
  onion_free(o);
  
  END_LOCAL();
}

void t02_server_epoll(){
  INIT_LOCAL();
  
  o=onion_new(O_POLL);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set(1,0.1,1,1);
  onion_free(o);

  o=onion_new(O_POLL);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set(1,0.001,100,1);
  onion_free(o);

  o=onion_new(O_POOL);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set(1,0.001,100,1);
  onion_free(o);

  o=onion_new(O_POOL);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set_threaded(1,0.001,100,2,10);
  onion_free(o);

  o=onion_new(O_POOL);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set_threaded(1,0.001,100,2,15);
  onion_free(o);

  o=onion_new(O_POOL);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  do_petition_set_threaded(1,0.001,100,2,100);
  onion_free(o);

  END_LOCAL();
}

void t03_server_https(){
  INIT_LOCAL();
  
  o=onion_new(O_ONE_LOOP | O_DETACH_LISTEN);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  FAIL_IF_NOT_EQUAL_INT(onion_set_certificate(o, O_SSL_CERTIFICATE_KEY, "mycert.pem", "mycert.pem"),0);
  FAIL_IF_NOT_EQUAL_INT(onion_listen(o),0);
  //do_petition_set(1,1,1,1);
  sleep(1);
  //FAIL_IF_EQUAL_INT(  curl_get_to_fail("http://localhost:8080"), HTTP_OK);
  sleep(1);
  FAIL_IF_NOT_EQUAL_INT(  curl_get("https://localhost:8080"), HTTP_OK);
  sleep(1);
  onion_free(o);
  
  END_LOCAL();
}

void t04_server_timeout_threaded(){
  INIT_LOCAL();
  
  o=onion_new(O_THREADED | O_DETACH_LISTEN);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  onion_set_port(o,"8082");
  onion_set_timeout(o,2000);
  onion_listen(o);
  sleep(1);
  
  int fd=connect_to("localhost","8082");
  sleep(3);
  // Should have closed the connection
  int w=write(fd,"GET /\n\n",7);
  FAIL_IF_NOT_EQUAL_INT(w,7);
  char data[256];
  FAIL_IF(read(fd, data,sizeof(data))>0);
  close(fd);

  FAIL_IF_NOT(curl_get("http://localhost:8082"));
  
  onion_free(o);
  END_LOCAL();
}


void t05_server_timeout_threaded_ssl(){
  INIT_LOCAL();
  ONION_DEBUG("%s",__FUNCTION__);
  o=onion_new(O_THREADED | O_DETACH_LISTEN);
  onion_set_root_handler(o,onion_handler_new((void*)process_request,NULL,NULL));
  FAIL_IF_NOT_EQUAL_INT(onion_set_certificate(o, O_SSL_CERTIFICATE_KEY, "mycert.pem", "mycert.pem"),0);
  onion_set_port(o,"8081");
  onion_set_timeout(o,3000);
  onion_listen(o);
  sleep(1);
  
  int fd=connect_to("localhost","8081");
  sleep(4);
  // Should have closed the connection
  int w=write(fd,"GET /\n\n",7);
  FAIL_IF_NOT_EQUAL_INT(w,7);
  char data[256];
  FAIL_IF(read(fd, data,sizeof(data))>0);
  close(fd);

  FAIL_IF_NOT(curl_get("https://localhost:8081"));

	onion_free(o);
  
  END_LOCAL();
}

int main(int argc, char **argv){
  START();
  pthread_t watchdog_thread;
  pthread_create(&watchdog_thread, NULL, (void*)watchdog, NULL);

	t01_server_one();
	t02_server_epoll();
	t03_server_https();
	t04_server_timeout_threaded();
	t05_server_timeout_threaded_ssl();
  
  okexit=1;
  pthread_cancel(watchdog_thread);
  pthread_join(watchdog_thread,NULL);
  
  if (null_file)
    fclose(null_file);

	END();
}

