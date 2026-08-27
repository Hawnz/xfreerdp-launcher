CC ?= gcc
PKG_CONFIG ?= pkg-config
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0)
SSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl)
SSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl)

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=gnu11 $(GTK_CFLAGS) $(SSL_CFLAGS)
LDFLAGS += $(GTK_LIBS) $(SSL_LIBS)

TARGET := xfreerdp-launcher
SRCS := src/main.c src/config.c
OBJS := $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.c src/launcher.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install
