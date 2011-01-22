#! /bin/sh
### BEGIN INIT INFO
# Provides:		redis-server
# Required-Start:	$syslog $remote_fs
# Required-Stop:	$syslog $remote_fs
# Should-Start:		$local_fs
# Should-Stop:		$local_fs
# Default-Start:	2 3 4 5
# Default-Stop:		0 1 6
# Short-Description:	redis-server - Persistent key-value db
# Description:		redis-server - Persistent key-value db
### END INIT INFO


PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
DAEMON=/usr/bin/redis-server
DAEMON_ARGS=/etc/redis/redis.conf
NAME=redis-server
DESC=redis-server
PIDFILE=/var/run/redis.pid

test -x $DAEMON || exit 0

set -e

case "$1" in
  start)
	echo -n "Starting $DESC: "
	touch $PIDFILE
	chown redis:redis $PIDFILE
	if start-stop-daemon --start --quiet --umask 007 --pidfile $PIDFILE --chuid redis:redis --exec $DAEMON -- $DAEMON_ARGS
	then
		echo "$NAME."
	else
		echo "failed"
	fi
	;;
  stop)
	echo -n "Stopping $DESC: "
	if start-stop-daemon --stop --retry 10 --quiet --oknodo --pidfile $PIDFILE --exec $DAEMON
	then
		echo "$NAME."
	else
		echo "failed"
	fi
	rm -f $PIDFILE
	;;

  restart|force-reload)
	${0} stop
	${0} start
	;;

  status)
	echo -n "$DESC is "
	if start-stop-daemon --stop --quiet --signal 0 --name ${NAME} --pidfile ${PIDFILE}
	then
		echo "running"
	else
		echo "not running"
		exit 1
	fi
	;;

  *)
	echo "Usage: /etc/init.d/$NAME {start|stop|restart|force-reload}" >&2
	exit 1
	;;
esac

exit 0
