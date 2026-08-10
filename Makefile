PREFIX    = /usr/local
BINPREFIX = $(DESTDIR)$(PREFIX)/bin
CXX       = g++
CXXFLAGS  = -Wall -Wextra
CXXLIBS   = -lX11 -lXext -lXrandr

xgds: xgds.cpp
	$(CXX) -o xgds xgds.cpp $(CXXLIBS) $(CXXFLAGS)

clean:
	rm -f xgds

install: xgds
	mkdir -p $(BINPREFIX)
	cp -f xgds $(BINPREFIX)/

uninstall:
	rm -f $(BINPREFIX)/xgds

.PHONY: install uninstall clean

