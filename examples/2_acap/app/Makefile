PROG = queue_display
PKGS = glib-2.0 gio-2.0 axparameter libcurl

CFLAGS  += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
CFLAGS  += -Wall -Wno-format-overflow
LDFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDFLAGS += -lpthread -s

all: $(PROG)

$(PROG): main.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

clean:
	rm -rf $(PROG) *.o *.eap* *_LICENSE.txt manifest.json package.conf* param.conf
