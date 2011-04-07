#include "redis.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode *n, int slot);
sds clusterGenNodesDescription(void);
clusterNode *clusterLookupNode(char *name);
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave);
int clusterAddSlot(clusterNode *n, int slot);

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

void clusterGetRandomName(char *p) {
    FILE *fp = fopen("/dev/urandom","r");
    char *charset = "0123456789abcdef";
    int j;

    if (!fp) {
        redisLog(REDIS_WARNING,
            "Unrecovarable error: can't open /dev/urandom:%s" ,strerror(errno));
        exit(1);
    }
    fread(p,REDIS_CLUSTER_NAMELEN,1,fp);
    for (j = 0; j < REDIS_CLUSTER_NAMELEN; j++)
        p[j] = charset[p[j] & 0x0F];
    fclose(fp);
}

int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    char *line;
    int maxline, j;
   
    if (fp == NULL) return REDIS_ERR;

    /* Parse the file. Note that single liens of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case REDIS_CLUSTER_SLOTS/2 integers.
     * To simplify we allocate 1024+REDIS_CLUSTER_SLOTS*16 bytes per line. */
    maxline = 1024+REDIS_CLUSTER_SLOTS*16;
    line = zmalloc(maxline);
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv = sdssplitargs(line,&argc);
        clusterNode *n, *master;
        char *p, *s;

        /* Create this node if it does not exist */
        n = clusterLookupNode(argv[0]);
        if (!n) {
            n = createClusterNode(argv[0],0);
            clusterAddNode(n);
        }
        /* Address and port */
        if ((p = strchr(argv[1],':')) == NULL) goto fmterr;
        *p = '\0';
        memcpy(n->ip,argv[1],strlen(argv[1])+1);
        n->port = atoi(p+1);

        /* Parse flags */
        p = s = argv[2];
        while(p) {
            p = strchr(s,',');
            if (p) *p = '\0';
            if (!strcasecmp(s,"myself")) {
                redisAssert(server.cluster.myself == NULL);
                server.cluster.myself = n;
                n->flags |= REDIS_NODE_MYSELF;
            } else if (!strcasecmp(s,"master")) {
                n->flags |= REDIS_NODE_MASTER;
            } else if (!strcasecmp(s,"slave")) {
                n->flags |= REDIS_NODE_SLAVE;
            } else if (!strcasecmp(s,"fail?")) {
                n->flags |= REDIS_NODE_PFAIL;
            } else if (!strcasecmp(s,"fail")) {
                n->flags |= REDIS_NODE_FAIL;
            } else if (!strcasecmp(s,"handshake")) {
                n->flags |= REDIS_NODE_HANDSHAKE;
            } else if (!strcasecmp(s,"noaddr")) {
                n->flags |= REDIS_NODE_NOADDR;
            } else if (!strcasecmp(s,"noflags")) {
                /* nothing to do */
            } else {
                redisPanic("Unknown flag in redis cluster config file");
            }
            if (p) s = p+1;
        }

        /* Get master if any. Set the master and populate master's
         * slave list. */
        if (argv[3][0] != '-') {
            master = clusterLookupNode(argv[3]);
            if (!master) {
                master = createClusterNode(argv[3],0);
                clusterAddNode(master);
            }
            n->slaveof = master;
            clusterNodeAddSlave(master,n);
        }

        /* Set ping sent / pong received timestamps */
        if (atoi(argv[4])) n->ping_sent = time(NULL);
        if (atoi(argv[5])) n->pong_received = time(NULL);

        /* Populate hash slots served by this instance. */
        for (j = 7; j < argc; j++) {
            int start, stop;

            if ((p = strchr(argv[j],'-')) != NULL) {
                *p = '\0';
                start = atoi(argv[j]);
                stop = atoi(p+1);
            } else {
                start = stop = atoi(argv[j]);
            }
            while(start <= stop) clusterAddSlot(n, start++);
        }

        sdssplitargs_free(argv,argc);
    }
    zfree(line);
    fclose(fp);

    /* Config sanity check */
    redisAssert(server.cluster.myself != NULL);
    redisLog(REDIS_NOTICE,"Node configuration loaded, I'm %.40s",
        server.cluster.myself->name);
    return REDIS_OK;

fmterr:
    redisLog(REDIS_WARNING,"Unrecovarable error: corrupted cluster config file.");
    fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned. */
