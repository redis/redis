APP=xredis
Version=0.0.1
Package=$APP-$Version

if [ -f $package ]; then
    echo remove $Package
    rm -rf $Package
fi
echo make clean
make clean
mkdir $Package
ls -a | egrep -v "Debug|^\.|$Package" | xargs -J %  cp -r %  $Package
echo create $Package.tar.gz...
tar -czf $Package.tar.gz $Package
rm -rf $Package
