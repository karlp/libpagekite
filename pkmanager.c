/******************************************************************************
pkmanager.c - A manager for multiple pagekite connections.

This file is Copyright 2011, 2012, The Beanstalks Project ehf.

This program is free software: you can redistribute it and/or modify it under
the terms  of the  Apache  License 2.0  as published by the  Apache  Software
Foundation.

This program is distributed in the hope that it will be useful,  but  WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
details.

You should have received a copy of the Apache License along with this program.
If not, see: <http://www.apache.org/licenses/>

Note: For alternate license terms, see the file COPYING.md.

******************************************************************************/

#include "common.h"
#include "utils.h"
#include "pkerror.h"
#include "pkconn.h"
#include "pkstate.h"
#include "pkproto.h"
#include "pkblocker.h"
#include "pkmanager.h"
#include "pklogging.h"


void pkm_yield(struct pk_manager *pkm)
{
  pthread_mutex_unlock(&(pkm->loop_lock));
  pthread_mutex_lock(&(pkm->loop_lock));
}
static void pkm_interrupt_cb(EV_P_ ev_async *w, int revents)
{
  struct pk_manager* pkm = (struct pk_manager*) w->data;
  pkm_yield(pkm);
  /* -Wall dislikes unused arguments */
  (void) loop;
  (void) (revents);
}
void pkm_interrupt(struct pk_manager *pkm)
{
  ev_async_send(pkm->loop, &(pkm->interrupt));
}

void pkm_block(struct pk_manager *pkm)
{
  if (pthread_self() != pkm->main_thread) {
/*
    while (0 != pthread_mutex_trylock(&(pkm->loop_lock))) {
      pkm_interrupt(pkm);
    }
*/
    pkm_interrupt(pkm);
    pthread_mutex_trylock(&(pkm->loop_lock));
  }
}
void pkm_unblock(struct pk_manager *pkm)
{
  if (pthread_self() != pkm->main_thread) {
    pthread_mutex_unlock(&(pkm->loop_lock));
  }
}

static void pkm_quit_cb(EV_P_ ev_async *w, int revents)
{
  ev_unloop(EV_A_ EVUNLOOP_ALL);
  /* -Wall dislikes unused arguments */
  (void) w;
  (void) revents;
}
void pkm_quit(struct pk_manager* pkm)
{
  ev_async_send(pkm->loop, &(pkm->quit));
}


void pkm_chunk_cb(struct pk_frontend* fe, struct pk_chunk *chunk)
{
  struct pk_backend_conn* pkb; /* FIXME: What if we are a front-end? */
  char reply[PK_REJECT_MAXSIZE], pre[PK_REJECT_MAXSIZE], rej[PK_REJECT_MAXSIZE];
  char *post;
  int bytes;

  pk_log_chunk(chunk);

  pkb = NULL;
  if (NULL != chunk->sid) {
    if ((NULL != (pkb = pkm_find_be_conn(fe->manager, fe, chunk->sid))) ||
        (NULL != chunk->noop) ||
        (NULL != chunk->eof) ||
        (NULL != (pkb = pkm_connect_be(fe, chunk)))) {
      /* We are happy, pkb should be a valid connection. */
    }
    else {
      /* FIXME: Send back a nicer error */
      if (fe->manager->fancy_pagekite_net_rejection) {
        sprintf(pre, PK_REJECT_PRE_PAGEKITE, "BE", pk_state.app_id_short,
                     chunk->request_proto, chunk->request_host);
        post = PK_REJECT_POST_PAGEKITE;
      }
      else {
        pre[0] = '\0';
        post = pre;
      }
      sprintf(rej, PK_REJECT_FMT,
                   pre, "be", pk_state.app_id_short,
                   chunk->request_proto, chunk->request_host, post);

      bytes = pk_format_reply(reply, chunk->sid, strlen(rej), rej);
      pkc_write(&(fe->conn), reply, bytes);

      bytes = pk_format_eof(reply, chunk->sid, PK_EOF);
      pkc_write(&(fe->conn), reply, bytes);
      pk_log(PK_LOG_TUNNEL_CONNS, "No stream found: %s, %s://%s", chunk->sid,
                                  chunk->request_proto, chunk->request_host);
    }
  }

  if (NULL != chunk->noop) {
    if (NULL != chunk->ping) {
      bytes = pk_format_pong(reply);
      pkc_write(&(fe->conn), reply, bytes);
      pk_log(PK_LOG_TUNNEL_DATA, "> --- > Pong!");
    }
  }
  else if (NULL != pkb) {
    if (NULL == chunk->eof) {
      pkc_write(&(pkb->conn), chunk->data, chunk->length);
    }
    else {
      pkm_parse_eof(pkb, chunk->eof);
    }
  }

  if (NULL != pkb) {
    if (0 < chunk->throttle_spd) {
      if (pkb->conn.send_window_kb > CONN_WINDOW_SIZE_KB_MINIMUM) {
        pkb->conn.send_window_kb *= 0.8;
      }
    }
    if (0 < chunk->remote_sent_kb) {
      pkb->conn.sent_kb = chunk->remote_sent_kb;
    }
    pkm_update_io(fe, pkb);
  }
}

struct pk_backend_conn* pkm_connect_be(struct pk_frontend* fe,
                                       struct pk_chunk* chunk)
{
  /* Connect to the backend, or free the conn object if we fail */
  int sockfd;
  struct sockaddr_in addr_buf;
  struct sockaddr_in* addr;
  struct hostent *backend;
  struct pk_backend_conn* pkb;
  struct pk_pagekite *kite;

  /* FIXME: Better error handling? */
  if ((NULL == chunk->request_proto) || (NULL == chunk->request_host)) {
    pk_log(PK_LOG_TUNNEL_CONNS, "pkm_connect_be: Request details missing.");
    return NULL;
  }