int clusterSaveConfig(void) {
    sds ci = clusterGenNodesDescription();
    int fd;
    
    if ((fd = open(server.cluster.configfile,O_WRONLY|O_CREAT|O_TRUNC,0644))
        == -1) goto err;
    if (write(fd,ci,sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
    close(fd);
    sdsfree(ci);
    return 0;

err:
    sdsfree(ci);
    return -1;
}

void clusterSaveConfigOrDie(void) {
    if (clusterSaveConfig() == -1) {
        redisLog(REDIS_WARNING,"Fatal: can't update cluster config file.");
        exit(1);
    }
}

void clusterInit(void) {
    int saveconf = 0;

    server.cluster.myself = NULL;
    server.cluster.state = REDIS_CLUSTER_FAIL;
    server.cluster.nodes = dictCreate(&clusterNodesDictType,NULL);
    server.cluster.node_timeout = 15;
    memset(server.cluster.migrating_slots_to,0,
        sizeof(server.cluster.migrating_slots_to));
    memset(server.cluster.importing_slots_from,0,
        sizeof(server.cluster.importing_slots_from));
    memset(server.cluster.slots,0,
        sizeof(server.cluster.slots));
    if (clusterLoadConfig(server.cluster.configfile) == REDIS_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        server.cluster.myself = createClusterNode(NULL,REDIS_NODE_MYSELF);
        redisLog(REDIS_NOTICE,"No cluster configuration found, I'm %.40s",
            server.cluster.myself->name);
        clusterAddNode(server.cluster.myself);
        saveconf = 1;
    }
    if (saveconf) clusterSaveConfigOrDie();
    /* We need a listening TCP port for our cluster messaging needs */
    server.cfd = anetTcpServer(server.neterr,
            server.port+REDIS_CLUSTER_PORT_INCR, server.bindaddr);
    if (server.cfd == -1) {
        redisLog(REDIS_WARNING, "Opening cluster TCP port: %s", server.neterr);
        exit(1);
    }
    if (aeCreateFileEvent(server.el, server.cfd, AE_READABLE,
        clusterAcceptHandler, NULL) == AE_ERR) oom("creating file event");
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */

clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->sndbuf = sdsempty();
    link->rcvbuf = sdsempty();
    link->node = node;
    link->fd = -1;
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * Just this function will make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
void freeClusterLink(clusterLink *link) {
    if (link->fd != -1) {
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
        aeDeleteFileEvent(server.el, link->fd, AE_READABLE);
    }
    sdsfree(link->sndbuf);
    sdsfree(link->rcvbuf);
    if (link->node)
        link->node->link = NULL;
    close(link->fd);
    zfree(link);
}

void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    char cip[128];
    clusterLink *link;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    cfd = anetTcpAccept(server.neterr, fd, cip, &cport);
    if (cfd == AE_ERR) {
        redisLog(REDIS_VERBOSE,"Accepting cluster node: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE,"Accepted cluster node %s:%d", cip, cport);
    /* We need to create a temporary node in order to read the incoming
     * packet in a valid contest. This node will be released once we
     * read the packet and reply. */
    link = createClusterLink(NULL);
    link->fd = cfd;
    aeCreateFileEvent(server.el,cfd,AE_READABLE,clusterReadHandler,link);
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 4096 hash slots. The hash slot of a given key is obtained
 * as the least significant 12 bits of the crc16 of the key. */
unsigned int keyHashSlot(char *key, int keylen) {
    return crc16(key,keylen) & 0x0FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. */
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));

    if (nodename)
        memcpy(node->name, nodename, REDIS_CLUSTER_NAMELEN);
    else
        clusterGetRandomName(node->name);
    node->flags = flags;
    memset(node->slots,0,sizeof(node->slots));
    node->numslaves = 0;
    node->slaves = NULL;
    node->slaveof = NULL;
    node->ping_sent = node->pong_received = 0;
    node->configdigest = NULL;
    node->configdigest_ts = 0;
    node->link = NULL;
    return node;
}

int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
    int j;

    for (j = 0; j < master->numslaves; j++) {
        if (master->slaves[j] == slave) {
            memmove(master->slaves+j,master->slaves+(j+1),
                (master->numslaves-1)-j);
            master->numslaves--;
            return REDIS_OK;
        }
    }
    return REDIS_ERR;
}

int clusterNodeAddSlave(clusterNode *master, clusterNode *slave) {
    int j;

    /* If it's already a slave, don't add it again. */
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] == slave) return REDIS_ERR;
    master->slaves = zrealloc(master->slaves,
        sizeof(clusterNode*)*(master->numslaves+1));
    master->slaves[master->numslaves] = slave;
    master->numslaves++;
    return REDIS_OK;
}

void clusterNodeResetSlaves(clusterNode *n) {
    zfree(n->slaves);
    n->numslaves = 0;
}

void freeClusterNode(clusterNode *n) {
    sds nodename;
    
    nodename = sdsnewlen(n->name, REDIS_CLUSTER_NAMELEN);
    redisAssert(dictDelete(server.cluster.nodes,nodename) == DICT_OK);
    sdsfree(nodename);
    if (n->slaveof) clusterNodeRemoveSlave(n->slaveof, n);
    if (n->link) freeClusterLink(n->link);
    zfree(n);
}

/* Add a node to the nodes hash table */
int clusterAddNode(clusterNode *node) {
    int retval;
    
    retval = dictAdd(server.cluster.nodes,
            sdsnewlen(node->name,REDIS_CLUSTER_NAMELEN), node);
    return (retval == DICT_OK) ? REDIS_OK : REDIS_ERR;
}

/* Node lookup by name */
clusterNode *clusterLookupNode(char *name) {
    sds s = sdsnewlen(name, REDIS_CLUSTER_NAMELEN);
    struct dictEntry *de;

    de = dictFind(server.cluster.nodes,s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetEntryVal(de);
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->name, REDIS_CLUSTER_NAMELEN);
   
    redisLog(REDIS_DEBUG,"Renaming node %.40s into %.40s",
        node->name, newname);
    retval = dictDelete(server.cluster.nodes, s);
    sdsfree(s);
    redisAssert(retval == DICT_OK);
    memcpy(node->name, newname, REDIS_CLUSTER_NAMELEN);
    clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. */
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
    uint16_t count = ntohs(hdr->count);
    clusterMsgDataGossip *g = (clusterMsgDataGossip*) hdr->data.ping.gossip;
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender);

    while(count--) {
        sds ci = sdsempty();
        uint16_t flags = ntohs(g->flags);
        clusterNode *node;

        if (flags == 0) ci = sdscat(ci,"noflags,");
        if (flags & REDIS_NODE_MYSELF) ci = sdscat(ci,"myself,");
        if (flags & REDIS_NODE_MASTER) ci = sdscat(ci,"master,");
        if (flags & REDIS_NODE_SLAVE) ci = sdscat(ci,"slave,");
        if (flags & REDIS_NODE_PFAIL) ci = sdscat(ci,"fail?,");
        if (flags & REDIS_NODE_FAIL) ci = sdscat(ci,"fail,");
        if (flags & REDIS_NODE_HANDSHAKE) ci = sdscat(ci,"handshake,");
        if (flags & REDIS_NODE_NOADDR) ci = sdscat(ci,"noaddr,");
        if (ci[sdslen(ci)-1] == ',') ci[sdslen(ci)-1] = ' ';

        redisLog(REDIS_DEBUG,"GOSSIP %.40s %s:%d %s",
            g->nodename,
            g->ip,
            ntohs(g->port),
            ci);
        sdsfree(ci);

        /* Update our state accordingly to the gossip sections */
        node = clusterLookupNode(g->nodename);
        if (node != NULL) {
            /* We already know this node. Let's start updating the last
             * time PONG figure if it is newer than our figure.
             * Note that it's not a problem if we have a PING already 
             * in progress against this node. */
            if (node->pong_received < ntohl(g->pong_received)) {
                 redisLog(REDIS_DEBUG,"Node pong_received updated by gossip");
                node->pong_received = ntohl(g->pong_received);
            }
            /* Mark this node as FAILED if we think it is possibly failing
             * and another node also thinks it's failing. */
            if (node->flags & REDIS_NODE_PFAIL &&
                (flags & (REDIS_NODE_FAIL|REDIS_NODE_PFAIL)))
            {
                redisLog(REDIS_NOTICE,"Received a PFAIL acknowledge from node %.40s, marking node %.40s as FAIL!", hdr->sender, node->name);
                node->flags &= ~REDIS_NODE_PFAIL;
                node->flags |= REDIS_NODE_FAIL;
                /* Broadcast the failing node name to everybody */
                clusterSendFail(node->name);
                clusterUpdateState();
                clusterSaveConfigOrDie();
            }
        } else {
            /* If it's not in NOADDR state and we don't have it, we
             * start an handshake process against this IP/PORT pairs.
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster. */
            if (sender && !(flags & REDIS_NODE_NOADDR)) {
                clusterNode *newnode;

                redisLog(REDIS_DEBUG,"Adding the new node");
                newnode = createClusterNode(NULL,REDIS_NODE_HANDSHAKE);
                memcpy(newnode->ip,g->ip,sizeof(g->ip));
                newnode->port = ntohs(g->port);
                clusterAddNode(newnode);
            }
        }

        /* Next node */
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 16 bytes. */
void nodeIp2String(char *buf, clusterLink *link) {
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);

    if (getpeername(link->fd, (struct sockaddr*) &sa, &salen) == -1)
        redisPanic("getpeername() failed.");
    strncpy(buf,inet_ntoa(sa.sin_addr),sizeof(link->node->ip));
}


