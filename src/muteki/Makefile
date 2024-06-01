CC = arm-none-bestaeabi-gcc
ELF2BESTAPE = elf2bestape
TARBALL = InfoNES097J

# InfoNES
.CFILES =	./../K6502.cpp \
		./../InfoNES.cpp \
		./../InfoNES_Mapper.cpp \
		./../InfoNES_pAPU.cpp \
		./InfoNES_System_Muteki.cpp

.OFILES	=	$(.CFILES:.cpp=.o)

#CCFLAGS = `gtk-config --cflags` -O6
#CCFLAGS_NOOPT = `gtk-config --cflags` -O0
#LDFLAGS = `gtk-config --libs gthread` -lstdc++		# gcc3.x.x

CCFLAGS = -Wall -O3 -pipe -std=c++11
CCFLAGS_NOOPT = -Wall -O0 -pipe -std=c++11
LDFLAGS = -lstdc++			# gcc3.x.x

all: InfoNES.exe

InfoNES.elf: $(.OFILES)
	$(CC) $(INCLUDES) -o $@ $(.OFILES) $(LDFLAGS) -lm

InfoNES.exe: InfoNES.elf
	$(ELF2BESTAPE) -o $@ $<

# To avoid the gcc optimization bug
./../InfoNES_pAPU.o: ./../InfoNES_pAPU.cpp
	$(CC) $(INCLUDES) -c $(CCFLAGS) ./../InfoNES_pAPU.cpp -o ./../InfoNES_pAPU.o

.cpp.o:
	$(CC) $(INCLUDES) -c $(CCFLAGS) $*.cpp  -o $@

clean:
	rm -f $(.OFILES) ../*~ ../*/*~ core

cleanall:
	rm -f $(.OFILES) ../*~ ../*/*~ core InfoNES

release: clean all

tar:
	( cd ..; \
	tar cvf $(TARBALL).tar ./*; \
	gzip $(TARBALL).tar \
	)

install:
	install ./InfoNES /usr/local/bin

# End of Makefile
