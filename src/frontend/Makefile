# src/frontend/Makefile — standalone frontend builds (test + preview).
# Does NOT replace the root Makefile. For frontend development only.
#
#   make test      build + run behavioral tests (GL-free)
#   make preview   build SDL2+GL preview binary
#   make clean

CC       ?= cc
CFLAGS   ?= -O2 -std=c99 -Wall -Wextra
CFLAGS   += -Wno-unused-function

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs   2>/dev/null)

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  GL_LIBS  := -framework OpenGL
  CFLAGS   += -DGL_SILENCE_DEPRECATION
else
  GL_LIBS  := -lGL
endif

BUILD := ../../build/frontend

.PHONY: all test preview clean

all: test preview

$(BUILD):
	@mkdir -p $(BUILD)

# ---- GL-free state-machine test ----
test: $(BUILD)/frontend_test
	$(BUILD)/frontend_test

$(BUILD)/frontend_test: ../../tools/frontend_test.c frontend.c frontend.h | $(BUILD)
	$(CC) $(CFLAGS) -I. ../../tools/frontend_test.c frontend.c -o $@ -lm

# ---- SDL2+GL interactive preview ----
preview: $(BUILD)/frontend_preview
	@echo "Run:  $(BUILD)/frontend_preview [width height]"
	@echo "  default 640x480, try 1280 720 for widescreen"

$(BUILD)/frontend_preview: ../../tools/frontend_preview.c frontend.c frontend_draw.c frontend.h frontend_draw.h | $(BUILD)
	$(CC) $(CFLAGS) -I. $(SDL_CFLAGS) ../../tools/frontend_preview.c frontend.c frontend_draw.c -o $@ $(SDL_LIBS) $(GL_LIBS) -lm

clean:
	rm -rf $(BUILD)