/* Update the node address to the IP address that can be extracted
 * from link->fd, and at the specified port. */
void nodeUpdateAddress(clusterNode *node, clusterLink *link, int port) {
}

/* When this function is called, there is a packet to process starting
 * at node->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). */
int clusterProcessPacket(clusterLink *link) {
    clusterMsg *hdr = (clusterMsg*) link->rcvbuf;
    uint32_t totlen = ntohl(hdr->totlen);
    uint16_t type = ntohs(hdr->type);
    clusterNode *sender;

    redisLog(REDIS_DEBUG,"--- packet to process %lu bytes (%lu) ---",
        (unsigned long) totlen, sdslen(link->rcvbuf));
    if (totlen < 8) return 1;
    if (totlen > sdslen(link->rcvbuf)) return 1;
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        uint16_t count = ntohs(hdr->count);
        uint32_t explen; /* expected length of this packet */

        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip)*count);
        if (totlen != explen) return 1;
    }
    if (type == CLUSTERMSG_TYPE_FAIL) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataFail);
        if (totlen != explen) return 1;
    }

    sender = clusterLookupNode(hdr->sender);
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        int update_config = 0;
        redisLog(REDIS_DEBUG,"Ping packet received: %p", link->node);

        /* Add this node if it is new for us and the msg type is MEET.
         * In this stage we don't try to add the node with the right
         * flags, slaveof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the server. */
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            clusterNode *node;

            node = createClusterNode(NULL,REDIS_NODE_HANDSHAKE);
            nodeIp2String(node->ip,link);
            node->port = ntohs(hdr->port);
            clusterAddNode(node);
            update_config = 1;
        }

        /* Get info from the gossip section */
        clusterProcessGossipSection(hdr,link);

        /* Anyway reply with a PONG */
        clusterSendPing(link,CLUSTERMSG_TYPE_PONG);

        /* Update config if needed */
        if (update_config) clusterSaveConfigOrDie();
    } else if (type == CLUSTERMSG_TYPE_PONG) {
        int update_state = 0;
        int update_config = 0;

        redisLog(REDIS_DEBUG,"Pong packet received: %p", link->node);
        if (link->node) {
            if (link->node->flags & REDIS_NODE_HANDSHAKE) {
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. */
                if (sender) {
                    redisLog(REDIS_WARNING,
                        "Handshake error: we already know node %.40s, updating the address if needed.", sender->name);
                    nodeUpdateAddress(sender,link,ntohs(hdr->port));
                    freeClusterNode(link->node); /* will free the link too */
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was an handshake stage. */
                clusterRenameNode(link->node, hdr->sender);
                redisLog(REDIS_DEBUG,"Handshake with node %.40s completed.",
                    link->node->name);
                link->node->flags &= ~REDIS_NODE_HANDSHAKE;
                update_config = 1;
            } else if (memcmp(link->node->name,hdr->sender,
                        REDIS_CLUSTER_NAMELEN) != 0)
            {
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. */
                redisLog(REDIS_DEBUG,"PONG contains mismatching sender ID");
                link->node->flags |= REDIS_NODE_NOADDR;
                freeClusterLink(link);
                update_config = 1;
                /* FIXME: remove this node if we already have it.
                 *
                 * If we already have it but the IP is different, use
                 * the new one if the old node is in FAIL, PFAIL, or NOADDR
                 * status... */
                return 0;
            }
        }
        /* Update our info about the node */
        link->node->pong_received = time(NULL);

        /* Update master/slave info */
        if (sender) {
            if (!memcmp(hdr->slaveof,REDIS_NODE_NULL_NAME,
                sizeof(hdr->slaveof)))
            {
                sender->flags &= ~REDIS_NODE_SLAVE;
                sender->flags |= REDIS_NODE_MASTER;
                sender->slaveof = NULL;
            } else {
                clusterNode *master = clusterLookupNode(hdr->slaveof);

                sender->flags &= ~REDIS_NODE_MASTER;
                sender->flags |= REDIS_NODE_SLAVE;
                if (sender->numslaves) clusterNodeResetSlaves(sender);
                if (master) clusterNodeAddSlave(master,sender);
            }
        }

        /* Update our info about served slots if this new node is serving
         * slots that are not served from our point of view. */
        if (sender && sender->flags & REDIS_NODE_MASTER) {
            int newslots, j;

            newslots =
                memcmp(sender->slots,hdr->myslots,sizeof(hdr->myslots)) != 0;
            memcpy(sender->slots,hdr->myslots,sizeof(hdr->myslots));
            if (newslots) {
                for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
                    if (clusterNodeGetSlotBit(sender,j)) {
                        if (server.cluster.slots[j] == sender) continue;
                        if (server.cluster.slots[j] == NULL ||
                            server.cluster.slots[j]->flags & REDIS_NODE_FAIL)
                        {
                            server.cluster.slots[j] = sender;
                            update_state = update_config = 1;
                        }
                    }
                }
            }
        }

        /* Get info from the gossip section */
        clusterProcessGossipSection(hdr,link);

        /* Update the cluster state if needed */
        if (update_state) clusterUpdateState();
        if (update_config) clusterSaveConfigOrDie();
    } else if (type == CLUSTERMSG_TYPE_FAIL && sender) {
        clusterNode *failing;

        failing = clusterLookupNode(hdr->data.fail.about.nodename);
        if (failing && !(failing->flags & REDIS_NODE_FAIL)) {
            redisLog(REDIS_NOTICE,
                "FAIL message received from %.40s about %.40s",
                hdr->sender, hdr->data.fail.about.nodename);
            failing->flags |= REDIS_NODE_FAIL;
            failing->flags &= ~REDIS_NODE_PFAIL;
            clusterUpdateState();
            clusterSaveConfigOrDie();
        }
    } else {
        redisLog(REDIS_NOTICE,"Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.
   
   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. */
void clusterWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    clusterLink *link = (clusterLink*) privdata;
    ssize_t nwritten;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    nwritten = write(fd, link->sndbuf, sdslen(link->sndbuf));
    if (nwritten <= 0) {
        redisLog(REDIS_NOTICE,"I/O error writing to node link: %s",
            strerror(errno));
        handleLinkIOError(link);
        return;
    }
    link->sndbuf = sdsrange(link->sndbuf,nwritten,-1);
    if (sdslen(link->sndbuf) == 0)
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[1024];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = (clusterLink*) privdata;
    int readlen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

again:
    if (sdslen(link->rcvbuf) >= 4) {
        hdr = (clusterMsg*) link->rcvbuf;
        readlen = ntohl(hdr->totlen) - sdslen(link->rcvbuf);
    } else {
        readlen = 4 - sdslen(link->rcvbuf);
    }

    nread = read(fd,buf,readlen);
    if (nread == -1 && errno == EAGAIN) return; /* Just no data */

    if (nread <= 0) {
        /* I/O error... */
        redisLog(REDIS_NOTICE,"I/O error reading from node link: %s",
            (nread == 0) ? "connection closed" : strerror(errno));
        handleLinkIOError(link);
        return;
    } else {
        /* Read data and recast the pointer to the new buffer. */
        link->rcvbuf = sdscatlen(link->rcvbuf,buf,nread);
        hdr = (clusterMsg*) link->rcvbuf;
    }

    /* Total length obtained? read the payload now instead of burning
     * cycles waiting for a new event to fire. */
    if (sdslen(link->rcvbuf) == 4) goto again;

    /* Whole packet in memory? We can process it. */
    if (sdslen(link->rcvbuf) == ntohl(hdr->totlen)) {
        if (clusterProcessPacket(link)) {
            sdsfree(link->rcvbuf);
            link->rcvbuf = sdsempty();
        }
    }
}

/* Put stuff into the send buffer. */
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {
    if (sdslen(link->sndbuf) == 0 && msglen != 0)
        aeCreateFileEvent(server.el,link->fd,AE_WRITABLE,
                    clusterWriteHandler,link);

    link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);
}

