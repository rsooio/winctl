CC      = x86_64-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LIBS    = -lole32 -loleaut32 -luuid -luser32 -luiautomationcore -lshell32

OBJ = main.o json.o win.o uia.o

winctl.exe: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LIBS)

%.o: %.c json.h win.h uia.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) winctl.exe

.PHONY: clean
