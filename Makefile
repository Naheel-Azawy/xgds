PREFIX    = /usr/local
BINPREFIX = $(DESTDIR)$(PREFIX)/bin

xgds: xgds.cpp
	g++ -o xgds xgds.cpp -lX11 -lXext -lXrandr -Wall

clean:
	rm -f xgds

install: xgds
	mkdir -p $(BINPREFIX)
	cp -f xgds $(BINPREFIX)/

uninstall:
	rm -f $(BINPREFIX)/xgds

.PHONY: install uninstall clean

