PROG1 = aib100
OBJS1 = main.c Model.c Video.c imgprovider.c labelparse.c detection_nms.c occupancy.c ota_translator.c ACAP.c cJSON.c B100.c
PROGS = $(PROG1)

PKGS = glib-2.0 gio-2.0 gio-unix-2.0 liblarod vdostream axevent axparameter fcgi libcurl

CFLAGS += -DLAROD_API_VERSION_3 -Wall -Wno-format-overflow -Wno-format-truncation
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS += -s -lm -ldl -lpthread

all: $(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(PROGS) *.o *.eap* *_LICENSE.txt manifest.json package.conf* param.conf