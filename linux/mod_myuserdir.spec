Summary: Apache 1.3 module for dynamically configured mass userdirs.
Name: mod_myuserdir
Version: 0.89.2
Release: 1
Packager: Igor Popov <igorpopov@newmail.ru>
Group: System Environment/Daemons
URL: http://igorpopov.newmail.ru/
Source: %{name}_%{version}.tar.gz
License: Apache 2.0
BuildRoot: %{_tmppath}/%{name}-root
BuildPrereq: httpd-devel, mysql-devel
Requires: httpd, mysql

%description
mod_myuserdir is Apache 1.3.xx module for dynamically configured
mass userdirs, all configurations are stored in MySQL database.
  
No need to have every user in /etc/passwd, no need to restart apache
after configuration changed.

After all, it is capable to change settings of PHP4 dynamically (if php is
loadable module or it is linked with apache) for every user. By default,
it sets open_basedir as homedir to prevent user from stoling files from other
users and from your server, but you can change any parameter that exists in
php.ini, for example, you can turn on safe_mode or register_globals for
particular user, if it has old php scripts that use global variables.

%prep
%setup -q

%build
make all

%install
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT%{_libdir}/httpd/modules
install -m755 %{name}.so $RPM_BUILD_ROOT%{_libdir}/httpd/modules

%clean
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc AUTHORS Changelog INSTALL README TODO
%{_libdir}/httpd/modules/%{name}.so

%changelog
* Jul 25 2005
- The first public release

* Aug 22 2005
- Small bugfixes
