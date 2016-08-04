/*
  Copyright 2011 Kristian Nielsen and Monty Program Ab

  This file is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  MySQL non-blocking client library functions.
*/

#include "my_global.h"
#include "my_sys.h"
#include "mysql.h"
#include "errmsg.h"
#include "sql_common.h"
#include "my_context.h"
#include "violite.h"
#include "mysql_async.h"


#ifdef __WIN__
/*
  Windows does not support MSG_DONTWAIT for send()/recv(). So we need to ensure
  that the socket is non-blocking at the start of every operation.
*/
#define WIN_SET_NONBLOCKING(mysql) { \
    my_bool old_mode; \
    if ((mysql)->net.vio) vio_blocking((mysql)->net.vio, FALSE, &old_mode); \
  }
#else
#define WIN_SET_NONBLOCKING(mysql)
#endif


void
my_context_install_suspend_resume_hook(struct mysql_async_context *b,
                                       void (*hook)(my_bool, void *),
                                       void *user_data)
{
  b->suspend_resume_hook= hook;
  b->suspend_resume_hook_user_data= user_data;
}


/* Asynchronous connect(); socket must already be set non-blocking. */
int
my_connect_async(struct mysql_async_context *b, my_socket fd,
                 const struct sockaddr *name, uint namelen, int vio_timeout)
{
  int res;
  size_socket s_err_size;

  /* Make the socket non-blocking. */
#ifdef __WIN__
  ulong arg= 1;
  ioctlsocket(fd, FIONBIO, (void *)&arg);
#else
  fcntl(fd, F_SETFL, O_NONBLOCK);
#endif

  b->events_to_wait_for= 0;
  /*
    Start to connect asynchronously.
    If this will block, we suspend the call and return control to the
    application context. The application will then resume us when the socket
    polls ready for write, indicating that the connection attempt completed.
  */
  res= connect(fd, name, namelen);
  if (res != 0)
  {
#ifdef __WIN__
    int wsa_err= WSAGetLastError();
    if (wsa_err != WSAEWOULDBLOCK)
      return res;
    b->events_to_wait_for|= MYSQL_WAIT_EXCEPT;
#else
    int err= errno;
    if (err != EINPROGRESS && err != EALREADY && err != EAGAIN)
      return res;
#endif
    b->events_to_wait_for|= MYSQL_WAIT_WRITE;
    if (vio_timeout >= 0)
    {
      b->timeout_value= vio_timeout;
      b->events_to_wait_for|= MYSQL_WAIT_TIMEOUT;
    }
    else
      b->timeout_value= 0;
    if (b->suspend_resume_hook)
      (*b->suspend_resume_hook)(TRUE, b->suspend_resume_hook_user_data);
    my_context_yield(&b->async_context);
    if (b->suspend_resume_hook)
      (*b->suspend_resume_hook)(FALSE, b->suspend_resume_hook_user_data);
    if (b->events_occurred & MYSQL_WAIT_TIMEOUT)
      return -1;

    s_err_size= sizeof(res);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*) &res, &s_err_size) != 0)
      return -1;
    if (res)
    {
      errno= res;
      return -1;
    }
  }
  return res;
}

#define IS_BLOCKING_ERROR()                   \
  IF_WIN(WSAGetLastError() != WSAEWOULDBLOCK, \
         (errno != EAGAIN && errno != EINTR))

#ifdef _AIX
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#endif

ssize_t
my_recv_async(struct mysql_async_context *b, int fd,
              unsigned char *buf, size_t size, int timeout)
{
  ssize_t res;

  for (;;)
  {
    res= recv(fd, buf, size, IF_WIN(0, MSG_DONTWAIT));
    if (res >= 0 || IS_BLOCKING_ERROR())
      return res;
    b->events_to_wait_for= MYSQL_WAIT_READ;
    if (timeout >= 0)
    {
      b->events_to_wait_for|= MYSQL_WAIT_TIMEOUT;
      b->timeout_value= timeout;
    }
    if (b->suspend_resume_hook)
      (*b->suspend_resume_hook)(TRUE, b->suspend_resume_hook_user_data);
    my_context_yield(&b->async_context);
    if (b->suspend_resume_hook)
      (*b->suspend_resume_hook)(FALSE, b->suspend_resume_hook_user_data);
    if (b->events_occurred & MYSQL_WAIT_TIMEOUT)
      return -1;
  }
}


