APP=xredis
Version=0.0.2
Package=$APP-$Version

find . -name ctrip.h -exec sed -i "s/XREDIS_VERSION \"\(.*\)\"/XREDIS_VERSION \"$Version\"/" {} \;

if [ -f $package ]; then
    echo remove $Package
    rm -rf $Package
fi
echo make clean
make distclean
mkdir $Package
ls -a | egrep -v "Debug|^\.|$Package" | xargs -J %  cp -r %  $Package
echo create $Package.tar.gz...
tar -czf $Package.tar.gz $Package
rm -rf $Package
