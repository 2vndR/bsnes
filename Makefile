ifeq ($(shell uname -s),Darwin)
default: cocoa
else
default: sdl
endif

BIN := build/bin
OBJ := build/obj

CC := clang

CFLAGS += -Werror -Wall -std=gnu11 -ICore -D_GNU_SOURCE
SDL_LDFLAGS := -lSDL
LDFLAGS += -lc -lm

ifeq ($(shell uname -s),Darwin)
CFLAGS += -F/Library/Frameworks
OCFLAGS += -x objective-c -fobjc-arc -Wno-deprecated-declarations -isysroot $(shell xcode-select -p)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.11.sdk -mmacosx-version-min=10.9
LDFLAGS += -framework AppKit
SDL_LDFLAGS := -framework SDL
endif

cocoa: $(BIN)/Sameboy.app
sdl: $(BIN)/sdl/sameboy $(BIN)/sdl/dmg_boot.bin $(BIN)/sdl/cgb_boot.bin
bootroms: $(BIN)/BootROMs/cgb_boot.bin $(BIN)/BootROMs/dmg_boot.bin

CORE_SOURCES := $(shell echo Core/*.c)
SDL_SOURCES := $(shell echo SDL/*.c)

ifeq ($(shell uname -s),Darwin)
COCOA_SOURCES := $(shell echo Cocoa/*.m)
SDL_SOURCES += $(shell echo SDL/*.m)
endif

CORE_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(CORE_SOURCES))
COCOA_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(COCOA_SOURCES))
SDL_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(SDL_SOURCES))

ALL_OBJECTS := $(CORE_OBJECTS) $(COCOA_OBJECTS) $(SDL_OBJECTS)

# Automatic dependency generation

-include $(ALL_OBJECTS:.o=.dep)

$(OBJ)/%.dep: %
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MT $(OBJ)/$^.o -M $^ -c -o $@

$(OBJ)/%.c.o: %.c
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	
$(OBJ)/%.m.o: %.m
	-@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OCFLAGS) -c $< -o $@

# Cocoa Port

$(BIN)/Sameboy.app: $(BIN)/Sameboy.app/Contents/MacOS/Sameboy \
					$(shell echo Cocoa/*.icns) \
					$(shell echo Cocoa/info.plist) \
					$(BIN)/BootROMs/dmg_boot.bin \
					$(BIN)/BootROMs/cgb_boot.bin \
					$(BIN)/Sameboy.app/Contents/Resources/Base.lproj/Document.nib \
					$(BIN)/Sameboy.app/Contents/Resources/Base.lproj/MainMenu.nib
	mkdir -p $(BIN)/Sameboy.app/Contents/Resources
	cp Cocoa/*.icns $(BIN)/BootROMs/dmg_boot.bin $(BIN)/BootROMs/cgb_boot.bin $(BIN)/Sameboy.app/Contents/Resources/
	cp Cocoa/info.plist $(BIN)/Sameboy.app/Contents/

$(BIN)/Sameboy.app/Contents/MacOS/Sameboy: $(CORE_OBJECTS) $(COCOA_OBJECTS)
	-@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS) -framework OpenGL -framework AudioUnit

$(BIN)/Sameboy.app/Contents/Resources/Base.lproj/%.nib: Cocoa/%.xib
	ibtool --compile $@ $^
	
$(BIN)/sdl/sameboy: $(CORE_OBJECTS) $(SDL_OBJECTS)
	-@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS) $(SDL_LDFLAGS)
	
$(BIN)/BootROMs/%.bin: BootROMs/%.asm
	-@mkdir -p $(dir $@)
	cd BootROMs && rgbasm -o ../$@.tmp ../$<
	rgblink -o $@.tmp2 $@.tmp
	head -c $(if $(filter dmg,$(CC)), 256, 2309) $@.tmp2 > $@
	@rm $@.tmp $@.tmp2

$(BIN)/sdl/%.bin: $(BIN)/BootROMs/%.bin
	-@mkdir -p $(dir $@)
	cp -f $^ $@
	
clean:
	rm -rf build
	