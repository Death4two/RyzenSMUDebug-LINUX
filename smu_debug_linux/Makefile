CC      ?= gcc
CFLAGS   = -Wall -Wextra -O2 -I. -I./ryzen_smu_lib \
           -fPIE -fstack-protector-strong -Wformat=2 -Werror=format-security \
           -D_FORTIFY_SOURCE=2
LDFLAGS  = -lpthread -lm -pie

# Optional GTK4 for GUI (smu_debug_tool --gui)
GTK_CFLAGS := $(shell pkg-config --cflags gtk4 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk4 2>/dev/null)

TARGET   = smu_debug_tool
OBJS     = launcher.o smu_debug_tool.o libsmu.o

ifneq ($(GTK_CFLAGS),)
  CFLAGS  += $(GTK_CFLAGS) -DHAVE_GTK
  LDFLAGS += $(GTK_LIBS)
  OBJS    += smu_gui.o
  HAVE_GTK = 1
endif

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@if [ -n "$(HAVE_GTK)" ]; then echo "Build complete. Run with --gui for the GUI."; fi

launcher.o: launcher.c smu_common.h
	$(CC) $(CFLAGS) -c $< -o $@

smu_debug_tool.o: smu_debug_tool.c smu_common.h
	$(CC) $(CFLAGS) -c $< -o $@

smu_gui.o: smu_gui.c smu_common.h
	$(CC) $(CFLAGS) -c $< -o $@

libsmu.o: ryzen_smu_lib/libsmu.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f launcher.o smu_debug_tool.o smu_gui.o libsmu.o $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -Dm 644 com.ryzen.smudebug.policy /usr/share/polkit-1/actions/com.ryzen.smudebug.policy
	install -Dm 644 com.ryzen.smudebug.desktop /usr/share/applications/com.ryzen.smudebug.desktop

.PHONY: all clean install