ssize_t
my_send_async(struct mysql_async_context *b, int fd,
              const unsigned char *buf, size_t size, int timeout)
{
  ssize_t res;

  for (;;)
  {
    res= send(fd, buf, size, IF_WIN(0, MSG_DONTWAIT));
    if (res >= 0 || IS_BLOCKING_ERROR())
      return res;
    b->events_to_wait_for= MYSQL_WAIT_WRITE;
    if (timeout >= 0)
    {
      b->events_to_wait_for|= MYSQL_WAIT_TIMEOUT;
      b->timeout_value= timeout;
    }
    if (b->suspend_resume_hook)
      (*b->suspend_resume_hook)(TRUE, b->suspend_resume_hook_user_data);
    my_context_yield(&b->async_context);
    if (b->suspend_resume_hook)
      (*b->suspend_resume_hook)(FALSE, b->suspend_resume_hook_user_data);
    if (b->events_occurred & MYSQL_WAIT_TIMEOUT)
      return -1;
  }
}


my_bool
my_io_wait_async(struct mysql_async_context *b, enum enum_vio_io_event event,
                 int timeout)
{
  switch (event)
  {
  case VIO_IO_EVENT_READ:
    b->events_to_wait_for = MYSQL_WAIT_READ;
    break;
  case VIO_IO_EVENT_WRITE:
    b->events_to_wait_for = MYSQL_WAIT_WRITE;
    break;
  case VIO_IO_EVENT_CONNECT:
    b->events_to_wait_for = MYSQL_WAIT_WRITE | IF_WIN(0, MYSQL_WAIT_EXCEPT);
    break;
  }

  if (timeout >= 0)
  {
    b->events_to_wait_for |= MYSQL_WAIT_TIMEOUT;
    b->timeout_value= timeout;
  }
  if (b->suspend_resume_hook)
    (*b->suspend_resume_hook)(TRUE, b->suspend_resume_hook_user_data);
  my_context_yield(&b->async_context);
  if (b->suspend_resume_hook)
    (*b->suspend_resume_hook)(FALSE, b->suspend_resume_hook_user_data);
  return (b->events_occurred & MYSQL_WAIT_TIMEOUT) ? 0 : 1;
}


#ifdef HAVE_OPENSSL
static my_bool
my_ssl_async_check_result(int res, struct mysql_async_context *b, SSL *ssl)
{
  int ssl_err;
  b->events_to_wait_for= 0;
  if (res >= 0)
    return 1;
  ssl_err= SSL_get_error(ssl, res);
  if (ssl_err == SSL_ERROR_WANT_READ)
    b->events_to_wait_for|= MYSQL_WAIT_READ;
  else if (ssl_err == SSL_ERROR_WANT_WRITE)
    b->events_to_wait_for|= MYSQL_WAIT_WRITE;
  else
    return 1;
  if (b->suspend_resume_hook)
    (*b->suspend_resume_hook)(TRUE, b->suspend_resume_hook_user_data);
  my_context_yield(&b->async_context);
  if (b->suspend_resume_hook)
    (*b->suspend_resume_hook)(FALSE, b->suspend_resume_hook_user_data);
  return 0;
}

int
my_ssl_read_async(struct mysql_async_context *b, SSL *ssl,
                  void *buf, int size)
{
  int res;

  for (;;)
  {
    res= SSL_read(ssl, buf, size);
    if (my_ssl_async_check_result(res, b, ssl))
      return res;
  }
}

int
my_ssl_write_async(struct mysql_async_context *b, SSL *ssl,
                   const void *buf, int size)
{
  int res;

  for (;;)
  {
    res= SSL_write(ssl, buf, size);
    if (my_ssl_async_check_result(res, b, ssl))
      return res;
  }
}
#endif  /* HAVE_OPENSSL */

