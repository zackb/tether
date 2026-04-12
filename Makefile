default: debug

debug:
	cmake --preset debug
	cmake --build --preset debug
	ln -sf build/debug/compile_commands.json

release:
	cmake --preset release
	cmake --build --preset release
	ln -sf build/release/compile_commands.json

test: debug
	ctest --test-dir build/debug --output-on-failure

install: release
	cmake --install build/release

run-daemon: debug
	./build/debug/tetherd

run-cli: debug
	./build/debug/tether

run-gtk: debug
	./build/debug/tether-gtk

.PHONY: fmt
fmt:
	@echo "Formatting code with clang-format..."
	@find ./src -name "*.cpp" -o -name "*.hpp" -print0 | xargs -0 -n 1 clang-format -i
	@echo "Done."

clean:
	rm -rf build
	rm -f compile_commands.json
