#include "redis.h"

/*-----------------------------------------------------------------------------
 * Config file parsing
 *----------------------------------------------------------------------------*/

int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}

void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}

void resetServerSaveParams() {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}

void loadServerConfigFromString(char *config) {
    char *err = NULL;
    int linenum = 0, totlines, i;
    sds *lines;

    lines = sdssplitlen(config,strlen(config),"\n",1,&totlines);

    for (i = 0; i < totlines; i++) {
        sds *argv;
        int argc;

        linenum = i+1;
        lines[i] = sdstrim(lines[i]," \t\r\n");

        /* Skip comments and blank lines*/
        if (lines[i][0] == '#' || lines[i][0] == '\0') continue;

        /* Split into arguments */
        argv = sdssplitargs(lines[i],&argc);
        sdstolower(argv[0]);

        /* Execute config directives */
        if (!strcasecmp(argv[0],"timeout") && argc == 2) {
            server.maxidletime = atoi(argv[1]);
            if (server.maxidletime < 0) {
                err = "Invalid timeout value"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"port") && argc == 2) {
            server.port = atoi(argv[1]);
            if (server.port < 0 || server.port > 65535) {
                err = "Invalid port"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"bind") && argc == 2) {
            server.bindaddr = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"unixsocket") && argc == 2) {
            server.unixsocket = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"unixsocketperm") && argc == 2) {
            errno = 0;
            server.unixsocketperm = (mode_t)strtol(argv[1], NULL, 8);
            if (errno || server.unixsocketperm > 0777) {
                err = "Invalid socket file permissions"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"save")) {
            if (argc == 3) {
                int seconds = atoi(argv[1]);
                int changes = atoi(argv[2]);
                if (seconds < 1 || changes < 0) {
                    err = "Invalid save parameters"; goto loaderr;
                }
                appendServerSaveParams(seconds,changes);
            } else if (argc == 2 && !strcasecmp(argv[1],"")) {
                resetServerSaveParams();
            }
        } else if (!strcasecmp(argv[0],"dir") && argc == 2) {
            if (chdir(argv[1]) == -1) {
                redisLog(REDIS_WARNING,"Can't chdir to '%s': %s",
                    argv[1], strerror(errno));
                exit(1);
            }
        } else if (!strcasecmp(argv[0],"loglevel") && argc == 2) {
            if (!strcasecmp(argv[1],"debug")) server.verbosity = REDIS_DEBUG;
            else if (!strcasecmp(argv[1],"verbose")) server.verbosity = REDIS_VERBOSE;
            else if (!strcasecmp(argv[1],"notice")) server.verbosity = REDIS_NOTICE;
            else if (!strcasecmp(argv[1],"warning")) server.verbosity = REDIS_WARNING;
            else {
                err = "Invalid log level. Must be one of debug, notice, warning";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"logfile") && argc == 2) {
            FILE *logfp;

            server.logfile = zstrdup(argv[1]);
            if (!strcasecmp(server.logfile,"stdout")) {
                zfree(server.logfile);
                server.logfile = NULL;
            }
            if (server.logfile) {
                /* Test if we are able to open the file. The server will not
                 * be able to abort just for this problem later... */
                logfp = fopen(server.logfile,"a");
                if (logfp == NULL) {
                    err = sdscatprintf(sdsempty(),
                        "Can't open the log file: %s", strerror(errno));
                    goto loaderr;
                }
                fclose(logfp);
            }
        } else if (!strcasecmp(argv[0],"syslog-enabled") && argc == 2) {
            if ((server.syslog_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"syslog-ident") && argc == 2) {
            if (server.syslog_ident) zfree(server.syslog_ident);
            server.syslog_ident = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"syslog-facility") && argc == 2) {
            struct {
                const char     *name;
                const int       value;
            } validSyslogFacilities[] = {
                {"user",    LOG_USER},
                {"local0",  LOG_LOCAL0},
                {"local1",  LOG_LOCAL1},
                {"local2",  LOG_LOCAL2},
                {"local3",  LOG_LOCAL3},
                {"local4",  LOG_LOCAL4},
                {"local5",  LOG_LOCAL5},
                {"local6",  LOG_LOCAL6},
                {"local7",  LOG_LOCAL7},
                {NULL, 0}
            };
            int i;

            for (i = 0; validSyslogFacilities[i].name; i++) {
                if (!strcasecmp(validSyslogFacilities[i].name, argv[1])) {
                    server.syslog_facility = validSyslogFacilities[i].value;
                    break;
                }
            }

            if (!validSyslogFacilities[i].name) {
                err = "Invalid log facility. Must be one of USER or between LOCAL0-LOCAL7";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"databases") && argc == 2) {
            server.dbnum = atoi(argv[1]);
            if (server.dbnum < 1) {
                err = "Invalid number of databases"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"include") && argc == 2) {
            loadServerConfig(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"maxclients") && argc == 2) {
            server.maxclients = atoi(argv[1]);
        } else if (!strcasecmp(argv[0],"maxmemory") && argc == 2) {
            server.maxmemory = memtoll(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"maxmemory-policy") && argc == 2) {
            if (!strcasecmp(argv[1],"volatile-lru")) {
                server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_LRU;
            } else if (!strcasecmp(argv[1],"volatile-random")) {
                server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_RANDOM;
            } else if (!strcasecmp(argv[1],"volatile-ttl")) {
                server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_TTL;
            } else if (!strcasecmp(argv[1],"allkeys-lru")) {
                server.maxmemory_policy = REDIS_MAXMEMORY_ALLKEYS_LRU;
            } else if (!strcasecmp(argv[1],"allkeys-random")) {
                server.maxmemory_policy = REDIS_MAXMEMORY_ALLKEYS_RANDOM;
            } else if (!strcasecmp(argv[1],"noeviction")) {
                server.maxmemory_policy = REDIS_MAXMEMORY_NO_EVICTION;
            } else {
                err = "Invalid maxmemory policy";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"maxmemory-samples") && argc == 2) {
            server.maxmemory_samples = atoi(argv[1]);
            if (server.maxmemory_samples <= 0) {
                err = "maxmemory-samples must be 1 or greater";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"slaveof") && argc == 3) {
            server.masterhost = sdsnew(argv[1]);
            server.masterport = atoi(argv[2]);
            server.repl_state = REDIS_REPL_CONNECT;
        } else if (!strcasecmp(argv[0],"repl-ping-slave-period") && argc == 2) {
            server.repl_ping_slave_period = atoi(argv[1]);
            if (server.repl_ping_slave_period <= 0) {
                err = "repl-ping-slave-period must be 1 or greater";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"repl-timeout") && argc == 2) {
            server.repl_timeout = atoi(argv[1]);
            if (server.repl_timeout <= 0) {
                err = "repl-timeout must be 1 or greater";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"masterauth") && argc == 2) {
        	server.masterauth = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"slave-serve-stale-data") && argc == 2) {
            if ((server.repl_serve_stale_data = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"slave-read-only") && argc == 2) {
            if ((server.repl_slave_ro = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"rdbcompression") && argc == 2) {
            if ((server.rdb_compression = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"rdbchecksum") && argc == 2) {
            if ((server.rdb_checksum = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"activerehashing") && argc == 2) {
            if ((server.activerehashing = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"daemonize") && argc == 2) {
            if ((server.daemonize = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendonly") && argc == 2) {
            int yes;

            if ((yes = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
            server.aof_state = yes ? REDIS_AOF_ON : REDIS_AOF_OFF;
        } else if (!strcasecmp(argv[0],"appendfilename") && argc == 2) {
            zfree(server.aof_filename);
            server.aof_filename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"no-appendfsync-on-rewrite")
                   && argc == 2) {
            if ((server.aof_no_fsync_on_rewrite= yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendfsync") && argc == 2) {
            if (!strcasecmp(argv[1],"no")) {
                server.aof_fsync = AOF_FSYNC_NO;
            } else if (!strcasecmp(argv[1],"always")) {
                server.aof_fsync = AOF_FSYNC_ALWAYS;
            } else if (!strcasecmp(argv[1],"everysec")) {
                server.aof_fsync = AOF_FSYNC_EVERYSEC;
            } else {
                err = "argument must be 'no', 'always' or 'everysec'";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"auto-aof-rewrite-percentage") &&
                   argc == 2)
        {
            server.aof_rewrite_perc = atoi(argv[1]);
            if (server.aof_rewrite_perc < 0) {
                err = "Invalid negative percentage for AOF auto rewrite";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"auto-aof-rewrite-min-size") &&
                   argc == 2)
        {
            server.aof_rewrite_min_size = memtoll(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"requirepass") && argc == 2) {
            server.requirepass = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"pidfile") && argc == 2) {
            zfree(server.pidfile);
            server.pidfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"dbfilename") && argc == 2) {
            zfree(server.rdb_filename);
            server.rdb_filename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"hash-max-ziplist-entries") && argc == 2) {
            server.hash_max_ziplist_entries = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"hash-max-ziplist-value") && argc == 2) {
            server.hash_max_ziplist_value = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"list-max-ziplist-entries") && argc == 2){
            server.list_max_ziplist_entries = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"list-max-ziplist-value") && argc == 2) {
            server.list_max_ziplist_value = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"set-max-intset-entries") && argc == 2) {
            server.set_max_intset_entries = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"zset-max-ziplist-entries") && argc == 2) {
            server.zset_max_ziplist_entries = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"zset-max-ziplist-value") && argc == 2) {
            server.zset_max_ziplist_value = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"rename-command") && argc == 3) {
            struct redisCommand *cmd = lookupCommand(argv[1]);
            int retval;

            if (!cmd) {
                err = "No such command in rename-command";
                goto loaderr;
            }

            /* If the target command name is the emtpy string we just
             * remove it from the command table. */
            retval = dictDelete(server.commands, argv[1]);
            redisAssert(retval == DICT_OK);

            /* Otherwise we re-add the command under a different name. */
            if (sdslen(argv[2]) != 0) {
                sds copy = sdsdup(argv[2]);

                retval = dictAdd(server.commands, copy, cmd);
                if (retval != DICT_OK) {
                    sdsfree(copy);
                    err = "Target command name already exists"; goto loaderr;
                }
            }
        } else if (!strcasecmp(argv[0],"cluster-enabled") && argc == 2) {
            if ((server.cluster_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"cluster-config-file") && argc == 2) {
            zfree(server.cluster.configfile);
            server.cluster.configfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"lua-time-limit") && argc == 2) {
            server.lua_time_limit = strtoll(argv[1],NULL,10);
        } else if (!strcasecmp(argv[0],"slowlog-log-slower-than") &&
                   argc == 2)
        {
            server.slowlog_log_slower_than = strtoll(argv[1],NULL,10);
        } else if (!strcasecmp(argv[0],"slowlog-max-len") && argc == 2) {
            server.slowlog_max_len = strtoll(argv[1],NULL,10);
        } else if (!strcasecmp(argv[0],"plugindir") && argc == 2) {
            server.plugindir = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"client-output-buffer-limit") &&
                   argc == 5)
        {
            int class = getClientLimitClassByName(argv[1]);
            unsigned long long hard, soft;
            int soft_seconds;

            if (class == -1) {
                err = "Unrecognized client limit class";
                goto loaderr;
            }
            hard = memtoll(argv[2],NULL);
            soft = memtoll(argv[3],NULL);
            soft_seconds = atoi(argv[4]);
            if (soft_seconds < 0) {
                err = "Negative number of seconds in soft limt is invalid";
                goto loaderr;
            }
            server.client_obuf_limits[class].hard_limit_bytes = hard;
            server.client_obuf_limits[class].soft_limit_bytes = soft;
            server.client_obuf_limits[class].soft_limit_seconds = soft_seconds;
        } else if (!strcasecmp(argv[0],"stop-writes-on-bgsave-error") &&
                   argc == 2) {
            if ((server.stop_writes_on_bgsave_err = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        sdsfreesplitres(argv,argc);
    }
    sdsfreesplitres(lines,totlines);
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", lines[i]);
    fprintf(stderr, "%s\n", err);
    exit(1);
}

/* Load the server configuration from the specified filename.
 * The function appends the additional configuration directives stored
 * in the 'options' string to the config file before loading.
 *
 * Both filename and options can be NULL, in such a case are considered
 * emtpy. This way loadServerConfig can be used to just load a file or
 * just load a string. */
void loadServerConfig(char *filename, char *options) {
    sds config = sdsempty();
    char buf[REDIS_CONFIGLINE_MAX+1];

    /* Load the file content */
    if (filename) {
        FILE *fp;

        if (filename[0] == '-' && filename[1] == '\0') {
            fp = stdin;
        } else {
            if ((fp = fopen(filename,"r")) == NULL) {
                redisLog(REDIS_WARNING,
                    "Fatal error, can't open config file '%s'", filename);
                exit(1);
            }
        }
        while(fgets(buf,REDIS_CONFIGLINE_MAX+1,fp) != NULL)
            config = sdscat(config,buf);
        if (fp != stdin) fclose(fp);
    }
    /* Append the additional options */
    if (options) {
        config = sdscat(config,"\n");
        config = sdscat(config,options);
    }
    loadServerConfigFromString(config);
    sdsfree(config);
}

/*-----------------------------------------------------------------------------
 * CONFIG command for remote configuration
 *----------------------------------------------------------------------------*/

void configSetCommand(redisClient *c) {
    robj *o;
    long long ll;
    redisAssertWithInfo(c,c->argv[2],c->argv[2]->encoding == REDIS_ENCODING_RAW);
    redisAssertWithInfo(c,c->argv[2],c->argv[3]->encoding == REDIS_ENCODING_RAW);
    o = c->argv[3];

    if (!strcasecmp(c->argv[2]->ptr,"dbfilename")) {
        zfree(server.rdb_filename);
        server.rdb_filename = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"requirepass")) {
        zfree(server.requirepass);
        server.requirepass = ((char*)o->ptr)[0] ? zstrdup(o->ptr) : NULL;
    } else if (!strcasecmp(c->argv[2]->ptr,"masterauth")) {
        zfree(server.masterauth);
        server.masterauth = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"maxmemory")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR ||
            ll < 0) goto badfmt;
        server.maxmemory = ll;
        if (server.maxmemory) freeMemoryIfNeeded();
    } else if (!strcasecmp(c->argv[2]->ptr,"maxmemory-policy")) {
        if (!strcasecmp(o->ptr,"volatile-lru")) {
            server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_LRU;
        } else if (!strcasecmp(o->ptr,"volatile-random")) {
            server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_RANDOM;
        } else if (!strcasecmp(o->ptr,"volatile-ttl")) {
            server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_TTL;
        } else if (!strcasecmp(o->ptr,"allkeys-lru")) {
            server.maxmemory_policy = REDIS_MAXMEMORY_ALLKEYS_LRU;
        } else if (!strcasecmp(o->ptr,"allkeys-random")) {
            server.maxmemory_policy = REDIS_MAXMEMORY_ALLKEYS_RANDOM;
        } else if (!strcasecmp(o->ptr,"noeviction")) {
            server.maxmemory_policy = REDIS_MAXMEMORY_NO_EVICTION;
        } else {
            goto badfmt;
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"maxmemory-samples")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR ||
            ll <= 0) goto badfmt;
        server.maxmemory_samples = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"timeout")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR ||
            ll < 0 || ll > LONG_MAX) goto badfmt;
        server.maxidletime = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"appendfsync")) {
        if (!strcasecmp(o->ptr,"no")) {
            server.aof_fsync = AOF_FSYNC_NO;
        } else if (!strcasecmp(o->ptr,"everysec")) {
            server.aof_fsync = AOF_FSYNC_EVERYSEC;
        } else if (!strcasecmp(o->ptr,"always")) {
            server.aof_fsync = AOF_FSYNC_ALWAYS;
        } else {
            goto badfmt;
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"no-appendfsync-on-rewrite")) {
        int yn = yesnotoi(o->ptr);

        if (yn == -1) goto badfmt;
        server.aof_no_fsync_on_rewrite = yn;
    } else if (!strcasecmp(c->argv[2]->ptr,"appendonly")) {
        int enable = yesnotoi(o->ptr);

        if (enable == -1) goto badfmt;
        if (enable == 0 && server.aof_state != REDIS_AOF_OFF) {
            stopAppendOnly();
        } else if (enable && server.aof_state == REDIS_AOF_OFF) {
            if (startAppendOnly() == REDIS_ERR) {
                addReplyError(c,
                    "Unable to turn on AOF. Check server logs.");
                return;
            }
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"auto-aof-rewrite-percentage")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.aof_rewrite_perc = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"auto-aof-rewrite-min-size")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.aof_rewrite_min_size = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"save")) {
        int vlen, j;
        sds *v = sdssplitlen(o->ptr,sdslen(o->ptr)," ",1,&vlen);

        /* Perform sanity check before setting the new config:
         * - Even number of args
         * - Seconds >= 1, changes >= 0 */
        if (vlen & 1) {
            sdsfreesplitres(v,vlen);
            goto badfmt;
        }
        for (j = 0; j < vlen; j++) {
            char *eptr;
            long val;

            val = strtoll(v[j], &eptr, 10);
            if (eptr[0] != '\0' ||
                ((j & 1) == 0 && val < 1) ||
                ((j & 1) == 1 && val < 0)) {
                sdsfreesplitres(v,vlen);
                goto badfmt;
            }
        }
        /* Finally set the new config */
        resetServerSaveParams();
        for (j = 0; j < vlen; j += 2) {
            time_t seconds;
            int changes;

            seconds = strtoll(v[j],NULL,10);
            changes = strtoll(v[j+1],NULL,10);
            appendServerSaveParams(seconds, changes);
        }
        sdsfreesplitres(v,vlen);
    } else if (!strcasecmp(c->argv[2]->ptr,"slave-serve-stale-data")) {
        int yn = yesnotoi(o->ptr);

        if (yn == -1) goto badfmt;
        server.repl_serve_stale_data = yn;
    } else if (!strcasecmp(c->argv[2]->ptr,"slave-read-only")) {
        int yn = yesnotoi(o->ptr);

        if (yn == -1) goto badfmt;
        server.repl_slave_ro = yn;
    } else if (!strcasecmp(c->argv[2]->ptr,"dir")) {
        if (chdir((char*)o->ptr) == -1) {
            addReplyErrorFormat(c,"Changing directory: %s", strerror(errno));
            return;
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"hash-max-ziplist-entries")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.hash_max_ziplist_entries = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"hash-max-ziplist-value")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.hash_max_ziplist_value = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"list-max-ziplist-entries")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.list_max_ziplist_entries = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"list-max-ziplist-value")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.list_max_ziplist_value = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"set-max-intset-entries")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.set_max_intset_entries = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"zset-max-ziplist-entries")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.zset_max_ziplist_entries = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"zset-max-ziplist-value")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.zset_max_ziplist_value = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"lua-time-limit")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.lua_time_limit = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"slowlog-log-slower-than")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR) goto badfmt;
        server.slowlog_log_slower_than = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"slowlog-max-len")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        server.slowlog_max_len = (unsigned)ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"loglevel")) {
        if (!strcasecmp(o->ptr,"warning")) {
            server.verbosity = REDIS_WARNING;
        } else if (!strcasecmp(o->ptr,"notice")) {
            server.verbosity = REDIS_NOTICE;
        } else if (!strcasecmp(o->ptr,"verbose")) {
            server.verbosity = REDIS_VERBOSE;
        } else if (!strcasecmp(o->ptr,"debug")) {
            server.verbosity = REDIS_DEBUG;
        } else {
            goto badfmt;
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"client-output-buffer-limit")) {
        int vlen, j;
        sds *v = sdssplitlen(o->ptr,sdslen(o->ptr)," ",1,&vlen);

        /* We need a multiple of 4: <class> <hard> <soft> <soft_seconds> */
        if (vlen % 4) {
            sdsfreesplitres(v,vlen);
            goto badfmt;
        }

        /* Sanity check of single arguments, so that we either refuse the
         * whole configuration string or accept it all, even if a single
         * error in a single client class is present. */
        for (j = 0; j < vlen; j++) {
            char *eptr;
            long val;

            if ((j % 4) == 0) {
                if (getClientLimitClassByName(v[j]) == -1) {
                    sdsfreesplitres(v,vlen);
                    goto badfmt;
                }
            } else {
                val = strtoll(v[j], &eptr, 10);
                if (eptr[0] != '\0' || val < 0) {
                    sdsfreesplitres(v,vlen);
                    goto badfmt;
                }
            }
        }
        /* Finally set the new config */
        for (j = 0; j < vlen; j += 4) {
            int class;
            unsigned long long hard, soft;
            int soft_seconds;

            class = getClientLimitClassByName(v[j]);
            hard = strtoll(v[j+1],NULL,10);
            soft = strtoll(v[j+2],NULL,10);
            soft_seconds = strtoll(v[j+3],NULL,10);

            server.client_obuf_limits[class].hard_limit_bytes = hard;
            server.client_obuf_limits[class].soft_limit_bytes = soft;
            server.client_obuf_limits[class].soft_limit_seconds = soft_seconds;
        }
        sdsfreesplitres(v,vlen);
    } else if (!strcasecmp(c->argv[2]->ptr,"stop-writes-on-bgsave-error")) {
        int yn = yesnotoi(o->ptr);

        if (yn == -1) goto badfmt;
        server.stop_writes_on_bgsave_err = yn;
    } else if (!strcasecmp(c->argv[2]->ptr,"repl-ping-slave-period")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll <= 0) goto badfmt;
        server.repl_ping_slave_period = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"repl-timeout")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll <= 0) goto badfmt;
        server.repl_timeout = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"watchdog-period")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR || ll < 0) goto badfmt;
        if (ll)
            enableWatchdog(ll);
        else
            disableWatchdog();
    } else if (!strcasecmp(c->argv[2]->ptr,"rdbcompression")) {
        int yn = yesnotoi(o->ptr);

        if (yn == -1) goto badfmt;
        server.rdb_compression = yn;
    } else if (!strcasecmp(c->argv[2]->ptr,"rdbchecksum")) {
        int yn = yesnotoi(o->ptr);

        if (yn == -1) goto badfmt;
        server.rdb_checksum = yn;
    } else {
        addReplyErrorFormat(c,"Unsupported CONFIG parameter: %s",
            (char*)c->argv[2]->ptr);
        return;
    }
    addReply(c,shared.ok);
    return;

