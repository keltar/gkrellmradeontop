.PHONY: all clean run

CC:=gcc
CFLAGS:=-O2 -g0 -pipe -fPIC -Wall -Wextra -Winit-self `pkg-config gtk+-2.0 --cflags`
TARGET:=gkrellmradeontop.so
SRCS:=gkrellmradeontop.c
OBJS:=$(patsubst %.c, %.o, $(SRCS))
DEPS:=$(patsubst %.c, %.d, $(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -shared $^ -o $@

%.o: %c
	$(CC) $(CFLAGS) -c $< -o $@ -MMD

clean:
	$(RM) $(TARGET) $(DEPS) $(OBJS)

run: $(TARGET)
	gkrellm -p $(TARGET)

-include $(DEPS)