  /* First, search the list of configured back-ends for one that matches
     the request in the chunk.  If nothing is found, there is no point in
     continuing. */
  if (NULL == (kite = pkm_find_kite(fe->manager,
                                    chunk->request_proto,
                                    chunk->request_host,
                                    chunk->request_port))) {
    pk_log(PK_LOG_TUNNEL_CONNS, "pkm_connect_be: No such kite %s://%s:%d",
                                chunk->request_proto, chunk->request_host,
                                chunk->request_port);
    return NULL;
  }

  /* Allocate a connection for this request or die... */
  if (NULL == (pkb = pkm_alloc_be_conn(fe->manager, fe, chunk->sid))) {
    pk_log(PK_LOG_TUNNEL_CONNS, "pkm_connect_be: BE alloc failed for %s://%s:%d",
                                chunk->request_proto, chunk->request_host,
                                chunk->request_port);
    return NULL;
  }

  /* Look up the back-end... */
  addr = NULL;
  if (NULL != (backend = gethostbyname(kite->local_domain))) {
    addr = &addr_buf;
    memset((char *) addr, 0, sizeof(addr));
    addr->sin_family = AF_INET;
    bcopy((char*) backend->h_addr_list[0],
          (char*) &(addr->sin_addr.s_addr),
          backend->h_length);
    addr->sin_port = htons(kite->local_port);
  }

  /* Try to connect and set non-blocking. */
  errno = sockfd = 0;
  if ((NULL == addr) ||
      (0 > (sockfd = socket(AF_INET, SOCK_STREAM, 0))) ||
      (0 > connect(sockfd, (struct sockaddr*) addr, sizeof(*addr))) ||
      (0 > set_non_blocking(sockfd)))
  {
    if (errno != EINPROGRESS) {
      /* FIXME:
         EINPROGRESS never happens until we swap connect/set_non_blocking
         above.  Do that later once we've figured out error handling. */
      close(sockfd);
      pkm_free_be_conn(pkb);
      pk_log(PK_LOG_TUNNEL_CONNS, "pkm_connect_be: Failed to connect %s:%d",
                                  kite->local_domain, kite->local_port);
      return NULL;
    }
  }

  /* FIXME: This should be non-blocking for use on high volume front-ends,
   *        but that requires more buffering and fancy logic, so we're
   *        lazy for now.
   *        See also: http://developerweb.net/viewtopic.php?id=3196 */

  pkb->kite = kite;
  pkb->conn.sockfd = sockfd;

  ev_io_init(&(pkb->conn.watch_r), pkm_be_conn_readable_cb, sockfd, EV_READ);
  ev_io_init(&(pkb->conn.watch_w), pkm_be_conn_writable_cb, sockfd, EV_WRITE);
  ev_io_start(fe->manager->loop, &(pkb->conn.watch_r));
  ev_io_start(fe->manager->loop, &(pkb->conn.watch_w));
  pkb->conn.watch_r.data = pkb->conn.watch_w.data = (void *) pkb;

  PKS_STATE(pk_state.live_streams += 1);

  return pkb;
}

ssize_t pkm_write_chunked(struct pk_frontend* fe, struct pk_backend_conn* pkb,
                          ssize_t length, char* data)
{
  ssize_t overhead = 0;
  struct pk_conn* pkc = &(fe->conn);

  /* FIXME: Better error handling */

  /* Make sure there is space in our output buffer for the header. */
  overhead = pk_reply_overhead(pkb->sid, length);
  if (PKC_OUT_FREE(*pkc) < overhead)
    if (0 > pkc_flush(pkc, NULL, 0, BLOCKING_FLUSH, "pkm_write_chunked"))
      return -1;

  /* Write the chunk header to the output buffer */
  pkc->out_buffer_pos += pk_format_reply(PKC_OUT(*pkc), pkb->sid, length, NULL);

  /* Write the data (will pick up the header automatically) */
  return pkc_write(pkc, data, length);
}

int pkm_post_read(struct pk_conn* pkc, int bytes, int err)
{
  ssize_t delta;
  if (bytes > 0) {
    pkc->in_buffer_pos += bytes;

    /* Update KB counter and window... this is a bit messy. */
    pkc->read_bytes += bytes;
    while (pkc->read_bytes > 1024) {
      pkc->read_kb += 1;
      pkc->read_bytes -= 1024;
      if ((pkc->read_kb & 0x1f) == 0x00) {
        delta = (CONN_WINDOW_SIZE_KB_MAXIMUM - pkc->send_window_kb);
        if (delta < 0) delta = 0;
        pkc->send_window_kb += (delta/CONN_WINDOW_SIZE_STEPFACTOR);
      }
    }
  }
  else if (bytes == 0) {
    pkc->status |= CONN_STATUS_CLS_READ;
  }
  else switch (err) {
    case EINTR:
    case EAGAIN:
      break;
    default:
      pkc->status |= CONN_STATUS_BROKEN;
      break;
  }
  return bytes;
}

