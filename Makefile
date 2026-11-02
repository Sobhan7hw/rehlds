# ShotFix 1.1 - Metamod plugin build file (Linux, CS 1.6 / REHLDS)
#
# Adjust these paths to your SDK locations:
#   HLSDK   = Half-Life 1 SDK (common/ contains extdll.h, const.h, ...)
#   METAMOD = Metamod-R source dir containing meta_api.h / h_export.h
#   AMXX    = AMX Mod X sdk include dir (if you use its headers)

HLSDK    ?= ../hl-sdk
METAMOD  ?= ../metamod-r/core
AMXX     ?= ../amxmodx/sdk

# Must be compiled as C++ (metamod/extdll headers are C++)
CXX      ?= g++
TARGET   = shotfix.so

INCS     = -I$(HLSDK)/common -I$(HLSDK)/dlls -I$(HLSDK)/engine -I$(HLSDK)/pm_shared \
           -I$(METAMOD) -I$(AMXX)/include

# 32-bit is required for classic (x86) ReHLDS/metamod-r builds;
# remove -m32 only if you run a native x64 ReHLDS + metamod-r x64.
CXXFLAGS = -O2 -Wall -fPIC -m32 -mmmx -g -fno-strict-aliasing $(INCS)
LDFLAGS  = -m32 -shared
LIBS     = -lm

OBJS     = shotfix.o

all: $(TARGET)

$(TARGET): $(OBJS)
$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
rm -f $(OBJS) $(TARGET)

install: $(TARGET)
cp -f $(TARGET) "$(INSTALLDIR)"

.PHONY: all clean install
