# shairport24bit

$ sudo apt get install build-essential automake libtool libdaemon-dev libasound2-dev libpopt-dev libconfig-dev avahi-daemon libavahi-client-dev libssl-dev libpolarssl-dev libsoxr-dev

$ autoreconf -i -f

$ ./configure --sysconfdir=/etc --with-alsa --with-avahi --with-ssl=openssl --with-metadata --with-soxr --with-systemd

$ make
or $ make CFLAGS=-O3

$ sudo make install


--- about new option

	shairport-sync add to option "-f or --format=", new (default -f 16).

	if use 24 or 32 bit of DAC, editting "/var/www/inc/playerlib.php" file at "shairport-sync" command line.

	case using 16 bit DAC (no editting or add "-f 16")
	$cmd = '/usr/local/bin/shairport-sync -a "' . $_SE...

	case using 24 bit DAC
	$cmd = '/usr/local/bin/shairport-sync -f 24 -a "' . $_SE...

	case using 32 bit DAC
	$cmd = '/usr/local/bin/shairport-sync -f 32 -a "' . $_SE...