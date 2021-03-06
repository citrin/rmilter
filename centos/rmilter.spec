%define rmilter_user      _rmilter
%define rmilter_group     adm
%define rmilter_home      %{_localstatedir}/run/rmilter

%define has_systemd       1
%{?el6:%undefine has_systemd}
%{?!suse_version:%define not_suse 1}

Name:           rmilter
Version:        1.7.0
Release:        1
Summary:        Multi-purpose milter
Group:          System Environment/Daemons

# BSD License (two clause)
# http://www.freebsd.org/copyright/freebsd-license.html
%if 0%{?suse_version}
License:        BSD-2-Clause
%else
License:        BSD2c
%endif
URL:            https://github.com/vstakhov/rmilter
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}
%if 0%{?not_suse}
BuildRequires:  sendmail-milter
%endif
BuildRequires:  sendmail-devel,openssl-devel,pcre-devel,glib2-devel
BuildRequires:  cmake,bison,flex,libopendkim-devel
%if 0%{?has_systemd}
BuildRequires:  systemd
Requires(pre):  systemd
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
%endif
%if 0%{?suse_version}
Requires(pre):  shadow
%else
BuildRequires:  sqlite-devel
Requires(pre):  shadow-utils
%endif

%if 0%{?el6}
Requires:       logrotate
Requires(post): chkconfig
Requires(preun): chkconfig, initscripts
Requires(postun): initscripts
Source4:        %{name}.sh
%endif

Source0:    https://rspamd.com/downloads/%{name}-%{version}.tar.xz
Source1:	%{name}.conf
Source2:	%{name}.conf.common
Source3:	%{name}.conf.sysvinit
Source5:	80-rmilter.preset

%description
The rmilter utility is designed to act as milter for sendmail and postfix MTA.
It provides several filter and mail scan features.

%prep
%setup -q
rm -rf %{buildroot} || true

%build

%{__cmake} \
		-DCMAKE_C_OPT_FLAGS="%{optflags}" \
        -DCMAKE_INSTALL_PREFIX=%{_prefix} \
        -DCONFDIR=%{_sysconfdir}/rmilter \
        -DMANDIR=%{_mandir} \
%if 0%{?has_systemd}
        -DWANT_SYSTEMD_UNITS=ON \
        -DSYSTEMDDIR=%{_unitdir} \
%else
        -DWANT_SYSTEMD_UNITS=OFF \
%endif
%if 0%{?suse_version}
        -DCMAKE_SKIP_INSTALL_RPATH=ON \
%endif
        -DNO_SHARED=ON \
        -DRMILTER_GROUP=%{rmilter_group} \
        -DRMILTER_USER=%{rmilter_user}

%{__make} %{?jobs:-j%jobs}

%install
%{__make} install DESTDIR=%{buildroot} INSTALLDIRS=vendor

%{__install} -p -d -D -m 0755 %{buildroot}%{_sysconfdir}/%{name}
%{__install} -p -D -m 0644 %{SOURCE1} %{buildroot}%{_sysconfdir}/%{name}/
%{__install} -p -D -m 0644 %{SOURCE2} %{buildroot}%{_sysconfdir}/%{name}/
%{__install} -p -D -m 0644 %{SOURCE3} %{buildroot}%{_sysconfdir}/%{name}/
%if 0%{?el6}
%{__install} -p -D -m 0755 %{SOURCE4} %{buildroot}%{_initrddir}/%{name}
%{__install} -p -D -d -m 0755 %{buildroot}%{rmilter_home}
%else
%{__install} -p -D -m 0644 %{SOURCE5} %{buildroot}%{_presetdir}/80-rmilter.preset
%endif
%{__install} -p -D -d -m 0755 %{buildroot}%{_sysconfdir}/%{name}/rmilter.conf.d/


%clean
rm -rf %{buildroot}

%pre
%{_sbindir}/groupadd -r %{rmilter_group} 2>/dev/null || :
%{_sbindir}/useradd -g %{rmilter_group} -c "Rmilter user" -s /bin/false -r %{rmilter_user} 2>/dev/null || :

%if 0%{?suse_version}
%service_add_pre %{name}.service
%endif

%post
%{__chown} -R %{rmilter_user}:%{rmilter_group} %{rmilter_home}
%if 0%{?suse_version}
%service_add_post %{name}.service
%endif
%if 0%{?not_suse} && 0%{?has_systemd}
#Macro is not used as we want to do this on upgrade
#%systemd_post %{name}.service
systemctl --no-reload preset %{name}.service >/dev/null 2>&1 || :
%endif
%if 0%{?el6}
/sbin/chkconfig --add %{name}
%endif

%preun
%if 0%{?suse_version}
%service_del_preun %{name}.service
%endif
%if 0%{?not_suse} && 0%{?has_systemd}
%systemd_preun %{name}.service
%endif
%if 0%{?el6}
if [ $1 = 0 ]; then
    /sbin/service %{name} stop >/dev/null 2>&1
    /sbin/chkconfig --del %{name}
fi
%endif

%postun
%if 0%{?suse_version}
%service_del_postun %{name}.service
%endif
%if 0%{?not_suse} && 0%{?has_systemd}
%systemd_postun_with_restart %{name}.service
%endif
%if 0%{?el6}
if [ $1 -ge 1 ]; then
    /sbin/service %{name} condrestart > /dev/null 2>&1 || :
fi

%endif

%files
%defattr(-,root,root,-)
%if 0%{?has_systemd}
%{_unitdir}/%{name}.service
%{_presetdir}/80-rmilter.preset
%endif
%if 0%{?el6}
%{_initrddir}/%{name}
%attr(-, _rmilter, adm) %dir %{rmilter_home}
%endif
%{_sbindir}/rmilter
%dir %{_sysconfdir}/rmilter
%dir %{_sysconfdir}/rmilter/rmilter.conf.d
%config(noreplace) %{_sysconfdir}/rmilter/%{name}.conf
%config(noreplace) %{_sysconfdir}/rmilter/%{name}.conf.common
%config(noreplace) %{_sysconfdir}/rmilter/%{name}.conf.sysvinit

%changelog
* Mon Jul 06 2015 Vsevolod Stakhov <vsevolod-at-highsecure.ru> 1.6.3-1
- Update to 1.6.3
