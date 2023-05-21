/*
	Socket server to expose the /dev/ledc to the network

	This uses detached threads

	Socket commands:

	`>[ <message>]\n` -> truncate and optionally write line
	`>> <message>\n` -> append line
	`<\n` -> get current states
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <sys/time.h>

static int _run = 1;

#define DEV_FILE "/dev/ledc"

#define RECV_LEN 256

struct thread_data {
	int client_fd;
};

/*
	since threads are fire and forget and `data` is allocated on the heap,
	threads themselves will need to `free()` it before exiting
*/
void * thread_runner(void *data)
{
	struct thread_data *td = (struct thread_data*)data;
	unsigned char *buffer = NULL, *newline, *msg;
	unsigned buffer_len = 0, used_len = 0;
	int r;
	int dev_file = -1;

	{ /* set socket timeout */
		struct timeval timeout = {
			.tv_sec = 1,
			.tv_usec = 0
		};
		if(setsockopt(td->client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
		{
			fprintf(stderr, "error: failed to set socket receive timeout: %s\n", strerror(errno));
			goto _end;
		}
		if(setsockopt(td->client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
		{
			fprintf(stderr, "error: failed to set socket send timeout: %s\n", strerror(errno));
			goto _end;
		}
	}

	while(1)
	{
		/* expand buffer */
		if((buffer_len - used_len) < RECV_LEN)
		{
			unsigned char *new_buffer = realloc(buffer, used_len + RECV_LEN);
			if(!new_buffer)
			{
				fprintf(stderr, "error: failed to allocate memory: %s\n", strerror(errno));
				break;
			}
			buffer_len += RECV_LEN;
			buffer = new_buffer;
		}

		/* receive data */
		if((r = recv(td->client_fd, buffer+used_len, RECV_LEN, 0)) < 0)
		{
			if(errno == EAGAIN || errno == EWOULDBLOCK)
			{
				/* receive timeout, all good */
				break;
			}
			fprintf(stderr, "error: failed to receive data from client: %s\n", strerror(errno));
			break;
		}
		if(!r)
		{
			// EOF
			fprintf(stderr, "debug: client disconnect\n");
			break;
		}
		used_len += r;

		/* look for commands */
		while((newline = memchr(buffer, '\n', used_len)))
		{
			/* write/append */
			if(buffer[0] == '>')
			{
				msg = buffer+1;
				int open_flags = O_WRONLY;
				/* append? */
				if(buffer[1] == '>')
				{
					msg++;
					open_flags |= O_APPEND;
				}
				else
				{
					/* moslty for debug purposes */
					open_flags |= O_TRUNC;
				}
				dev_file = open(DEV_FILE, open_flags);
				if(dev_file < 0)
				{
					fprintf(stderr, "error: failed to open dev file: %s\n", strerror(errno));
					goto _end;
				}
				/* skip whitespace to message */
				while(isblank(*msg)) msg++;

				if(msg != newline)
				{ /* write to file*/
					int to_write = newline + 1 - msg;
					while(to_write)
					{
						if((r = write(dev_file, msg, to_write)) < 0)
						{
							fprintf(stderr, "error: failed to write to dev file: %s\n", strerror(errno));
							close(dev_file);
							goto _end;
						}
						to_write -= r;
						msg += r;
					}
				}
			}
			else if(buffer[0] == '<')
			{
				/* ignore the rest of the buffer */
				char rbuffer[128];
				dev_file = open(DEV_FILE, O_RDONLY);
				if(dev_file < 0)
				{
					fprintf(stderr, "error: failed to open dev file: %s\n", strerror(errno));
					goto _end;
				}
				while((r = read(dev_file, rbuffer, 128)))
				{
					int to_write = r, written = 0;
					if(r<0)
					{
						fprintf(stderr, "error: failed to read from dev file: %s\n", strerror(errno));
						close(dev_file);
						goto _end;
					}
					while(to_write)
					{
						if((r = send(td->client_fd, rbuffer+written, to_write, 0)) < 0)
						{
							fprintf(stderr, "error: failed to send to client: %s\n", strerror(errno));
							close(dev_file);
							goto _end;
						}
						to_write -= r;
						written += r;
					}
				}
				/* EOF on file*/
			}
			else
			{
				fprintf(stderr, "error: unknown command from client: '%c...'\n", buffer[0]);
				goto _end;
			}
			close(dev_file);	// common

			{ /* remove this line from the buffer */
				unsigned char *new_beg = newline+1;
				unsigned cmd_len = new_beg - buffer;
				unsigned remaining_size = buffer_len - cmd_len;
				buffer = memmove(buffer, new_beg, remaining_size);
				used_len -= cmd_len;
				/* reduce buffer size */
				new_beg = realloc(buffer, used_len);
				if(!new_beg && used_len)
				{
					fprintf(stderr, "warning: failed to reduce buffer size: %s\n", strerror(errno));
					// fortunately, the previous buffer is unchanged :)
				}
				else
				{
					buffer = new_beg;
					buffer_len = used_len;
				}
			}
		}
	}

_end:
	close(td->client_fd);
	free(data);
	if(buffer)
		free(buffer);
	return NULL;
}

int main(int argc, char **argv)
{
	int server_fd, client_fd;
	struct sockaddr_in server_addr, client_addr;
	int client_addr_len;
	int r;
	pthread_attr_t t_attr;

	/* TODO setup handling of SIGTERM */

	if((server_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
	{
		fprintf(stderr, "error: failed to create socket: %s\n", strerror(errno));
		return -1;
	}

	{ /* reuse address */
		int val = 1;
		if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
		{
			fprintf(stderr, "warning: failed to set REUSE_ADDR: %s\n", strerror(errno));
		}
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(9000);
	server_addr.sin_addr.s_addr = INADDR_ANY;
	if(bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)))
	{
		fprintf(stderr, "error: failed to bind socket: %s\n", strerror(errno));
		close(server_fd);
		return -1;
	}

	// thread attribute
	if((r=pthread_attr_init(&t_attr)))
	{
		fprintf(stderr, "error: failed to create thread attribute: %s\n", strerror(r));
		close(server_fd);
		return -1;
	}
	if((r=pthread_attr_setdetachstate(&t_attr, PTHREAD_CREATE_DETACHED)))
	{
		fprintf(stderr, "error: failed to set detached attribute: %s\n", strerror(r));
		pthread_attr_destroy(&t_attr);
		close(server_fd);
		return -1;
	}

	if(listen(server_fd, 1))
	{
		fprintf(stderr, "error: failed to listen on server: %s\n", strerror(errno));
		pthread_attr_destroy(&t_attr);
		close(server_fd);
		return -1;
	}

	while(_run)
	{
		pthread_t client_thread;
		struct thread_data *td;
		client_addr_len = sizeof(client_addr);
		if((client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len)) < 0)
		{
			if(errno == EINTR)
				continue;
			fprintf(stderr, "error: failed to accept client: %s\n", strerror(errno));
			break;
		}
		if(!(td = (struct thread_data*)malloc(sizeof(struct thread_data))))
		{
			fprintf(stderr, "error: failed to allocate memory: %s\n", strerror(errno));
			close(client_fd);
			break;
		}
		td->client_fd = client_fd;
		if((r=pthread_create(&client_thread, &t_attr, thread_runner, (void*)td)))
		{
			fprintf(stderr, "error: failed to create thread: %s\n", strerror(r));
			close(client_fd);
			break;
		}
	}

	// alright, clean stuff
	fprintf(stderr, "debug: cleaning\n");

	pthread_attr_destroy(&t_attr);
	close(server_fd);

	/* if we had an unexpected exit from the loop, it was still running then return is -1 */
	return -_run;
}