int pkm_update_io(struct pk_frontend* fe, struct pk_backend_conn* pkb)
{
  int i;
  int bytes;
  int loglevel;
  char buffer[1024];
  int eof = 0;
  int flows = 2;
  struct pk_conn* pkc;
  struct pk_manager* pkm = fe->manager;

  if (pkb != NULL) {
    pkc = &(pkb->conn);
    loglevel = PK_LOG_BE_DATA;
  }
  else {
    pkc = &(fe->conn);
    loglevel = PK_LOG_TUNNEL_DATA;
  }
  if (0 >= pkc->sockfd)
    return 0;

  if (pkb != NULL) {
    if (pkc->read_kb > pkc->sent_kb + pkc->send_window_kb)
      pkm_flow_control_conn(pkc, CONN_DEST_BLOCKED);
    else
      pkm_flow_control_conn(pkc, CONN_DEST_UNBLOCKED);
  }

  if (pkc->status & (CONN_STATUS_CLS_READ|CONN_STATUS_END_READ)) {
    if (pkc->status & CONN_STATUS_END_READ) {
      /* They know, we know, we know they know... */
    }
    else {
      /* Other end doesn't know we're closing, send an EOF. */
      eof |= PK_EOF_READ;
    }
    /* Not going to read anymore, stop listening. */
    pkc->status |= (CONN_STATUS_END_READ | CONN_STATUS_CLS_READ);
    ev_io_stop(pkm->loop, &(pkc->watch_r));
    shutdown(pkc->sockfd, SHUT_RD);
    flows -= 1;
    pk_log(loglevel, "%d: Closed for reading.", pkc->sockfd);
    if (pkb == NULL) {
      /* Frontend: If we can't read the tunnel, we can't write it either. */
      pkc->status |= CONN_STATUS_CLS_WRITE;
    }
  }
  else {
    if ((pkc->status & CONN_STATUS_BLOCKED) &&
        !(pkc->status & CONN_STATUS_WANT_READ)) {
      pk_log(loglevel, "%d: Throttled.", pkc->sockfd);
      ev_io_stop(pkm->loop, &(pkc->watch_r));
    }
    else {
      pk_log(loglevel, "%d: Watching for input.", pkc->sockfd);
      ev_io_start(pkm->loop, &(pkc->watch_r));
    }
  }

  if (pkc->status & CONN_STATUS_CLS_WRITE) {
    /* Writing is impossible, discard buffer and shutdown. */
    if (pkc->status & CONN_STATUS_END_WRITE) {
      /* They know, we know, we know they know... */
    }
    else {
      /* Other end doesn't know we're closing, send an EOF. */
      eof |= PK_EOF_WRITE;
    }
    pkc->status |= (CONN_STATUS_END_WRITE | CONN_STATUS_CLS_WRITE);
    pkc->out_buffer_pos = 0;
    shutdown(pkc->sockfd, SHUT_WR);
    ev_io_stop(pkm->loop, &(pkc->watch_w));
    flows -= 1;
    pk_log(loglevel, "%d: Closed for writing.", pkc->sockfd);
  }
  else if ((0 < pkc->out_buffer_pos) ||
           (pkc->status & CONN_STATUS_WANT_WRITE)) {
    /* Blocked: activate write listener */
    ev_io_start(pkm->loop, &(pkc->watch_w));
    pk_log(loglevel, "%d: Blocked!", pkc->sockfd);
    pkm_flow_control_fe(fe, CONN_TUNNEL_BLOCKED);
  }
  else {
    if (pkc->status & CONN_STATUS_END_WRITE) {
      /* Not blocked, no more data (sources closed), shutdown. */
      pkc->status |= CONN_STATUS_CLS_WRITE;
      shutdown(pkc->sockfd, SHUT_WR);
      flows -= 1;
      pk_log(loglevel, "%d: Closed for writing (remote).", pkc->sockfd);
    }
    else {
      pk_log(loglevel, "%d: Unblocked!", pkc->sockfd);
      pkm_flow_control_fe(fe, CONN_TUNNEL_UNBLOCKED);
    }
    ev_io_stop(pkm->loop, &(pkc->watch_w));
  }

  if (eof) {
    if (pkb != NULL) {
      /* This is a backend conn, send EOF to over tunnel. */
      bytes = pk_format_eof(buffer, pkb->sid, eof);
      pkc_write(&(fe->conn), buffer, bytes);
      pk_log(loglevel, "%d: Sent EOF (0x%x)", pkc->sockfd, eof);
    }
    else {
      /* This is a tunnel, send EOF to all backends, mark for reconnection. */
      /* FIXME: This is O(n), but rare.  Maybe OK? */
      pk_log(loglevel, "%d: Shutting down tunnel.", pkc->sockfd);
      for (i = 0; i < pkm->be_conn_max; i++) {
        pkb = (pkm->be_conns+i);
        if ((pkb->frontend == fe) && (pkb->conn.status != CONN_STATUS_UNKNOWN)) {
          pkb->conn.status |= (CONN_STATUS_END_WRITE|CONN_STATUS_END_READ);
          pkm_update_io(fe, pkb);
        }
      }
      pkb = NULL;
    }
  }

  if (0 == flows) {
    /* Nothing to read or write, close and clean up. */
    if (0 <= pkc->sockfd) close(pkc->sockfd);
    if (pkb != NULL) {
      pkm_free_be_conn(pkb);
      PKS_STATE(pk_state.live_streams -= 1);
    }
    else {
      /* FIXME: Is this the right way to clean up dead tunnels? */
      PKS_STATE(pk_state.live_frontends -= 1;
                pkm->status = PK_STATUS_PROBLEMS);
      pkc_reset_conn(&(fe->conn));
      fe->request_count = 0;
      pkm_tick(pkm);
    }
    pk_log(loglevel, "%d: Closed.", pkc->sockfd, eof);
    pkc->sockfd = -1;
  }

  pkm_yield(pkm);
  return flows;
}

void pkm_flow_control_fe(struct pk_frontend* fe, flow_op op)
{
  int i;
  struct pk_backend_conn* pkb;
  struct pk_manager* pkm = fe->manager;

  for (i = 0; i < pkm->be_conn_max; i++) {
    pkb = (pkm->be_conns + i);
    if (pkb->frontend == fe) {
      if (pkb->conn.status & CONN_STATUS_TNL_BLOCKED) {
        if (op == CONN_TUNNEL_UNBLOCKED) {
          pk_log(PK_LOG_TUNNEL_DATA, "%d: Tunnel unblocked", pkb->conn.sockfd);
          pkb->conn.status &= ~CONN_STATUS_TNL_BLOCKED;
          pkm_update_io(fe, pkb);
        }
      }
      else
        if (op == CONN_TUNNEL_BLOCKED) {
          pk_log(PK_LOG_TUNNEL_DATA, "%d: Tunnel blocked", pkb->conn.sockfd);
          pkb->conn.status |= CONN_STATUS_TNL_BLOCKED;
          pkm_update_io(fe, pkb);
        }
    }
  }
}

