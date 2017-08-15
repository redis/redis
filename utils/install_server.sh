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
# Service installer for redis server, runs interactively by default.
#
# To run this script non-interactively (for automation/provisioning purposes),
# feed the variables into the script. Any missing variables will be prompted!
# Tip: Environment variables also support command substitution (see REDIS_EXECUTABLE)
#
# Example:
#
# sudo REDIS_PORT=1234 \
# 		 REDIS_CONFIG_FILE=/etc/redis/1234.conf \
# 		 REDIS_LOG_FILE=/var/log/redis_1234.log \
# 		 REDIS_DATA_DIR=/var/lib/redis/1234 \
# 		 REDIS_EXECUTABLE=`command -v redis-server` ./utils/install_server.sh
#
# This generates a redis config file and an /etc/init.d script, and installs them.
#
# /!\ This script should be run as root
#
################################################################################

die () {
	echo "ERROR: $1. Aborting!"
	exit 1
}


#Absolute path to this script
SCRIPT=$(readlink -f $0)
#Absolute path this script is in
SCRIPTPATH=$(dirname $SCRIPT)

#Initial defaults
_REDIS_PORT=6379
_MANUAL_EXECUTION=false

echo "Welcome to the redis service installer"
echo "This script will help you easily set up a running redis server"
echo

#check for root user
if [ "$(id -u)" -ne 0 ] ; then
	echo "You must run this script as root. Sorry!"
	exit 1
fi

if ! echo $REDIS_PORT | egrep -q '^[0-9]+$' ; then
	_MANUAL_EXECUTION=true
	#Read the redis port
	read  -p "Please select the redis port for this instance: [$_REDIS_PORT] " REDIS_PORT
	if ! echo $REDIS_PORT | egrep -q '^[0-9]+$' ; then
		echo "Selecting default: $_REDIS_PORT"
		REDIS_PORT=$_REDIS_PORT
	fi
fi

if [ -z "$REDIS_CONFIG_FILE" ] ; then
	_MANUAL_EXECUTION=true
	#read the redis config file
	_REDIS_CONFIG_FILE="/etc/redis/$REDIS_PORT.conf"
	read -p "Please select the redis config file name [$_REDIS_CONFIG_FILE] " REDIS_CONFIG_FILE
	if [ -z "$REDIS_CONFIG_FILE" ] ; then
		REDIS_CONFIG_FILE=$_REDIS_CONFIG_FILE
		echo "Selected default - $REDIS_CONFIG_FILE"
	fi
fi

if [ -z "$REDIS_LOG_FILE" ] ; then
	_MANUAL_EXECUTION=true
	#read the redis log file path
	_REDIS_LOG_FILE="/var/log/redis_$REDIS_PORT.log"
	read -p "Please select the redis log file name [$_REDIS_LOG_FILE] " REDIS_LOG_FILE
	if [ -z "$REDIS_LOG_FILE" ] ; then
		REDIS_LOG_FILE=$_REDIS_LOG_FILE
		echo "Selected default - $REDIS_LOG_FILE"
	fi
fi

if [ -z "$REDIS_DATA_DIR" ] ; then
	_MANUAL_EXECUTION=true
	#get the redis data directory
	_REDIS_DATA_DIR="/var/lib/redis/$REDIS_PORT"
	read -p "Please select the data directory for this instance [$_REDIS_DATA_DIR] " REDIS_DATA_DIR
	if [ -z "$REDIS_DATA_DIR" ] ; then
		REDIS_DATA_DIR=$_REDIS_DATA_DIR
		echo "Selected default - $REDIS_DATA_DIR"
	fi
fi

if [ ! -x "$REDIS_EXECUTABLE" ] ; then
	_MANUAL_EXECUTION=true
	#get the redis executable path
	_REDIS_EXECUTABLE=`command -v redis-server`
	read -p "Please select the redis executable path [$_REDIS_EXECUTABLE] " REDIS_EXECUTABLE
	if [ ! -x "$REDIS_EXECUTABLE" ] ; then
		REDIS_EXECUTABLE=$_REDIS_EXECUTABLE

		if [ ! -x "$REDIS_EXECUTABLE" ] ; then
			echo "Mmmmm...  it seems like you don't have a redis executable. Did you run make install yet?"
			exit 1
		fi
	fi
fi

#check the default for redis cli
CLI_EXEC=`command -v redis-cli`
if [ -z "$CLI_EXEC" ] ; then
	CLI_EXEC=`dirname $REDIS_EXECUTABLE`"/redis-cli"
fi

echo "Selected config:"

echo "Port           : $REDIS_PORT"
echo "Config file    : $REDIS_CONFIG_FILE"
echo "Log file       : $REDIS_LOG_FILE"
echo "Data dir       : $REDIS_DATA_DIR"
echo "Executable     : $REDIS_EXECUTABLE"
echo "Cli Executable : $CLI_EXEC"

if $_MANUAL_EXECUTION == true ; then
	read -p "Is this ok? Then press ENTER to go on or Ctrl-C to abort." _UNUSED_
fi

mkdir -p `dirname "$REDIS_CONFIG_FILE"` || die "Could not create redis config directory"
mkdir -p `dirname "$REDIS_LOG_FILE"` || die "Could not create redis log dir"
mkdir -p "$REDIS_DATA_DIR" || die "Could not create redis data directory"

