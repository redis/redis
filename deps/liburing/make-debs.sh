#!/usr/bin/env bash
# Copyright (C) 2019  Liu Changcheng <changcheng.liu@aliyun.com>
# Author: Liu Changcheng <changcheng.liu@aliyun.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
set -xe

# Create dir for build
base=${1:-/tmp/release}
distro=unstable
releasedir=$base/$(lsb_release -si)/liburing
rm -rf $releasedir
mkdir -p $releasedir
HEAD=$(which head)
DCH=$(which dch)

src_dir=$(readlink -e `basename $0`)
liburing_dir=$(dirname $src_dir)
basename=$(basename $liburing_dir)
dirname=$(dirname $liburing_dir)
version=$(git describe --match "lib*" | cut -d '-' -f 2)
outfile="liburing-$version"
orgfile=$(echo $outfile | tr '-' '_')

# Prepare source code
cp -arf ${dirname}/${basename} ${releasedir}/${outfile}
cd ${releasedir}/${outfile}
git clean -dxf

# Change changelog if it's needed
cur_ver=`$HEAD < debian/changelog | sed -n -e 's/.* (\(.*\)) .*/\1/p'`
if [ "$cur_ver" != "$version-1" ]; then
	$DCH -D $distro --force-distribution -b -v "$version-1" "new version"
fi

# Create tar archieve
cd ../
tar cvzf ${outfile}.tar.gz ${outfile}
ln -s ${outfile}.tar.gz ${orgfile}.orig.tar.gz

# Build debian package
cd -
debuild