void pkm_flow_control_conn(struct pk_conn* pkc, flow_op op)
{
  if (pkc->status & CONN_STATUS_DST_BLOCKED) {
    if (op == CONN_DEST_UNBLOCKED) {
      pk_log(PK_LOG_BE_DATA, "%d: Destination unblocked", pkc->sockfd);
      pkc->status &= ~CONN_STATUS_DST_BLOCKED;
    }
  }
  else
    if (op == CONN_DEST_BLOCKED) {
      pk_log(PK_LOG_BE_DATA, "%d: Destination blocked", pkc->sockfd);
      pkc->status |= CONN_STATUS_DST_BLOCKED;
    }
}

void pkm_parse_eof(struct pk_backend_conn* pkb, char *eof)
{
  struct pk_conn* pkc = &(pkb->conn);
  int eof_read = 0;
  int eof_write = 0;
  char *p;

  /* Figure out what kind of EOF this is */
  for (p = eof; (p != NULL) && (*p != '\0'); p++) {
    if (*p == 'R' || *p == 'r')
      eof_read = 1;
    else if (*p == 'W' || *p == 'w')
      eof_write = 1;
  }
  if (!eof_write && !eof_read) /* Legacy EOF support */
    eof_write = eof_read = 1;

  /* If we cannot write anymore, there is no point in reading. */
  if (eof_write) {
    pkc->status |= CONN_STATUS_END_READ;
  }

  /* If we cannot read anymore, we won't be writing either. */
  if (eof_read) {
    pkc->status |= CONN_STATUS_END_WRITE;
  }
}

void pkm_tunnel_readable_cb(EV_P_ ev_io *w, int revents)
{
  struct pk_frontend* fe = (struct pk_frontend*) w->data;
  fe->conn.status &= ~CONN_STATUS_WANT_READ;
  if (0 < pkc_read(&(fe->conn))) {
    if (0 > pk_parser_parse(fe->parser,
                            fe->conn.in_buffer_pos,
                            (char *) fe->conn.in_buffer))
    {
      /* Parse failed: remote is borked: should kill this conn. */
      fe->conn.status |= CONN_STATUS_BROKEN;
      /* FIXME: Log this event? */
    }
    fe->conn.in_buffer_pos = 0;
  }
  pkm_update_io(fe, NULL);
  /* -Wall dislikes unused arguments */
  (void) loop;
  (void) revents;
}

void pkm_tunnel_writable_cb(EV_P_ ev_io *w, int revents)
{
  struct pk_frontend* fe = (struct pk_frontend*) w->data;

  /* This is necessary for SSL handshakes and the like. */
  if (fe->conn.status & CONN_STATUS_WANT_WRITE) {
    fe->conn.status &= ~CONN_STATUS_WANT_WRITE;
    if (0 == fe->conn.out_buffer_pos)
      pkc_raw_write(&(fe->conn), NULL, 0);
  }
  pkc_flush(&(fe->conn), NULL, 0, NON_BLOCKING_FLUSH, "tunnel");

  pkm_update_io(fe, NULL);
  /* -Wall dislikes unused arguments */
  (void) loop;
  (void) revents;
}

void pkm_be_conn_readable_cb(EV_P_ ev_io *w, int revents)
{
  struct pk_backend_conn* pkb = (struct pk_backend_conn*) w->data;
  size_t bytes;

  pkb->conn.status &= ~CONN_STATUS_WANT_READ;
  bytes = pkc_read(&(pkb->conn));
  if ((0 < bytes) &&
      (0 <= pkm_write_chunked(pkb->frontend, pkb,
                              pkb->conn.in_buffer_pos,
                              pkb->conn.in_buffer))) {
    pkb->conn.in_buffer_pos = 0;
    pk_log(PK_LOG_BE_DATA, ">%5.5s> DATA: %d bytes", pkb->sid, bytes);
  }
  else if (bytes == 0) {
    pk_log(PK_LOG_BE_DATA, ">%5.5s> EOF: read", pkb->sid);
  }
  pkm_update_io(pkb->frontend, pkb);
  /* -Wall dislikes unused arguments */
  (void) loop;
  (void) revents;
}

void pkm_be_conn_writable_cb(EV_P_ ev_io *w, int revents)
{
  struct pk_backend_conn* pkb = (struct pk_backend_conn*) w->data;

  /* This is necessary for SSL handshakes and the like. */
  if (pkb->conn.status & CONN_STATUS_WANT_WRITE) {
    pkb->conn.status &= ~CONN_STATUS_WANT_WRITE;
    if (0 == pkb->conn.out_buffer_pos)
      pkc_raw_write(&(pkb->conn), NULL, 0);
  }

  pkc_flush(&(pkb->conn), NULL, 0, NON_BLOCKING_FLUSH, "be_conn");
  if (pkb->conn.out_buffer_pos == 0)
  {
    pk_log(PK_LOG_BE_DATA, "Flushed: %s:%d (done)",
           pkb->kite->local_domain, pkb->kite->local_port);
  }
  else {
    pk_log(PK_LOG_BE_DATA, "Flushed: %s:%d\n",
           pkb->kite->local_domain, pkb->kite->local_port);
  }
  pkm_update_io(pkb->frontend, pkb);
  /* -Wall dislikes unused arguments */
  (void) loop;
  (void) revents;
}

