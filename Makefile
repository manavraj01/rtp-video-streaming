# Primary target is Linux:
#   sudo apt install libavformat-dev libavcodec-dev libavutil-dev
#   make
#
# On a machine without pkg-config entries for FFmpeg, point FFMPEG_DEV at
# the SDK root instead:
#   make FFMPEG_DEV=/path/to/ffmpeg-dev

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS ?=

ifdef FFMPEG_DEV
CXXFLAGS += -I$(FFMPEG_DEV)/include
LDFLAGS  += -L$(FFMPEG_DEV)/lib -lavformat -lavcodec -lavutil
else
CXXFLAGS += $(shell pkg-config --cflags libavformat libavcodec libavutil)
LDFLAGS  += $(shell pkg-config --libs libavformat libavcodec libavutil)
endif

.PHONY: all clean

all: rtp_sender rtp_receiver

rtp_sender: sender/main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

rtp_receiver: receiver/main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f rtp_sender rtp_receiver rtp_sender.exe rtp_receiver.exe
