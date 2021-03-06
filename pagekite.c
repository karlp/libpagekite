/******************************************************************************
pagekite.c - A high-performance PageKite implementation in C.

Usage: pagekite LPORT PROTO NAME.pagekite.me PPORT SECRET

*******************************************************************************

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
#include "pkstate.h"
#include "pkerror.h"
#include "pkconn.h"
#include "pkproto.h"
#include "pklogging.h"
#include "pkblocker.h"
#include "pkmanager.h"

#include "pagekite_net.h"


struct pk_global_state pk_state;

void usage(void) {
  fprintf(stderr, "Usage: pagekite LPORT PROTO NAME.pagekite.me PPORT SECRET\n");
}

int main(int argc, char **argv) {
  struct pk_manager *m;
  char* proto;
  char* kitename;
  char* secret;
  int ac;
  int pport;
  int lport;
  SSL_CTX* ssl_ctx;

  /* FIXME: Is this too lame? */
  srand(time(0) ^ getpid());

  if ((argc-1) < 5 || ((argc-1) % 5) != 0) {
    usage();
    exit(1);
  }

  pks_global_init(PK_LOG_ALL);
  PKS_SSL_INIT(ssl_ctx);
  if (NULL == (m = pkm_manager_init(NULL, 0, NULL,
                                    10, /* Kites */
                                    PAGEKITE_NET_FE_MAX,
                                    25, /* Simultaneous connections */
                                    PAGEKITE_NET_DDNS, ssl_ctx))) {
    pk_perror(argv[0]);
    exit(1);
  }

  for (ac = 0; ac+5 < argc; ac += 5) {
    if ((1 != sscanf(argv[ac+1], "%d", &lport)) ||
        (1 != sscanf(argv[ac+4], "%d", &pport))) {
      usage();
      exit(2);
    }
    proto = argv[ac+2];
    kitename = argv[ac+3];
    secret = argv[ac+5];
    if ((NULL == (pkm_add_kite(m, proto, kitename, 0, secret,
                               "localhost", lport))) ||
        (0 > (pkm_add_frontend(m, kitename, pport, FE_STATUS_AUTO)))) {
      pk_perror(argv[0]);
      exit(3);
    }
  }

  if ((0 > (pkm_add_frontend(m, PAGEKITE_NET_V4FRONTENDS, FE_STATUS_AUTO))) ||
      (0 > (pkm_add_frontend(m, PAGEKITE_NET_V6FRONTENDS, FE_STATUS_AUTO))) ||
      (0 > pkm_run_in_thread(m))) {
    pk_perror(argv[0]);
    exit(4);
  }

  pkm_wait_thread(m);
  return 0;
}