int pkm_reconnect_all(struct pk_manager *pkm) {
  struct pk_frontend *fe;
  struct pk_kite_request *kite_r;
  unsigned int status;
  int i, j, reconnect, tried, connected;

  tried = connected = 0;

  /* Loop through all configured kites:
   *   - if missing a desired front-end, tear down tunnels and reconnect.
   */
  pkm_block(pkm);
  for (i = 0; i < pkm->frontend_max; i++) {
    fe = (pkm->frontends + i);

    if (fe->fe_hostname == NULL) continue;
    if (!(fe->conn.status & (FE_STATUS_WANTED|FE_STATUS_IN_DNS))) continue;

    if (fe->requests == NULL || fe->request_count != pkm->kite_max) {
      fe->request_count = pkm->kite_max;
      memset(fe->requests, 0, pkm->kite_max * sizeof(struct pk_kite_request));
      for (kite_r = fe->requests, j = 0; j < pkm->kite_max; j++, kite_r++) {
        kite_r->kite = (pkm->kites + j);
        kite_r->status = PK_KITE_UNKNOWN;
      }
    }

    reconnect = 0;
    for (kite_r = fe->requests, j = 0; j < pkm->kite_max; j++, kite_r++) {
      if (kite_r->status == PK_KITE_UNKNOWN) reconnect++;
    }

    if (reconnect) {
      tried++;
      PKS_STATE(pkm->status = PK_STATUS_CONNECT);
      if (0 <= fe->conn.sockfd) {
        ev_io_stop(pkm->loop, &(fe->conn.watch_r));
        ev_io_stop(pkm->loop, &(fe->conn.watch_w));
        close(fe->conn.sockfd);
        fe->conn.sockfd = -1;
      }
      status = fe->conn.status;
      pkc_reset_conn(&(fe->conn));
      fe->conn.status = (CONN_STATUS_ALLOCATED | (status & FE_STATUS_BITS));

      /* Unblock the event loop while we attempt to connect. */
      pkm_unblock(pkm);

      if ((0 <= pk_connect_ai(&(fe->conn), fe->ai, 0,
                              fe->request_count, fe->requests,
                              (fe->fe_session), fe->manager->ssl_ctx)) &&
          (0 < set_non_blocking(fe->conn.sockfd))) {
        pk_log(PK_LOG_MANAGER_INFO, "Connected!");
        pkm_block(pkm); /* Re-block */

        ev_io_init(&(fe->conn.watch_r),
                   pkm_tunnel_readable_cb, fe->conn.sockfd, EV_READ);
        ev_io_init(&(fe->conn.watch_w),
                   pkm_tunnel_writable_cb, fe->conn.sockfd, EV_WRITE);
        ev_io_start(pkm->loop, &(fe->conn.watch_r));
        fe->conn.watch_r.data = fe->conn.watch_w.data = (void *) fe;

        PKS_STATE(pk_state.live_frontends += 1);
        connected++;
      }
      else {
        pkm_block(pkm); /* Re-block */

        /* FIXME: Is this the right behavior? */
        pk_log(PK_LOG_MANAGER_INFO, "Connect failed: %d", fe->conn.sockfd);
        fe->request_count = 0;

        status = fe->conn.status;
        if (pk_error == ERR_CONNECT_REJECTED) {
          status |= FE_STATUS_REJECTED;
          PKS_STATE(pkm->status = PK_STATUS_REJECTED);
        }
        pkc_reset_conn(&(fe->conn));
        fe->conn.status = (CONN_STATUS_ALLOCATED | (status & FE_STATUS_BITS));

        pk_perror("pkmanager.c");
      }
    }
  }
  pkm_unblock(pkm);
  return (tried - connected);
}

void pkm_tick(struct pk_manager* pkm)
{
  ev_async_send(pkm->loop, &(pkm->tick));
}
static void pkm_tick_cb(EV_P_ ev_async *w, int revents)
{
  struct pk_manager* pkm = (struct pk_manager*) w->data;

  /* First, we look at the state of the world and schedule (or cancel)
   * our next tick. */
  if (pkm->enable_timer || (pkm->status != PK_STATUS_NO_NETWORK &&
                            pkm->status != PK_STATUS_REJECTED &&
                            pkm->status != PK_STATUS_FLYING))
  {
    pkm->timer.repeat = pkm->next_tick;
    ev_timer_again(pkm->loop, &(pkm->timer));
    pk_log(PK_LOG_MANAGER_DEBUG, "Tick!  [repeating=%s, next=%d]",
           pkm->enable_timer ? "yes" : "no", pkm->next_tick);

    /* We slow down exponentially by default, no matter what. */
    pkm->next_tick *= 2;
    if (pkm->next_tick > PK_HOUSEKEEPING_INTERVAL_MAX)
      pkm->next_tick = PK_HOUSEKEEPING_INTERVAL_MAX;
  }
  else {
    ev_timer_stop(pkm->loop, &(pkm->timer));
    pk_log(PK_LOG_MANAGER_DEBUG, "Tick!  [repeating=%s, stopped]",
           pkm->enable_timer ? "yes" : "no");
    /* Reset interval. */
    pkm->next_tick = PK_HOUSEKEEPING_INTERVAL_MIN;
  }

  /* FIXME: Loop through all configured tunnels:
   *   - if idle, queue a ping.
   *   - if dead, shut 'em down.
   *   - this will force need_timer to nonzero, skip on mobile?
   */

  /* Finally, trigger the frontend check on the blocking thread. */
  if (pkm->last_world_update + PK_CHECK_WORLD_INTERVAL < time(0)) {
    pkb_add_job(&(pkm->blocking_jobs), PK_CHECK_WORLD, pkm);
    /* After checking the state of the world, we are a bit more aggressive
     * about following up on things, reset the fallback. */
    pkm->next_tick = PK_HOUSEKEEPING_INTERVAL_MIN;
  }
  else {
    pkb_add_job(&(pkm->blocking_jobs), PK_CHECK_FRONTENDS, pkm);
  }
  pkm_yield(pkm);

  /* -Wall dislikes unused arguments */
  (void) loop;
  (void) revents;
}

