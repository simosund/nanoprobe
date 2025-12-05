Name: nanoprobe
Version: 1.0.0
Release: 1
Summary: Latency measurement tool
Group: Applications/Internet
License: MIT
URL: https://github.com/simosund/nanoprobe
Source: https://packages.nntb.no/software/nanoprobe/%{name}-%{version}.tar.xz

AutoReqProv: on
BuildRequires: cmake
BuildRequires: gcc
BuildRequires: gcc-g++
BuildRoot: %{_tmppath}/%{name}-%{version}-build

Requires: iproute
Recommends: dynmhs
Recommends: hipercontracer
Recommends: netperfmeter
Recommends: subnetcalc
Suggests: td-system-info


%description
A high time accuracy ping-like program, based on nanoping
(https://github.com/iij/nanoping), that relies on the hardware time stamping
function of the Network Interface Card (NIC). The nanoprobe tool enables
precise packet timestamping at NIC phy/mac layer, and thereby allows precise
measurements of the actual network latency that is unaffected by any
additional delay/jitter components form the local network stack or
application. Timestamp resolution and precision is defined by the hardware
oscillator on the Ethernet controller.

%prep
%setup -q

%build
%cmake -DCMAKE_INSTALL_PREFIX=/usr .
%cmake_build

%install
%cmake_install

%files
%{_bindir}/nanoprobe
%{_datadir}/bash-completion/completions/nanoprobe
%{_mandir}/man1/nanoprobe.1.gz

%doc


%changelog
* Tue Dec 04 2025 Thomas Dreibholz <dreibh@simula.no> - 1.0.0
- New upstream release.