#render the templates
TMP_FILE="/tmp/${REDIS_PORT}.conf"
DEFAULT_CONFIG="${SCRIPTPATH}/../redis.conf"
INIT_TPL_FILE="${SCRIPTPATH}/redis_init_script.tpl"
INIT_SCRIPT_DEST="/etc/init.d/redis_${REDIS_PORT}"
PIDFILE="/var/run/redis_${REDIS_PORT}.pid"

if [ ! -f "$DEFAULT_CONFIG" ]; then
	echo "Mmmmm... the default config is missing. Did you switch to the utils directory?"
	exit 1
fi

#Generate config file from the default config file as template
#changing only the stuff we're controlling from this script
echo "## Generated by install_server.sh ##" > $TMP_FILE

read -r SED_EXPR <<-EOF
s#^port .\+#port ${REDIS_PORT}#; \
s#^logfile .\+#logfile ${REDIS_LOG_FILE}#; \
s#^dir .\+#dir ${REDIS_DATA_DIR}#; \
s#^pidfile .\+#pidfile ${PIDFILE}#; \
s#^daemonize no#daemonize yes#;
EOF
sed "$SED_EXPR" $DEFAULT_CONFIG >> $TMP_FILE

#cat $TPL_FILE | while read line; do eval "echo \"$line\"" >> $TMP_FILE; done
cp $TMP_FILE $REDIS_CONFIG_FILE || die "Could not write redis config file $REDIS_CONFIG_FILE"

#Generate sample script from template file
rm -f $TMP_FILE

#we hard code the configs here to avoid issues with templates containing env vars
#kinda lame but works!
REDIS_INIT_HEADER=\
"#!/bin/sh\n
#Configurations injected by install_server below....\n\n
EXEC=$REDIS_EXECUTABLE\n
CLIEXEC=$CLI_EXEC\n
PIDFILE=\"$PIDFILE\"\n
CONF=\"$REDIS_CONFIG_FILE\"\n\n
REDISPORT=\"$REDIS_PORT\"\n\n
###############\n\n"

REDIS_CHKCONFIG_INFO=\
"# REDHAT chkconfig header\n\n
# chkconfig: - 58 74\n
# description: redis_${REDIS_PORT} is the redis daemon.\n
### BEGIN INIT INFO\n
# Provides: redis_6379\n
# Required-Start: \$network \$local_fs \$remote_fs\n
# Required-Stop: \$network \$local_fs \$remote_fs\n
# Default-Start: 2 3 4 5\n
# Default-Stop: 0 1 6\n
# Should-Start: \$syslog \$named\n
# Should-Stop: \$syslog \$named\n
# Short-Description: start and stop redis_${REDIS_PORT}\n
# Description: Redis daemon\n
### END INIT INFO\n\n"

if command -v chkconfig >/dev/null; then
	#if we're a box with chkconfig on it we want to include info for chkconfig
	echo "$REDIS_INIT_HEADER" "$REDIS_CHKCONFIG_INFO" > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
else
	#combine the header and the template (which is actually a static footer)
	echo "$REDIS_INIT_HEADER" > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
fi

###
# Generate sample script from template file
# - No need to check which system we are on. The init info are comments and
#   do not interfere with update_rc.d systems. Additionally:
#     Ubuntu/debian by default does not come with chkconfig, but does issue a
#     warning if init info is not available.

cat > ${TMP_FILE} <<EOT
#!/bin/sh
#Configurations injected by install_server below....

EXEC=$REDIS_EXECUTABLE
CLIEXEC=$CLI_EXEC
PIDFILE=$PIDFILE
CONF="$REDIS_CONFIG_FILE"
REDISPORT="$REDIS_PORT"
###############
# SysV Init Information
# chkconfig: - 58 74
# description: redis_${REDIS_PORT} is the redis daemon.
### BEGIN INIT INFO
# Provides: redis_${REDIS_PORT}
# Required-Start: \$network \$local_fs \$remote_fs
# Required-Stop: \$network \$local_fs \$remote_fs
# Default-Start: 2 3 4 5
# Default-Stop: 0 1 6
# Should-Start: \$syslog \$named
# Should-Stop: \$syslog \$named
# Short-Description: start and stop redis_${REDIS_PORT}
# Description: Redis daemon
### END INIT INFO

EOT
cat ${INIT_TPL_FILE} >> ${TMP_FILE}

#copy to /etc/init.d
cp $TMP_FILE $INIT_SCRIPT_DEST && \
	chmod +x $INIT_SCRIPT_DEST || die "Could not copy redis init script to  $INIT_SCRIPT_DEST"
echo "Copied $TMP_FILE => $INIT_SCRIPT_DEST"

#Install the service
echo "Installing service..."
if command -v chkconfig >/dev/null 2>&1; then
	# we're chkconfig, so lets add to chkconfig and put in runlevel 345
	chkconfig --add redis_${REDIS_PORT} && echo "Successfully added to chkconfig!"
	chkconfig --level 345 redis_${REDIS_PORT} on && echo "Successfully added to runlevels 345!"
elif command -v update-rc.d >/dev/null 2>&1; then
	#if we're not a chkconfig box assume we're able to use update-rc.d
	update-rc.d redis_${REDIS_PORT} defaults && echo "Success!"
else
	echo "No supported init tool found."
fi

/etc/init.d/redis_$REDIS_PORT start || die "Failed starting service..."

#tada
echo "Installation successful!"
exit 0