void pkm_timer_cb(EV_P_ ev_timer *w, int revents)
{
  struct pk_manager* pkm = (struct pk_manager*) w->data;
  pkm_tick(pkm);
  /* -Wall dislikes unused arguments */
  (void) loop;
  (void) revents;
}
void pkm_reset_timer(struct pk_manager* pkm) {
  ev_timer_set(&(pkm->timer), 0.0, PK_HOUSEKEEPING_INTERVAL_MIN);
  ev_timer_start(pkm->loop, &(pkm->timer));
  pkm->next_tick = PK_HOUSEKEEPING_INTERVAL_MIN;
}
void pkm_set_timer_enabled(struct pk_manager* pkm, int enabled) {
  pkm->enable_timer = (enabled > 0);
  if (pkm->enable_timer) {
    pkm_reset_timer(pkm);
  }
  else ev_timer_stop(pkm->loop, &(pkm->timer));
}


struct pk_manager* pkm_manager_init(struct ev_loop* loop,
                                    int buffer_size, char* buffer,
                                    int kites, int frontends, int conns,
                                    const char* dynamic_dns_url, SSL_CTX* ctx)
{
  struct pk_manager* pkm;
  int i;
  unsigned int parse_buffer_bytes;

#ifdef HAVE_OPENSSL
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_ZERO_RETURN = %d", SSL_ERROR_ZERO_RETURN);
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_WANT_WRITE = %d", SSL_ERROR_WANT_WRITE);
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_WANT_READ = %d", SSL_ERROR_WANT_READ);
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_WANT_CONNECT = %d", SSL_ERROR_WANT_CONNECT);
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_WANT_ACCEPT = %d", SSL_ERROR_WANT_ACCEPT);
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_WANT_X509_LOOKUP = %d", SSL_ERROR_WANT_X509_LOOKUP);
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_SYSCALL = %d", SSL_ERROR_SYSCALL);
  pk_log(PK_LOG_TUNNEL_DATA, "SSL_ERROR_SSL = %d", SSL_ERROR_SSL);
#endif

  if (kites < MIN_KITE_ALLOC) kites = MIN_KITE_ALLOC;
  if (frontends < MIN_FE_ALLOC) frontends = MIN_FE_ALLOC;
  if (conns < MIN_CONN_ALLOC) conns = MIN_CONN_ALLOC;

  if (buffer == NULL) {
    buffer_size = PK_MANAGER_BUFSIZE(kites, frontends, conns, PARSER_BYTES_AVG);
    buffer = malloc(buffer_size);
  }
  else {
    i = PK_MANAGER_BUFSIZE(kites, frontends, conns, PARSER_BYTES_MIN);
    if (buffer_size < i) {
      pk_log(PK_LOG_MANAGER_ERROR,
             "pkm_manager_init: Buffer (%d bytes) too small, need %d.",
             buffer_size, i);
      return pk_err_null(ERR_TOOBIG_PARSERS);
    }
  }
  if (buffer == NULL) {
    pk_log(PK_LOG_MANAGER_ERROR, "pkm_manager_init: No buffer! Malloc failed?");
    return NULL;
  }

  if (loop == NULL) loop = EV_DEFAULT;

  memset(buffer, 0, buffer_size);

  pkm = (struct pk_manager*) buffer;
  pkm->status = PK_STATUS_STARTUP;
  pkm->buffer_bytes_free = buffer_size;

  pkm->buffer = buffer + sizeof(struct pk_manager);
  pkm->buffer_bytes_free -= sizeof(struct pk_manager);
  pkm->buffer_base = pkm->buffer;
  pkm->loop = loop;
  if (pkm->buffer_bytes_free < 0) return pk_err_null(ERR_TOOBIG_MANAGER);

  /* Allocate space for the kites */
  pkm->buffer_bytes_free -= sizeof(struct pk_pagekite) * kites;
  if (pkm->buffer_bytes_free < 0) return pk_err_null(ERR_TOOBIG_KITES);
  pkm->kites = (struct pk_pagekite *) pkm->buffer;
  pkm->kite_max = kites;
  pkm->buffer += sizeof(struct pk_pagekite) * kites;

  /* Allocate space for the frontends */
  pkm->buffer_bytes_free -= (sizeof(struct pk_frontend) * frontends);
  pkm->buffer_bytes_free -= (sizeof(struct pk_kite_request) * kites * frontends);
  if (pkm->buffer_bytes_free < 0) return pk_err_null(ERR_TOOBIG_FRONTENDS);
  pkm->frontends = (struct pk_frontend *) pkm->buffer;
  pkm->frontend_max = frontends;
  pkm->buffer += sizeof(struct pk_frontend) * frontends;
  for (i = 0; i < frontends; i++) {
    (pkm->frontends + i)->ai = NULL;
    (pkm->frontends + i)->requests = (struct pk_kite_request*) pkm->buffer;
#ifdef HAVE_OPENSSL
    (pkm->frontends + i)->conn.ssl = NULL;
#endif
    pkm->buffer += (sizeof(struct pk_kite_request) * kites);
  }

  /* Allocate space for the backend connections */
  pkm->buffer_bytes_free -= sizeof(struct pk_backend_conn) * conns;
  if (pkm->buffer_bytes_free < 0) return pk_err_null(ERR_TOOBIG_BE_CONNS);
  pkm->be_conns = (struct pk_backend_conn *) pkm->buffer;
  pkm->be_conn_max = conns;
  for (i = 0; i < conns; i++) {
    (pkm->be_conns+i)->conn.sockfd = -1;
#ifdef HAVE_OPENSSL
    (pkm->be_conns+i)->conn.ssl = NULL;
#endif
    pkc_reset_conn(&(pkm->be_conns+i)->conn);
  }
  pkm->buffer += sizeof(struct pk_backend_conn) * conns;

  /* Allocate space for the blocking job queue */
  pkm->buffer_bytes_free -= sizeof(struct pk_job) * (conns+frontends);
  if (pkm->buffer_bytes_free < 0) return pk_err_null(ERR_TOOBIG_BE_CONNS);
  pkm->blocking_jobs.pile = (struct pk_job *) pkm->buffer;
  pkm->blocking_jobs.max = (conns+frontends);
  for (i = 0; i < (conns+frontends); i++) {
    (pkm->blocking_jobs.pile+i)->job = PK_NO_JOB;
  }
  pkm->buffer += sizeof(struct pk_job) * (conns+frontends);

  /* Whatever is left, we divide evenly between the protocol parsers... */
  parse_buffer_bytes = (pkm->buffer_bytes_free-1) / frontends;
  /* ... within reason. */
  if (parse_buffer_bytes > PARSER_BYTES_MAX)
    parse_buffer_bytes = PARSER_BYTES_MAX;
  if (parse_buffer_bytes < PARSER_BYTES_MIN)
    return pk_err_null(ERR_TOOBIG_PARSERS);

  /* Initialize the frontend structs... */
  for (i = 0; i < frontends; i++) {
    (pkm->frontends+i)->manager = pkm;
    (pkm->frontends+i)->conn.sockfd = -1;
#ifdef HAVE_OPENSSL
    (pkm->frontends+i)->conn.ssl = NULL;
#endif
    (pkm->frontends+i)->parser = pk_parser_init(parse_buffer_bytes,
                                                (char *) pkm->buffer,
                                               (pkChunkCallback*) &pkm_chunk_cb,
                                              (pkm->frontends+i));
    pkm->buffer += parse_buffer_bytes;
    pkm->buffer_bytes_free -= parse_buffer_bytes;
  }
  pkm->fancy_pagekite_net_rejection = 1;
  pkm->want_spare_frontends = 0;
  pkm->last_world_update = (time_t) 0;
  pkm->dynamic_dns_url = dynamic_dns_url ? strdup(dynamic_dns_url) : NULL;

  pkm->ssl_ctx = ctx;
  PKS_STATE(pk_state.have_ssl = (ctx != NULL);
            pk_state.force_update = 1)

  /* Set up our event-loop callbacks */
  ev_timer_init(&(pkm->timer), pkm_timer_cb, 0, 0);
  pkm->timer.data = (void *) pkm;
  pkm_reset_timer(pkm);
  pkm->enable_timer = 1;

  /* Let external threads shut us down */
  ev_async_init(&(pkm->quit), pkm_quit_cb);
  ev_async_start(loop, &(pkm->quit));

  /* Let external threads control our "periodic housekeeping" */
  ev_async_init(&(pkm->tick), pkm_tick_cb);
  pkm->tick.data = (void *) pkm;
  ev_async_start(loop, &(pkm->tick));

  /* Let external threads interrupt the loop */
  ev_async_init(&(pkm->interrupt), pkm_interrupt_cb);
  pkm->interrupt.data = (void *) pkm;
  ev_async_start(loop, &(pkm->interrupt));

  /* Prepare blocking thread structures. */
  pthread_mutex_init(&(pkm->loop_lock), NULL);
  pthread_mutex_init(&(pkm->blocking_jobs.mutex), NULL);
  pthread_cond_init(&(pkm->blocking_jobs.cond), NULL);
  pkm->blocking_jobs.count = 0;

  pk_log(PK_LOG_MANAGER_INFO,
         "Initialized %s manager v%s/%s (using %d bytes)",
         pk_state.app_id_long, PK_VERSION, pk_state.app_id_short, buffer_size);
  return pkm;
}

