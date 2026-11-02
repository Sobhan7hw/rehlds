# ClientShot 1.0 (+ ShotFix) - Metamod plugin build file (Linux, CS 1.6 / ReHLDS)
#
# Adjust these paths to your SDK locations:
#   HLSDK   = Half-Life 1 SDK (common/ contains extdll.h, const.h, ...)
#   METAMOD = Metamod-R source dir containing meta_api.h / h_export.h
#   AMXX    = AMX Mod X sdk include dir (if you use its headers)

HLSDK    ?= ../hl-sdk
METAMOD  ?= ../metamod-r/core
AMXX     ?= ../amxmodx/sdk

CXX      ?= g++
TARGET   = shotfix.so
TARGET2  = clientshot_server.so

INCS     = -I$(HLSDK)/common -I$(HLSDK)/dlls -I$(HLSDK)/engine -I$(HLSDK)/pm_shared \
           -I$(METAMOD) -I$(AMXX)/include

# 32-bit is required for classic (x86) ReHLDS/metamod-r builds;
# remove -m32 only if you run a native x64 ReHLDS + metamod-r x64.
CXXFLAGS = -O2 -Wall -fPIC -m32 -mmmx -g -fno-strict-aliasing $(INCS)
LDFLAGS  = -m32 -shared
LIBS     = -lm

all: $(TARGET) $(TARGET2)

$(TARGET): shotfix.o
$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(TARGET2): clientshot_server.o
$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
rm -f shotfix.o clientshot_server.o $(TARGET) $(TARGET2)

install: $(TARGET) $(TARGET2)
cp -f $(TARGET) "$(INSTALLDIR)"

.PHONY: all clean install

# ---------------------------------------------------------------------------
# CLIENT MODULE (Windows only) - build on a Windows box with MinGW-w64/MSVC:
#   mingw32-make -f Makefile.client      OR      the single line below:
#   i686-w64-mingw32-g++ -O2 -shared -m32 -DCSH_IN_CLDLL clientshot_client.cpp \
#        -o clientshot.dll -ladvapi32
# See header of clientshot_client.cpp for deployment reality-check: the HL
# engine does NOT auto-execute downloaded dlls; you need a launcher/injector.
# ---------------------------------------------------------------------------
