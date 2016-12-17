# shairport24bit

$ sudo apt get install build-essential automake libtool libdaemon-dev libasound2-dev libpopt-dev libconfig-dev avahi-daemon libavahi-client-dev libssl-dev libpolarssl-dev libsoxr-dev

$ autoreconf -i -f

$ ./configure --sysconfdir=/etc --with-alsa --with-avahi --with-ssl=openssl --with-metadata --with-soxr --with-systemd

$ make
or $ make CFLAGS=-O3

$ sudo make install
