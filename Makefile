# ZenLock Makefile
# ====================
# Builds a single C binary that replaces both daemon.c and monitor.py.

CC      = gcc
STD     = -std=c11
WARN    = -Wall -Wextra -Wpedantic
OPT     = -O2
THREAD  = -pthread
TARGET  = zenlock
SRC     = src/zenlock.c
PIPE_PATH = /tmp/zenlock.pipe

.PHONY: all ai no-curl clean run run-pomodoro run-pause lock unlock pause resume pomo-start pomo-stop quit help

# Default: try libcurl, fall back if not found
all:
	@if curl-config --libs > /dev/null 2>&1; then \
		echo "[make] Building with Gemini AI support (libcurl found)"; \
		$(CC) $(STD) $(WARN) $(OPT) $(THREAD) $$(curl-config --cflags) $(SRC) -o $(TARGET) $$(curl-config --libs) -lpthread; \
	else \
		echo "[make] libcurl not found - building without AI (keyword-only mode)"; \
		$(CC) $(STD) $(WARN) $(OPT) $(THREAD) -DNO_CURL $(SRC) -o $(TARGET) -lpthread; \
	fi
	@echo "Built ./$(TARGET)"

ai:
	$(CC) $(STD) $(WARN) $(OPT) $(THREAD) $$(curl-config --cflags) $(SRC) -o $(TARGET) $$(curl-config --libs) -lpthread
	@echo "Built ./$(TARGET) (with Gemini AI)"

no-curl:
	$(CC) $(STD) $(WARN) $(OPT) $(THREAD) -DNO_CURL $(SRC) -o $(TARGET) -lpthread
	@echo "Built ./$(TARGET) (no AI, keyword-only)"

clean:
	rm -f $(TARGET)
	rm -f $(PIPE_PATH)

run: $(TARGET)
	./$(TARGET)

run-pomodoro: $(TARGET)
	./$(TARGET) --pomodoro

run-pause: $(TARGET)
	./$(TARGET) --pause

lock:
	echo "LOCK" > $(PIPE_PATH)
unlock:
	echo "UNLOCK" > $(PIPE_PATH)
pause:
	echo "PAUSE" > $(PIPE_PATH)
resume:
	echo "RESUME" > $(PIPE_PATH)
pomo-start:
	echo "POMO_START" > $(PIPE_PATH)
pomo-stop:
	echo "POMO_STOP" > $(PIPE_PATH)
quit:
	echo "QUIT" > $(PIPE_PATH)

help:
	@echo "make / make ai / make no-curl / make run / make run-pomodoro"
	@echo "Pipe: make lock | unlock | pause | resume | pomo-start | pomo-stop | quit"
