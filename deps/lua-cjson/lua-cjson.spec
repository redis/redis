%define luaver 5.1
%define lualibdir %{_libdir}/lua/%{luaver}

Name:		lua-cjson
Version:	1.0.1
Release:	1%{?dist}
Summary:	JSON support for the Lua language

Group:		Development/Libraries
License:	MIT
URL:		http://www.kyne.com.au/~mark/software/lua-cjson/
Source0:	http://www.kyne.com.au/~mark/software/lua-cjson/lua-cjson-%{version}.tar.gz
BuildRoot:	%(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

BuildRequires:	lua >= %{luaver}, lua-devel >= %{luaver}
Requires:	lua >= %{luaver}

%description
CJSON provides fast JSON parsing and encoding functions for Lua. It
allows a Lua application to quickly serialise a data structure to
JSON, or deserialise from JSON to Lua.

%prep
%setup -q


%build
make %{?_smp_mflags} CFLAGS="%{optflags}" LUA_INCLUDE_DIR="%{_includedir}"


%install
rm -rf "$RPM_BUILD_ROOT"
make install DESTDIR="$RPM_BUILD_ROOT" LUA_LIB_DIR="%{lualibdir}"


%clean
rm -rf "$RPM_BUILD_ROOT"


%files
%defattr(-,root,root,-)
%doc LICENSE NEWS performance.txt README rfc4627.txt tests TODO
%{lualibdir}/*


%changelog
* Sun May 1 2011 Mark Pulford <mark@kyne.com.au> - 1.0-1
- Initial package
