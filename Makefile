CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Os -flto=auto -Wall -Wextra -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables
# link libstdc++/libgcc in (only the parts used) but keep libc shared: lowest real
# memory (PSS) and faster startup than loading all of libstdc++.so
LDFLAGS  ?= -s -Wl,--gc-sections -static-libstdc++ -static-libgcc
PREFIX   ?= $(HOME)/.local

vsc: vsc.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# fully static build: lowest RSS figure, but higher real memory (PSS) since nothing is shared with other processes
static: vsc.cpp
	$(CXX) $(CXXFLAGS) -static -o vsc $< $(LDFLAGS)

install: vsc
	install -Dm755 vsc $(PREFIX)/bin/vsc

clean:
	rm -f vsc

.PHONY: static install clean
