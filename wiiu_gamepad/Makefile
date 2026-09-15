# The Wii U GamePad client. Not part of the host's build, on purpose:
# it needs libdrc, a fork of x264 and a C++ compiler, and a machine with
# no Realtek adapter has no use for it. The host stays one gcc line over
# a list of C files.
#
# The two vendored libraries live in vendor/ beside this file, copied
# from the sibling project. They are NOT in git -- see .gitignore -- so a
# fresh checkout has to get them again, either by copying:
#
#   cp -a ../../bottom_screen_server/gamepad/vendor .
#
# (cp -a, not -r: libx264.so is a symlink to libx264.so.140 and a
# dereferenced copy links but confuses anyone reading the directory)
#
# or by pointing at them where they already are:
#
#   make VENDOR=../../bottom_screen_server/gamepad/vendor
#
# See ../WIIU_GAMEPAD_HANDOVER.md for what they are and why neither can
# be the system's.

VENDOR ?= vendor

CXX      ?= g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 \
            -I$(VENDOR)/libdrc/include -I$(VENDOR)/x264/include \
            $(shell pkg-config --cflags libavcodec libavutil libswscale opus cairo 2>/dev/null)

# rpath rather than LD_LIBRARY_PATH, so it runs from wherever it is
# without anybody setting anything. $ORIGIN is resolved by the loader at
# run time, not by make.
LDFLAGS  := -L$(VENDOR)/libdrc -L$(VENDOR)/x264/lib \
            -Wl,-rpath,'$$ORIGIN/$(VENDOR)/libdrc' \
            -Wl,-rpath,'$$ORIGIN/$(VENDOR)/x264/lib'
LDLIBS   := -ldrc -lx264 \
            $(shell pkg-config --libs libavcodec libavutil libswscale opus cairo 2>/dev/null) \
            -lpthread

all: wiiu_pad

wiiu_pad: wiiu_pad.cpp pad_menu.cpp pad_menu.h ../c2s_protocol.h ../gamepad_bridge.h
	@test -d $(VENDOR)/libdrc || { \
	  echo "No libdrc under $(VENDOR). It is not in git; copy it back:"; \
	  echo "  cp -a ../../bottom_screen_server/gamepad/vendor ."; \
	  echo "or build against it where it is:"; \
	  echo "  make VENDOR=../../bottom_screen_server/gamepad/vendor"; \
	  exit 1; }
	$(CXX) $(CXXFLAGS) -o $@ wiiu_pad.cpp pad_menu.cpp $(LDFLAGS) $(LDLIBS)

clean:
	rm -f wiiu_pad

.PHONY: all clean
