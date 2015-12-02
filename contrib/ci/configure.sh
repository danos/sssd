#
# Configure argument management.
#
# Copyright (C) 2014 Red Hat
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

if [ -z ${_CONFIGURE_SH+set} ]; then
declare -r _CONFIGURE_SH=

. distro.sh

# List of "configure" arguments.
declare -a CONFIGURE_ARG_LIST=(
    "--disable-dependency-tracking"
    "--disable-rpath"
    "--disable-static"
    "--enable-ldb-version-check"
    "--with-syslog=journald"
)


if [[ "$DISTRO_BRANCH" == -redhat-redhatenterprise*-6.*- ||
      "$DISTRO_BRANCH" == -redhat-centos-6.*- ]]; then
    CONFIGURE_ARG_LIST+=(
        "--disable-cifs-idmap-plugin"
        "--with-syslog=syslog"
        "--without-python3-bindings"
    )
fi

if [[ "$DISTRO_BRANCH" == -redhat-redhatenterprise*-7.*- ||
      "$DISTRO_BRANCH" == -redhat-centos-7.*- ]]; then
    CONFIGURE_ARG_LIST+=(
        "--without-python3-bindings"
    )
fi

declare -r -a CONFIGURE_ARG_LIST

fi # _CONFIGURE_SH
