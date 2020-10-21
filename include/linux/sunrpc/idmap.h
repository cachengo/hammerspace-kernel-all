/*
 * fs/nfs/nfs4idmap.h
 *
 *  UID and GID to name mapping for clients.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Marius Aamodt Eriksen <marius@umich.edu>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef SUNRPC_IDMAP_H
#define SUNRPC_IDMAP_H

#include <linux/uidgid.h>

/* Forward declaration to make this header independent of others */
struct rpc_clnt;

#include <linux/types.h>

/* XXX from bits/utmp.h  */
#define IDMAP_NAMESZ  128

#define IDMAP_TYPE_USER  0
#define IDMAP_TYPE_GROUP 1

#define IDMAP_CONV_IDTONAME 0
#define IDMAP_CONV_NAMETOID 1

#define IDMAP_STATUS_INVALIDMSG 0x01
#define IDMAP_STATUS_AGAIN      0x02
#define IDMAP_STATUS_LOOKUPFAIL 0x04
#define IDMAP_STATUS_SUCCESS    0x08

struct sunrpc_idmap_msg {
	__u8  im_type;
	__u8  im_conv;
	char  im_name[IDMAP_NAMESZ];
	__u32 im_id;
	__u8  im_status;
};

int sunrpc_idmap_init(void);
void sunrpc_idmap_quit(void);
int sunrpc_idmap_new(struct rpc_clnt *);
void sunrpc_idmap_delete(struct rpc_clnt *);

int sunrpc_idmap_name_to_uid(struct rpc_clnt *, const char *, size_t, kuid_t *);
int sunrpc_idmap_group_to_gid(struct rpc_clnt *, const char *, size_t, kgid_t *);
int sunrpc_idmap_uid_to_name(const struct rpc_clnt *, kuid_t, char *, size_t, bool);
int sunrpc_idmap_gid_to_group(const struct rpc_clnt *, kgid_t, char *, size_t, bool);

int sunrpc_idmap_string_to_numeric(const char *name, size_t namelen, __u32 *res);

void sunrpc_idmap_set_cache_timeout(unsigned int secs);
unsigned int sunrpc_idmap_get_cache_timeout(void);
#endif /* SUNRPC_IDMAP_H */