static void pkm_reset_manager(struct pk_manager* pkm) {
  int i;
  struct pk_conn* pkc;

  for (i = 0; i < pkm->kite_max; i++) {
    pk_reset_pagekite(pkm->kites+i);
  }
  for (i = 0; i < pkm->frontend_max; i++) {
    pkc = &((pkm->frontends+i)->conn);
    if (pkc->status != CONN_STATUS_UNKNOWN) {
      ev_io_stop(pkm->loop, &(pkc->watch_r));
      ev_io_stop(pkm->loop, &(pkc->watch_w));
      pkc_reset_conn(pkc);
    }
  }
  for (i = 0; i < pkm->be_conn_max; i++) {
    pkc = &((pkm->be_conns+i)->conn);
    if (pkc->status != CONN_STATUS_UNKNOWN) {
      ev_io_stop(pkm->loop, &(pkc->watch_r));
      ev_io_stop(pkm->loop, &(pkc->watch_w));
      pkc_reset_conn(pkc);
    }
  }
  ev_async_stop(pkm->loop, &(pkm->quit));
}

struct pk_pagekite* pkm_find_kite(struct pk_manager* pkm,
                                  const char* protocol,
                                  const char* domain,
                                  int port)
{
  int which;
  struct pk_pagekite* kite;
  struct pk_pagekite* found;

  /* FIXME: This is O(N), we'll need a nicer data structure for frontends */
  found = NULL;
  for (which = 0; which < pkm->kite_max; which++) {
    kite = pkm->kites+which;
    if (kite->protocol != NULL) {
      if ((0 == strcasecmp(domain, kite->public_domain)) &&
          (0 == strcasecmp(protocol, kite->protocol))) {
        if (kite->public_port <= 0)
          found = kite;
        else if (kite->public_port == port)
          return kite;
      }
    }
  }
  return found;
}

