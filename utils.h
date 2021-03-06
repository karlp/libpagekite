/******************************************************************************
utils.h - Utility functions for pagekite.

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

int zero_first_crlf(int, char*);
int dbg_write(int, char *, int);
int set_non_blocking(int);
int set_blocking(int);
ssize_t timed_read(int, void*, size_t, int);
char *in_ipaddr_to_str(const struct sockaddr*, char*, size_t);
char *in_addr_to_str(const struct sockaddr*, char*, size_t);
int addrcmp(const struct sockaddr *, const struct sockaddr *);
int http_get(const char*, char*, size_t);
void digest_to_hex(const unsigned char* digest, char *output);