/* Build the message header */
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen;

    memset(hdr,0,sizeof(*hdr));
    hdr->type = htons(type);
    memcpy(hdr->sender,server.cluster.myself->name,REDIS_CLUSTER_NAMELEN);
    memcpy(hdr->myslots,server.cluster.myself->slots,
        sizeof(hdr->myslots));
    memset(hdr->slaveof,0,REDIS_CLUSTER_NAMELEN);
    if (server.cluster.myself->slaveof != NULL) {
        memcpy(hdr->slaveof,server.cluster.myself->slaveof->name,
                                    REDIS_CLUSTER_NAMELEN);
    }
    hdr->port = htons(server.port);
    hdr->state = server.cluster.state;
    memset(hdr->configdigest,0,32); /* FIXME: set config digest */

    if (type == CLUSTERMSG_TYPE_FAIL) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataFail);
    }
    hdr->totlen = htonl(totlen);
    /* For PING, PONG, and MEET, fixing the totlen field is up to the caller */
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip informations. */
void clusterSendPing(clusterLink *link, int type) {
    unsigned char buf[1024];
    clusterMsg *hdr = (clusterMsg*) buf;
    int gossipcount = 0, totlen;
    /* freshnodes is the number of nodes we can still use to populate the
     * gossip section of the ping packet. Basically we start with the nodes
     * we have in memory minus two (ourself and the node we are sending the
     * message to). Every time we add a node we decrement the counter, so when
     * it will drop to <= zero we know there is no more gossip info we can
     * send. */
    int freshnodes = dictSize(server.cluster.nodes)-2;

    if (link->node && type == CLUSTERMSG_TYPE_PING)
        link->node->ping_sent = time(NULL);
    clusterBuildMessageHdr(hdr,type);
        
    /* Populate the gossip fields */
    while(freshnodes > 0 && gossipcount < 3) {
        struct dictEntry *de = dictGetRandomKey(server.cluster.nodes);
        clusterNode *this = dictGetEntryVal(de);
        clusterMsgDataGossip *gossip;
        int j;

        /* Not interesting to gossip about ourself.
         * Nor to send gossip info about HANDSHAKE state nodes (zero info). */
        if (this == server.cluster.myself ||
            this->flags & REDIS_NODE_HANDSHAKE) {
                freshnodes--; /* otherwise we may loop forever. */
                continue;
        }

        /* Check if we already added this node */
        for (j = 0; j < gossipcount; j++) {
            if (memcmp(hdr->data.ping.gossip[j].nodename,this->name,
                    REDIS_CLUSTER_NAMELEN) == 0) break;
        }
        if (j != gossipcount) continue;

        /* Add it */
        freshnodes--;
        gossip = &(hdr->data.ping.gossip[gossipcount]);
        memcpy(gossip->nodename,this->name,REDIS_CLUSTER_NAMELEN);
        gossip->ping_sent = htonl(this->ping_sent);
        gossip->pong_received = htonl(this->pong_received);
        memcpy(gossip->ip,this->ip,sizeof(this->ip));
        gossip->port = htons(this->port);
        gossip->flags = htons(this->flags);
        gossipcount++;
    }
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*gossipcount);
    hdr->count = htons(gossipcount);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(link,buf,totlen);
}

