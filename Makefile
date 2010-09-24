#%seteval X86 uname -m|grep i.86
#%setifdef X86 -march=i586 ARCH
#%ifswitch --diet diet DIET
#%setifdef DIET -nostdinc DIETINC
#%setifdef DIET -L/opt/diet/lib DIETLIB
#%prepifdef DIET /opt/diet/lib:/opt/diet/include
#%switch --prefix PREFIX
#%switch --mandir MANDIR
#%switch --sysconfdir SYSCONFDIR
#%ifnswitch --prefix /usr PREFIX
#%ifnswitch --sysconfdir /etc SYSCONFDIR
#%ifnswitch --mandir $(PREFIX)/share/man MANDIR
#?V=`cat version.txt|cut -d ' ' -f 2`
#?CFLAGS=$(ARCH) -Os -Wall -DVERSION=\"$(V)\" -DPREFIX=\"$(PREFIX)\" -DSYSCONFDIR=\"$(SYSCONFDIR)\"
#?CC=$(DIET) gcc $(DIETINC)
#?eth-affinity:	aff.o jelopt.o jelist.o
#?	$(CC) -static $(DIETLIB) -o eth-affinity aff.o jelopt.o jelist.o
#?install:	eth-affinity
#?	strip eth-affinity
#?	rm -f $(PREFIX)/bin/eth-affinity
#?	cp -f eth-affinity $(PREFIX)/bin
#?	chown root.root $(PREFIX)/bin/eth-affinity
#?clean:
#?	rm -f eth-affinity *.o
#?tarball:	clean
#?	make-tarball.sh
X86=i686
ARCH=-march=i586
DIET= diet
DIETINC=-nostdinc
DIETLIB=-L/opt/diet/lib
PREFIX= /usr
SYSCONFDIR= /etc
MANDIR= $(PREFIX)/share/man
V=`cat version.txt|cut -d ' ' -f 2`
CFLAGS=$(ARCH) -Os -Wall -DVERSION=\"$(V)\" -DPREFIX=\"$(PREFIX)\" -DSYSCONFDIR=\"$(SYSCONFDIR)\"
CC=$(DIET) gcc $(DIETINC)
eth-affinity:	aff.o jelopt.o jelist.o
	$(CC) -static $(DIETLIB) -o eth-affinity aff.o jelopt.o jelist.o
install:	eth-affinity
	strip eth-affinity
	rm -f $(PREFIX)/bin/eth-affinity
	cp -f eth-affinity $(PREFIX)/bin
	chown root.root $(PREFIX)/bin/eth-affinity
clean:
	rm -f eth-affinity *.o
tarball:	clean
	make-tarball.sh
