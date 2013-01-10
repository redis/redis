#!/bin/sh

# Copyright 2011 Dvir Volk <dvirsk at gmail dot com>. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   1. Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
#
#   2. Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer in the
#   documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
# WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
# EVENT SHALL Dvir Volk OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
# OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
# NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
# EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
################################################################################
#
# Interactive service installer for redis server
# this generates a redis config file and an /etc/init.d script, and installs them
# this scripts should be run as root


# Functions
###################################################
help() {
echo "
  -h | --help         Show help
  --non-interactive   If provided turn off interactive mode
  --port              PORT for Redis instance
  --config            Path of configuration
  --config-tpl        Template for configuration file
  --log               Path of Log file
  --data              Path of date directory
"
}

die () {
	echo "ERROR: $1. Aborting!"
	exit 1
}

# Handle command line params
###################################################
# NOTE: This requires GNU getopt.  On Mac OS X, you get BSD getopt by default,
# which doesn't work; see below.
TEMP=`getopt -o h \
             --long port:,config:,config-tpl:,log:,data:,non-interactive,help \
             -n 'install_server.sh' -- "$@"`

if [ $? != 0 ] ; then echo "Terminating..." >&2 ; exit 1 ; fi

# Note the quotes around `$TEMP': they are essential!
eval set -- "$TEMP"

#Initial defaults
###################################################
_REDIS_PORT=6379
_INTERACTIVE=1

## Set params
###################################################
while true; do
  case "$1" in
    -h | --help ) help; exit;;
    --port ) REDIS_PORT="$2"; shift 2 ;;
    --config ) REDIS_CONFIG_FILE="$2"; shift 2 ;;
    --config-tpl ) CUSTOM_CONFIG="$2"; shift 2 ;;
    --log ) REDIS_LOG_FILE="$2"; shift 2 ;;
    --data ) REDIS_DATA_DIR="$2"; shift 2 ;;
    --non-interactive ) _INTERACTIVE=0; shift ;;
    -- ) shift; break ;;
    -* ) echo "$0: error - unrecognized option $1" 1>&2; help; exit 1;;
    * ) break ;;
  esac
done


echo "Welcome to the redis service installer"
echo "This script will help you easily set up a running redis server

"

#check for root user TODO: replace this with a call to "id"
if [ `whoami` != "root" ] ; then
	echo "You must run this script as root. Sorry!"
	exit 1
fi


#Read the redis port
if [ -z "$REDIS_PORT" ]; then
    if [ $_INTERACTIVE -eq 1 ]
    then
        read  -p "Please select the redis port for this instance: [$_REDIS_PORT] " REDIS_PORT
        if [ ! `echo $REDIS_PORT | egrep "^[0-9]+\$"`  ] ; then
	       echo "Selecting default: $_REDIS_PORT"
	       REDIS_PORT=$_REDIS_PORT
        fi
    else
         REDIS_PORT=$_REDIS_PORT
    fi
fi;



#read the redis config file
_REDIS_CONFIG_FILE="/etc/redis/$REDIS_PORT.conf"
if [ -z "$REDIS_CONFIG_FILE" ]; then
    if [ $_INTERACTIVE -eq 1 ]
    then
        read -p "Please select the redis config file name [$_REDIS_CONFIG_FILE] " REDIS_CONFIG_FILE
        if [ !"$REDIS_CONFIG_FILE" ] ; then
	        REDIS_CONFIG_FILE=$_REDIS_CONFIG_FILE
	        echo "Selected default - $REDIS_CONFIG_FILE"
        fi
    else
         REDIS_CONFIG_FILE=$_REDIS_CONFIG_FILE
    fi
fi;
#try and create it
mkdir -p `dirname "$REDIS_CONFIG_FILE"` || die "Could not create redis config directory"

#read the redis log file path
_REDIS_LOG_FILE="/var/log/redis_$REDIS_PORT.log"
if [ -z "$REDIS_LOG_FILE" ]; then
    if [ $_INTERACTIVE  -eq 1 ]
    then
        read -p "Please select the redis log file name [$_REDIS_LOG_FILE] " REDIS_LOG_FILE
        if [ !"$REDIS_LOG_FILE" ] ; then
	        REDIS_LOG_FILE=$_REDIS_LOG_FILE
	        echo "Selected default - $REDIS_LOG_FILE"
        fi
    else
         REDIS_LOG_FILE=$_REDIS_LOG_FILE
    fi
fi;

#get the redis data directory
_REDIS_DATA_DIR="/var/lib/redis/$REDIS_PORT"
if [ -z "$REDIS_DATA_DIR" ]; then
    if [ $_INTERACTIVE -eq 1 ]
    then
        read -p "Please select the data directory for this instance [$_REDIS_DATA_DIR] " REDIS_DATA_DIR
        if [ !"$REDIS_DATA_DIR" ] ; then
	        REDIS_DATA_DIR=$_REDIS_DATA_DIR
	        echo "Selected default - $REDIS_DATA_DIR"
        fi
    else
        REDIS_DATA_DIR=$_REDIS_DATA_DIR
    fi