struct pk_pagekite* pkm_add_kite(struct pk_manager* pkm,
                                 const char* protocol,
                                 const char* public_domain, int public_port,
                                 const char* auth_secret,
                                 const char* local_domain, int local_port)
{
  int which;
  struct pk_pagekite* kite;

  /* FIXME: This is O(N), we'll need a nicer data structure for frontends */
  for (which = 0; which < pkm->kite_max; which++) {
    kite = pkm->kites+which;
    if (kite->protocol == NULL) break;
  }
  if (which >= pkm->kite_max)
    return pk_err_null(ERR_NO_MORE_KITES);

  kite->protocol = strdup(protocol);
  kite->auth_secret = strdup(auth_secret);
  kite->public_domain = strdup(public_domain);
  kite->public_port = public_port;
  kite->local_domain = strdup(local_domain);
  kite->local_port = local_port;

  return kite;
}

int pkm_add_frontend(struct pk_manager* pkm,
                     const char* hostname, int port, int flags)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  char printip[128], sport[128];
  int rv, count;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  sprintf(sport, "%d", port);

  rv = getaddrinfo(hostname, sport, &hints, &result);
  count = 0;
  if (rv == 0) {
    for (rp = result; rp != NULL; rp = rp->ai_next) {
      if (NULL != pkm_add_frontend_ai(pkm, rp, hostname, port, flags)) {
        pk_log(PK_LOG_MANAGER_DEBUG, "Front-end IP: %s",
               in_addr_to_str(rp->ai_addr, printip, 128));
        count++;
      }
    }
  }
  return count;
}

struct pk_frontend* pkm_add_frontend_ai(struct pk_manager* pkm,
                                        struct addrinfo *ai,
                                        const char* hostname, int port,
                                        int flags)
{
  int which;
  struct pk_frontend* fe;
  struct pk_frontend* adding = NULL;

  /* Scan the front-end list to see if we already have this IP or,
   * if not, find an available slot.
   */
  for (which = 0; which < pkm->frontend_max; which++) {
    fe = pkm->frontends+which;
    if (fe->ai == NULL) {
      if (adding == NULL) adding = fe;
    }
    else if ((ai->ai_addrlen > 0) &&
             (0 == addrcmp(fe->ai->ai_addr, ai->ai_addr)))
    {
      return NULL;
    }
  }
  if (adding == NULL)
    return pk_err_null(ERR_NO_MORE_FRONTENDS);

  adding->ai = ai;
  adding->fe_hostname = strdup(hostname);
  adding->fe_port = port;
  adding->conn.status = (flags | CONN_STATUS_ALLOCATED);
  adding->request_count = 0;
  adding->priority = 0;

  return adding;
}

unsigned char pkm_sid_shift(char *sid)
{
  unsigned char shift;
  char *c;

  for (c = sid, shift = 0; *c != '\0'; c++) {
    shift = (shift << 3) | (shift >> 5);
    shift ^= *c;
  }
  return shift;
}

struct pk_backend_conn* pkm_alloc_be_conn(struct pk_manager* pkm,
                                          struct pk_frontend* fe, char *sid)
{
  int i;
  unsigned char shift;
  struct pk_backend_conn* pkb;

  shift = pkm_sid_shift(sid);
  for (i = 0; i < pkm->be_conn_max; i++) {
    pkb = (pkm->be_conns + ((i + shift) % pkm->be_conn_max));
    if (!(pkb->conn.status & CONN_STATUS_ALLOCATED)) {
      pkc_reset_conn(&(pkb->conn));
      pkb->frontend = fe;
      pkb->conn.status = CONN_STATUS_ALLOCATED;
      strncpy(pkb->sid, sid, BE_MAX_SID_SIZE);
      return pkb;
    }
  }
  return NULL;
}

void pkm_free_be_conn(struct pk_backend_conn* pkb)
{
  pkb->conn.status = CONN_STATUS_UNKNOWN;
}

struct pk_backend_conn* pkm_find_be_conn(struct pk_manager* pkm,
                                         struct pk_frontend* fe, char* sid)
{
  int i;
  unsigned char shift;
  struct pk_backend_conn* pkb;

  shift = pkm_sid_shift(sid);
  for (i = 0; i < pkm->be_conn_max; i++) {
    pkb = (pkm->be_conns + ((i + shift) % pkm->be_conn_max));
    if ((pkb->conn.status & CONN_STATUS_ALLOCATED) &&
        (pkb->frontend == fe) &&
        (0 == strncmp(pkb->sid, sid, BE_MAX_SID_SIZE))) {
      return pkb;
    }
  }
  return NULL;
}



/*** High level API stuff ****************************************************/

void* pkm_run(void *void_pkm) {
  struct pk_manager* pkm = (struct pk_manager*) void_pkm;

  pkb_start_blocker(pkm);
  pthread_mutex_lock(&(pkm->loop_lock));
  ev_loop(pkm->loop, 0);
  pthread_mutex_unlock(&(pkm->loop_lock));

  pkb_stop_blocker(pkm);
  pkm_reset_manager(pkm);
  pk_log(PK_LOG_MANAGER_DEBUG, "Event loop exited.");
  return void_pkm;
}

int pkm_run_in_thread(struct pk_manager* pkm) {
  pk_log(PK_LOG_MANAGER_INFO, "Starting manager in new thread");
  return pthread_create(&(pkm->main_thread), NULL, pkm_run, (void *) pkm);
}

int pkm_wait_thread(struct pk_manager* pkm) {
  return pthread_join(pkm->main_thread, NULL);
}

int pkm_stop_thread(struct pk_manager* pkm) {
  pk_log(PK_LOG_MANAGER_DEBUG, "Stopping manager...");
  pkm_quit(pkm);
  return pthread_join(pkm->main_thread, NULL);
}