/* Send a message to all the nodes with a reliable link */
void clusterBroadcastMessage(void *buf, size_t len) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(server.cluster.nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetEntryVal(de);

        if (!node->link) continue;
        if (node->flags & (REDIS_NODE_MYSELF|REDIS_NODE_NOADDR)) continue;
        clusterSendMessage(node->link,buf,len);
    }
    dictReleaseIterator(di);
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (REDIS_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to REDIS_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. */
void clusterSendFail(char *nodename) {
    unsigned char buf[1024];
    clusterMsg *hdr = (clusterMsg*) buf;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAIL);
    memcpy(hdr->data.fail.about.nodename,nodename,REDIS_CLUSTER_NAMELEN);
    clusterBroadcastMessage(buf,ntohl(hdr->totlen));
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

/* This is executed 1 time every second */
void clusterCron(void) {
    dictIterator *di;
    dictEntry *de;
    int j;
    time_t min_ping_sent = 0;
    clusterNode *min_ping_node = NULL;

    /* Check if we have disconnected nodes and reestablish the connection. */
    di = dictGetIterator(server.cluster.nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetEntryVal(de);

        if (node->flags & (REDIS_NODE_MYSELF|REDIS_NODE_NOADDR)) continue;
        if (node->link == NULL) {
            int fd;
            clusterLink *link;

            fd = anetTcpNonBlockConnect(server.neterr, node->ip,
                node->port+REDIS_CLUSTER_PORT_INCR);
            if (fd == -1) continue;
            link = createClusterLink(node);
            link->fd = fd;
            node->link = link;
            aeCreateFileEvent(server.el,link->fd,AE_READABLE,clusterReadHandler,link);
            /* If the node is flagged as MEET, we send a MEET message instead
             * of a PING one, to force the receiver to add us in its node
             * table. */
            clusterSendPing(link, node->flags & REDIS_NODE_MEET ?
                    CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
            /* We can clear the flag after the first packet is sent.
             * If we'll never receive a PONG, we'll never send new packets
             * to this node. Instead after the PONG is received and we
             * are no longer in meet/handshake status, we want to send
             * normal PING packets. */
            node->flags &= ~REDIS_NODE_MEET;

            redisLog(REDIS_NOTICE,"Connecting with Node %.40s at %s:%d", node->name, node->ip, node->port+REDIS_CLUSTER_PORT_INCR);
        }
    }
    dictReleaseIterator(di);

    /* Ping some random node. Check a few random nodes and ping the one with
     * the oldest ping_sent time */
    for (j = 0; j < 5; j++) {
        de = dictGetRandomKey(server.cluster.nodes);
        clusterNode *this = dictGetEntryVal(de);

        if (this->link == NULL) continue;
        if (this->flags & (REDIS_NODE_MYSELF|REDIS_NODE_HANDSHAKE)) continue;
        if (min_ping_node == NULL || min_ping_sent > this->ping_sent) {
            min_ping_node = this;
            min_ping_sent = this->ping_sent;
        }
    }
    if (min_ping_node) {
        redisLog(REDIS_DEBUG,"Pinging node %40s", min_ping_node->name);
        clusterSendPing(min_ping_node->link, CLUSTERMSG_TYPE_PING);
    }

    /* Iterate nodes to check if we need to flag something as failing */
    di = dictGetIterator(server.cluster.nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetEntryVal(de);
        int delay;

        if (node->flags &
            (REDIS_NODE_MYSELF|REDIS_NODE_NOADDR|REDIS_NODE_HANDSHAKE|
             REDIS_NODE_FAIL)) continue;
        /* Check only if we already sent a ping and did not received
         * a reply yet. */
        if (node->ping_sent == 0 ||
            node->ping_sent <= node->pong_received) continue;

        delay = time(NULL) - node->pong_received;
        if (delay < server.cluster.node_timeout) {
            /* The PFAIL condition can be reversed without external
             * help if it is not transitive (that is, if it does not
             * turn into a FAIL state).
             *
             * The FAIL condition is also reversible if there are no slaves
             * for this host, so no slave election should be in progress.
             *
             * TODO: consider all the implications of resurrecting a
             * FAIL node. */
            if (node->flags & REDIS_NODE_PFAIL) {
                node->flags &= ~REDIS_NODE_PFAIL;
            } else if (node->flags & REDIS_NODE_FAIL && !node->numslaves) {
                node->flags &= ~REDIS_NODE_FAIL;
            }
        } else {
            /* Timeout reached. Set the noad se possibly failing if it is
             * not already in this state. */
            if (!(node->flags & REDIS_NODE_PFAIL)) {
                redisLog(REDIS_DEBUG,"*** NODE %.40s possibly failing",
                    node->name);
                node->flags |= REDIS_NODE_PFAIL;
            }
        }
    }
    dictReleaseIterator(di);
}

/* -----------------------------------------------------------------------------
 * Slots management
 * -------------------------------------------------------------------------- */

/* Set the slot bit and return the old value. */
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
    off_t byte = slot/8;
    int bit = slot&7;
    int old = (n->slots[byte] & (1<<bit)) != 0;
    n->slots[byte] |= 1<<bit;
    return old;
}

/* Clear the slot bit and return the old value. */
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    off_t byte = slot/8;
    int bit = slot&7;
    int old = (n->slots[byte] & (1<<bit)) != 0;
    n->slots[byte] &= ~(1<<bit);
    return old;
}