fi;
mkdir -p $REDIS_DATA_DIR || die "Could not create redis data directory"

#get the redis executable path
REDIS_EXECUTABLE=`which redis-server`

if [ $_INTERACTIVE -eq 1 ]; then
    read -p "Please select the redis executable path [$_REDIS_EXECUTABLE] " REDIS_EXECUTABLE
fi

if [ ! -f "$REDIS_EXECUTABLE" ] ; then
	echo "Mmmmm...  it seems like you don't have a redis executable. Did you run make install yet?"
	exit 1
fi


#render the templates
TMP_FILE="/tmp/$REDIS_PORT.conf"
INIT_TPL_FILE="./redis_init_script.tpl"
INIT_SCRIPT_DEST="/etc/init.d/redis_$REDIS_PORT"
PIDFILE="/var/run/redis_$REDIS_PORT.pid"

#set custom config template
if [ -z "$DEFAULT_CONFIG" ]; then
	DEFAULT_CONFIG="../redis.conf"
fi;
if [ ! -f "$DEFAULT_CONFIG" ] ; then
	die "Config template '$DEFAULT_CONFIG' not found"
fi


#check the default for redis cli
CLI_EXEC=`which redis-cli`
if [ ! "$CLI_EXEC" ] ; then
	CLI_EXEC=`dirname $REDIS_EXECUTABLE`"/redis-cli"
fi

#Generate config file from the default config file as template
#changing only the stuff we're controlling from this script
echo "## Generated by install_server.sh ##" > $TMP_FILE

SED_EXPR="s#^port [0-9]{4}\$#port ${REDIS_PORT}#;\
s#^logfile .+\$#logfile ${REDIS_LOG_FILE}#;\
s#^dir .+\$#dir ${REDIS_DATA_DIR}#;\
s#^pidfile .+\$#pidfile ${PIDFILE}#;\
s#^daemonize no\$#daemonize yes#;"
echo $SED_EXPR
sed -r "$SED_EXPR" $DEFAULT_CONFIG  >> $TMP_FILE

#cat $TPL_FILE | while read line; do eval "echo \"$line\"" >> $TMP_FILE; done
cp -f $TMP_FILE $REDIS_CONFIG_FILE || exit 1

#Generate sample script from template file
rm -f $TMP_FILE

#we hard code the configs here to avoid issues with templates containing env vars
#kinda lame but works!
REDIS_INIT_HEADER=\
"#/bin/sh\n
#Configurations injected by install_server below....\n\n
EXEC=$REDIS_EXECUTABLE\n
CLIEXEC=$CLI_EXEC\n
PIDFILE=$PIDFILE\n
CONF=\"$REDIS_CONFIG_FILE\"\n\n
REDISPORT=\"$REDIS_PORT\"\n\n
###############\n\n"

REDIS_CHKCONFIG_INFO=\
"# REDHAT chkconfig header\n\n
# chkconfig: - 58 74\n
# description: redis_6379 is the redis daemon.\n
### BEGIN INIT INFO\n
# Provides: redis_6379\n
# Required-Start: $network $local_fs $remote_fs\n
# Required-Stop: $network $local_fs $remote_fs\n
# Default-Start: 2 3 4 5\n
# Default-Stop: 0 1 6\n
# Should-Start: $syslog $named\n
# Should-Stop: $syslog $named\n
# Short-Description: start and stop redis_6379\n
# Description: Redis daemon\n
### END INIT INFO\n\n"

if [ !`which chkconfig` ] ; then
	#combine the header and the template (which is actually a static footer)
	echo $REDIS_INIT_HEADER > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
else
	#if we're a box with chkconfig on it we want to include info for chkconfig
	echo -e $REDIS_INIT_HEADER $REDIS_CHKCONFIG_INFO > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
fi

#copy to /etc/init.d
cp -f $TMP_FILE $INIT_SCRIPT_DEST && chmod +x $INIT_SCRIPT_DEST || die "Could not copy redis init script to  $INIT_SCRIPT_DEST"
echo "Copied $TMP_FILE => $INIT_SCRIPT_DEST"

#Install the service
echo "Installing service..."
if [ !`which chkconfig` ] ; then
	#if we're not a chkconfig box assume we're able to use update-rc.d
	update-rc.d redis_$REDIS_PORT defaults && echo "Success!"
else
	# we're chkconfig, so lets add to chkconfig and put in runlevel 345
	chkconfig --add redis_$REDIS_PORT && echo "Successfully added to chkconfig!"
	chkconfig --level 345 redis_$REDIS_PORT on && echo "Successfully added to runlevels 345!"
fi

/etc/init.d/redis_$REDIS_PORT start || die "Failed starting service..."

#tada
echo "Installation successful!"
exit 0

