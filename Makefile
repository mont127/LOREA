BINDIR ?= $(HOME)/bin
BUILD  ?= build
JOBS   ?= 8

.PHONY: all build install uninstall reinstall run clean

all: build

build:
	@cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release >/dev/null
	@cmake --build $(BUILD) -j$(JOBS)

install: build
	@mkdir -p "$(BINDIR)"
	@install -m 0755 "$(BUILD)/ocli" "$(BINDIR)/ocli"
	@install -m 0755 search_backend.py "$(BINDIR)/search_backend.py"
	@install -m 0755 nvidia_backend.py "$(BINDIR)/nvidia_backend.py"
	@echo "Installed: $(BINDIR)/ocli  (+ search_backend.py, nvidia_backend.py alongside it)"
	@case ":$$PATH:" in *":$(BINDIR):"*) echo "Run it from anywhere:  ocli" ;; \
	  *) echo "NOTE: add $(BINDIR) to your PATH, then run:  ocli" ;; esac

uninstall:
	@rm -f "$(BINDIR)/ocli" "$(BINDIR)/search_backend.py" "$(BINDIR)/nvidia_backend.py"
	@echo "Removed ocli from $(BINDIR)"

reinstall: uninstall install

run: build
	@$(BUILD)/ocli

clean:
	@rm -rf $(BUILD)