/* Return the slot bit from the cluster node structure. */
int clusterNodeGetSlotBit(clusterNode *n, int slot) {
    off_t byte = slot/8;
    int bit = slot&7;
    return (n->slots[byte] & (1<<bit)) != 0;
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return REDIS_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and REDIS_ERR is returned. */
int clusterAddSlot(clusterNode *n, int slot) {
    redisAssert(clusterNodeSetSlotBit(n,slot) == 0);
    server.cluster.slots[slot] = server.cluster.myself;
    printf("SLOT %d added to %.40s\n", slot, n->name);
    return REDIS_OK;
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function
 * -------------------------------------------------------------------------- */
void clusterUpdateState(void) {
    int ok = 1;
    int j;

    for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
        if (server.cluster.slots[j] == NULL ||
            server.cluster.slots[j]->flags & (REDIS_NODE_FAIL))
        {
            ok = 0;
            break;
        }
    }
    if (ok) {
        if (server.cluster.state == REDIS_CLUSTER_NEEDHELP) {
            server.cluster.state = REDIS_CLUSTER_NEEDHELP;
        } else {
            server.cluster.state = REDIS_CLUSTER_OK;
        }
    } else {
        server.cluster.state = REDIS_CLUSTER_FAIL;
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */

sds clusterGenNodesDescription(void) {
    sds ci = sdsempty();
    dictIterator *di;
    dictEntry *de;
    int j, start;

    di = dictGetIterator(server.cluster.nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetEntryVal(de);

        /* Node coordinates */
        ci = sdscatprintf(ci,"%.40s %s:%d ",
            node->name,
            node->ip,
            node->port);

        /* Flags */
        if (node->flags == 0) ci = sdscat(ci,"noflags,");
        if (node->flags & REDIS_NODE_MYSELF) ci = sdscat(ci,"myself,");
        if (node->flags & REDIS_NODE_MASTER) ci = sdscat(ci,"master,");
        if (node->flags & REDIS_NODE_SLAVE) ci = sdscat(ci,"slave,");
        if (node->flags & REDIS_NODE_PFAIL) ci = sdscat(ci,"fail?,");
        if (node->flags & REDIS_NODE_FAIL) ci = sdscat(ci,"fail,");
        if (node->flags & REDIS_NODE_HANDSHAKE) ci =sdscat(ci,"handshake,");
        if (node->flags & REDIS_NODE_NOADDR) ci = sdscat(ci,"noaddr,");
        if (ci[sdslen(ci)-1] == ',') ci[sdslen(ci)-1] = ' ';

        /* Slave of... or just "-" */
        if (node->slaveof)
            ci = sdscatprintf(ci,"%.40s ",node->slaveof->name);
        else
            ci = sdscatprintf(ci,"- ");

        /* Latency from the POV of this node, link status */
        ci = sdscatprintf(ci,"%ld %ld %s",
            (long) node->ping_sent,
            (long) node->pong_received,
            node->link ? "connected" : "disconnected");

        /* Slots served by this instance */
        start = -1;
        for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
            int bit;

            if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
                if (start == -1) start = j;
            }
            if (start != -1 && (!bit || j == REDIS_CLUSTER_SLOTS-1)) {
                if (j == REDIS_CLUSTER_SLOTS-1) j++;

                if (start == j-1) {
                    ci = sdscatprintf(ci," %d",start);
                } else {
                    ci = sdscatprintf(ci," %d-%d",start,j-1);
                }
                start = -1;
            }
        }
        ci = sdscatlen(ci,"\n",1);
    }
    dictReleaseIterator(di);
    return ci;
}

void clusterCommand(redisClient *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (!strcasecmp(c->argv[1]->ptr,"meet") && c->argc == 4) {
        clusterNode *n;
        struct sockaddr_in sa;
        long port;

        /* Perform sanity checks on IP/port */
        if (inet_aton(c->argv[2]->ptr,&sa.sin_addr) == 0) {
            addReplyError(c,"Invalid IP address in MEET");
            return;
        }
        if (getLongFromObjectOrReply(c, c->argv[3], &port, NULL) != REDIS_OK ||
                    port < 0 || port > (65535-REDIS_CLUSTER_PORT_INCR))
        {
            addReplyError(c,"Invalid TCP port specified");
            return;
        }

        /* Finally add the node to the cluster with a random name, this 
         * will get fixed in the first handshake (ping/pong). */
        n = createClusterNode(NULL,REDIS_NODE_HANDSHAKE|REDIS_NODE_MEET);
        strncpy(n->ip,inet_ntoa(sa.sin_addr),sizeof(n->ip));
        n->port = port;
        clusterAddNode(n);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"nodes") && c->argc == 2) {
        robj *o;
        sds ci = clusterGenNodesDescription();

        o = createObject(REDIS_STRING,ci);
        addReplyBulk(c,o);
        decrRefCount(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"addslots") && c->argc >= 3) {
        int j;
        long long slot;
        unsigned char *slots = zmalloc(REDIS_CLUSTER_SLOTS);

        memset(slots,0,REDIS_CLUSTER_SLOTS);
        /* Check that all the arguments are parsable and that all the
         * slots are not already busy. */
        for (j = 2; j < c->argc; j++) {
            if (getLongLongFromObject(c->argv[j],&slot) != REDIS_OK ||
                slot < 0 || slot > REDIS_CLUSTER_SLOTS)
            {
                addReplyError(c,"Invalid or out of range slot index");
                zfree(slots);
                return;
            }
            if (server.cluster.slots[slot]) {
                addReplyErrorFormat(c,"Slot %lld is already busy", slot);
                zfree(slots);
                return;
            }
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",
                    (int)slot);
                zfree(slots);
                return;
            }
        }
        for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
            if (slots[j]) {
                int retval = clusterAddSlot(server.cluster.myself,j);
                
                redisAssert(retval == REDIS_OK);
            }
        }
        zfree(slots);
        clusterUpdateState();
        clusterSaveConfigOrDie();
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        char *statestr[] = {"ok","fail","needhelp"};
        int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
        int j;

        for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
            clusterNode *n = server.cluster.slots[j];

            if (n == NULL) continue;
            slots_assigned++;
            if (n->flags & REDIS_NODE_FAIL) {
                slots_fail++;
            } else if (n->flags & REDIS_NODE_PFAIL) {
                slots_pfail++;
            } else {
                slots_ok++;
            }
        }

        sds info = sdscatprintf(sdsempty(),
            "cluster_state:%s\r\n"
            "cluster_slots_assigned:%d\r\n"
            "cluster_slots_ok:%d\r\n"
            "cluster_slots_pfail:%d\r\n"
            "cluster_slots_fail:%d\r\n"
            , statestr[server.cluster.state],
            slots_assigned,
            slots_ok,
            slots_pfail,
            slots_fail
        );
        addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
            (unsigned long)sdslen(info)));
        addReplySds(c,info);
        addReply(c,shared.crlf);
    } else {
        addReplyError(c,"Wrong CLUSTER subcommand or number of arguments");
    }
}

