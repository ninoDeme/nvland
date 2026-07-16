all:
	meson compile -C build && ./build/nvland

run:
	./build/nvland

build:
	meson compile -C build

clean:
	rm -rf ./build

setup:
	meson setup build