badfmt: /* Bad format errors */
    addReplyErrorFormat(c,"Invalid argument '%s' for CONFIG SET '%s'",
            (char*)o->ptr,
            (char*)c->argv[2]->ptr);
}

#define config_get_string_field(_name,_var) do { \
    if (stringmatch(pattern,_name,0)) { \
        addReplyBulkCString(c,_name); \
        addReplyBulkCString(c,_var ? _var : ""); \
        matches++; \
    } \
} while(0);

#define config_get_bool_field(_name,_var) do { \
    if (stringmatch(pattern,_name,0)) { \
        addReplyBulkCString(c,_name); \
        addReplyBulkCString(c,_var ? "yes" : "no"); \
        matches++; \
    } \
} while(0);

#define config_get_numerical_field(_name,_var) do { \
    if (stringmatch(pattern,_name,0)) { \
        ll2string(buf,sizeof(buf),_var); \
        addReplyBulkCString(c,_name); \
        addReplyBulkCString(c,buf); \
        matches++; \
    } \
} while(0);

void configGetCommand(redisClient *c) {
    robj *o = c->argv[2];
    void *replylen = addDeferredMultiBulkLength(c);
    char *pattern = o->ptr;
    char buf[128];
    int matches = 0;
    redisAssertWithInfo(c,o,o->encoding == REDIS_ENCODING_RAW);

    /* String values */
    config_get_string_field("dbfilename",server.rdb_filename);
    config_get_string_field("requirepass",server.requirepass);
    config_get_string_field("masterauth",server.requirepass);
    config_get_string_field("bind",server.bindaddr);
    config_get_string_field("unixsocket",server.unixsocket);
    config_get_string_field("logfile",server.logfile);
    config_get_string_field("pidfile",server.pidfile);

    /* Numerical values */
    config_get_numerical_field("maxmemory",server.maxmemory);
    config_get_numerical_field("maxmemory-samples",server.maxmemory_samples);
    config_get_numerical_field("timeout",server.maxidletime);
    config_get_numerical_field("auto-aof-rewrite-percentage",
            server.aof_rewrite_perc);
    config_get_numerical_field("auto-aof-rewrite-min-size",
            server.aof_rewrite_min_size);
    config_get_numerical_field("hash-max-ziplist-entries",
            server.hash_max_ziplist_entries);
    config_get_numerical_field("hash-max-ziplist-value",
            server.hash_max_ziplist_value);
    config_get_numerical_field("list-max-ziplist-entries",
            server.list_max_ziplist_entries);
    config_get_numerical_field("list-max-ziplist-value",
            server.list_max_ziplist_value);
    config_get_numerical_field("set-max-intset-entries",
            server.set_max_intset_entries);
    config_get_numerical_field("zset-max-ziplist-entries",
            server.zset_max_ziplist_entries);
    config_get_numerical_field("zset-max-ziplist-value",
            server.zset_max_ziplist_value);
    config_get_numerical_field("lua-time-limit",server.lua_time_limit);
    config_get_numerical_field("slowlog-log-slower-than",
            server.slowlog_log_slower_than);
    config_get_numerical_field("slowlog-max-len",
            server.slowlog_max_len);
    config_get_numerical_field("port",server.port);
    config_get_numerical_field("databases",server.dbnum);
    config_get_numerical_field("repl-ping-slave-period",server.repl_ping_slave_period);
    config_get_numerical_field("repl-timeout",server.repl_timeout);
    config_get_numerical_field("maxclients",server.maxclients);
    config_get_numerical_field("watchdog-period",server.watchdog_period);

    /* Bool (yes/no) values */
    config_get_bool_field("no-appendfsync-on-rewrite",
            server.aof_no_fsync_on_rewrite);
    config_get_bool_field("slave-serve-stale-data",
            server.repl_serve_stale_data);
    config_get_bool_field("slave-read-only",
            server.repl_slave_ro);
    config_get_bool_field("stop-writes-on-bgsave-error",
            server.stop_writes_on_bgsave_err);
    config_get_bool_field("daemonize", server.daemonize);
    config_get_bool_field("rdbcompression", server.rdb_compression);
    config_get_bool_field("rdbchecksum", server.rdb_checksum);
    config_get_bool_field("activerehashing", server.activerehashing);

    /* Everything we can't handle with macros follows. */

    if (stringmatch(pattern,"appendonly",0)) {
        addReplyBulkCString(c,"appendonly");
        addReplyBulkCString(c,server.aof_state == REDIS_AOF_OFF ? "no" : "yes");
        matches++;
    }
    if (stringmatch(pattern,"dir",0)) {
        char buf[1024];

        if (getcwd(buf,sizeof(buf)) == NULL)
            buf[0] = '\0';

        addReplyBulkCString(c,"dir");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"maxmemory-policy",0)) {
        char *s;

        switch(server.maxmemory_policy) {
        case REDIS_MAXMEMORY_VOLATILE_LRU: s = "volatile-lru"; break;
        case REDIS_MAXMEMORY_VOLATILE_TTL: s = "volatile-ttl"; break;
        case REDIS_MAXMEMORY_VOLATILE_RANDOM: s = "volatile-random"; break;
        case REDIS_MAXMEMORY_ALLKEYS_LRU: s = "allkeys-lru"; break;
        case REDIS_MAXMEMORY_ALLKEYS_RANDOM: s = "allkeys-random"; break;
        case REDIS_MAXMEMORY_NO_EVICTION: s = "noeviction"; break;
        default: s = "unknown"; break; /* too harmless to panic */
        }
        addReplyBulkCString(c,"maxmemory-policy");
        addReplyBulkCString(c,s);
        matches++;
    }
    if (stringmatch(pattern,"appendfsync",0)) {
        char *policy;

        switch(server.aof_fsync) {
        case AOF_FSYNC_NO: policy = "no"; break;
        case AOF_FSYNC_EVERYSEC: policy = "everysec"; break;
        case AOF_FSYNC_ALWAYS: policy = "always"; break;
        default: policy = "unknown"; break; /* too harmless to panic */
        }
        addReplyBulkCString(c,"appendfsync");
        addReplyBulkCString(c,policy);
        matches++;
    }
    if (stringmatch(pattern,"save",0)) {
        sds buf = sdsempty();
        int j;

        for (j = 0; j < server.saveparamslen; j++) {
            buf = sdscatprintf(buf,"%ld %d",
                    server.saveparams[j].seconds,
                    server.saveparams[j].changes);
            if (j != server.saveparamslen-1)
                buf = sdscatlen(buf," ",1);
        }
        addReplyBulkCString(c,"save");
        addReplyBulkCString(c,buf);
        sdsfree(buf);
        matches++;
    }
    if (stringmatch(pattern,"loglevel",0)) {
        char *s;

        switch(server.verbosity) {
        case REDIS_WARNING: s = "warning"; break;
        case REDIS_VERBOSE: s = "verbose"; break;
        case REDIS_NOTICE: s = "notice"; break;
        case REDIS_DEBUG: s = "debug"; break;
        default: s = "unknown"; break; /* too harmless to panic */
        }
        addReplyBulkCString(c,"loglevel");
        addReplyBulkCString(c,s);
        matches++;
    }
    if (stringmatch(pattern,"client-output-buffer-limit",0)) {
        sds buf = sdsempty();
        int j;

        for (j = 0; j < REDIS_CLIENT_LIMIT_NUM_CLASSES; j++) {
            buf = sdscatprintf(buf,"%s %llu %llu %ld",
                    getClientLimitClassName(j),
                    server.client_obuf_limits[j].hard_limit_bytes,
                    server.client_obuf_limits[j].soft_limit_bytes,
                    (long) server.client_obuf_limits[j].soft_limit_seconds);
            if (j != REDIS_CLIENT_LIMIT_NUM_CLASSES-1)
                buf = sdscatlen(buf," ",1);
        }
        addReplyBulkCString(c,"client-output-buffer-limit");
        addReplyBulkCString(c,buf);
        sdsfree(buf);
        matches++;
    }
    if (stringmatch(pattern,"unixsocketperm",0)) {
        char buf[32];
        snprintf(buf,sizeof(buf),"%o",server.unixsocketperm);
        addReplyBulkCString(c,"unixsocketperm");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"slaveof",0)) {
        char buf[256];

        addReplyBulkCString(c,"slaveof");
        if (server.masterhost)
            snprintf(buf,sizeof(buf),"%s %d",
                server.masterhost, server.masterport);
        else
            buf[0] = '\0';
        addReplyBulkCString(c,buf);
        matches++;
    }
    setDeferredMultiBulkLength(c,replylen,matches*2);
}

void configCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"set")) {
        if (c->argc != 4) goto badarity;
        configSetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"get")) {
        if (c->argc != 3) goto badarity;
        configGetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"resetstat")) {
        if (c->argc != 2) goto badarity;
        server.stat_keyspace_hits = 0;
        server.stat_keyspace_misses = 0;
        server.stat_numcommands = 0;
        server.stat_numconnections = 0;
        server.stat_expiredkeys = 0;
        server.stat_rejected_conn = 0;
        server.stat_fork_time = 0;
        server.aof_delayed_fsync = 0;
        resetCommandTableStats();
        addReply(c,shared.ok);
    } else {
        addReplyError(c,
            "CONFIG subcommand must be one of GET, SET, RESETSTAT");
    }
    return;

badarity:
    addReplyErrorFormat(c,"Wrong number of arguments for CONFIG %s",
        (char*) c->argv[1]->ptr);
}
