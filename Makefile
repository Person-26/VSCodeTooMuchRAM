CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Os -flto=auto -Wall -Wextra -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables
# link libstdc++/libgcc in (only the parts used) but keep libc shared: lowest real
# memory (PSS) and faster startup than loading all of libstdc++.so
LDFLAGS  ?= -s -Wl,--gc-sections -static-libstdc++ -static-libgcc
PREFIX   ?= $(HOME)/.local

all: vsc vsc-pdf

vsc: vsc.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# the PDF page renderer (Okular's engine, so Qt): its own build, needs okular-devel. vsc works without it.
vsc-pdf: vsc-pdf.cpp CMakeLists.txt
	cmake -S . -B build-pdf >/dev/null
	cmake --build build-pdf
	cp build-pdf/vsc-pdf $@

# fully static build: lowest RSS figure, but higher real memory (PSS) since nothing is shared with other processes
static: vsc.cpp
	$(CXX) $(CXXFLAGS) -static -o vsc $< $(LDFLAGS)

install: all
	install -Dm755 vsc $(PREFIX)/bin/vsc
	install -Dm755 vsc-pdf $(PREFIX)/bin/vsc-pdf

clean:
	rm -rf vsc vsc-pdf build-pdf

.PHONY: all static install clean