/* -----------------------------------------------------------------------------
 * RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

/* RESTORE key ttl serialized-value */
void restoreCommand(redisClient *c) {
    FILE *fp;
    char buf[64];
    robj *o;
    unsigned char *data;
    long ttl;

    /* Make sure this key does not already exist here... */
    if (dbExists(c->db,c->argv[1])) {
        addReplyError(c,"Target key name is busy.");
        return;
    }

    /* Check if the TTL value makes sense */
    if (getLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != REDIS_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* rdbLoadObject() only works against file descriptors so we need to
     * dump the serialized object into a file and reload. */
    snprintf(buf,sizeof(buf),"redis-restore-%d.tmp",getpid());
    fp = fopen(buf,"w+");
    if (!fp) {
        redisLog(REDIS_WARNING,"Can't open tmp file for RESTORE: %s",
            strerror(errno));
        addReplyErrorFormat(c,"RESTORE failed, tmp file creation error: %s",
            strerror(errno));
        return;
    }
    unlink(buf);

    /* Write the actual data and rewind the file */
    data = (unsigned char*) c->argv[3]->ptr;
    if (fwrite(data+1,sdslen((sds)data)-1,1,fp) != 1) {
        redisLog(REDIS_WARNING,"Can't write against tmp file for RESTORE: %s",
            strerror(errno));
        addReplyError(c,"RESTORE failed, tmp file I/O error.");
        fclose(fp);
        return;
    }
    rewind(fp);

    /* Finally create the object from the serialized dump and
     * store it at the specified key. */
    if ((data[0] > 4 && data[0] < 9) ||
         data[0] > 11 ||
        (o = rdbLoadObject(data[0],fp)) == NULL)
    {
        addReplyError(c,"Bad data format.");
        fclose(fp);
        return;
    }
    fclose(fp);

    /* Create the key and set the TTL if any */
    dbAdd(c->db,c->argv[1],o);
    if (ttl) setExpire(c->db,c->argv[1],time(NULL)+ttl);
    addReply(c,shared.ok);
}

/* MIGRATE host port key dbid timeout */
void migrateCommand(redisClient *c) {
    int fd;
    long timeout;
    long dbid;
    char buf[64];
    FILE *fp;
    time_t ttl;
    robj *o;
    unsigned char type;
    off_t payload_len;

    /* Sanity check */
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != REDIS_OK)
        return;
    if (getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != REDIS_OK)
        return;
    if (timeout <= 0) timeout = 1;

    /* Check if the key is here. If not we reply with success as there is
     * nothing to migrate (for instance the key expired in the meantime), but
     * we include such information in the reply string. */
    if ((o = lookupKeyRead(c->db,c->argv[3])) == NULL) {
        addReplySds(c,sdsnew("+NOKEY"));
        return;
    }
    
    /* Connect */
    fd = anetTcpNonBlockConnect(server.neterr,c->argv[1]->ptr,
                atoi(c->argv[2]->ptr));
    if (fd == -1) {
        addReplyErrorFormat(c,"Can't connect to target node: %s",
            server.neterr);
        return;
    }
    if ((aeWait(fd,AE_WRITABLE,timeout*1000) & AE_WRITABLE) == 0) {
        addReplyError(c,"Timeout connecting to the client");
        return;
    }

    /* Create temp file */
    snprintf(buf,sizeof(buf),"redis-migrate-%d.tmp",getpid());
    fp = fopen(buf,"w+");
    if (!fp) {
        redisLog(REDIS_WARNING,"Can't open tmp file for MIGRATE: %s",
            strerror(errno));
        addReplyErrorFormat(c,"MIGRATE failed, tmp file creation error: %s.",
            strerror(errno));
        return;
    }
    unlink(buf);

    /* Build the SELECT + RESTORE query writing it in our temp file. */
    if (fwriteBulkCount(fp,'*',2) == 0) goto file_wr_err;
    if (fwriteBulkString(fp,"SELECT",6) == 0) goto file_wr_err;
    if (fwriteBulkLongLong(fp,dbid) == 0) goto file_wr_err;

    ttl = getExpire(c->db,c->argv[3]);
    type = o->type;
    if (fwriteBulkCount(fp,'*',4) == 0) goto file_wr_err;
    if (fwriteBulkString(fp,"RESTORE",7) == 0) goto file_wr_err;
    if (fwriteBulkObject(fp,c->argv[3]) == 0) goto file_wr_err;
    if (fwriteBulkLongLong(fp, (ttl == -1) ? 0 : ttl) == 0) goto file_wr_err;

    /* Finally the last argument that is the serailized object payload
     * in the form: <type><rdb-serailized-object>. */
    payload_len = rdbSavedObjectLen(o);
    if (fwriteBulkCount(fp,'$',payload_len+1) == 0) goto file_wr_err;
    if (fwrite(&type,1,1,fp) == 0) goto file_wr_err;
    if (rdbSaveObject(fp,o) == -1) goto file_wr_err;
    if (fwrite("\r\n",2,1,fp) == 0) goto file_wr_err;

    /* Tranfer the query to the other node */
    rewind(fp);
    {
        char buf[4096];
        size_t nread;

        while ((nread = fread(buf,1,sizeof(buf),fp)) != 0) {
            int nwritten;

            nwritten = syncWrite(fd,buf,nread,timeout);
            if (nwritten != (signed)nread) goto socket_wr_err;
        }
        if (ferror(fp)) goto file_rd_err;
    }

    /* Read back the reply */
    {
        char buf1[1024];
        char buf2[1024];

        /* Read the two replies */
        if (syncReadLine(fd, buf1, sizeof(buf1), timeout) <= 0)
            goto socket_rd_err;
        if (syncReadLine(fd, buf2, sizeof(buf2), timeout) <= 0)
                goto socket_rd_err;
        if (buf1[0] == '-' || buf2[0] == '-') {
            addReplyErrorFormat(c,"Target instance replied with error: %s",
                (buf1[0] == '-') ? buf1+1 : buf2+1);
        } else {
            dbDelete(c->db,c->argv[3]);
            addReply(c,shared.ok);
        }
    }
    fclose(fp);
    close(fd);
    return;

file_wr_err:
    redisLog(REDIS_WARNING,"Can't write on tmp file for MIGRATE: %s",
        strerror(errno));
    addReplyErrorFormat(c,"MIGRATE failed, tmp file write error: %s.",
        strerror(errno));
    fclose(fp);
    close(fd);
    return;

file_rd_err:
    redisLog(REDIS_WARNING,"Can't read from tmp file for MIGRATE: %s",
        strerror(errno));
    addReplyErrorFormat(c,"MIGRATE failed, tmp file read error: %s.",
        strerror(errno));
    fclose(fp);
    close(fd);
    return;

socket_wr_err:
    redisLog(REDIS_NOTICE,"Can't write to target node for MIGRATE: %s",
        strerror(errno));
    addReplyErrorFormat(c,"MIGRATE failed, writing to target node: %s.",
        strerror(errno));
    fclose(fp);
    close(fd);
    return;

socket_rd_err:
    redisLog(REDIS_NOTICE,"Can't read from target node for MIGRATE: %s",
        strerror(errno));
    addReplyErrorFormat(c,"MIGRATE failed, reading from target node: %s.",
        strerror(errno));
    fclose(fp);
    close(fd);
    return;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
void dumpCommand(redisClient *c) {
    char buf[64];
    FILE *fp;
    robj *o, *dumpobj;
    sds dump = NULL;
    off_t payload_len;
    unsigned int type;

    /* Check if the key is here. */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }
    
    /* Create temp file */
    snprintf(buf,sizeof(buf),"redis-dump-%d.tmp",getpid());
    fp = fopen(buf,"w+");
    if (!fp) {
        redisLog(REDIS_WARNING,"Can't open tmp file for MIGRATE: %s",
            strerror(errno));
        addReplyErrorFormat(c,"DUMP failed, tmp file creation error: %s.",
            strerror(errno));
        return;
    }
    unlink(buf);

    /* Dump the serailized object and read it back in memory.
     * We prefix it with a one byte containing the type ID.
     * This is the serialization format understood by RESTORE. */
    if (rdbSaveObject(fp,o) == -1) goto file_wr_err;
    payload_len = ftello(fp);
    if (fseeko(fp,0,SEEK_SET) == -1) goto file_rd_err;
    dump = sdsnewlen(NULL,payload_len+1);
    if (payload_len && fread(dump+1,payload_len,1,fp) != 1) goto file_rd_err;
    fclose(fp);
    type = o->type;
    if (type == REDIS_LIST && o->encoding == REDIS_ENCODING_ZIPLIST)
        type = REDIS_LIST_ZIPLIST;
    else if (type == REDIS_HASH && o->encoding == REDIS_ENCODING_ZIPMAP)
        type = REDIS_HASH_ZIPMAP;
    else if (type == REDIS_SET && o->encoding == REDIS_ENCODING_INTSET)
        type = REDIS_SET_INTSET;
    else
        type = o->type;
    dump[0] = type;

    /* Transfer to the client */
    dumpobj = createObject(REDIS_STRING,dump);
    addReplyBulk(c,dumpobj);
    decrRefCount(dumpobj);
    return;

file_wr_err:
    redisLog(REDIS_WARNING,"Can't write on tmp file for DUMP: %s",
        strerror(errno));
    addReplyErrorFormat(c,"DUMP failed, tmp file write error: %s.",
        strerror(errno));
    sdsfree(dump);
    fclose(fp);
    return;

file_rd_err:
    redisLog(REDIS_WARNING,"Can't read from tmp file for DUMP: %s",
        strerror(errno));
    addReplyErrorFormat(c,"DUMP failed, tmp file read error: %s.",
        strerror(errno));
    sdsfree(dump);
    fclose(fp);
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * -------------------------------------------------------------------------- */

/* Return the pointer to the cluster node that is able to serve the query
 * as all the keys belong to hash slots for which the node is in charge.
 *
 * If keys in query spawn multiple nodes NULL is returned. */
clusterNode *getNodeByQuery(redisClient *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot) {
    clusterNode *n = NULL;
    multiState *ms, _ms;
    multiCmd mc;
    int i;

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything */
    if (cmd->proc == execCommand) {
        /* If REDIS_MULTI flag is not set EXEC is just going to return an
         * error. */
        if (!(c->flags & REDIS_MULTI)) return server.cluster.myself;
        ms = &c->mstate;
    } else {
        /* Create a fake Multi State structure, with just one command */
        ms = &_ms;
        _ms.commands = &mc;
        _ms.count = 1;
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
    }

    for (i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, *keyindex, numkeys, j;

        mcmd = ms->commands[i].cmd;
        margc = ms->commands[i].argc;
        margv = ms->commands[i].argv;

        keyindex = getKeysFromCommand(mcmd,margv,margc,&numkeys,
                                      REDIS_GETKEYS_PRELOAD);
        for (j = 0; j < numkeys; j++) {
            int slot = keyHashSlot((char*)margv[keyindex[j]]->ptr,
                sdslen(margv[keyindex[j]]->ptr));
            struct clusterNode *slotnode;

            slotnode = server.cluster.slots[slot];
            if (hashslot) *hashslot = slot;
            /* Node not assigned? (Should never happen actually
             * if we reached this function).
             * Different node than the previous one?
             * Return NULL, the cluster can't serve multi-node requests */
            if (slotnode == NULL || (n && slotnode != n)) {
                getKeysFreeResult(keyindex);
                return NULL;
            } else {
                n = slotnode;
            }
        }
        getKeysFreeResult(keyindex);
    }
    return (n == NULL) ? server.cluster.myself : n;
}
